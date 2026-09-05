# -*- coding: utf-8 -*-
"""把 .LOC 檔案匯入資料庫 -> parse -> 寫入 nodes 表。

依遊戲世代走不同解包路徑（見 loc_codec.uses_codec）：
- Blood Money：呼叫 HitmanKi.exe 解包成巢狀括號 TXT，再用 loc_txt_format.parse()。
- Hitman 2 / Contracts：沒有可用的外部工具，直接用純 Python 的 loc_codec.decode()
  把 .LOC 二進位解成同一種 fmt.Container 樹（格式見 G:\\bloodmoney\\md\\file\\loc解析.md
  §4 / §6）。
`.txt` 來源一律當成已經是巢狀括號格式，直接讀取、不呼叫任何工具。
"""
import datetime
import sqlite3
import subprocess
import sys
from pathlib import Path

import db as dbmod
import loc_codec
import loc_txt_format as fmt

TOOL_DIR = Path(__file__).resolve().parent.parent / "tools"
UNPACKER = TOOL_DIR / "HitmanKi.exe"


def unpack_loc(loc_path: Path, txt_out: Path) -> None:
    """呼叫 HitmanKi.exe 把 .LOC 解包成巢狀括號 TXT。"""
    if not UNPACKER.exists():
        raise FileNotFoundError(f"找不到 HitmanKi.exe：{UNPACKER}")
    result = subprocess.run(
        [str(UNPACKER), str(loc_path), str(txt_out)],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0 or not txt_out.exists():
        raise RuntimeError(
            f"HitmanKi.exe 解包失敗（returncode={result.returncode}）\n"
            f"stdout: {result.stdout}\nstderr: {result.stderr}"
        )


def load_unpacked_text(src_path: Path, txt_cache_path: Path = None) -> str:
    """回傳巢狀括號 TXT 內容：`.LOC` 呼叫 HitmanKi.exe 解包；`.txt` 直接讀取檔案
    （假設內容已經是 HitmanKi.exe 解包後的巢狀括號格式，不再呼叫外部工具）。"""
    src_path = Path(src_path)
    if src_path.suffix.lower() == ".txt":
        return src_path.read_text(encoding="utf-8", newline="")
    if txt_cache_path is None:
        raise ValueError("來源不是 .txt 時必須提供 txt_cache_path")
    unpack_loc(src_path, txt_cache_path)
    return Path(txt_cache_path).read_text(encoding="utf-8", newline="")


def _insert_children(cur: sqlite3.Cursor, scene_id: int, parent_id, children) -> None:
    for seq, node in enumerate(children):
        if isinstance(node, fmt.Container):
            cur.execute(
                "INSERT INTO nodes"
                "(scene_id, parent_id, seq, node_type, name, has_value,"
                " source_value, translated_value, trailing, status)"
                " VALUES (?,?,?,?,?,?,?,?,?,?)",
                (scene_id, parent_id, seq, "container", node.name, 0, None, None, None, "na"),
            )
            node_id = cur.lastrowid
            _insert_children(cur, scene_id, node_id, node.children)
        else:
            has_value = 1 if node.value is not None else 0
            status = "untranslated" if has_value else "na"
            cur.execute(
                "INSERT INTO nodes"
                "(scene_id, parent_id, seq, node_type, name, has_value,"
                " source_value, translated_value, trailing, status)"
                " VALUES (?,?,?,?,?,?,?,?,?,?)",
                (
                    scene_id, parent_id, seq, "entry", node.key, has_value,
                    node.value, None, node.trailing, status,
                ),
            )


def load_tree(
    loc_path: Path,
    scene_name: str,
    game_key: str = dbmod.DEFAULT_GAME,
    txt_cache_dir: Path = None,
) -> fmt.Container:
    """把一份 .LOC / .txt 來源解成 fmt.Container 樹。

    - `.txt`：一律當巢狀括號格式直接 parse（不分遊戲）。
    - `.LOC` + 走 codec 的遊戲（H2 / Contracts）：loc_codec.decode()。
    - `.LOC` + Blood Money：HitmanKi.exe 解包成 TXT 再 parse（並把中繼 TXT 留在
      txt_cache_dir，維持既有行為）。
    """
    loc_path = Path(loc_path)
    if loc_path.suffix.lower() == ".txt":
        return fmt.parse(load_unpacked_text(loc_path))
    if loc_codec.uses_codec(game_key):
        return loc_codec.decode_file(loc_path, game_key)
    txt_cache_dir = Path(txt_cache_dir) if txt_cache_dir else loc_path.parent
    txt_cache_dir.mkdir(parents=True, exist_ok=True)
    txt_path = txt_cache_dir / f"{scene_name}_unpacked.txt"
    return fmt.parse(load_unpacked_text(loc_path, txt_path))


def import_scene(
    conn: sqlite3.Connection,
    loc_path: Path,
    scene_name: str = None,
    txt_cache_dir: Path = None,
    game_key: str = dbmod.DEFAULT_GAME,
) -> int:
    """匯入一份 .LOC 或 .txt（巢狀括號格式）檔案為一個 scene。重複匯入同名 scene
    會整個重建（覆蓋舊的 source_value/樹狀結構，但目前實作不保留舊的
    translated_value——見專案 README「已知限制」）。回傳 scene id。
    """
    loc_path = Path(loc_path)
    scene_name = scene_name or loc_path.stem
    tree = load_tree(loc_path, scene_name, game_key, txt_cache_dir)

    cur = conn.cursor()
    now = datetime.datetime.now().isoformat(timespec="seconds")
    row = cur.execute("SELECT id FROM scenes WHERE name = ?", (scene_name,)).fetchone()
    if row:
        scene_id = row["id"]
        cur.execute("DELETE FROM nodes WHERE scene_id = ?", (scene_id,))
        cur.execute("UPDATE scenes SET imported_at = ? WHERE id = ?", (now, scene_id))
    else:
        cur.execute(
            "INSERT INTO scenes(name, imported_at) VALUES (?, ?)", (scene_name, now)
        )
        scene_id = cur.lastrowid

    _insert_children(cur, scene_id, None, tree.children)
    conn.commit()
    return scene_id


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("用法: python import_loc.py <LOC檔案路徑> [scene名稱] [遊戲代號]")
        print(f"  遊戲代號預設 {dbmod.DEFAULT_GAME}，可選：{list(dbmod.GAMES)}")
        sys.exit(1)
    loc_file = Path(sys.argv[1])
    name = sys.argv[2] if len(sys.argv) > 2 else None
    game = sys.argv[3] if len(sys.argv) > 3 else dbmod.DEFAULT_GAME

    conn = dbmod.connect(dbmod.game_db_path(game))
    dbmod.init_db(conn)
    scene_id = import_scene(conn, loc_file, name, game_key=game)
    count = conn.execute(
        "SELECT COUNT(*) c FROM nodes WHERE scene_id=? AND node_type='entry' AND has_value=1",
        (scene_id,),
    ).fetchone()["c"]
    print(f"[OK] 已匯入 scene_id={scene_id}，可翻譯 entry 共 {count} 筆")
