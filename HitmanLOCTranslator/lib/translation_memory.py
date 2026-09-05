# -*- coding: utf-8 -*-
"""翻譯記憶：可依「分類+key+原文」、「key+原文」或「只看原文」判斷
entry 是否為相同項目，翻完一次可同步套用到全部相同項目（跨全部 scene）。

預設行為只填「目前還沒有譯文」的項目（避免不小心覆蓋掉某個項目故意給的
不同翻譯）。overwrite=True 才會覆蓋既有譯文，供使用者手動選擇。
"""
import sqlite3
from typing import List, Optional

MATCH_CATEGORY_KEY_SOURCE = "category_key_source"
MATCH_KEY_SOURCE = "key_source"
MATCH_SOURCE_ONLY = "source_only"


def category_path(conn: sqlite3.Connection, node_id: int) -> str:
    """回傳 entry 所屬容器的完整分類路徑（由最外層到最內層）。"""
    rows = conn.execute(
        """
        WITH RECURSIVE ancestors(id, parent_id, name, depth) AS (
            SELECT id, parent_id, name, 0 FROM nodes WHERE id = ?
            UNION ALL
            SELECT n.id, n.parent_id, n.name, ancestors.depth + 1
            FROM nodes n JOIN ancestors ON n.id = ancestors.parent_id
        )
        SELECT name FROM ancestors WHERE depth > 0 ORDER BY depth DESC
        """,
        (node_id,),
    ).fetchall()
    return "|".join(row["name"] for row in rows)


def _match_where(match_mode: str) -> str:
    if match_mode == MATCH_SOURCE_ONLY:
        return "node_type='entry' AND has_value=1 AND source_value=? AND id != ?"
    return "node_type='entry' AND has_value=1 AND name=? AND source_value=? AND id != ?"


def _match_params(key: str, source_value: str, exclude_node_id, match_mode: str) -> list:
    if match_mode == MATCH_SOURCE_ONLY:
        return [source_value, exclude_node_id]
    return [key, source_value, exclude_node_id]


def _matching_ids(
    conn: sqlite3.Connection,
    key: str,
    source_value: str,
    exclude_node_id,
    match_mode: str,
    category: Optional[str],
) -> List[int]:
    """找出符合模式的 entry id；分類模式比對完整容器路徑。"""
    where = _match_where(match_mode)
    params = _match_params(key, source_value, exclude_node_id, match_mode)
    if match_mode != MATCH_CATEGORY_KEY_SOURCE:
        return [row["id"] for row in conn.execute(f"SELECT id FROM nodes WHERE {where}", params)]

    rows = conn.execute(
        f"""
        WITH RECURSIVE
        candidates(id, parent_id) AS (
            SELECT id, parent_id FROM nodes WHERE {where}
        ),
        ancestors(entry_id, parent_id, category_path) AS (
            SELECT id, parent_id, '' FROM candidates
            UNION ALL
            SELECT ancestors.entry_id, n.parent_id,
                   CASE WHEN ancestors.category_path = '' THEN n.name
                        ELSE n.name || '|' || ancestors.category_path END
            FROM ancestors JOIN nodes n ON n.id = ancestors.parent_id
            WHERE ancestors.parent_id IS NOT NULL
        )
        SELECT entry_id AS id FROM ancestors
        WHERE parent_id IS NULL AND category_path = ?
        """,
        [*params, category or ""],
    ).fetchall()
    return [row["id"] for row in rows]


def count_matches(
    conn: sqlite3.Connection,
    key: str,
    source_value: str,
    exclude_node_id: Optional[int] = None,
    match_mode: str = MATCH_CATEGORY_KEY_SOURCE,
    category: Optional[str] = None,
) -> int:
    """回傳跨全部 scene、排除自己以外符合比對條件的 entry 數量。"""
    return len(_matching_ids(conn, key, source_value, exclude_node_id, match_mode, category))


def propagate_translation(
    conn: sqlite3.Connection,
    key: str,
    source_value: str,
    translated_value: str,
    status: str,
    exclude_node_id: Optional[int] = None,
    overwrite: bool = False,
    match_mode: str = MATCH_CATEGORY_KEY_SOURCE,
    category: Optional[str] = None,
) -> List[int]:
    """把翻譯套用到符合比對條件的其他 entry，回傳實際被更新的 node id 清單。"""
    ids = _matching_ids(conn, key, source_value, exclude_node_id, match_mode, category)
    if not overwrite and ids:
        placeholders = ",".join("?" for _ in ids)
        ids = [row["id"] for row in conn.execute(
            f"SELECT id FROM nodes WHERE id IN ({placeholders}) "
            "AND (translated_value IS NULL OR translated_value = '')", ids)]
    if ids:
        conn.executemany(
            "UPDATE nodes SET translated_value=?, status=? WHERE id=?",
            [(translated_value, status, node_id) for node_id in ids],
        )
    return ids


def propagate_review(
    conn: sqlite3.Connection,
    key: str,
    source_value: str,
    translated_value: str,
    exclude_node_id: Optional[int] = None,
    overwrite: bool = False,
    status: str = "reviewed",
    match_mode: str = MATCH_CATEGORY_KEY_SOURCE,
    category: Optional[str] = None,
) -> List[int]:
    """把「已審閱」狀態套用到符合比對條件的其他 entry。"""
    ids = _matching_ids(conn, key, source_value, exclude_node_id, match_mode, category)
    if not overwrite and ids:
        placeholders = ",".join("?" for _ in ids)
        ids = [row["id"] for row in conn.execute(
            f"SELECT id FROM nodes WHERE id IN ({placeholders}) AND "
            "(translated_value = ? OR translated_value IS NULL OR translated_value = '')",
            [*ids, translated_value])]
    if ids:
        conn.executemany(
            "UPDATE nodes SET translated_value=?, status=? WHERE id=?",
            [(translated_value, status, node_id) for node_id in ids],
        )
    return ids