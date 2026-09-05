# -*- coding: utf-8 -*-
"""
HitmanKi.exe / HitmanBe.exe 使用的巢狀括號 TXT 格式 parser/serializer。

格式規則（見 G:\\bloodmoney\\md\\血錢事前漢化準備.md 第2、5節）：
    [
        "容器名" [
            "key" = "value"
            "只有key沒有value的特例"
            "子容器" [
                ...
            ]
        ]
    ]

- 字串一律用雙引號包住，字串內的雙引號用 \\" 跳脫；沒有其他跳脫序列
  （已用位元組統計驗證：檔案裡所有反斜線都是 \\" 的一部分）。
- 字串值可以內含真正的換行字元（例如 EULA 長段落、字幕），跳脫規則不變。
- 有些 entry 只有 key、沒有 "= value"（原始資料裡該欄位是空字串），
  parser 用 Entry.value = None 表示這個特例，跟 value = "" 區分開。
- 部分 entry（不論有沒有 value）後面會接 1~2 個不帶引號的數字，
  例如 `"Info01" = "..." 521424 521424` 或 `"M00_BRS_04FactoryGirl" 401328`，
  語意未知（推測是對白/音效的時間碼，跟畫面顯示文字無關），一律原樣保留、
  不解讀、不翻譯，存在 Entry.trailing。已對全檔掃描驗證：這種尾隨內容
  100% 符合「一個數字」或「兩個數字中間一個空白」的格式，沒有其他變體。
- 檔案換行一律是 CRLF。
"""
from dataclasses import dataclass, field
from typing import List, Optional, Union


class LocTxtParseError(Exception):
    pass


@dataclass
class Entry:
    key: str
    value: Optional[str]  # None = 原始檔案中這個 key 沒有 "= value" 部分
    trailing: Optional[str] = None  # 原始尾隨數字metadata（見上方docstring），原樣保留不編輯


@dataclass
class Container:
    name: Optional[str]  # 根容器為 None
    children: List[Union["Container", "Entry"]] = field(default_factory=list)


def _tokenize(text: str):
    """回傳token串流：('[', None) / (']', None) / ('=', None) / ('STR', 字串內容)"""
    tokens = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c in " \t\r\n":
            i += 1
            continue
        if c == "[":
            tokens.append(("[", None))
            i += 1
            continue
        if c == "]":
            tokens.append(("]", None))
            i += 1
            continue
        if c == "=":
            tokens.append(("=", None))
            i += 1
            continue
        if c == '"':
            j = i + 1
            buf = []
            closed = False
            while j < n:
                cj = text[j]
                if cj == "\\" and j + 1 < n and text[j + 1] == '"':
                    buf.append('"')
                    j += 2
                    continue
                if cj == '"':
                    closed = True
                    break
                buf.append(cj)
                j += 1
            if not closed:
                raise LocTxtParseError(f"未封閉的字串，開始於位置 {i}")
            tokens.append(("STR", "".join(buf)))
            i = j + 1
            continue
        # WORD：不帶引號的裸露字元序列，目前已知只用於 entry 尾隨的數字metadata
        j = i
        while j < n and text[j] not in ' \t\r\n[]="':
            j += 1
        if j == i:
            raise LocTxtParseError(f"未預期字元 {c!r}，位置 {i}")
        tokens.append(("WORD", text[i:j]))
        i = j
    return tokens


class _TokenStream:
    def __init__(self, tokens):
        self._tokens = tokens
        self._pos = 0

    def peek(self):
        if self._pos >= len(self._tokens):
            return (None, None)
        return self._tokens[self._pos]

    def next(self):
        tok = self.peek()
        if tok[0] is None:
            raise LocTxtParseError("未預期的檔案結尾")
        self._pos += 1
        return tok

    def expect(self, kind):
        tok = self.next()
        if tok[0] != kind:
            raise LocTxtParseError(f"預期 {kind!r}，實際得到 {tok!r}")
        return tok


def _consume_trailing_words(ts: _TokenStream) -> Optional[str]:
    words = []
    while ts.peek()[0] == "WORD":
        words.append(ts.next()[1])
    return " ".join(words) if words else None


def _parse_children(ts: _TokenStream) -> List[Union[Container, Entry]]:
    children = []
    while ts.peek()[0] == "STR":
        key_tok = ts.next()
        name_or_key = key_tok[1]
        nxt = ts.peek()
        if nxt[0] == "[":
            ts.next()
            sub_children = _parse_children(ts)
            ts.expect("]")
            children.append(Container(name_or_key, sub_children))
        elif nxt[0] == "=":
            ts.next()
            val_tok = ts.expect("STR")
            trailing = _consume_trailing_words(ts)
            children.append(Entry(name_or_key, val_tok[1], trailing))
        else:
            trailing = _consume_trailing_words(ts)
            children.append(Entry(name_or_key, None, trailing))
    return children


def parse(text: str) -> Container:
    tokens = _tokenize(text)
    ts = _TokenStream(tokens)
    ts.expect("[")
    children = _parse_children(ts)
    ts.expect("]")
    if ts.peek()[0] is not None:
        raise LocTxtParseError("結尾根容器 ']' 之後還有多餘內容")
    return Container(None, children)


def _quote(s: str) -> str:
    return '"' + s.replace('"', '\\"') + '"'


def serialize(root: Container) -> str:
    """把 Container 樹重建回 HitmanBe.exe 吃得下的 TXT（CRLF 換行）。"""
    lines: List[str] = []

    def emit(node, depth):
        indent = "\t" * depth
        if isinstance(node, Container):
            if node.name is None:
                lines.append("[")
            else:
                lines.append(f"{indent}{_quote(node.name)} [")
            for child in node.children:
                emit(child, depth + 1)
            lines.append(f"{indent}]")
        else:
            key_part = _quote(node.key)
            if node.value is None:
                head = key_part
            else:
                head = f"{key_part} = {_quote(node.value)}"
            # trailing 內的 NUL 是 loc_codec 給 H2/Contracts「顯式空葉」用的內部標記
            # （見 loc_codec._EMPTY_LEAF_MARK / _BM_CMT），不是 HitmanBe 文法的一部分，
            # 純文字傾印時濾掉。
            trailing = (node.trailing or "").replace("\x00", " ").strip()
            if trailing:
                lines.append(f"{indent}{head} {trailing}")
            else:
                lines.append(f"{indent}{head}")

    emit(root, 0)
    return "\r\n".join(lines) + "\r\n"


if __name__ == "__main__":
    import sys

    src_path = sys.argv[1] if len(sys.argv) > 1 else (
        r"G:\bloodmoney\Hitman Blood Money PRM+LOC tool\M00_main_unpacked.txt"
    )
    with open(src_path, "r", encoding="utf-8", newline="") as f:
        original = f.read()

    tree = parse(original)
    rebuilt = serialize(tree)

    if rebuilt == original:
        print(f"[OK] round-trip 100% byte-identical（{len(original)} bytes）")
    else:
        print("[FAIL] round-trip 有差異")
        import difflib

        diff = difflib.unified_diff(
            original.splitlines(keepends=True),
            rebuilt.splitlines(keepends=True),
            fromfile="original",
            tofile="rebuilt",
        )
        for line in list(diff)[:50]:
            print(line, end="")
        sys.exit(1)
