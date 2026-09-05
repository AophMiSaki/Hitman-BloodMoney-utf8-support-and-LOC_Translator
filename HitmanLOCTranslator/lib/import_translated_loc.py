# -*- coding: utf-8 -*-
"""匯入「已翻譯好的LOC/TXT」——跟既有「新增LOC」（import_loc.import_scene）不同：這
是把一份已經含有中文譯文的 .LOC 或 .txt 檔案（例如別人翻好的、或自己先前匯出的）
合併回資料庫，而不是灌入一份全新的、未翻譯的 scene 骨架。

流程：
1. 用 original_db.ensure_original_db() 確保「原始參考資料庫」存在，當作唯一的
   原文/結構比對基準。
2. validate_and_diff()：驗證這份LOC的scene名稱/樹狀結構（容器與key的排列，不比
   對數值內容）確實跟遊戲原始資料吻合，不吻合就視為不是這個遊戲的LOC檔，丟
   LocValidationError；通過後逐一比對每個entry，匯入值跟原始英文值不同才算「真
   的有翻譯」，其餘（原封不動的英文/未翻譯fallback）一律忽略，避免拿fallback英
   文覆蓋掉別人已經做好的翻譯。
3. 呼叫端（gui.py）依使用者選擇套用：apply_cross_scene()（依相同key套用到目前
   資料庫全部scene）/ apply_same_scene()（只套用到這份LOC對應的scene）/
   create_database_from_translations()（建立全新資料庫）。
"""
import sqlite3
from pathlib import Path
from typing import List, NamedTuple, Optional

import batch_import
import db as dbmod
import export_loc
import import_loc
import loc_txt_format as fmt
import original_db


def _import_txt_cache_dir(game_key: str = dbmod.DEFAULT_GAME) -> Path:
    return batch_import.txt_cache_dir(game_key)


class LocValidationError(Exception):
    """匯入的LOC檔案結構跟遊戲原始資料對不上，判定不是這個遊戲的LOC檔。"""


class ExtractedTranslation(NamedTuple):
    key: str
    source_value: str
    translated_value: str


def _find_original_loc_file(scene_name: str, game_key: str = dbmod.DEFAULT_GAME) -> Optional[Path]:
    matches = list(batch_import.orig_loc_dir(game_key).rglob(f"{scene_name}.LOC"))
    return matches[0] if matches else None


def _diff_trees(
    imported: fmt.Container, original: fmt.Container, path: str = ""
) -> List[ExtractedTranslation]:
    """逐節點並排走訪兩棵樹，結構（node_type/name/順序）須完全一致，否則視為不
    是同一份LOC（LocValidationError）。回傳「匯入值跟原始英文值不同」的entry清
    單，即真正有翻譯內容的項目。"""
    if len(imported.children) != len(original.children):
        raise LocValidationError(
            f"節點 {path or '(root)'} 底下的項目數量不符"
            f"（匯入檔 {len(imported.children)} 筆 / 遊戲原始 {len(original.children)} 筆）"
        )
    results: List[ExtractedTranslation] = []
    for a, b in zip(imported.children, original.children):
        if type(a) is not type(b):
            raise LocValidationError(f"節點 {path or '(root)'} 底下項目型別不符")
        if isinstance(a, fmt.Container):
            if a.name != b.name:
                raise LocValidationError(
                    f"節點 {path or '(root)'} 底下容器名稱不符：{a.name!r} != {b.name!r}"
                )
            results.extend(_diff_trees(a, b, f"{path}/{a.name}"))
        else:
            if a.key != b.key:
                raise LocValidationError(
                    f"節點 {path or '(root)'} 底下 key 不符：{a.key!r} != {b.key!r}"
                )
            if a.value is None or b.value is None:
                continue  # 裸key沒有可翻譯內容
            if a.value != b.value:
                results.append(ExtractedTranslation(a.key, b.value, a.value))
    return results


def validate_and_diff(loc_path, game_key: str = dbmod.DEFAULT_GAME):
    """驗證+解析一份已翻譯LOC/TXT，回傳 (scene_name, List[ExtractedTranslation])。
    接受 `.LOC`（Blood Money 呼叫 HitmanKi.exe；H2/Contracts 用 loc_codec）或
    `.txt`（假設已是巢狀括號格式，直接讀取）兩種格式，見 import_loc.load_tree()。
    驗證失敗丟 LocValidationError。"""
    loc_path = Path(loc_path)
    scene_name = loc_path.stem

    orig_db_path = original_db.ensure_original_db(game_key)
    orig_conn = dbmod.connect(orig_db_path)
    try:
        row = orig_conn.execute("SELECT id FROM scenes WHERE name=?", (scene_name,)).fetchone()
        if row is None:
            raise LocValidationError(
                f"原始參考資料庫裡找不到名為「{scene_name}」的 scene，"
                f"這份LOC/TXT可能不是 {dbmod.GAMES[game_key]['label']} 的檔案。"
            )
        original_tree = export_loc.build_tree(orig_conn, row["id"])
    finally:
        orig_conn.close()

    cache_dir = _import_txt_cache_dir(game_key)
    cache_dir.mkdir(parents=True, exist_ok=True)
    imported_tree = import_loc.load_tree(loc_path, scene_name, game_key, cache_dir)

    translations = _diff_trees(imported_tree, original_tree)
    # 套用階段(apply_cross_scene/apply_same_scene)依使用者需求只認key、不分樹狀
    # 位置，所以同一個key在scene裡出現多次時（例如同一個key依平台/難度各出現
    # 一份，這款遊戲的LOC檔很常見）必須先在這裡去重，只保留最後一次出現的值，
    # 否則套用時同一個key的多筆UPDATE會互相覆蓋，且回報的更新筆數會重複計算。
    deduped = {}
    for t in translations:
        deduped[t.key] = t
    return scene_name, list(deduped.values())


# 覆蓋模式：取代原本的 skip_reviewed 布林值，改成三選一，對應套用「已翻譯LOC/TXT」
# 時使用者可挑選的覆蓋範圍。
OVERWRITE_ALL = "all"  # 全部匯入：不考慮已審閱或已儲存，全部取代
OVERWRITE_SKIP_REVIEWED = "skip_reviewed"  # 覆蓋未翻譯和未審閱（不動已審閱）
OVERWRITE_UNTRANSLATED_ONLY = "untranslated_only"  # 只覆蓋未翻譯


def _overwrite_where_clause(overwrite_mode: str) -> str:
    if overwrite_mode == OVERWRITE_ALL:
        return ""
    if overwrite_mode == OVERWRITE_SKIP_REVIEWED:
        return " AND status NOT IN ('reviewed','reviewed_same')"
    if overwrite_mode == OVERWRITE_UNTRANSLATED_ONLY:
        return " AND status='untranslated'"
    raise ValueError(f"未知的覆蓋模式：{overwrite_mode!r}")


def apply_cross_scene(
    conn: sqlite3.Connection, translations: List[ExtractedTranslation], overwrite_mode: str
) -> dict:
    """依「相同key」套用到目前資料庫全部scene（不比對原文，只認key——同一個key
    在這款遊戲裡幾乎總是對應同一段原文，這是使用者確認過的比對標準）。overwrite_mode
    見 OVERWRITE_ALL/OVERWRITE_SKIP_REVIEWED/OVERWRITE_UNTRANSLATED_ONLY。回傳依「被
    覆蓋前」原始status分類的筆數統計，例如 {"untranslated": 3, "translated": 2}，
    供呼叫端顯示「覆蓋了幾筆未翻譯/未審閱/已審閱」的結果摘要。"""
    where = "node_type='entry' AND has_value=1 AND name=?" + _overwrite_where_clause(overwrite_mode)
    breakdown: dict = {}
    for t in translations:
        rows = conn.execute(f"SELECT id, status FROM nodes WHERE {where}", (t.key,)).fetchall()
        if not rows:
            continue
        for r in rows:
            breakdown[r["status"]] = breakdown.get(r["status"], 0) + 1
        ids = [r["id"] for r in rows]
        placeholders = ",".join("?" * len(ids))
        conn.execute(
            f"UPDATE nodes SET translated_value=?, status='translated' WHERE id IN ({placeholders})",
            (t.translated_value, *ids),
        )
    return breakdown


def apply_same_scene(
    conn: sqlite3.Connection,
    scene_name: str,
    translations: List[ExtractedTranslation],
    overwrite_mode: str,
) -> dict:
    """只套用到這份LOC/TXT對應的scene（依key比對，範圍限定在同一個scene_id）。
    overwrite_mode 意義同 apply_cross_scene。目前資料庫還沒有這個scene時回傳空
    dict，不會自動建立scene骨架（那是「新增LOC」或建立新資料庫流程的責任）。"""
    row = conn.execute("SELECT id FROM scenes WHERE name=?", (scene_name,)).fetchone()
    if row is None:
        return {}
    scene_id = row["id"]
    where = (
        "scene_id=? AND node_type='entry' AND has_value=1 AND name=?"
        + _overwrite_where_clause(overwrite_mode)
    )
    breakdown: dict = {}
    for t in translations:
        rows = conn.execute(
            f"SELECT id, status FROM nodes WHERE {where}", (scene_id, t.key)
        ).fetchall()
        if not rows:
            continue
        for r in rows:
            breakdown[r["status"]] = breakdown.get(r["status"], 0) + 1
        ids = [r["id"] for r in rows]
        placeholders = ",".join("?" * len(ids))
        conn.execute(
            f"UPDATE nodes SET translated_value=?, status='translated' WHERE id IN ({placeholders})",
            (t.translated_value, *ids),
        )
    return breakdown


def create_database_from_translations(
    game_key: str,
    new_name: str,
    scene_name: str,
    translations: List[ExtractedTranslation],
    overwrite_mode: str = OVERWRITE_ALL,
):
    """建立一份新資料庫：先用遊戲原始LOC匯入這份LOC對應的scene骨架（原文/譯文分
    離，全部entry初始status='untranslated'），再套用這次解析出的翻譯內容。命名
    重複、找不到對應原始LOC都會丟例外，且不會留下殘缺的db檔案。overwrite_mode
    意義同 apply_cross_scene（新骨架全部是untranslated，三種模式此時效果相同，
    保留參數是為了跟既有資料庫匯入流程共用同一個「覆蓋範圍」選擇對話框、並回傳
    一致格式的breakdown統計）。回傳 (db_path, breakdown)。"""
    if dbmod.custom_db_name_exists(game_key, new_name):
        raise ValueError(f"資料庫名稱「{new_name}」已經存在，請換一個名稱")

    original_loc_file = _find_original_loc_file(scene_name, game_key)
    if original_loc_file is None:
        raise LocValidationError(f"找不到「{scene_name}」對應的遊戲原始 .LOC 檔案")

    db_path = dbmod.custom_db_path(game_key, new_name)
    conn = dbmod.connect(db_path)
    try:
        dbmod.init_db(conn)
        import_loc.import_scene(conn, original_loc_file, scene_name=scene_name, game_key=game_key)
        breakdown = apply_same_scene(conn, scene_name, translations, overwrite_mode)
        conn.commit()
    except Exception:
        conn.close()
        db_path.unlink(missing_ok=True)
        raise
    conn.close()
    return db_path, breakdown
