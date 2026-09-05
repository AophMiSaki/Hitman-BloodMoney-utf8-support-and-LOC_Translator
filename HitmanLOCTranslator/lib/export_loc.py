# -*- coding: utf-8 -*-
"""從資料庫重建樹狀結構 -> 打包成 .LOC。

依遊戲世代（見 loc_codec.uses_codec）：
- Blood Money：重建巢狀括號 TXT -> 呼叫 HitmanBe.exe 打包。
- Hitman 2 / Contracts：純 Python loc_codec.encode() 直接輸出 .LOC 二進位。
"""
import subprocess
import sqlite3
import sys
from pathlib import Path

import db as dbmod
import loc_codec
import loc_txt_format as fmt

TOOL_DIR = Path(__file__).resolve().parent.parent / "tools"
PACKER = TOOL_DIR / "HitmanBe.exe"


def build_tree(conn: sqlite3.Connection, scene_id: int) -> fmt.Container:
    cur = conn.cursor()

    def build(parent_id):
        rows = cur.execute(
            "SELECT * FROM nodes WHERE scene_id=? AND parent_id IS ? ORDER BY seq",
            (scene_id, parent_id),
        ).fetchall()
        children = []
        for row in rows:
            if row["node_type"] == "container":
                children.append(fmt.Container(row["name"], build(row["id"])))
            else:
                if row["has_value"]:
                    value = row["translated_value"]
                    if value is None or value == "":
                        value = row["source_value"]  # 未翻譯時退回原文，維持LOC可用
                else:
                    value = None
                children.append(fmt.Entry(row["name"], value, row["trailing"]))
        return children

    return fmt.Container(None, build(None))


def pack_txt(txt_path: Path, loc_out: Path) -> None:
    if not PACKER.exists():
        raise FileNotFoundError(f"找不到 HitmanBe.exe：{PACKER}")
    result = subprocess.run(
        [str(PACKER), str(txt_path), str(loc_out)],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0 or not loc_out.exists():
        raise RuntimeError(
            f"HitmanBe.exe 打包失敗（returncode={result.returncode}）\n"
            f"stdout: {result.stdout}\nstderr: {result.stderr}"
        )


def _scene_id(conn: sqlite3.Connection, scene_name: str) -> int:
    row = conn.execute("SELECT id FROM scenes WHERE name = ?", (scene_name,)).fetchone()
    if not row:
        raise ValueError(f"找不到 scene：{scene_name}")
    return row["id"]


def _scene_text(conn: sqlite3.Connection, scene_name: str) -> str:
    return fmt.serialize(build_tree(conn, _scene_id(conn, scene_name)))


def export_scene_txt(conn: sqlite3.Connection, scene_name: str, txt_out_path: Path) -> Path:
    """把資料庫譯文重建成巢狀括號 TXT（跟 HitmanKi.exe 解包出來的格式一致，不呼叫
    HitmanBe.exe、不產生 .LOC）。對 Blood Money 這是 HitmanBe 的輸入；對 H2 /
    Contracts 純粹是人類可讀的傾印（沒有吃這個格式的外部打包器）。"""
    text = _scene_text(conn, scene_name)
    txt_out_path = Path(txt_out_path)
    txt_out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(txt_out_path, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    return txt_out_path


def export_scene(
    conn: sqlite3.Connection,
    scene_name: str,
    loc_out_path: Path,
    txt_cache_path: Path = None,
    game_key: str = dbmod.DEFAULT_GAME,
) -> Path:
    """把資料庫譯文打包成 .LOC。

    Blood Money：先重建 TXT（留在 txt_cache_path），再呼叫 HitmanBe.exe 打包。
    Hitman 2 / Contracts：loc_codec.encode() 直接寫出 .LOC 二進位，不產生 TXT。
    """
    loc_out_path = Path(loc_out_path)
    if loc_codec.uses_codec(game_key):
        tree = build_tree(conn, _scene_id(conn, scene_name))
        data = loc_codec.encode(tree, game_key)
        loc_out_path.parent.mkdir(parents=True, exist_ok=True)
        loc_out_path.write_bytes(data)
        return loc_out_path

    txt_cache_path = Path(txt_cache_path) if txt_cache_path else loc_out_path.with_suffix(".txt")
    export_scene_txt(conn, scene_name, txt_cache_path)
    pack_txt(txt_cache_path, loc_out_path)
    return loc_out_path


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("用法: python export_loc.py <scene名稱> <輸出路徑> [--txt] [--game <遊戲代號>]")
        print(f"  預設打包成 .LOC；加上 --txt 只打包成 TXT。遊戲代號預設 {dbmod.DEFAULT_GAME}")
        sys.exit(1)
    scene_name = sys.argv[1]
    out_path = Path(sys.argv[2])
    rest = sys.argv[3:]
    as_txt = "--txt" in rest
    game = rest[rest.index("--game") + 1] if "--game" in rest else dbmod.DEFAULT_GAME

    conn = dbmod.connect(dbmod.game_db_path(game))
    dbmod.init_db(conn)
    if as_txt:
        result_path = export_scene_txt(conn, scene_name, out_path)
    else:
        result_path = export_scene(conn, scene_name, out_path, game_key=game)
    print(f"[OK] 已匯出至 {result_path}")
