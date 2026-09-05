# -*- coding: utf-8 -*-
"""翻譯進度資料庫（SQLite）。

Schema 設計：
- scenes：一份 .LOC 檔案對應一個 scene（例如 "M00_main"）。
- nodes：完整保留原始 TXT 的巢狀樹狀結構（container/entry、parent_id、seq），
  這樣匯出時才能 100% 重建回 HitmanBe.exe 吃得下的格式，不只是存一份扁平的
  key/value 清單。entry 節點才有 source_value/translated_value/status；
  container 節點跟「沒有 = value 的裸 key」(has_value=0) 都不能翻譯。

同一份 LOC 裡跨關卡共用的 key（例如 ClothNames 底下的 M01_.../M02_...）
一樣存在同一個 scene 底下、用 name 分辨，不需要額外欄位處理。
"""
import re
import sqlite3
from pathlib import Path
from typing import List, Tuple

SCHEMA = """
CREATE TABLE IF NOT EXISTS scenes (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE,
    imported_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS nodes (
    id INTEGER PRIMARY KEY,
    scene_id INTEGER NOT NULL REFERENCES scenes(id) ON DELETE CASCADE,
    parent_id INTEGER REFERENCES nodes(id) ON DELETE CASCADE,
    seq INTEGER NOT NULL,
    node_type TEXT NOT NULL CHECK(node_type IN ('container','entry')),
    name TEXT NOT NULL,
    has_value INTEGER NOT NULL DEFAULT 0,
    source_value TEXT,
    translated_value TEXT,
    trailing TEXT,
    status TEXT NOT NULL DEFAULT 'na'
);

CREATE INDEX IF NOT EXISTS idx_nodes_scene ON nodes(scene_id);
CREATE INDEX IF NOT EXISTS idx_nodes_parent ON nodes(parent_id, seq);
CREATE INDEX IF NOT EXISTS idx_nodes_status ON nodes(scene_id, node_type, status);
CREATE INDEX IF NOT EXISTS idx_nodes_key_source ON nodes(name, source_value);
"""


def connect(db_path) -> sqlite3.Connection:
    conn = sqlite3.connect(str(db_path))
    conn.execute("PRAGMA foreign_keys = ON")
    conn.row_factory = sqlite3.Row
    return conn


def init_db(conn: sqlite3.Connection) -> None:
    conn.executescript(SCHEMA)
    conn.commit()


GAMES = {
    "silentassassin": {"label": "Hitman 2: Silent Assassin (2002)", "folder": "SilentAssassin", "db_file": "SilentAssassin_Database.db"},
    "contracts": {"label": "Hitman: Contracts (2004)", "folder": "Contracts", "db_file": "Contracts_Database.db"},
    "bloodmoney": {"label": "Hitman: Blood Money (2006)", "folder": "BloodMoney", "db_file": "BloodMoney_Database.db"},
}
DEFAULT_GAME = "bloodmoney"


def game_db_dir(game_key: str = DEFAULT_GAME) -> Path:
    """每個遊戲的資料庫/原始素材都放在 Database\\<遊戲資料夾名稱>\\ 底下，資料夾
    名稱跟 GAMES[game_key]["folder"] 一致。第一次切到某個遊戲時資料夾可能還不
    存在（例如尚未匯入過的新遊戲），所以這裡順便建立，確保 sqlite3.connect() 一
    定能成功開檔。"""
    # 本模組在 import\ 底下，資料資料夾在其上一層的專案根目錄
    path = Path(__file__).resolve().parent.parent / "Database" / GAMES[game_key]["folder"]
    path.mkdir(parents=True, exist_ok=True)
    return path


def game_db_path(game_key: str = DEFAULT_GAME) -> Path:
    return game_db_dir(game_key) / GAMES[game_key]["db_file"]


def default_db_path() -> Path:
    return game_db_path(DEFAULT_GAME)


# ---------- 多資料庫支援（[[匯入已翻譯LOC]]功能用） ----------
# 每個遊戲底下可以有多個db檔案並存：
#   - 「主要資料庫」：game_db_path() 回傳的既有db，使用者原本就在用的那份，維持不變。
#   - 「原始db」：original_db_path() 回傳的獨立參考db，只放遊戲原始(未翻譯)內容，
#     translated_value 永遠是NULL，不透過GUI手動編輯，專門當作驗證「這份LOC是不是
#     這個遊戲的」以及取得原文的唯一權威來源（見 original_db.py）。
#   - 「自訂db」：使用者透過「匯入已翻譯LOC」建立的新資料庫，檔名規則
#     "{主要db檔名}_custom_{名稱}.db"，用檔名前綴掃描資料夾動態列舉，不需要
#     額外維護一份清單檔。

_CUSTOM_DB_INFIX = "_custom_"


def _db_stem(game_key: str = DEFAULT_GAME) -> str:
    return Path(GAMES[game_key]["db_file"]).stem


def original_db_path(game_key: str = DEFAULT_GAME) -> Path:
    return game_db_dir(game_key) / f"{_db_stem(game_key)}_original.db"


def _sanitize_db_name(name: str) -> str:
    # 保留中日韓文字/英數字/常見符號，其餘一律換成底線，避免變成不合法檔名。
    return re.sub(r"[^\w\-一-鿿]+", "_", name).strip("_") or "custom"


def custom_db_path(game_key: str, name: str) -> Path:
    return game_db_dir(game_key) / f"{_db_stem(game_key)}{_CUSTOM_DB_INFIX}{_sanitize_db_name(name)}.db"


def list_custom_dbs(game_key: str = DEFAULT_GAME) -> List[Tuple[str, Path]]:
    """回傳這個遊戲底下已經存在的自訂db：[(名稱, 路徑), ...]，依檔名排序。"""
    prefix = f"{_db_stem(game_key)}{_CUSTOM_DB_INFIX}"
    result = []
    for path in sorted(game_db_dir(game_key).glob(f"{prefix}*.db")):
        result.append((path.stem[len(prefix):], path))
    return result


def list_all_databases(game_key: str = DEFAULT_GAME) -> List[dict]:
    """回傳這個遊戲可切換的全部資料庫：[{"label","path","kind"}, ...]。
    kind: "main"（既有工作用db）/ "original"（唯讀原始參考db）/ "custom"（使用者自建）。
    「原始db」即使檔案還沒建立也一律列出（第一次選到/第一次匯入時才會自動建立）。
    """
    entries = [{"label": "主要資料庫", "path": game_db_path(game_key), "kind": "main"}]
    entries.append({"label": "原始db", "path": original_db_path(game_key), "kind": "original"})
    for name, path in list_custom_dbs(game_key):
        entries.append({"label": name, "path": path, "kind": "custom"})
    return entries


def custom_db_name_exists(game_key: str, name: str) -> bool:
    """新資料庫命名檢查用：跟保留名稱（主要資料庫/原始db）重複、或這個名稱經過
    _sanitize_db_name() 後會撞到現有自訂db的實際檔案，都算存在。用「檔案是否已
    存在」當唯一判斷依據（而不是拿原始輸入字串去比對既有db的顯示名稱），才能
    跟 custom_db_path() 用同一套正規化邏輯，避免兩邊各自處理造成判斷不一致
    （例如 "_foo" 正規化後會變成 "foo"，若各自比對就會漏判成沒有重複）。"""
    if name in ("主要資料庫", "原始db"):
        return True
    return custom_db_path(game_key, name).exists()
