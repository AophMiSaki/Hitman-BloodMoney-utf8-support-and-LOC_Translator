# -*- coding: utf-8 -*-
"""批次把某個遊戲的原版 .LOC 資料夾整包匯入資料庫。

原始素材放在 Database\\<遊戲資料夾>\\<game_key>_orig_LOC\\（例：BloodMoney\\
bloodmoney_orig_LOC、SilentAssassin\\silentassassin_orig_LOC、Contracts\\
contracts_orig_LOC），見 orig_loc_dir()。每個 .LOC 的檔名（不含副檔名）即 scene
名稱。Blood Money 用 HitmanKi.exe 解包產生的中繼 TXT 集中放在同層
_unpacked_txt_cache\\；H2 / Contracts 走純 Python 不產生中繼檔。
"""
import sys
from pathlib import Path

import db as dbmod
import import_loc


def orig_loc_dir(game_key: str = dbmod.DEFAULT_GAME) -> Path:
    return dbmod.game_db_dir(game_key) / f"{game_key}_orig_LOC"


def txt_cache_dir(game_key: str = dbmod.DEFAULT_GAME) -> Path:
    return dbmod.game_db_dir(game_key) / "_unpacked_txt_cache"


# 舊呼叫端相容：模組層常數維持指向預設遊戲（Blood Money）。
SOURCE_DIR = orig_loc_dir(dbmod.DEFAULT_GAME)
TXT_CACHE_DIR = txt_cache_dir(dbmod.DEFAULT_GAME)


def import_all_scenes(conn, game_key: str = dbmod.DEFAULT_GAME):
    """把該遊戲 orig_loc_dir() 底下全部原版 .LOC 匯入到指定的 conn（不限定是主要
    資料庫，original_db.py 建立原始參考資料庫時也共用這個函式）。回傳 (ok, failed)，
    ok=[(scene_name, count), ...]，failed=[(scene_name, error_message), ...]。
    """
    source_dir = orig_loc_dir(game_key)
    cache_dir = txt_cache_dir(game_key)
    if not source_dir.exists():
        raise FileNotFoundError(f"找不到來源資料夾：{source_dir}")

    loc_files = sorted(source_dir.rglob("*.LOC"))
    if not loc_files:
        raise FileNotFoundError(f"{source_dir} 底下沒有任何 .LOC 檔案")

    ok, failed = [], []
    for loc_path in loc_files:
        scene_name = loc_path.stem
        try:
            scene_id = import_loc.import_scene(
                conn, loc_path, scene_name=scene_name,
                txt_cache_dir=cache_dir, game_key=game_key,
            )
            count = conn.execute(
                "SELECT COUNT(*) c FROM nodes WHERE scene_id=? AND node_type='entry' AND has_value=1",
                (scene_id,),
            ).fetchone()["c"]
            print(f"[OK] {scene_name:24s} <- {loc_path.relative_to(source_dir)}  ({count} 筆可翻譯)")
            ok.append((scene_name, count))
        except Exception as e:
            print(f"[FAIL] {scene_name:24s} <- {loc_path.relative_to(source_dir)}  錯誤：{e}")
            failed.append((scene_name, str(e)))
    return ok, failed


def main():
    game = sys.argv[1] if len(sys.argv) > 1 else dbmod.DEFAULT_GAME
    conn = dbmod.connect(dbmod.game_db_path(game))
    dbmod.init_db(conn)

    try:
        ok, failed = import_all_scenes(conn, game)
    except FileNotFoundError as e:
        print(f"[錯誤] {e}")
        sys.exit(1)

    total = sum(c for _, c in ok)
    print()
    print(f"完成：{len(ok)}/{len(ok) + len(failed)} 個 scene 匯入成功，共 {total} 筆可翻譯 entry。")
    if failed:
        print(f"失敗 {len(failed)} 個：")
        for name, err in failed:
            print(f"  - {name}: {err}")
        sys.exit(1)


if __name__ == "__main__":
    main()
