# -*- coding: utf-8 -*-
"""維護「原始參考資料庫」——只放遊戲原始（未翻譯）LOC 內容，translated_value 永遠
是 NULL，不透過 GUI 手動編輯，專門當作「匯入已翻譯LOC」功能驗證/取得原文的唯一
權威來源（見 import_translated_loc.py）。

跟 batch_import.py 用同一份原始素材（Database\\<遊戲>\\<game_key>_orig_LOC）、
同一套匯入邏輯（import_all_scenes），只是寫入的目標db檔案不同（db.original_db_path()）。
"""
from pathlib import Path

import batch_import
import db as dbmod


def ensure_original_db(game_key: str = dbmod.DEFAULT_GAME, rebuild: bool = False) -> Path:
    """回傳這個遊戲的原始參考db路徑。檔案不存在（或 rebuild=True）時，重新從
    Database\\BloodMoney\\bloodmoney_orig_LOC 匯入全部原始 scene 建立/覆蓋整份db。
    """
    path = dbmod.original_db_path(game_key)
    if path.exists() and not rebuild:
        return path

    conn = dbmod.connect(path)
    try:
        dbmod.init_db(conn)
        ok, failed = batch_import.import_all_scenes(conn, game_key)
    finally:
        conn.close()

    if failed:
        # 建立失敗就不留下殘缺的參考db，避免之後的驗證拿到不完整的比對基準。
        path.unlink(missing_ok=True)
        detail = "; ".join(f"{name}({err})" for name, err in failed)
        raise RuntimeError(f"建立原始參考資料庫時有 {len(failed)} 個檔案匯入失敗：{detail}")
    return path


if __name__ == "__main__":
    result_path = ensure_original_db(rebuild=True)
    print(f"[OK] 原始參考資料庫已建立：{result_path}")
