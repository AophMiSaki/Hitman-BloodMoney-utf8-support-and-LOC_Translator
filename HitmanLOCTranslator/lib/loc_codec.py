# -*- coding: utf-8 -*-
"""純 Python 的 .LOC 二進位 <-> 樹狀結構編解碼器（不呼叫任何外部 .exe）。

支援三個世代的 Glacier `.LOC`：

    family="bm"  Hitman: Blood Money (2006)        —— 有型別位元組 0x10/0x08/0x09/0x28/0x29
    family="h2"  Hitman 2: Silent Assassin (2002)  —— 無型別位元組，字串＝父子 name 對
                 Hitman: Contracts (2004)          —— 與 H2 格式完全相同

樹狀結構直接用 `loc_txt_format.Container` / `loc_txt_format.Entry`（跟 HitmanKi.exe
解包 -> loc_txt_format.parse() 產出的樹一模一樣），所以 import_loc._insert_children /
export_loc.build_tree 兩邊都不必改，DB schema 也不動。

格式權威文件：G:\\bloodmoney\\md\\file\\loc解析.md（BM §1-3、H2 §4、Contracts §6）。
參考實作（皆 round-trip byte-exact）：同目錄 loc_tool_ref.py（BM）、h2_loc_ref.py（H2/H3）。

—— family <-> Entry 欄位對應（decode/encode 兩向一致）——

BM（型別位元語意：0x08 元素基底／0x10 容器／|0x01 內嵌字串／|0x20 資源數字／
    |0x02 附註字串「, "comment"」）：
    0x10 容器            <-> Container(name, children)
    0x08 空元素          <-> Entry(name, value=None,  trailing=None)
    0x28 空元素+1數字    <-> Entry(name, value=None,  trailing="<n>")
    0x09 字串            <-> Entry(name, value=val,   trailing=None)
    0x29 字串+1數字(=0)  <-> Entry(name, value=val,   trailing="0")
    0x29 字串+2數字      <-> Entry(name, value=val,   trailing="<a> <b>")   (實測 a==b)
    0x0b 字串+附註       <-> Entry(name, value=val,   trailing="\\x00<comment>")
    0x2b 字串+附註+2數字 <-> Entry(name, value=val,   trailing="\\x00<comment>\\x00<a> <b>")

    0x0b/0x2b 只在 M02_main / M05_main（ActorSounds 註解、TV 主播稿的角色提示）出現，
    loc解析.md 舊版未記載。<comment> 是設計者註記、遊戲不顯示，原樣保留、不翻譯。
    註：社群工具 HitmanKi.exe 解包成文字時寫作 `"key" = "val" , "comment" [nums]`，
    但本專案的 loc_txt_format.py 尚無法解析那個 `,` 語法（見 export_loc/import_loc 對
    BM 仍走 HitmanKi/HitmanBe 的說明）。

H2 / Contracts（沒有型別、沒有 value 欄、沒有尾隨數字）：
    容器（childCount>=2，或 childCount==1 且其子節點不是「裸葉」）
                                        <-> Container(name, children)
    字串（childCount==1 且唯一子節點是「裸葉」，該葉 name 即譯文）
                                        <-> Entry(key, value=<葉 name>, trailing=None)
    空葉（childCount==0）：
      磁碟上有兩種寫法——
        「裸葉」    name\\0 剛好填滿父層區間，不寫 count 位元組
        「顯式空葉」name\\0 之後補一個 0x00
      實測（H3 的 Civilians/.../SmallTalk/MaleNN）顯示：就算在區間尾，原檔仍
      可能寫「顯式 0x00」，位置無法反推，故必須保存。用 Entry.trailing 標記：
        裸葉       <-> Entry(key, value=None, trailing=None)
        顯式空葉   <-> Entry(key, value=None, trailing=_EMPTY_LEAF_MARK)   ("\\x00")
"""
import struct
from typing import Tuple

import loc_txt_format as fmt

# GAMES key（db.py）-> family
FAMILY_BY_GAME = {
    "bloodmoney": "bm",
    "silentassassin": "h2",
    "contracts": "h2",
}


def family_for_game(game_key: str) -> str:
    try:
        return FAMILY_BY_GAME[game_key]
    except KeyError:
        raise ValueError(f"未知的遊戲代號：{game_key!r}（loc_codec 支援 {list(FAMILY_BY_GAME)}）")


def uses_codec(game_key: str) -> bool:
    """這個遊戲的 .LOC 二進位是否由本模組直接處理（True），或仍走外部 HitmanKi/
    HitmanBe（False，只有 Blood Money —— 它有 0x0b/0x2b 以外還牽動既有已驗證流程，
    保守起見不改）。import_loc / export_loc 用這個決定走哪條路。"""
    return family_for_game(game_key) != "bm"


class LocBinaryError(Exception):
    pass


# =====================================================================
# Blood Money（有型別位元組）
# =====================================================================

_BM_CONTAINER = 0x10
_BM_EMPTY = 0x08
_BM_EMPTY_NUM = 0x28
_BM_STRING = 0x09
_BM_STRING_NUM = 0x29
_BM_STRING_CMT = 0x0B      # 0x09 | 0x02
_BM_STRING_CMT_NUM = 0x2B  # 0x29 | 0x02

# trailing 內用來包住 0x0b/0x2b「附註字串」的分隔符（NUL 不可能出現在正常 trailing）
_BM_CMT = "\x00"


def _bm_decode(data: bytes) -> fmt.Container:
    N = len(data)

    def u32(o: int) -> int:
        return struct.unpack_from("<I", data, o)[0]

    def cstr(o: int) -> Tuple[bytes, int]:
        e = data.find(b"\x00", o)
        if e < 0:
            raise LocBinaryError(f"字串未在 {o:#x} 之後收尾")
        return data[o:e], e + 1

    def item(o: int):
        name, p = cstr(o)
        if p >= N:
            raise LocBinaryError(f"{o:#x} 的元素缺少型別位元組")
        t = data[p]
        p += 1
        if t == _BM_CONTAINER:
            cnt = data[p]
            p += 1
            offs = [0] + [u32(p + 4 * i) for i in range(cnt - 1)]
            p += 4 * (cnt - 1)
            base = p
            kids = []
            end = base
            for k in range(cnt):
                node, end = item(base + offs[k])
                kids.append(node)
            return fmt.Container(name.decode("utf-8", "surrogateescape"), kids), end
        if t == _BM_EMPTY:
            return fmt.Entry(name.decode("utf-8", "surrogateescape"), None, None), p
        if t == _BM_EMPTY_NUM:
            n1 = u32(p)
            p += 4
            return fmt.Entry(name.decode("utf-8", "surrogateescape"), None, str(n1)), p
        if t in (_BM_STRING, _BM_STRING_CMT):
            val, p = cstr(p)
            comment = None
            if t == _BM_STRING_CMT:
                cmt, p = cstr(p)
                comment = cmt.decode("utf-8", "surrogateescape")
            z = u32(p)
            p += 4
            if z != 0:
                raise LocBinaryError(f"{t:#04x} 字串在 {o:#x} 之後尾隨非 0 值 {z}")
            trailing = f"{_BM_CMT}{comment}" if comment is not None else None
            return fmt.Entry(name.decode("utf-8", "surrogateescape"), val.decode("utf-8", "surrogateescape"), trailing), p
        if t in (_BM_STRING_NUM, _BM_STRING_CMT_NUM):
            val, p = cstr(p)
            comment = None
            if t == _BM_STRING_CMT_NUM:
                cmt, p = cstr(p)
                comment = cmt.decode("utf-8", "surrogateescape")
            a = u32(p)
            p += 4
            if a == 0:
                numpart = "0"
            else:
                b = u32(p)
                p += 4
                numpart = f"{a} {b}"
            if comment is not None:
                trailing = f"{_BM_CMT}{comment}{_BM_CMT}{numpart}"
            else:
                trailing = numpart
            return fmt.Entry(name.decode("utf-8", "surrogateescape"), val.decode("utf-8", "surrogateescape"), trailing), p
        raise LocBinaryError(f"未知的元素型別 {t:#x}（位於 {o:#x}）")

    top_cnt = data[0]
    offs = [0] + [u32(1 + 4 * i) for i in range(top_cnt - 1)]
    base = 1 + 4 * (top_cnt - 1)
    tops = [item(base + offs[k])[0] for k in range(top_cnt)]
    return fmt.Container(None, tops)


def _bm_nums_from(part: str, key: str):
    """把 trailing 的數字段（"0" 或 "a b"）轉成要寫進檔案的 u32 tuple。"""
    toks = part.split()
    if len(toks) == 1:
        if toks[0] != "0":
            raise LocBinaryError(f"字串 {key!r} 若只有一個尾隨數字則必為 0（實際 {toks[0]!r}）")
        return (0,)
    if len(toks) == 2:
        return (int(toks[0]), int(toks[1]))
    raise LocBinaryError(f"字串 {key!r} 的尾隨數字段格式非法：{part!r}")


def _bm_fields(entry: fmt.Entry):
    """回傳 (type, comment_or_None, nums_tuple)。nums_tuple 是要寫進檔案的 u32 序列
    （0x09/0x0b 恆為 (0,)＝那個尾端 zero；0x29/0x2b 為 (0,) 或 (a, b)）。"""
    raw = entry.trailing or ""
    if raw.startswith(_BM_CMT):  # 0x0b / 0x2b 附註字串
        if entry.value is None:
            raise LocBinaryError(f"{entry.key!r} 帶附註卻沒有 value（loc_codec 未見此組合）")
        segs = raw[1:].split(_BM_CMT)
        if len(segs) == 1:
            return _BM_STRING_CMT, segs[0], (0,)
        if len(segs) == 2:
            return _BM_STRING_CMT_NUM, segs[0], _bm_nums_from(segs[1], entry.key)
        raise LocBinaryError(f"{entry.key!r} 的附註 trailing 格式非法：{raw!r}")

    trailing = raw.strip()
    if entry.value is None:
        if not trailing:
            return _BM_EMPTY, None, ()
        toks = trailing.split()
        if len(toks) != 1:
            raise LocBinaryError(f"0x28 空元素 {entry.key!r} 的尾隨欄位必須是單一數字：{trailing!r}")
        return _BM_EMPTY_NUM, None, (int(toks[0]),)
    if not trailing:
        return _BM_STRING, None, (0,)
    return _BM_STRING_NUM, None, _bm_nums_from(trailing, entry.key)


def _bm_emit(node) -> bytes:
    if isinstance(node, fmt.Container):
        out = bytearray(node.name.encode("utf-8", "surrogateescape"))
        out.append(0)
        out.append(_BM_CONTAINER)
        kids = node.children
        blobs = [_bm_emit(k) for k in kids]
        acc, offs = 0, []
        for b in blobs:
            offs.append(acc)
            acc += len(b)
        out.append(len(kids))
        for k in range(1, len(kids)):
            out += struct.pack("<I", offs[k])
        for b in blobs:
            out += b
        return bytes(out)

    t, comment, nums = _bm_fields(node)
    out = bytearray(node.key.encode("utf-8", "surrogateescape"))
    out.append(0)
    out.append(t)
    if t == _BM_EMPTY:
        pass
    elif t == _BM_EMPTY_NUM:
        out += struct.pack("<I", nums[0])
    else:  # 0x09 / 0x0b / 0x29 / 0x2b —— 皆先 value\0，帶附註者再 comment\0，最後數字段
        out += node.value.encode("utf-8", "surrogateescape")
        out.append(0)
        if comment is not None:
            out += comment.encode("utf-8", "surrogateescape")
            out.append(0)
        for x in nums:
            out += struct.pack("<I", x)
    return bytes(out)


def _bm_encode(root: fmt.Container) -> bytes:
    tops = root.children
    blobs = [_bm_emit(t) for t in tops]
    acc, offs = 0, []
    for b in blobs:
        offs.append(acc)
        acc += len(b)
    out = bytearray([len(tops)])
    for k in range(1, len(tops)):
        out += struct.pack("<I", offs[k])
    for b in blobs:
        out += b
    return bytes(out)


# =====================================================================
# Hitman 2 / Contracts（無型別位元組）
# =====================================================================

# 顯式空葉（磁碟上 name\0 之後多一個 0x00）的標記，塞在 Entry.trailing。
_EMPTY_LEAF_MARK = "\x00"

# 「字串陣列」節點的標記（見 loc解析.md §4.9）。磁碟佈局：
#     name\0  <u8 marker>  <cstr\0> × (marker - 1)
# ——marker >= 2、其後**沒有 offset 表**，[q,end) 剛好是 (marker-1) 個 NUL 結尾字串。
# 目前整個 corpus 只有 Silent Assassin 8 個任務的 `formatstring` key 用這個構造
# （簡報投影片的 LOC key 路徑模板，如 "Missionbriefing/Missionbriefing%d"；內部字串、
# 中文化時不動）。表示成 Entry(key, value=<第1個字串>,
#     trailing = _FMTSTR_MARK + "\x00".join(<第2個以後的字串>)）。
_FMTSTR_MARK = "\x00fmt\x00"


def _h2_decode(data: bytes) -> fmt.Container:
    N = len(data)

    def u32(o: int) -> int:
        return struct.unpack_from("<I", data, o)[0]

    def raw(beg: int, end: int):
        """回傳原始 h2 節點：dict(name, kids=list|None, explicit0=bool, fmtstr=list|None)。
        explicit0：這是「顯式空葉」（磁碟上寫了 0x00），只有 kids is None 時有意義。
        fmtstr：非 None 時代表「字串陣列」節點（見 _FMTSTR_MARK），其值為字串清單。"""
        z = data.find(b"\x00", beg, end + 1)
        if z < 0:
            raise LocBinaryError(f"名稱字串未在 {beg:#x}..{end:#x} 內收尾")
        name = data[beg:z]
        p = z + 1
        if p == end:
            return {"name": name, "kids": None, "explicit0": False, "fmtstr": None}  # 裸葉
        cnt = data[p]
        p += 1
        if cnt == 0:
            if p != end:
                raise LocBinaryError(f"顯式空葉 {beg:#x} 之後仍有多餘位元組")
            return {"name": name, "kids": None, "explicit0": True, "fmtstr": None}  # 顯式空葉
        if cnt == 1:
            return {"name": name, "kids": [raw(p, end)], "explicit0": False, "fmtstr": None}
        # cnt >= 2：可能是正常容器（後接 offset 表），也可能是「字串陣列」節點
        # （見 _FMTSTR_MARK）——後者沒有 offset 表，[p,end) 剛好是 cnt-1 個 cstr。
        tbl_end = p + 4 * (cnt - 1)
        table_ok = tbl_end <= end
        if table_ok:
            trial = [0] + [u32(p + 4 * i) for i in range(cnt - 1)]
            tb = [tbl_end + o for o in trial] + [end]
            table_ok = all(tbl_end <= tb[k] <= tb[k + 1] <= end for k in range(cnt)) and all(
                data.find(b"\x00", tb[k], tb[k + 1] + 1) >= 0 for k in range(cnt)
            )
        if not table_ok:
            strs, pos = [], p
            while pos < end:
                e = data.find(b"\x00", pos, end)
                if e < 0:
                    break
                strs.append(data[pos:e])
                pos = e + 1
            if pos == end and len(strs) == cnt - 1 >= 1:
                return {
                    "name": name, "kids": None, "explicit0": False,
                    "fmtstr": [s.decode("utf-8", "surrogateescape") for s in strs],
                }
            raise LocBinaryError(
                f"{beg:#x} 的節點 childCount={cnt} 但 offset 表無效、也不是字串陣列節點"
            )
        offs = [0] + [u32(p + 4 * i) for i in range(cnt - 1)]
        p = tbl_end
        bounds = [p + o for o in offs] + [end]
        return {
            "name": name,
            "kids": [raw(bounds[k], bounds[k + 1]) for k in range(cnt)],
            "explicit0": False,
            "fmtstr": None,
        }

    def leaf_entry(node):
        name = node["name"].decode("utf-8", "surrogateescape")
        return fmt.Entry(name, None, _EMPTY_LEAF_MARK if node["explicit0"] else None)

    def to_fmt(node):
        """h2 raw 節點 -> fmt.Container / fmt.Entry。"""
        kids = node["kids"]
        name = node["name"].decode("utf-8", "surrogateescape")
        if node["fmtstr"] is not None:
            vals = node["fmtstr"]
            trailing = _FMTSTR_MARK + "\x00".join(vals[1:])
            return fmt.Entry(name, vals[0], trailing)
        if kids is None:
            return leaf_entry(node)
        if (len(kids) == 1 and kids[0]["kids"] is None
                and not kids[0]["explicit0"] and kids[0]["fmtstr"] is None):
            # 唯一子節點是「裸葉」-> 字串：該葉 name 即譯文
            value = kids[0]["name"].decode("utf-8", "surrogateescape")
            return fmt.Entry(name, value, None)
        return fmt.Container(name, [to_fmt(k) for k in kids])

    top_cnt = data[0]
    offs = [0] + [u32(1 + 4 * i) for i in range(top_cnt - 1)]
    base = 1 + 4 * (top_cnt - 1)
    bounds = [base + o for o in offs] + [N]
    tops = [to_fmt(raw(bounds[k], bounds[k + 1])) for k in range(top_cnt)]
    return fmt.Container(None, tops)


def _h2_emit(node, is_range_tail: bool) -> bytes:
    """把一個 fmt 節點序列化成 h2 位元組。

    is_range_tail：這個節點是否剛好填滿父層給的區間尾。只有「非顯式標記」的空葉
    才靠這個決定要不要省略 0x00；帶 _EMPTY_LEAF_MARK 的一律寫 0x00。
    """
    if isinstance(node, fmt.Entry):
        out = bytearray(node.key.encode("utf-8", "surrogateescape"))
        out.append(0)
        if node.trailing is not None and node.trailing.startswith(_FMTSTR_MARK):
            # 「字串陣列」節點：marker(u8) = 字串數 + 1，其後 marker-1 個 cstr、無 offset 表
            extra = node.trailing[len(_FMTSTR_MARK):]
            vals = [node.value] + (extra.split("\x00") if extra else [])
            out.append(len(vals) + 1)
            for v in vals:
                out += v.encode("utf-8", "surrogateescape")
                out.append(0)
            return bytes(out)
        if node.value is None:
            if node.trailing == _EMPTY_LEAF_MARK or not is_range_tail:
                out.append(0)  # 顯式空葉，或中間空葉的 0x00 佔位
            return bytes(out)
        # 字串：key 之下掛一個 childCount==1 的裸葉，葉 name = 譯文
        out.append(1)
        out += node.value.encode("utf-8", "surrogateescape")
        out.append(0)
        return bytes(out)

    # Container
    out = bytearray(node.name.encode("utf-8", "surrogateescape"))
    out.append(0)
    kids = node.children
    if len(kids) == 0:
        if not is_range_tail:
            out.append(0)
        return bytes(out)
    if len(kids) == 1:
        out.append(1)
        return bytes(out) + _h2_emit(kids[0], True)
    out.append(len(kids))
    blobs = [_h2_emit(k, k is kids[-1]) for k in kids]
    acc, offs = 0, []
    for b in blobs:
        offs.append(acc)
        acc += len(b)
    for k in range(1, len(kids)):
        out += struct.pack("<I", offs[k])
    for b in blobs:
        out += b
    return bytes(out)


def _h2_encode(root: fmt.Container) -> bytes:
    tops = root.children
    blobs = [_h2_emit(t, True) for t in tops]
    acc, offs = 0, []
    for b in blobs:
        offs.append(acc)
        acc += len(b)
    out = bytearray([len(tops)])
    for k in range(1, len(tops)):
        out += struct.pack("<I", offs[k])
    for b in blobs:
        out += b
    return bytes(out)


# =====================================================================
# 對外 API
# =====================================================================


def decode(data: bytes, game_key: str) -> fmt.Container:
    """把 .LOC 位元組解成 fmt.Container 樹（根 name=None，children 為頂層節點）。"""
    fam = family_for_game(game_key)
    if not data:
        raise LocBinaryError("空檔案")
    return _bm_decode(data) if fam == "bm" else _h2_decode(data)


def _assert_packable(root: fmt.Container) -> None:
    """LOC 的字串一律 NUL 結尾、無長度前綴 —— value / 容器名 / key / 附註內含 `\\x00`
    會被當成結尾，靜默截斷整個檔。打包前先擋下來，指名是哪個節點。
    （其餘控制字元 TAB/LF/CR/VT(0x0b)/FF/ESC… 都會逐位元組原樣寫回，不受影響。）"""
    def walk(n, path):
        if isinstance(n, fmt.Container):
            here = f"{path}/{n.name}" if n.name is not None else path
            if n.name is not None and "\x00" in n.name:
                raise LocBinaryError(f"容器名含 NUL：{here!r}")
            for c in n.children:
                walk(c, here)
        else:
            if "\x00" in n.key:
                raise LocBinaryError(f"key 含 NUL：{path}/{n.key!r}")
            if n.value is not None and "\x00" in n.value:
                raise LocBinaryError(
                    f"譯文含 NUL（0x00）無法打包：{path}/{n.key}  —— LOC 字串以 NUL 結尾，"
                    f"請把該字元移除或改用可見替代。"
                )
    walk(root, "")


def encode(root: fmt.Container, game_key: str) -> bytes:
    """把 fmt.Container 樹序列化回 .LOC 位元組。"""
    fam = family_for_game(game_key)
    _assert_packable(root)
    return _bm_encode(root) if fam == "bm" else _h2_encode(root)


def decode_file(path, game_key: str) -> fmt.Container:
    with open(path, "rb") as f:
        return decode(f.read(), game_key)


if __name__ == "__main__":
    import sys

    SAMPLES = {
        "bloodmoney": [
            r"G:\bloodmoney\Hitman Blood Money PRM+LOC tool\blood money\M00_main.LOC",
            r"G:\bloodmoney\Hitman Blood Money PRM+LOC tool\gbk\M00_main.LOC",
        ],
        "silentassassin": [
            r"G:\bloodmoney\Hitman Blood Money PRM+LOC tool\hitman2\LevelMenu.LOC",
            r"G:\bloodmoney\Hitman Blood Money PRM+LOC tool\hitman2\C0-1__MAIN.LOC",
        ],
        "contracts": [
            r"G:\bloodmoney\Hitman Blood Money PRM+LOC tool\hitman3\LevelMenu.LOC",
            r"G:\bloodmoney\Hitman Blood Money PRM+LOC tool\hitman3\C00-1_MAIN.LOC",
        ],
    }
    args = sys.argv[1:]
    fails = 0
    if len(args) == 2:
        items = [(args[0], args[1])]
    else:
        items = [(g, p) for g, ps in SAMPLES.items() for p in ps]
    for game_key, path in items:
        try:
            data = open(path, "rb").read()
        except OSError as e:
            print(f"[skip] {path}  ({e})")
            continue
        try:
            tree = decode(data, game_key)
            back = encode(tree, game_key)
            ok = back == data
            print(f"[{'OK ' if ok else 'BAD'}] {game_key:14s} {len(data):>8d} B  {path}")
            if not ok:
                fails += 1
                for i in range(min(len(data), len(back))):
                    if data[i] != back[i]:
                        print(f"        first diff @ {i:#x}: orig {data[i]:#04x} / mine {back[i]:#04x}")
                        break
                if len(data) != len(back):
                    print(f"        length differs: orig {len(data)} / mine {len(back)}")
        except Exception as e:  # noqa: BLE001
            fails += 1
            print(f"[ERR] {game_key:14s} {path}\n      {type(e).__name__}: {e}")
    sys.exit(1 if fails else 0)
