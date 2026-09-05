# -*- coding: utf-8 -*-
"""Hitman Blood Money LOC 翻譯工具 - tkinter GUI。

功能：匯入 .LOC -> 依 scene/巢狀路徑瀏覽 -> 編輯譯文 -> 匯出 .LOC。
"""
import ctypes
import ctypes.wintypes
import json
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, simpledialog, ttk

import db as dbmod
import export_loc
import import_loc
import import_translated_loc as itl
import original_db
import translation_memory as tm

_CF_UNICODETEXT = 13
_GMEM_MOVEABLE = 0x0002


def _set_clipboard_text(text: str) -> None:
    """透過 Windows API 直接寫入剪貼簿，繞開 Tk 在 Windows 上把 \\n 自動轉回
    \\r\\n 的內建行為（tkinter 的 clipboard_append 無法避免這個轉換）。"""
    user32 = ctypes.windll.user32
    kernel32 = ctypes.windll.kernel32
    kernel32.GlobalAlloc.restype = ctypes.c_void_p
    kernel32.GlobalLock.restype = ctypes.c_void_p
    kernel32.GlobalLock.argtypes = [ctypes.c_void_p]
    kernel32.GlobalUnlock.argtypes = [ctypes.c_void_p]
    user32.SetClipboardData.argtypes = [ctypes.c_uint, ctypes.c_void_p]
    user32.SetClipboardData.restype = ctypes.c_void_p

    data = text.encode("utf-16-le") + b"\x00\x00"
    user32.OpenClipboard(0)
    try:
        user32.EmptyClipboard()
        h_global = kernel32.GlobalAlloc(_GMEM_MOVEABLE, len(data))
        ptr = kernel32.GlobalLock(h_global)
        ctypes.memmove(ptr, data, len(data))
        kernel32.GlobalUnlock(h_global)
        user32.SetClipboardData(_CF_UNICODETEXT, h_global)
    finally:
        user32.CloseClipboard()

_STATUS_KEYS = {
    "untranslated": "未翻譯",
    "translated": "未審閱",
    "reviewed": "已審閱",
    "reviewed_same": "已審閱",
}

# 淺色/深色兩套配色。status 底下四色語意跟原本的白/橘/綠/粉對應，只是深色版
# 降低亮度避免刺眼；其餘欄位對應到 ttk.Style 與純tk元件（Text/Canvas/Menu）
# 各自要手動上色的部分。
THEMES = {
    "light": {
        "bg": "#f0f0f0",
        "fg": "#000000",
        "entry_bg": "#ffffff",
        "select_bg": "#0078d7",
        "select_fg": "#ffffff",
        "text_bg": "#ffffff",
        "text_fg": "#000000",
        "tree_bg": "#ffffff",
        "tree_fg": "#000000",
        "tree_heading_bg": "#e1e1e1",
        "linenum_bg": "#f0f0f0",
        "linenum_fg": "#666666",
        "match_fg": "#0a6",
        "disabled_fg": "#a3a3a3",
        "status": {
            "untranslated": "#ffffff",  # 白底：譯文是空白的
            "translated": "#ffcc80",  # 橘底：譯文有內容，但還沒標記已審閱
            "reviewed": "#a5d6a7",  # 綠底：譯文有內容，且已標記已審閱
            "reviewed_same": "#d9adb8",  # 粉底（不過亮）：譯文=原文，儲存並審閱
        },
    },
    "dark": {
        "bg": "#2b2b2b",
        "fg": "#e0e0e0",
        "entry_bg": "#3c3f41",
        "select_bg": "#3a6ea5",
        "select_fg": "#ffffff",
        "text_bg": "#1e1e1e",
        "text_fg": "#e0e0e0",
        "tree_bg": "#252526",
        "tree_fg": "#e0e0e0",
        "tree_heading_bg": "#333333",
        "linenum_bg": "#2b2b2b",
        "linenum_fg": "#9a9a9a",
        "match_fg": "#4fd17e",
        "disabled_fg": "#6a6a6a",
        "status": {
            "untranslated": "#3a3a3a",
            "translated": "#7a5a1e",
            "reviewed": "#2e5a34",
            "reviewed_same": "#6a3b4d",
        },
    },
}

# 空白/控制字元視覺化：在原文/譯文框裡把「肉眼分不出來」的字元用底色標出來。
# 檢視時常見的困擾是不知道某個位置到底是半形空格、Tab、還是落單的 CR(0x0D)——
# DB 裡多行文字正常用 \r\n 換行（匯出 LOC 要 100% 還原），所以 \r\n 配對只給
# 淡底色當背景資訊；真正可疑的（落單 CR、落單 LF、NBSP、其他控制字元）才加外框
# 一眼跳出來。每個 tag 一組淺色/深色配色。
_WS_TAG_NAMES = (
    "ws_space", "ws_tab", "ws_crlf", "ws_cr_lone", "ws_lf_lone", "ws_nbsp", "ws_ctrl",
    "ws_zw",
)
_WS_STYLES = {
    "light": {
        "ws_space": {"background": "#dbe9ff"},
        "ws_tab": {"background": "#ffe6a8", "borderwidth": 1, "relief": "solid"},
        "ws_crlf": {"background": "#e6e6e6"},
        "ws_cr_lone": {"background": "#ff9a9a", "borderwidth": 1, "relief": "solid"},
        "ws_lf_lone": {"background": "#ffc078", "borderwidth": 1, "relief": "solid"},
        "ws_nbsp": {"background": "#ffb896", "borderwidth": 1, "relief": "solid"},
        "ws_ctrl": {"background": "#ff6b6b", "borderwidth": 1, "relief": "solid"},
        "ws_zw": {"background": "#ff4dff", "borderwidth": 2, "relief": "solid"},
    },
    "dark": {
        "ws_space": {"background": "#2f3f5a"},
        "ws_tab": {"background": "#5c4a1e", "borderwidth": 1, "relief": "solid"},
        "ws_crlf": {"background": "#3a3a3a"},
        "ws_cr_lone": {"background": "#7a2e2e", "borderwidth": 1, "relief": "solid"},
        "ws_lf_lone": {"background": "#7a4a1e", "borderwidth": 1, "relief": "solid"},
        "ws_nbsp": {"background": "#6a3f28", "borderwidth": 1, "relief": "solid"},
        "ws_ctrl": {"background": "#7a2e2e", "borderwidth": 1, "relief": "solid"},
        "ws_zw": {"background": "#a300a3", "borderwidth": 2, "relief": "solid"},
    },
}
# 圖例小標籤：(tag 名, 顯示文字)。顯示文字走 self.T() 可翻譯。
_WS_LEGEND = (
    ("ws_space", "空格"),
    ("ws_tab", "Tab"),
    ("ws_crlf", "換行(CRLF)"),
    ("ws_cr_lone", "落單CR"),
    ("ws_lf_lone", "落單LF"),
    ("ws_nbsp", "NBSP"),
    ("ws_zw", "零寬字元"),
)

# 零寬 / 隱形格式字元：遊戲字幕不會刻意用到，卻會夾在從外部（LLM／翻譯網站／
# 文件）貼進來的譯文裡。編輯框看不出來、也不會被存檔正規化擋掉，會一路留到
# 匯出的 LOC 檔。集中列出，供「顯示空白/控制字元」標示、「儲存時去除」、
# 全域搜尋篩選共用。key 是字元本身、value 是圖例/警告要顯示的名稱。
_INVISIBLE_CHARS = {
    "\u200b": "U+200B ZWSP",
    "\u200c": "U+200C ZWNJ",
    "\u200d": "U+200D ZWJ",
    "\u200e": "U+200E LRM",
    "\u200f": "U+200F RLM",
    "\u2060": "U+2060 WJ",
    "\ufeff": "U+FEFF BOM",
    "\u00ad": "U+00AD SHY",
    "\u202a": "U+202A LRE",
    "\u202b": "U+202B RLE",
    "\u202c": "U+202C PDF",
    "\u202d": "U+202D LRO",
    "\u202e": "U+202E RLO",
    "\u2066": "U+2066 LRI",
    "\u2067": "U+2067 RLI",
    "\u2068": "U+2068 FSI",
    "\u2069": "U+2069 PDI",
}
_INVISIBLE_CHARS_SET = frozenset(_INVISIBLE_CHARS)
_INVISIBLE_DELETE_MAP = {ord(ch): None for ch in _INVISIBLE_CHARS}


def _strip_invisible(text: str):
    """移除零寬/隱形格式字元，回傳 (清理後字串, 移除數量)。"""
    if not text:
        return text, 0
    cleaned = text.translate(_INVISIBLE_DELETE_MAP)
    return cleaned, len(text) - len(cleaned)

_UI_SETTINGS_PATH = Path(__file__).resolve().parent.parent / "ui_settings.json"
_LANG_DIR = Path(__file__).resolve().parent.parent / "Language"

# UI語言：檔名(不含.json) -> 選單顯示名稱。新增語言時在 Language\ 底下加一份
# 對應的 json，並在這裡登記即可，不用改其他程式碼。
LANGUAGES = {
    "zh_TW": "繁體中文",
    "en": "English",
}
DEFAULT_LANGUAGE = "zh_TW"

_lang_cache: dict[str, dict] = {}


def _load_language(code: str) -> dict:
    """讀取 Language\\<code>.json 並快取。找不到檔案/格式錯誤時回傳空dict——
    App.T() 遇到查無key時會直接fallback回傳原始（繁體中文）字串，所以就算
    語言檔缺項目或整份讀取失敗，畫面頂多顯示繁體中文，不會壞掉或丟例外。"""
    if code not in _lang_cache:
        try:
            data = json.loads((_LANG_DIR / f"{code}.json").read_text(encoding="utf-8"))
        except Exception:
            data = {}
        _lang_cache[code] = data
    return _lang_cache[code]


def _load_ui_settings() -> dict:
    try:
        return json.loads(_UI_SETTINGS_PATH.read_text(encoding="utf-8"))
    except Exception:
        return {}


def _save_ui_settings(**updates) -> None:
    """讀取現有設定後合併寫回，避免theme/language各自獨立存檔時互相覆蓋掉
    對方已經存的設定。"""
    data = _load_ui_settings()
    data.update(updates)
    try:
        _UI_SETTINGS_PATH.write_text(json.dumps(data, ensure_ascii=False), encoding="utf-8")
    except Exception:
        pass


def _load_theme_pref() -> str:
    theme = _load_ui_settings().get("theme")
    return theme if theme in THEMES else "light"


def _save_theme_pref(theme: str) -> None:
    _save_ui_settings(theme=theme)


def _load_lang_pref() -> str:
    lang = _load_ui_settings().get("language")
    return lang if lang in LANGUAGES else DEFAULT_LANGUAGE


def _save_lang_pref(lang: str) -> None:
    _save_ui_settings(language=lang)


class App(tk.Tk):
    def __init__(self):
        super().__init__()

        self.lang_code = _load_lang_pref()
        self._lang_dict = _load_language(self.lang_code)

        self.current_game = dbmod.DEFAULT_GAME
        self.current_db_path = dbmod.game_db_path(self.current_game)
        self.current_db_kind = "main"
        self.conn = dbmod.connect(self.current_db_path)
        dbmod.init_db(self.conn)

        self.current_scene_id = None
        self.current_entry_id = None
        self.current_entry_key = None
        self.current_entry_source = None
        self.current_entry_category = None
        self._context_target = None

        self._show_ws = bool(_load_ui_settings().get("show_ws", True))
        self._strip_zw_on_save = bool(_load_ui_settings().get("strip_zw_on_save", True))
        self._ws_highlight_job = None
        # 主搜尋列的「模糊」開關：勾選＝不分大小寫（現行行為），取消＝區分大小寫。
        self._fuzzy_search = bool(_load_ui_settings().get("fuzzy_search", True))

        self.theme_name = _load_theme_pref()
        self._theme = THEMES[self.theme_name]

        self._build_ui()
        self._apply_theme(self.theme_name)
        self._retranslate_ui()
        self.update_idletasks()
        # Tk 對可調整大小的視窗，預設會依packed子元件的自然大小反推出一個隱性
        # 最小視窗尺寸，只要內容自然高度超過螢幕，視窗就會被撐大到那個隱性最小
        # 值，蓋過我們後面 _auto_fit_screen() 裡明確設定的 geometry()。這裡先把
        # 隱性最小值蓋掉，讓 geometry() 真的能把視窗訂在螢幕範圍內。
        self.minsize(400, 300)
        self._auto_fit_screen()
        self._reload_scenes()

    # ---------- 語言（UI字串翻譯） ----------
    def T(self, text: str, **kwargs) -> str:
        """把寫死在程式碼裡的繁體中文字串（可含 {placeholder}）當作查表用的
        key，查目前語言字典找對應顯示文字；查無此key時直接fallback用原始
        繁體中文字串本身（等於繁體中文語言檔可以整份是空的也沒關係）。"""
        template = self._lang_dict.get(text, text)
        return template.format(**kwargs) if kwargs else template

    def _status_label(self, status: str) -> str:
        return self.T(_STATUS_KEYS.get(status, status))

    def _on_language_selected(self, event=None):
        name_to_code = {name: code for code, name in LANGUAGES.items()}
        code = name_to_code.get(self.lang_var.get(), self.lang_code)
        if code == self.lang_code:
            return
        self.lang_code = code
        self._lang_dict = _load_language(self.lang_code)
        self._retranslate_ui()
        _save_lang_pref(self.lang_code)

    def _retranslate_ui(self):
        """語言切換後，重新套用一次全部靜態UI文字（標籤/按鈕/選單/欄位標題），
        並重新整理有翻譯內容的動態區塊（樹狀清單狀態文字、統計數字、資料庫
        下拉選單標籤等），讓畫面不必重開就能整個換語言。"""
        self.title(self.T("Hitman Blood Money LOC 翻譯工具"))

        self.lang_var.set(LANGUAGES[self.lang_code])
        self.theme_btn.configure(
            text=self.T("☀ 淺色模式") if self.theme_name == "dark" else self.T("🌙 深色模式")
        )

        self.game_label.configure(text=self.T("選擇遊戲："))
        self.db_label.configure(text=self.T("資料庫："))
        self.scene_label.configure(text=self.T("Scene："))
        self.import_btn.configure(text=self.T("新增 LOC..."))
        self.import_translated_btn.configure(text=self.T("匯入已翻譯LOC..."))
        self.export_loc_btn.configure(text=self.T("匯出 LOC..."))
        self.export_txt_btn.configure(text=self.T("匯出 TXT..."))
        self.chk_only_untranslated.configure(text=self.T("只顯示未翻譯"))
        self.chk_only_unreviewed.configure(text=self.T("只顯示未審閱"))
        self.chk_always_expand.configure(text=self.T("每次都展開(變數部分)"))

        self.category_label.configure(text=self.T("分類："))
        self.key_label.configure(text=self.T("變數："))
        self.source_search_label.configure(text=self.T("原文："))
        self.translated_search_label.configure(text=self.T("譯文："))
        self.search_btn.configure(text=self.T("搜尋"))
        self.fuzzy_search_chk.configure(text=self.T("模糊（不分大小寫）"))
        self.replace_btn.configure(text=self.T("取代 (Ctrl+H)"))
        self.global_search_btn.configure(text=self.T("全域搜尋"))

        self.tree.heading("#0", text=self.T("路徑 / Key"))
        self.tree.heading("status", text=self.T("狀態"))
        self.tree_menu.entryconfig(0, label=self.T("全部展開"))
        self.tree_menu.entryconfig(1, label=self.T("全部收起"))
        self.tree_menu.entryconfig(3, label=self.T("複製變數"))
        self.tree_menu.entryconfig(5, label=self.T("譯文=原文（儲存並審閱）"))
        self.tree_menu.entryconfig(6, label=self.T("標記已審閱"))

        self.source_edit_label.configure(text=self.T("原文："))
        self.translated_edit_label.configure(text=self.T("譯文："))

        self.sync_condition_label.configure(text=self.T("同步條件："))
        self.radio_cat_key_src.configure(text=self.T("分類+key+原文 都相同"))
        self.radio_key_src.configure(text=self.T("key+原文 都相同（忽略分類）"))
        self.radio_src_only.configure(text=self.T("原文相同（忽略分類+key）"))
        self.overwrite_check.configure(text=self.T("覆蓋其他已翻譯的相同項目（預設只填未翻譯的）"))
        self.chk_strip_zw.configure(text=self.T("儲存時去除零寬字元"))
        self.zw_strip_btn.configure(text=self.T("移除零寬字元 (F8)"))
        self._update_zw_warning()
        self.chk_show_ws.configure(text=self.T("顯示空白/控制字元"))
        for _tag, label, chip in getattr(self, "_ws_legend_chips", ()):
            chip.configure(text=self.T(label))

        self.copy_source_btn.configure(text=self.T("譯文=原文 (F3)"))
        self.save_btn.configure(text=self.T("儲存 (Ctrl+S)"))
        self.review_btn.configure(text=self.T("標記已審閱 (F1)"))
        self.clear_btn.configure(text=self.T("清空 (F4)"))
        self.undo_btn.configure(text=self.T("回上次修改 (Ctrl+Z)"))

        self._reload_db_combo()
        self._reload_tree()
        self._update_scene_stats()
        self._update_match_label()
        self.status_bar.configure(text=self.T("就緒"))

    def _auto_fit_screen(self):
        """開啟時預設用「視窗化」狀態（不是原生最大化），大小訂為 Windows 工作區
        （扣掉工作列）——這同時也是使用者手動按「還原」時會回到的大小，所以還原
        後一樣不會蓋到工作列；要全螢幕的話使用者自己按最大化即可（那是 OS 原生
        zoomed，本來就會自動避開工作列，不受這裡影響）。
        呼叫時機在 _build_ui() 之後：量出視窗實際可用高度，如果元件自然高度還是
        比可用高度高，就縮小 Treeview 顯示列數、原文/譯文欄顯示行數，確保底下的
        按鈕列/狀態列一定看得到。"""
        try:
            SPI_GETWORKAREA = 0x0030
            rect = ctypes.wintypes.RECT()
            ok = ctypes.windll.user32.SystemParametersInfoW(
                SPI_GETWORKAREA, 0, ctypes.byref(rect), 0
            )
            if not ok:
                raise OSError("SystemParametersInfoW 失敗")
            x, y = rect.left, rect.top
            w, h = rect.right - rect.left, rect.bottom - rect.top
        except Exception:
            x, y = 0, 0
            w = self.winfo_screenwidth()
            h = self.winfo_screenheight()
        self.geometry(f"{w}x{h}+{x}+{y}")
        self.update()

        # self.geometry() 設定的 w/h 只是「client 區塊」大小，視窗的標題列/邊框
        # 還會往外多佔一圈，導致實際整個視窗（含標題列/邊框）比工作區還大、
        # 邊緣被工作列或螢幕邊界蓋住。這裡量出標題列/邊框實際佔用的寬高，
        # 扣掉後重新設定一次。
        try:
            GA_ROOT = 2
            hwnd = ctypes.windll.user32.GetAncestor(self.winfo_id(), GA_ROOT)
            outer_rect = ctypes.wintypes.RECT()
            ctypes.windll.user32.GetWindowRect(hwnd, ctypes.byref(outer_rect))
            chrome_w = (outer_rect.right - outer_rect.left) - self.winfo_width()
            chrome_h = (outer_rect.bottom - outer_rect.top) - self.winfo_height()
            if chrome_w > 0 or chrome_h > 0:
                w -= max(chrome_w, 0)
                h -= max(chrome_h, 0)
                self.geometry(f"{w}x{h}+{x}+{y}")
                self.update()
        except Exception:
            pass

        avail_h = self.winfo_height()
        tree_rows = int(self.tree.cget("height"))
        text_lines = int(self.source_text.cget("height"))
        while self.winfo_reqheight() > avail_h and (tree_rows > 6 or text_lines > 3):
            if text_lines > 3:
                text_lines -= 1
                self.source_text.configure(height=text_lines)
                self.translated_text.configure(height=text_lines)
            if tree_rows > 6:
                tree_rows -= 1
                self.tree.configure(height=tree_rows)
            self.update()

    # ---------- UI 建構 ----------
    def _build_ui(self):
        game_bar = ttk.Frame(self)
        game_bar.pack(side="top", fill="x", padx=6, pady=(4, 0))
        self.theme_btn = ttk.Button(game_bar, text="", command=self._on_toggle_theme)
        self.theme_btn.pack(side="right", padx=4)
        self.lang_var = tk.StringVar(value=LANGUAGES[self.lang_code])
        self.lang_combo = ttk.Combobox(
            game_bar,
            textvariable=self.lang_var,
            state="readonly",
            width=10,
            values=list(LANGUAGES.values()),
        )
        self.lang_combo.pack(side="right", padx=4)
        self.lang_combo.bind("<<ComboboxSelected>>", self._on_language_selected)
        self.game_label = ttk.Label(game_bar, text=self.T("選擇遊戲："))
        self.game_label.pack(side="left")
        self._game_label_to_key = {info["label"]: key for key, info in dbmod.GAMES.items()}
        self.game_var = tk.StringVar(value=dbmod.GAMES[self.current_game]["label"])
        self.game_combo = ttk.Combobox(
            game_bar,
            textvariable=self.game_var,
            state="readonly",
            width=28,
            values=list(self._game_label_to_key.keys()),
        )
        self.game_combo.pack(side="left", padx=4)
        self.game_combo.bind("<<ComboboxSelected>>", self._on_game_selected)

        self.db_label = ttk.Label(game_bar, text=self.T("資料庫："))
        self.db_label.pack(side="left", padx=(12, 0))
        self._db_label_to_info = {}
        self.db_var = tk.StringVar()
        self.db_combo = ttk.Combobox(
            game_bar, textvariable=self.db_var, state="readonly", width=24
        )
        self.db_combo.pack(side="left", padx=4)
        self.db_combo.bind("<<ComboboxSelected>>", self._on_db_selected)

        toolbar = ttk.Frame(self)
        toolbar.pack(side="top", fill="x", padx=6, pady=4)

        self.scene_label = ttk.Label(toolbar, text=self.T("Scene："))
        self.scene_label.pack(side="left")
        self.scene_var = tk.StringVar()
        self.scene_combo = ttk.Combobox(
            toolbar, textvariable=self.scene_var, state="readonly", width=28
        )
        self.scene_combo.pack(side="left", padx=4)
        self.scene_combo.bind("<<ComboboxSelected>>", self._on_scene_selected)

        self.import_btn = ttk.Button(toolbar, text=self.T("新增 LOC..."), command=self._on_import)
        self.import_btn.pack(side="left", padx=4)
        self.import_translated_btn = ttk.Button(
            toolbar, text=self.T("匯入已翻譯LOC..."), command=self._on_import_translated
        )
        self.import_translated_btn.pack(side="left", padx=4)
        self.export_loc_btn = ttk.Button(
            toolbar, text=self.T("匯出 LOC..."), command=self._on_export
        )
        self.export_loc_btn.pack(side="left", padx=4)
        self.export_txt_btn = ttk.Button(
            toolbar, text=self.T("匯出 TXT..."), command=self._on_export_txt
        )
        self.export_txt_btn.pack(side="left", padx=4)

        self.only_untranslated = tk.BooleanVar(value=False)
        self.chk_only_untranslated = ttk.Checkbutton(
            toolbar,
            text=self.T("只顯示未翻譯"),
            variable=self.only_untranslated,
            command=self._reload_tree,
        )
        self.chk_only_untranslated.pack(side="left", padx=12)

        self.only_unreviewed = tk.BooleanVar(value=False)
        self.chk_only_unreviewed = ttk.Checkbutton(
            toolbar,
            text=self.T("只顯示未審閱"),
            variable=self.only_unreviewed,
            command=self._reload_tree,
        )
        self.chk_only_unreviewed.pack(side="left", padx=(0, 12))

        self.always_expand_vars = tk.BooleanVar(value=False)
        self.chk_always_expand = ttk.Checkbutton(
            toolbar,
            text=self.T("每次都展開(變數部分)"),
            variable=self.always_expand_vars,
            command=self._reload_tree,
        )
        self.chk_always_expand.pack(side="left", padx=(0, 12))

        search_bar = ttk.Frame(self)
        search_bar.pack(side="top", fill="x", padx=6, pady=(0, 4))

        self.category_label = ttk.Label(search_bar, text=self.T("分類："))
        self.category_label.pack(side="left")
        self.search_category_var = tk.StringVar()
        search_category_entry = ttk.Entry(
            search_bar, textvariable=self.search_category_var, width=34
        )
        search_category_entry.pack(side="left", padx=(4, 0))
        search_category_entry.bind("<Return>", lambda e: self._reload_tree())
        self.clear_search_category_btn = tk.Button(
            search_bar, text="×", fg="red", relief="flat", padx=2,
            command=self._on_clear_search_category,
        )
        self.clear_search_category_btn.pack(side="left", padx=(0, 4))

        self.key_label = ttk.Label(search_bar, text=self.T("變數："))
        self.key_label.pack(side="left", padx=(8, 0))
        self.search_key_var = tk.StringVar()
        search_key_entry = ttk.Entry(search_bar, textvariable=self.search_key_var, width=16)
        search_key_entry.pack(side="left", padx=(4, 0))
        search_key_entry.bind("<Return>", lambda e: self._reload_tree())
        self.clear_search_key_btn = tk.Button(
            search_bar, text="×", fg="red", relief="flat", padx=2,
            command=self._on_clear_search_key,
        )
        self.clear_search_key_btn.pack(side="left", padx=(0, 4))

        self.source_search_label = ttk.Label(search_bar, text=self.T("原文："))
        self.source_search_label.pack(side="left", padx=(8, 0))
        self.search_var = tk.StringVar()
        search_entry = ttk.Entry(search_bar, textvariable=self.search_var, width=24)
        search_entry.pack(side="left", padx=(4, 0))
        search_entry.bind("<Return>", lambda e: self._reload_tree())
        self.clear_search_source_btn = tk.Button(
            search_bar, text="×", fg="red", relief="flat", padx=2,
            command=self._on_clear_search_source,
        )
        self.clear_search_source_btn.pack(side="left", padx=(0, 4))

        self.translated_search_label = ttk.Label(search_bar, text=self.T("譯文："))
        self.translated_search_label.pack(side="left", padx=(8, 0))
        self.search_translated_var = tk.StringVar()
        search_translated_entry = ttk.Entry(
            search_bar, textvariable=self.search_translated_var, width=24
        )
        search_translated_entry.pack(side="left", padx=(4, 0))
        search_translated_entry.bind("<Return>", lambda e: self._reload_tree())
        self.clear_search_translated_btn = tk.Button(
            search_bar, text="×", fg="red", relief="flat", padx=2,
            command=self._on_clear_search_translated,
        )
        self.clear_search_translated_btn.pack(side="left", padx=(0, 4))

        search_btn_bar = ttk.Frame(self)
        search_btn_bar.pack(side="top", fill="x", padx=6, pady=(0, 4))

        self.search_btn = ttk.Button(search_btn_bar, text=self.T("搜尋"), command=self._reload_tree)
        self.search_btn.pack(side="left")
        self.fuzzy_search_var = tk.BooleanVar(value=self._fuzzy_search)
        self.fuzzy_search_chk = ttk.Checkbutton(
            search_btn_bar, text=self.T("模糊（不分大小寫）"),
            variable=self.fuzzy_search_var, command=self._on_toggle_fuzzy_search,
        )
        self.fuzzy_search_chk.pack(side="left", padx=(8, 0))
        self.replace_btn = ttk.Button(
            search_btn_bar, text=self.T("取代 (Ctrl+H)"), command=self._on_open_find_replace
        )
        self.replace_btn.pack(side="left", padx=(4, 0))
        self.global_search_btn = ttk.Button(
            search_btn_bar, text=self.T("全域搜尋"), command=self._on_open_global_search
        )
        self.global_search_btn.pack(side="left", padx=(4, 0))
        self._global_search_dialog = None

        stats_frame = ttk.Frame(self)
        stats_frame.pack(side="top", fill="x", padx=6)
        self.stats_label1 = ttk.Label(stats_frame, text="")
        self.stats_label1.pack(anchor="w")
        self.stats_label2 = ttk.Label(stats_frame, text="")
        self.stats_label2.pack(anchor="w")

        self.status_bar = ttk.Label(self, text=self.T("就緒"), anchor="w")
        self.status_bar.pack(side="bottom", fill="x")

        body = ttk.Panedwindow(self, orient="horizontal")
        body.pack(fill="both", expand=True, padx=6, pady=4)

        # 左：樹狀清單
        tree_frame = ttk.Frame(body)
        self.tree = ttk.Treeview(
            tree_frame, columns=("status",), show="tree headings", height=20
        )
        self.tree.heading("#0", text=self.T("路徑 / Key"))
        self.tree.heading("status", text=self.T("狀態"))
        self.tree.column("status", width=80, anchor="center")
        # tag顏色隨主題變動，實際 tag_configure 交給 _apply_theme() 統一處理。
        self.tree.pack(side="left", fill="both", expand=True)
        vsb = ttk.Scrollbar(tree_frame, orient="vertical", command=self.tree.yview)
        self.tree.configure(yscrollcommand=vsb.set)
        vsb.pack(side="right", fill="y")
        self.tree.bind("<<TreeviewSelect>>", self._on_tree_select)

        self.tree_menu = tk.Menu(self, tearoff=0)
        self.tree_menu.add_command(label=self.T("全部展開"), command=self._on_expand_all)
        self.tree_menu.add_command(label=self.T("全部收起"), command=self._on_collapse_all)
        self.tree_menu.add_separator()
        self.tree_menu.add_command(label=self.T("複製變數"), command=self._on_copy_variable)
        self.tree_menu.add_separator()
        self.tree_menu.add_command(
            label=self.T("譯文=原文（儲存並審閱）"), command=self._on_bulk_copy_source_as_translation
        )
        self.tree_menu.add_command(
            label=self.T("標記已審閱"), command=self._on_bulk_mark_reviewed
        )
        self.tree.bind("<Button-3>", self._on_tree_right_click)
        # Caps Lock開啟時Windows Tk會把字母keysym回報成大寫，<Control-c>只比對小寫c
        # 不會觸發，所以大小寫都要綁（同理下面Ctrl+A的全選、Ctrl+Z的復原）。
        self.tree.bind("<Control-c>", lambda e: self._on_copy_variable())
        self.tree.bind("<Control-C>", lambda e: self._on_copy_variable())

        body.add(tree_frame, weight=3)

        # 右：編輯區
        edit_frame = ttk.Frame(body)
        text_area_frame = ttk.Frame(edit_frame)
        text_area_frame.grid_columnconfigure(0, weight=1)
        text_area_frame.grid_rowconfigure(1, weight=1)
        text_area_frame.grid_rowconfigure(3, weight=1)
        self.source_edit_label = ttk.Label(text_area_frame, text=self.T("原文："))
        self.source_edit_label.grid(row=0, column=0, sticky="w")
        source_frame = ttk.Frame(text_area_frame)
        source_frame.grid(row=1, column=0, sticky="nsew", pady=(0, 8))
        self.source_linenumbers = tk.Canvas(
            source_frame, width=40, highlightthickness=0, bg="#f0f0f0"
        )
        self.source_linenumbers.pack(side="left", fill="y")
        self.source_text = tk.Text(source_frame, height=8, wrap="word", state="disabled")
        source_vsb = ttk.Scrollbar(source_frame, orient="vertical", command=self.source_text.yview)
        self.source_text.pack(side="left", fill="both", expand=True)
        source_vsb.pack(side="right", fill="y")
        self._redraw_source_linenumbers = self._attach_line_numbers(
            self.source_text, self.source_linenumbers, source_vsb
        )

        self.translated_edit_label = ttk.Label(text_area_frame, text=self.T("譯文："))
        self.translated_edit_label.grid(row=2, column=0, sticky="w")
        translated_frame = ttk.Frame(text_area_frame)
        self.translated_linenumbers = tk.Canvas(
            translated_frame, width=40, highlightthickness=0, bg="#f0f0f0"
        )
        self.translated_linenumbers.pack(side="left", fill="y")
        self.translated_text = tk.Text(translated_frame, height=8, wrap="word", undo=True)
        translated_vsb = ttk.Scrollbar(
            translated_frame, orient="vertical", command=self.translated_text.yview
        )
        self.translated_text.pack(side="left", fill="both", expand=True)
        translated_vsb.pack(side="right", fill="y")
        self._redraw_translated_linenumbers = self._attach_line_numbers(
            self.translated_text, self.translated_linenumbers, translated_vsb
        )

        translated_frame.grid(row=3, column=0, sticky="nsew", pady=(0, 8))

        self._bind_copy_strip_cr(self.source_text)
        self._bind_copy_strip_cr(self.translated_text)
        self._bind_select_all(self.source_text)
        self._bind_select_all(self.translated_text)

        self.match_label = ttk.Label(edit_frame, text="", foreground="#0a6")

        self.match_mode_var = tk.StringVar(value=tm.MATCH_CATEGORY_KEY_SOURCE)
        match_mode_frame = ttk.Frame(edit_frame)
        self.sync_condition_label = ttk.Label(match_mode_frame, text=self.T("同步條件："))
        self.sync_condition_label.pack(anchor="w")
        self.radio_cat_key_src = ttk.Radiobutton(
            match_mode_frame, text=self.T("分類+key+原文 都相同"),
            value=tm.MATCH_CATEGORY_KEY_SOURCE,
            variable=self.match_mode_var, command=self._update_match_label,
        )
        self.radio_cat_key_src.pack(anchor="w", padx=(12, 0))
        self.radio_key_src = ttk.Radiobutton(
            match_mode_frame, text=self.T("key+原文 都相同（忽略分類）"),
            value=tm.MATCH_KEY_SOURCE,
            variable=self.match_mode_var, command=self._update_match_label,
        )
        self.radio_key_src.pack(anchor="w", padx=(12, 0))
        self.radio_src_only = ttk.Radiobutton(
            match_mode_frame, text=self.T("原文相同（忽略分類+key）"),
            value=tm.MATCH_SOURCE_ONLY,
            variable=self.match_mode_var, command=self._update_match_label,
        )
        self.radio_src_only.pack(anchor="w", padx=(12, 0))
        self.overwrite_var = tk.BooleanVar(value=False)
        self.overwrite_check = ttk.Checkbutton(
            edit_frame,
            text=self.T("覆蓋其他已翻譯的相同項目（預設只填未翻譯的）"),
            variable=self.overwrite_var,
        )
        self.strip_zw_var = tk.BooleanVar(value=self._strip_zw_on_save)
        self.chk_strip_zw = ttk.Checkbutton(
            edit_frame,
            text=self.T("儲存時去除零寬字元"),
            variable=self.strip_zw_var,
            command=self._on_toggle_strip_zw,
        )

        # 譯文含零寬/隱形格式字元時才顯示的警告列（一鍵移除）；預設隱藏，
        # 由 _update_zw_warning() 依內容 pack / pack_forget。
        self.zw_bar = ttk.Frame(edit_frame)
        self.zw_warning_label = ttk.Label(self.zw_bar, text="", foreground="#d63031")
        self.zw_warning_label.pack(side="left")
        self.zw_strip_btn = ttk.Button(
            self.zw_bar, text=self.T("移除零寬字元 (F8)"), command=self._on_strip_invisible
        )
        self.zw_strip_btn.pack(side="left", padx=(8, 0))

        # 空白/控制字元視覺化：開關 + 圖例
        self.ws_bar = ttk.Frame(edit_frame)
        self.show_ws_var = tk.BooleanVar(value=self._show_ws)
        self.chk_show_ws = ttk.Checkbutton(
            self.ws_bar, text=self.T("顯示空白/控制字元"),
            variable=self.show_ws_var, command=self._on_toggle_show_ws,
        )
        self.chk_show_ws.pack(side="left")
        self._ws_legend_chips = []
        for tag_name, label in _WS_LEGEND:
            chip = tk.Label(self.ws_bar, text=self.T(label), padx=3, bd=1, relief="solid")
            chip.pack(side="left", padx=(6, 0))
            self._ws_legend_chips.append((tag_name, label, chip))

        btn_row = ttk.Frame(edit_frame)
        self.copy_source_btn = ttk.Button(
            btn_row, text=self.T("譯文=原文 (F3)"), command=self._on_copy_source_as_translation
        )
        self.copy_source_btn.pack(side="left")
        self.save_btn = ttk.Button(btn_row, text=self.T("儲存 (Ctrl+S)"), command=self._on_save)
        self.save_btn.pack(side="left", padx=6)
        self.review_btn = ttk.Button(
            btn_row, text=self.T("標記已審閱 (F1)"), command=self._on_mark_reviewed
        )
        self.review_btn.pack(side="left", padx=6)
        self.clear_btn = ttk.Button(btn_row, text=self.T("清空 (F4)"), command=self._on_clear_translation)
        self.clear_btn.pack(side="left", padx=6)
        self.undo_btn = ttk.Button(
            btn_row, text=self.T("回上次修改 (Ctrl+Z)"), command=self._on_undo
        )
        self.undo_btn.pack(side="left", padx=6)
        self.bind_all("<Control-s>", lambda e: self._on_save())
        self.bind_all("<Control-S>", lambda e: self._on_save())
        self.bind_all("<Control-h>", lambda e: self._on_open_find_replace())
        self.bind_all("<Control-H>", lambda e: self._on_open_find_replace())
        self.bind_all("<F1>", lambda e: self._on_mark_reviewed())
        self.bind_all("<F3>", lambda e: self._on_copy_source_as_translation())
        self.bind_all("<F4>", lambda e: self._on_clear_translation())
        self.bind_all("<F8>", lambda e: self._on_strip_invisible())
        # bind_all掛在"all" bindtag，優先權最低：tk.Text原生對<Tab>/<Shift-Tab>/
        # <Control-Tab>都有自己的class binding（Tab自插入字元、Control-Tab才是
        # focus-next），會在"all"之前就攔截掉，導致焦點在譯文框時bind_all完全
        # 收不到事件。所以真正要生效的元件（譯文框/搜尋欄/樹狀清單）都要直接綁在
        # 元件本身（instance bindtag優先權比class高），bind_all只當作其餘元件
        # （按鈕等）的備援。
        self.bind_all("<Shift-Tab>", self._on_focus_tree_shortcut)
        self.bind_all("<Control-Tab>", self._on_focus_scene_combo_shortcut)
        for widget in (
            self.source_text, self.translated_text, self.tree,
            search_category_entry, search_key_entry, search_entry, search_translated_entry,
        ):
            widget.bind("<Shift-Tab>", self._on_focus_tree_shortcut)
            widget.bind("<Control-Tab>", self._on_focus_scene_combo_shortcut)

        def on_undo_key(event):
            self._on_undo()
            return "break"

        self.translated_text.bind("<Control-z>", on_undo_key)
        self.translated_text.bind("<Control-Z>", on_undo_key)

        # 編輯譯文時即時重畫空白/控制字元標記（debounce，避免每個按鍵都掃全文）
        self.translated_text.bind(
            "<KeyRelease>", lambda e: self._schedule_ws_highlight(), add="+"
        )

        # 由下往上依序 pack：先訂死按鈕列/checkbox/match_label 的高度，
        # 最後才讓兩個文字欄共同吃掉剩下的空間，避免畫面不夠高時
        # 這些固定控制項被 expand 的譯文欄擠到看不見。
        btn_row.pack(side="bottom", fill="x")
        self.chk_strip_zw.pack(side="bottom", anchor="w", pady=(2, 0))
        self.overwrite_check.pack(side="bottom", anchor="w", pady=(2, 6))
        self.ws_bar.pack(side="bottom", anchor="w", pady=(2, 0))
        match_mode_frame.pack(side="bottom", anchor="w", pady=(2, 0))
        self.match_label.pack(side="bottom", anchor="w")
        self.zw_bar.pack(side="bottom", anchor="w", pady=(2, 0))
        self.zw_bar.pack_forget()  # 預設隱藏，有零寬字元時才由 _update_zw_warning() 顯示
        text_area_frame.pack(fill="both", expand=True, pady=(0, 8))

        body.add(edit_frame, weight=2)

    def _bind_copy_strip_cr(self, text_widget):
        """複製到剪貼簿時去掉 \\r（DB 裡的多行原文/譯文用 \\r\\n 換行是為了匯出LOC
        時能100%還原，不能動資料本身，所以只在Ctrl+C/右鍵複製這一步處理）。"""

        def on_copy(event):
            try:
                selected = text_widget.get("sel.first", "sel.last")
            except tk.TclError:
                return "break"
            cleaned = selected.replace("\r\n", "\n").replace("\r", "")
            _set_clipboard_text(cleaned)
            return "break"

        text_widget.bind("<<Copy>>", on_copy)

    def _bind_select_all(self, text_widget):
        """tkinter Text 預設 Ctrl+A 是「游標移到行首」（Emacs式綁定），不是全選，
        改綁成真正的全選（即使 state='disabled' 也能選取/複製）。"""

        def on_select_all(event):
            text_widget.tag_add("sel", "1.0", "end-1c")
            return "break"

        text_widget.bind("<Control-a>", on_select_all)
        text_widget.bind("<Control-A>", on_select_all)

    def _attach_line_numbers(self, text_widget, canvas, scrollbar):
        """在 canvas 上依 text_widget 目前捲動位置畫出每行行號，並回傳 redraw 函式。"""

        def redraw(*_args):
            canvas.delete("all")
            i = text_widget.index("@0,0")
            while True:
                dline = text_widget.dlineinfo(i)
                if dline is None:
                    break
                y = dline[1]
                linenum = str(i).split(".")[0]
                canvas.create_text(
                    34, y, anchor="ne", text=linenum, font=text_widget.cget("font"),
                    fill=self._theme["linenum_fg"],
                )
                i = text_widget.index(f"{i}+1line")

        def on_yscroll(*args):
            scrollbar.set(*args)
            redraw()

        text_widget.configure(yscrollcommand=on_yscroll)
        text_widget.bind("<KeyRelease>", redraw, add="+")
        text_widget.bind("<Configure>", redraw, add="+")
        canvas.bind("<Configure>", redraw, add="+")
        return redraw

    # ---------- 空白/控制字元視覺化 ----------
    def _configure_ws_tags(self):
        """依目前主題設定兩個文字框的 ws_* tag 配色，並更新圖例色塊。"""
        styles = _WS_STYLES.get(self.theme_name, _WS_STYLES["light"])
        for widget in (self.source_text, self.translated_text):
            for name in _WS_TAG_NAMES:
                widget.tag_configure(name, **styles[name])
                # 壓在最底層，才不會蓋掉選取反白 / 搜尋highlight
                widget.tag_lower(name)
        for name, _label, chip in getattr(self, "_ws_legend_chips", ()):
            chip.configure(
                bg=styles[name].get("background", "#888888"),
                fg=self._theme["text_fg"],
            )

    def _highlight_ws(self, widget):
        """掃描 widget 內容，把空格/Tab/CR/LF/NBSP/其他控制字元各自套上 tag。
        非破壞性：只加 tag，不改內容，widget.get() 拿到的字串完全不受影響。"""
        for name in _WS_TAG_NAMES:
            widget.tag_remove(name, "1.0", "end")
        if not self._show_ws:
            return
        text = widget.get("1.0", "end-1c")
        n = len(text)
        if n == 0 or n > 50000:
            return
        line, col = 1, 0
        for i, ch in enumerate(text):
            if ch == "\n":
                prev = text[i - 1] if i else ""
                name = "ws_crlf" if prev == "\r" else "ws_lf_lone"
                widget.tag_add(name, f"{line}.{col}", f"{line}.{col}+1c")
                line += 1
                col = 0
                continue
            name = None
            if ch == "\r":
                nxt = text[i + 1] if i + 1 < n else ""
                name = "ws_crlf" if nxt == "\n" else "ws_cr_lone"
            elif ch == "\t":
                name = "ws_tab"
            elif ch == " ":
                name = "ws_space"
            elif ch == "\xa0":
                name = "ws_nbsp"
            elif ch in _INVISIBLE_CHARS_SET:
                name = "ws_zw"
            elif ord(ch) < 0x20 or ord(ch) == 0x7F:
                name = "ws_ctrl"
            if name:
                widget.tag_add(name, f"{line}.{col}", f"{line}.{col}+1c")
            col += 1

    def _refresh_translated_ws(self):
        """譯文框內容變動後，重畫空白/控制字元標記並更新零寬字元警告列。"""
        self._highlight_ws(self.translated_text)
        self._update_zw_warning()

    def _update_zw_warning(self):
        """譯文含零寬/隱形格式字元時，在編輯區顯示警告與一鍵移除按鈕；否則隱藏。"""
        if not hasattr(self, "zw_bar"):
            return
        text = self.translated_text.get("1.0", "end-1c")
        counts = {}
        for ch in text:
            if ch in _INVISIBLE_CHARS_SET:
                counts[ch] = counts.get(ch, 0) + 1
        if not counts:
            self.zw_bar.pack_forget()
            return
        total = sum(counts.values())
        detail = "、".join(f"{_INVISIBLE_CHARS[ch]}×{cnt}" for ch, cnt in counts.items())
        self.zw_warning_label.configure(
            text=self.T("⚠ 譯文含 {n} 個零寬/隱形字元：{detail}", n=total, detail=detail)
        )
        self.zw_bar.pack(side="bottom", anchor="w", pady=(2, 0), before=self.match_label)

    def _on_strip_invisible(self):
        """把目前譯文框裡的零寬/隱形格式字元全部刪掉（不自動儲存，讓使用者確認後再存）。"""
        if not self._check_writable():
            return
        cleaned, removed = _strip_invisible(self.translated_text.get("1.0", "end-1c"))
        if not removed:
            return
        self.translated_text.delete("1.0", "end")
        self.translated_text.insert("1.0", cleaned)
        self._redraw_translated_linenumbers()
        self._refresh_translated_ws()
        self.status_bar.configure(
            text=self.T("已從譯文移除 {n} 個零寬/隱形字元（尚未儲存）", n=removed)
        )

    def _schedule_ws_highlight(self):
        if self._ws_highlight_job is not None:
            try:
                self.after_cancel(self._ws_highlight_job)
            except Exception:
                pass
        self._ws_highlight_job = self.after(150, self._refresh_translated_ws)

    def _on_toggle_show_ws(self):
        self._show_ws = bool(self.show_ws_var.get())
        _save_ui_settings(show_ws=self._show_ws)
        self._highlight_ws(self.source_text)
        self._refresh_translated_ws()

    def _on_toggle_strip_zw(self):
        self._strip_zw_on_save = bool(self.strip_zw_var.get())
        _save_ui_settings(strip_zw_on_save=self._strip_zw_on_save)

    def _on_toggle_fuzzy_search(self):
        self._fuzzy_search = bool(self.fuzzy_search_var.get())
        _save_ui_settings(fuzzy_search=self._fuzzy_search)
        self._reload_tree()

    # ---------- 主題（淺色/深色） ----------
    def _on_toggle_theme(self):
        self.theme_name = "dark" if self.theme_name == "light" else "light"
        self._apply_theme(self.theme_name)
        _save_theme_pref(self.theme_name)

    def _apply_theme(self, name):
        theme = THEMES[name]
        self._theme = theme
        self.theme_name = name

        self.configure(bg=theme["bg"])

        # ttk.Combobox 下拉清單是獨立的 tk Listbox，不吃 ttk.Style，只能透過
        # option database 設定。
        self.option_add("*TCombobox*Listbox.background", theme["entry_bg"])
        self.option_add("*TCombobox*Listbox.foreground", theme["fg"])
        self.option_add("*TCombobox*Listbox.selectBackground", theme["select_bg"])
        self.option_add("*TCombobox*Listbox.selectForeground", theme["select_fg"])

        style = ttk.Style(self)
        try:
            style.theme_use("clam")  # Windows原生的vista/xpnative主題不吃自訂顏色，改用clam
        except tk.TclError:
            pass
        style.configure(".", background=theme["bg"], foreground=theme["fg"])
        style.configure("TFrame", background=theme["bg"])
        style.configure("TLabel", background=theme["bg"], foreground=theme["fg"])
        style.configure("TButton", background=theme["entry_bg"], foreground=theme["fg"])
        style.map(
            "TButton",
            background=[("active", theme["select_bg"]), ("disabled", theme["bg"])],
            foreground=[("disabled", theme["disabled_fg"])],
        )
        style.configure("TCheckbutton", background=theme["bg"], foreground=theme["fg"])
        style.map("TCheckbutton", background=[("active", theme["bg"])])
        style.configure(
            "TEntry", fieldbackground=theme["entry_bg"], foreground=theme["fg"],
            insertcolor=theme["fg"],
        )
        style.configure(
            "TCombobox", fieldbackground=theme["entry_bg"], foreground=theme["fg"],
            background=theme["entry_bg"], arrowcolor=theme["fg"],
        )
        style.map(
            "TCombobox",
            fieldbackground=[("readonly", theme["entry_bg"])],
            foreground=[("readonly", theme["fg"])],
            selectbackground=[("readonly", theme["entry_bg"])],
            selectforeground=[("readonly", theme["fg"])],
        )
        style.configure("TPanedwindow", background=theme["bg"])
        style.configure(
            "Treeview", background=theme["tree_bg"], foreground=theme["tree_fg"],
            fieldbackground=theme["tree_bg"],
        )
        style.configure(
            "Treeview.Heading", background=theme["tree_heading_bg"], foreground=theme["fg"],
        )
        style.map(
            "Treeview",
            background=[("selected", theme["select_bg"])],
            foreground=[("selected", theme["select_fg"])],
        )
        style.configure(
            "TScrollbar", background=theme["bg"], troughcolor=theme["bg"],
            arrowcolor=theme["fg"],
        )

        # ttk.Style 管不到的純tk元件（Text/Canvas/Menu/Button），逐一手動上色。
        self.source_linenumbers.configure(bg=theme["linenum_bg"])
        self.translated_linenumbers.configure(bg=theme["linenum_bg"])
        self.source_text.configure(
            bg=theme["text_bg"], fg=theme["text_fg"], insertbackground=theme["fg"],
            selectbackground=theme["select_bg"], selectforeground=theme["select_fg"],
        )
        self.translated_text.configure(
            bg=theme["text_bg"], fg=theme["text_fg"], insertbackground=theme["fg"],
            selectbackground=theme["select_bg"], selectforeground=theme["select_fg"],
        )
        self._configure_ws_tags()
        self._highlight_ws(self.source_text)
        self._highlight_ws(self.translated_text)
        self.match_label.configure(foreground=theme["match_fg"])
        self.tree_menu.configure(
            bg=theme["entry_bg"], fg=theme["fg"],
            activebackground=theme["select_bg"], activeforeground=theme["select_fg"],
        )
        for btn in (
            self.clear_search_category_btn,
            self.clear_search_key_btn,
            self.clear_search_source_btn,
            self.clear_search_translated_btn,
        ):
            btn.configure(bg=theme["entry_bg"], activebackground=theme["select_bg"])

        for status, color in theme["status"].items():
            self.tree.tag_configure(status, background=color, foreground=theme["tree_fg"])

        self.theme_btn.configure(text=self.T("☀ 淺色模式") if name == "dark" else self.T("🌙 深色模式"))

        self._redraw_source_linenumbers()
        self._redraw_translated_linenumbers()

    # ---------- 資料載入 ----------
    def _on_game_selected(self, event=None):
        key = self._game_label_to_key.get(self.game_var.get())
        if key is None or key == self.current_game:
            return
        self.current_game = key
        self._reload_db_combo()  # 換遊戲一律重置回該遊戲的主要資料庫
        self._connect_database(dbmod.game_db_path(key), "main")

    def _reload_db_combo(self):
        """依目前選擇的遊戲，重新列出可切換的資料庫（主要/原始db/自訂db），
        並依目前實際連線的db路徑找回對應選項（找不到——例如剛換了遊戲——才
        會退回選「主要資料庫」）。「主要資料庫」/「原始db」兩個保留標籤名稱
        會依目前UI語言顯示，自訂db的名稱是使用者自訂內容，不受語言影響。"""
        entries = dbmod.list_all_databases(self.current_game)
        label_overrides = {"主要資料庫": self.T("主要資料庫"), "原始db": self.T("原始db")}
        self._db_label_to_info = {}
        values = []
        selected_label = None
        for entry in entries:
            label = label_overrides.get(entry["label"], entry["label"])
            self._db_label_to_info[label] = entry
            values.append(label)
            if entry["path"] == self.current_db_path:
                selected_label = label
        self.db_combo["values"] = values
        self.db_var.set(selected_label or label_overrides["主要資料庫"])

    def _on_db_selected(self, event=None):
        info = self._db_label_to_info.get(self.db_var.get())
        if info is None or info["path"] == self.current_db_path:
            return
        self._connect_database(info["path"], info["kind"])

    def _connect_database(self, path: Path, kind: str):
        """切換目前連線的資料庫檔案（同一個遊戲底下的主要/原始/自訂db互切）。
        「原始db」第一次被選到時會自動建立（見 original_db.ensure_original_db）。"""
        if kind == "original":
            try:
                path = original_db.ensure_original_db(self.current_game)
            except Exception as e:
                messagebox.showerror(self.T("無法開啟原始參考資料庫"), str(e))
                return

        self.conn.close()
        self.current_db_path = path
        self.current_db_kind = kind
        self.conn = dbmod.connect(path)
        dbmod.init_db(self.conn)

        self.scene_var.set("")
        self.current_scene_id = None
        self.current_entry_id = None
        self.current_entry_key = None
        self.current_entry_source = None
        self._apply_db_readonly_state()
        self._reload_scenes()
        # 新db沒有任何scene時，_reload_scenes()不會觸發_on_scene_selected()，
        # 這裡要補呼叫_reload_tree()把畫面上殘留的上一個遊戲/資料庫項目清掉，
        # 否則畫面會誤以為還在顯示切換前那份資料庫的內容。
        self._reload_tree()
        self._update_scene_stats()

    def _apply_db_readonly_state(self):
        """「原始db」是自動維護的比對基準，不開放透過GUI手動編輯/新增/匯入，
        避免污染[[匯入已翻譯LOC]]功能賴以驗證的唯一權威來源。"""
        state = "disabled" if self.current_db_kind == "original" else "!disabled"
        for btn in (
            self.import_btn,
            self.import_translated_btn,
            self.save_btn,
            self.review_btn,
            self.copy_source_btn,
            self.clear_btn,
        ):
            btn.state([state])

    def _reload_scenes(self):
        rows = self.conn.execute("SELECT name FROM scenes ORDER BY name").fetchall()
        names = [r["name"] for r in rows]
        self.scene_combo["values"] = names
        if names and not self.scene_var.get():
            self.scene_var.set(names[0])
            self._on_scene_selected()

    def _on_scene_selected(self, event=None):
        name = self.scene_var.get()
        row = self.conn.execute("SELECT id FROM scenes WHERE name=?", (name,)).fetchone()
        self.current_scene_id = row["id"] if row else None
        self._reload_tree()
        self._update_scene_stats()

    def _update_scene_stats(self):
        if self.current_scene_id is None:
            self.stats_label1.configure(text="")
            self.stats_label2.configure(text="")
            return
        rows = self.conn.execute(
            "SELECT status, COUNT(*) c FROM nodes"
            " WHERE scene_id=? AND node_type='entry' AND has_value=1 GROUP BY status",
            (self.current_scene_id,),
        ).fetchall()
        counts = {r["status"]: r["c"] for r in rows}
        total = sum(counts.values())
        reviewed = counts.get("reviewed", 0) + counts.get("reviewed_same", 0)
        translated = counts.get("translated", 0)
        untranslated = counts.get("untranslated", 0)
        game_total = self.conn.execute(
            "SELECT COUNT(*) c FROM nodes WHERE node_type='entry' AND has_value=1"
        ).fetchone()["c"]
        game_reviewed = self.conn.execute(
            "SELECT COUNT(*) c FROM nodes WHERE node_type='entry' AND has_value=1"
            " AND status IN ('reviewed','reviewed_same')"
        ).fetchone()["c"]
        self.stats_label1.configure(
            text=self.T(
                "已審閱 {reviewed} / 當前loc總條目 {total} / 已審閱遊戲總條目 {game_reviewed} / 遊戲總條目 {game_total}",
                reviewed=reviewed, total=total, game_reviewed=game_reviewed, game_total=game_total,
            )
        )
        self.stats_label2.configure(
            text=self.T("未翻譯 {untranslated} / 未審閱 {translated}", untranslated=untranslated, translated=translated)
        )

    def _compute_rollups(self, scene_id):
        """回傳 dict：容器 node_id -> 彙總狀態('untranslated'/'translated'/'reviewed')。
        用一次性掃描整個scene(不受篩選影響)由下往上算，代表真實完成度：
        底下entry全部未翻譯->白；全部已審閱->綠；其他(有進度但未全部審閱完)->橘。
        """
        rows = self.conn.execute(
            "SELECT id, parent_id, node_type, has_value, status FROM nodes WHERE scene_id=?",
            (scene_id,),
        ).fetchall()
        info = {}
        children = {}
        for r in rows:
            info[r["id"]] = r
            children.setdefault(r["parent_id"], []).append(r["id"])

        rollup_status = {}

        def visit(node_id):
            row = info[node_id]
            if row["node_type"] == "entry":
                if row["has_value"]:
                    reviewed = 1 if row["status"] in ("reviewed", "reviewed_same") else 0
                    progressed = 1 if row["status"] in ("translated", "reviewed", "reviewed_same") else 0
                    return 1, reviewed, progressed
                return 0, 0, 0
            total = reviewed = progressed = 0
            for child_id in children.get(node_id, []):
                t, r, p = visit(child_id)
                total += t
                reviewed += r
                progressed += p
            if total == 0 or progressed == 0:
                rollup_status[node_id] = "untranslated"
            elif reviewed == total:
                rollup_status[node_id] = "reviewed"
            else:
                rollup_status[node_id] = "translated"
            return total, reviewed, progressed

        for top_id in children.get(None, []):
            visit(top_id)
        return rollup_status

    def _on_clear_search_category(self):
        self.search_category_var.set("")
        self._reload_tree()

    def _on_clear_search_key(self):
        self.search_key_var.set("")
        self._reload_tree()

    def _on_clear_search_source(self):
        self.search_var.set("")
        self._reload_tree()

    def _on_clear_search_translated(self):
        self.search_translated_var.set("")
        self._reload_tree()

    def _on_open_find_replace(self):
        """Ctrl+H / 搜尋列旁的按鈕：只在目前loc（current_scene_id）範圍內，
        對譯文內容做搜尋取代，取代後的條目一律變回「未審閱」(translated)，
        跟同步套用記憶的「已審閱」邏輯分開，取代結果仍要讓使用者重新審閱。"""
        if not self._check_writable():
            return
        if self.current_scene_id is None:
            messagebox.showinfo(self.T("搜尋取代"), self.T("請先選擇要編輯的loc"))
            return

        dialog = tk.Toplevel(self)
        dialog.title(self.T("搜尋取代（僅限目前loc）"))
        dialog.transient(self)
        dialog.resizable(False, False)

        frame = ttk.Frame(dialog, padding=10)
        frame.pack(fill="both", expand=True)

        ttk.Label(frame, text=self.T("尋找：")).grid(row=0, column=0, sticky="w", pady=(0, 4))
        find_var = tk.StringVar()
        find_entry = ttk.Entry(frame, textvariable=find_var, width=32)
        find_entry.grid(row=0, column=1, pady=(0, 4))

        ttk.Label(frame, text=self.T("取代為：")).grid(row=1, column=0, sticky="w")
        replace_var = tk.StringVar()
        replace_entry = ttk.Entry(frame, textvariable=replace_var, width=32)
        replace_entry.grid(row=1, column=1)

        btn_row = ttk.Frame(frame)
        btn_row.grid(row=2, column=0, columnspan=2, pady=(10, 0), sticky="e")
        ttk.Button(
            btn_row, text=self.T("全部取代"),
            command=lambda: self._execute_find_replace(dialog, find_var.get(), replace_var.get()),
        ).pack(side="left", padx=(0, 6))
        ttk.Button(btn_row, text=self.T("取消"), command=dialog.destroy).pack(side="left")

        find_entry.focus_set()
        dialog.bind("<Return>", lambda e: self._execute_find_replace(dialog, find_var.get(), replace_var.get()))
        dialog.bind("<Escape>", lambda e: dialog.destroy())

    def _execute_find_replace(self, dialog, find_text, replace_text):
        if not find_text:
            messagebox.showinfo(self.T("搜尋取代"), self.T("請輸入要尋找的文字"))
            return

        # 大小寫要區分，且用UTF8位元組比對/取代，避免Unicode case-fold
        # 對某些特殊字元（全形/組合字）產生非預期的比對或取代結果。
        find_bytes = find_text.encode("utf-8")
        replace_bytes = replace_text.encode("utf-8")
        rows = self.conn.execute(
            "SELECT id, translated_value FROM nodes "
            "WHERE scene_id=? AND node_type='entry' AND has_value=1 "
            "AND translated_value IS NOT NULL",
            (self.current_scene_id,),
        ).fetchall()
        matches = []
        for row in rows:
            value_bytes = row["translated_value"].encode("utf-8")
            if find_bytes in value_bytes:
                new_value = value_bytes.replace(find_bytes, replace_bytes).decode("utf-8")
                matches.append((row["id"], new_value))
        if not matches:
            messagebox.showinfo(self.T("搜尋取代"), self.T("目前loc的譯文中找不到「{text}」", text=find_text))
            return

        if not messagebox.askyesno(
            self.T("確認"),
            self.T(
                "確定要在目前loc的譯文中，把 {n} 筆「{find}」取代為「{replace}」嗎？\n取代後的條目會變回「未審閱」狀態。",
                n=len(matches), find=find_text, replace=replace_text,
            ),
        ):
            return

        self.conn.executemany(
            "UPDATE nodes SET translated_value=?, status='translated' WHERE id=?",
            [(new_value, node_id) for node_id, new_value in matches],
        )
        self.conn.commit()

        dialog.destroy()
        self._reload_tree()
        self._update_scene_stats()
        if self.current_entry_id in {node_id for node_id, _ in matches}:
            self._on_tree_select()
        self.status_bar.configure(text=self.T("已取代 {n} 筆條目，取代後已變回未審閱", n=len(matches)))

    def _on_open_global_search(self):
        """搜尋列旁的「全域搜尋」：跨目前資料庫所有loc(scene)搜尋變數/原文/譯文，
        區分大小寫（直接用Python字串in比對，不做lower()）。結果視窗維持開啟，
        不會因為點選結果而消失，方便連續點選多筆結果依序跳轉查看。"""
        if self._global_search_dialog is not None:
            try:
                self._global_search_dialog.deiconify()
                self._global_search_dialog.lift()
                self._global_search_query_entry.focus_set()
                return
            except tk.TclError:
                self._global_search_dialog = None

        dialog = tk.Toplevel(self)
        dialog.title(self.T("全域搜尋（所有loc檔案，區分大小寫）"))
        dialog.transient(self)
        dialog.geometry("760x560")
        dialog.minsize(560, 380)
        self._global_search_dialog = dialog

        def on_close():
            self._global_search_dialog = None
            dialog.destroy()

        dialog.protocol("WM_DELETE_WINDOW", on_close)

        frame = ttk.Frame(dialog, padding=10)
        frame.pack(fill="both", expand=True)

        # 跟主畫面搜尋列一樣分「分類/變數/原文/譯文」四欄，各自獨立比對、
        # 有輸入的欄位一起AND；跟主畫面搜尋列不同的是這裡全部區分大小寫。
        query_frame = ttk.Frame(frame)
        query_frame.pack(fill="x")
        query_frame.grid_columnconfigure(1, weight=1)

        ttk.Label(query_frame, text=self.T("分類：")).grid(row=0, column=0, sticky="w", pady=2)
        category_var = tk.StringVar()
        category_entry = ttk.Entry(query_frame, textvariable=category_var)
        category_entry.grid(row=0, column=1, sticky="ew", pady=2)
        self._global_search_query_entry = category_entry

        ttk.Label(query_frame, text=self.T("變數：")).grid(row=1, column=0, sticky="w", pady=2)
        key_var = tk.StringVar()
        key_entry = ttk.Entry(query_frame, textvariable=key_var)
        key_entry.grid(row=1, column=1, sticky="ew", pady=2)

        ttk.Label(query_frame, text=self.T("原文：")).grid(row=2, column=0, sticky="w", pady=2)
        source_var = tk.StringVar()
        source_entry = ttk.Entry(query_frame, textvariable=source_var)
        source_entry.grid(row=2, column=1, sticky="ew", pady=2)

        ttk.Label(query_frame, text=self.T("譯文：")).grid(row=3, column=0, sticky="w", pady=2)
        translated_var = tk.StringVar()
        translated_entry = ttk.Entry(query_frame, textvariable=translated_var)
        translated_entry.grid(row=3, column=1, sticky="ew", pady=2)

        # 特殊字元篩選：勾選的項目彼此 OR，再跟上面的分類/變數/原文/譯文字串 AND。
        # 用來快速抓出夾帶 Tab / 落單 CR / 落單 LF / NBSP / 零寬格式字元 的條目（原文或譯文任一命中）。
        sc_tab_var = tk.BooleanVar(value=False)
        sc_cr_var = tk.BooleanVar(value=False)
        sc_lf_var = tk.BooleanVar(value=False)
        sc_nbsp_var = tk.BooleanVar(value=False)
        sc_zw_var = tk.BooleanVar(value=False)
        sc_frame = ttk.LabelFrame(query_frame, text=self.T("含特殊字元（原文或譯文）"))
        sc_frame.grid(row=4, column=0, columnspan=2, sticky="ew", pady=(6, 0))
        ttk.Checkbutton(sc_frame, text=self.T("Tab (0x09)"), variable=sc_tab_var,
                        command=lambda: do_search()).pack(side="left", padx=4)
        ttk.Checkbutton(sc_frame, text=self.T("落單CR (0x0D)"), variable=sc_cr_var,
                        command=lambda: do_search()).pack(side="left", padx=4)
        ttk.Checkbutton(sc_frame, text=self.T("落單LF (0x0A)"), variable=sc_lf_var,
                        command=lambda: do_search()).pack(side="left", padx=4)
        ttk.Checkbutton(sc_frame, text=self.T("NBSP (0xA0)"), variable=sc_nbsp_var,
                        command=lambda: do_search()).pack(side="left", padx=4)
        ttk.Checkbutton(sc_frame, text=self.T("零寬/格式字元 (U+200B…)"), variable=sc_zw_var,
                        command=lambda: do_search()).pack(side="left", padx=4)

        btn_row = ttk.Frame(query_frame)
        btn_row.grid(row=5, column=0, columnspan=2, sticky="e", pady=(6, 0))

        # 筆數／狀態列先用 side="bottom" 卡住底部，list_frame 再吃剩下的空間，
        # 這樣結果多寡（"共 N 筆符合"）永遠看得到，不會被撐高的表單擠出視窗外。
        status_label = ttk.Label(frame, text=self.T("尚未搜尋"), anchor="w")
        status_label.pack(side="bottom", fill="x", pady=(4, 0))

        list_frame = ttk.Frame(frame)
        list_frame.pack(side="top", fill="both", expand=True, pady=(8, 0))
        result_tree = ttk.Treeview(
            list_frame, columns=("scene", "category", "key", "source", "translated"),
            show="headings", height=12,
        )
        for col, text, width in (
            ("scene", self.T("檔案(loc)"), 100),
            ("category", self.T("分類"), 120),
            ("key", self.T("變數"), 90),
            ("source", self.T("原文"), 190),
            ("translated", self.T("譯文"), 190),
        ):
            result_tree.heading(col, text=text)
            result_tree.column(col, width=width, anchor="w")
        result_tree.pack(side="left", fill="both", expand=True)
        result_vsb = ttk.Scrollbar(list_frame, orient="vertical", command=result_tree.yview)
        result_tree.configure(yscrollcommand=result_vsb.set)
        result_vsb.pack(side="right", fill="y")

        node_id_by_iid = {}

        def display_text(text, width=60):
            s = (text or "").replace("\r\n", " ").replace("\n", " ").replace("\r", " ")
            return s if len(s) <= width else s[:width] + "…"

        def has_special(text):
            """text 是否含任一勾選的特殊字元。CR/LF 只算「落單」的（先拿掉 \\r\\n
            配對再看還有沒有），避免每個多行條目都命中。"""
            if not text:
                return False
            if sc_tab_var.get() and "\t" in text:
                return True
            if sc_nbsp_var.get() and "\xa0" in text:
                return True
            if sc_zw_var.get() and any(c in _INVISIBLE_CHARS_SET for c in text):
                return True
            if sc_cr_var.get() or sc_lf_var.get():
                stripped = text.replace("\r\n", "")
                if sc_cr_var.get() and "\r" in stripped:
                    return True
                if sc_lf_var.get() and "\n" in stripped:
                    return True
            return False

        def do_search(event=None):
            result_tree.delete(*result_tree.get_children())
            node_id_by_iid.clear()
            q_category = category_var.get()
            q_key = key_var.get()
            q_source = source_var.get()
            q_translated = translated_var.get()
            want_special = (
                sc_tab_var.get() or sc_cr_var.get()
                or sc_lf_var.get() or sc_nbsp_var.get() or sc_zw_var.get()
            )
            if not (q_category or q_key or q_source or q_translated or want_special):
                status_label.configure(text=self.T("請至少輸入一個搜尋條件"))
                return
            rows = self.conn.execute(
                "SELECT nodes.id AS id, nodes.name AS name, nodes.source_value AS source_value,"
                " nodes.translated_value AS translated_value, scenes.name AS scene_name"
                " FROM nodes JOIN scenes ON scenes.id = nodes.scene_id"
                " WHERE nodes.node_type='entry' AND nodes.has_value=1"
            ).fetchall()
            count = 0
            for row in rows:
                name = row["name"] or ""
                source = row["source_value"] or ""
                translated = row["translated_value"] or ""
                if q_key and q_key not in name:
                    continue
                if q_source and q_source not in source:
                    continue
                if q_translated and q_translated not in translated:
                    continue
                if want_special and not (has_special(source) or has_special(translated)):
                    continue
                category = tm.category_path(self.conn, row["id"])
                if q_category and q_category not in category:
                    continue
                iid = f"r{count}"
                count += 1
                result_tree.insert(
                    "", "end", iid=iid,
                    values=(
                        row["scene_name"], category, name,
                        display_text(source), display_text(translated),
                    ),
                )
                node_id_by_iid[iid] = row["id"]
            status_label.configure(
                text=self.T("共 {n} 筆符合", n=count) if count else self.T("找不到符合的內容")
            )

        def on_result_activate(event=None):
            sel = result_tree.selection()
            if not sel:
                return
            node_id = node_id_by_iid.get(sel[0])
            if node_id is not None:
                self._jump_to_node(node_id)

        ttk.Button(btn_row, text=self.T("搜尋"), command=do_search).pack(side="left")
        for entry in (category_entry, key_entry, source_entry, translated_entry):
            entry.bind("<Return>", do_search)
        result_tree.bind("<<TreeviewSelect>>", on_result_activate)
        result_tree.bind("<Double-Button-1>", on_result_activate)
        dialog.bind("<Escape>", lambda e: on_close())

        category_entry.focus_set()

    def _jump_to_node(self, node_id):
        """依node_id切換到所屬的loc(scene)並在樹狀清單中展開/選取該節點。
        會先清掉目前的分類/變數/原文/譯文篩選字串與「只顯示未翻譯/未審閱」勾選，
        避免目標節點被目前主畫面上還留著的篩選條件擋掉、樹狀清單裡根本找不到它。"""
        row = self.conn.execute(
            "SELECT scene_id, parent_id FROM nodes WHERE id=?", (node_id,)
        ).fetchone()
        if row is None:
            return
        scene_row = self.conn.execute(
            "SELECT name FROM scenes WHERE id=?", (row["scene_id"],)
        ).fetchone()
        if scene_row is None:
            return

        self.search_category_var.set("")
        self.search_key_var.set("")
        self.search_var.set("")
        self.search_translated_var.set("")
        self.only_untranslated.set(False)
        self.only_unreviewed.set(False)

        if self.current_scene_id != row["scene_id"]:
            self.scene_var.set(scene_row["name"])
            self._on_scene_selected()  # 內部會呼叫 _reload_tree + _update_scene_stats
        else:
            self._reload_tree()
            self._update_scene_stats()

        ancestors = []
        parent_id = row["parent_id"]
        while parent_id is not None:
            ancestors.append(parent_id)
            prow = self.conn.execute(
                "SELECT parent_id FROM nodes WHERE id=?", (parent_id,)
            ).fetchone()
            parent_id = prow["parent_id"] if prow else None
        for ancestor_id in reversed(ancestors):
            iid = f"n{ancestor_id}"
            if self.tree.exists(iid):
                self.tree.item(iid, open=True)

        iid = f"n{node_id}"
        if self.tree.exists(iid):
            self.tree.selection_set(iid)
            self.tree.see(iid)
            self.tree.focus(iid)
            self._on_tree_select()

    def _reload_tree(self):
        self.tree.delete(*self.tree.get_children())
        if self.current_scene_id is None:
            return
        only_untranslated = self.only_untranslated.get()
        only_unreviewed = self.only_unreviewed.get()
        # 「模糊」勾選時不分大小寫（兩邊都 .lower()）；取消勾選則原樣比對＝區分大小寫。
        fold = (lambda s: s.lower()) if self.fuzzy_search_var.get() else (lambda s: s)
        keyword_category = fold(self.search_category_var.get().strip())
        keyword_key = fold(self.search_key_var.get().strip())
        keyword_source = fold(self.search_var.get().strip())
        keyword_translated = fold(self.search_translated_var.get().strip())
        rollups = self._compute_rollups(self.current_scene_id)

        def insert_children(parent_iid, parent_id, path_names):
            rows = self.conn.execute(
                "SELECT * FROM nodes WHERE scene_id=? AND parent_id IS ? ORDER BY seq",
                (self.current_scene_id, parent_id),
            ).fetchall()
            for row in rows:
                if row["node_type"] == "container":
                    iid = f"n{row['id']}"
                    self.tree.insert(parent_iid, "end", iid=iid, text=row["name"], values=(""))
                    insert_children(iid, row["id"], path_names + [row["name"]])
                    if not self.tree.get_children(iid):
                        self.tree.delete(iid)  # 篩選後底下沒東西的容器不顯示
                    else:
                        self.tree.item(iid, tags=(rollups.get(row["id"], "untranslated"),))
                else:
                    if row["has_value"] == 0:
                        continue  # 裸key沒有可翻譯內容
                    if only_untranslated and row["status"] != "untranslated":
                        continue
                    if only_unreviewed and row["status"] != "translated":
                        continue
                    if keyword_category:
                        # 分類路徑用"|"連接各層容器名稱，搜尋字串也用"|"分層，
                        # 例如 AllLevels|Actions 會比對到 .../AllLevels/Actions/... 底下的項目。
                        path_str = fold("|".join(path_names))
                        if keyword_category not in path_str:
                            continue
                    if keyword_key and keyword_key not in fold(row["name"]):
                        continue
                    if keyword_source and keyword_source not in fold(row["source_value"] or ""):
                        continue
                    if keyword_translated and keyword_translated not in fold(row["translated_value"] or ""):
                        continue
                    iid = f"n{row['id']}"
                    label = self._status_label(row["status"])
                    self.tree.insert(
                        parent_iid, "end", iid=iid, text=row["name"],
                        values=(label,), tags=(row["status"],),
                    )

        insert_children("", None, [])

        if self.always_expand_vars.get():
            self._set_all_open(True)

    # ---------- 事件處理 ----------
    def _on_tree_select(self, event=None):
        sel = self.tree.selection()
        if not sel:
            return
        node_id = int(sel[0][1:])
        row = self.conn.execute("SELECT * FROM nodes WHERE id=?", (node_id,)).fetchone()
        if row is None or row["node_type"] != "entry" or row["has_value"] == 0:
            self.current_entry_id = None
            self.current_entry_key = None
            self.current_entry_source = None
            self.current_entry_category = None
            self.match_label.configure(text="")
            return
        self.current_entry_id = node_id
        self.current_entry_key = row["name"]
        self.current_entry_source = row["source_value"]
        self.current_entry_category = tm.category_path(self.conn, node_id)

        self.source_text.configure(state="normal")
        self.source_text.delete("1.0", "end")
        self.source_text.insert("1.0", row["source_value"] or "")
        self.source_text.configure(state="disabled")
        self._redraw_source_linenumbers()
        self._highlight_ws(self.source_text)

        self.translated_text.delete("1.0", "end")
        self.translated_text.insert("1.0", row["translated_value"] or "")
        self._redraw_translated_linenumbers()
        self._refresh_translated_ws()

        self._update_match_label()

    def _update_match_label(self):
        if self.current_entry_id is None or self.current_entry_source is None:
            self.match_label.configure(text="")
            return
        match_mode = self.match_mode_var.get()
        match_count = tm.count_matches(
            self.conn,
            self.current_entry_key,
            self.current_entry_source,
            exclude_node_id=self.current_entry_id,
            match_mode=match_mode,
            category=self.current_entry_category,
        )
        if match_count:
            basis_key = {
                tm.MATCH_CATEGORY_KEY_SOURCE: "同分類+同key+同原文",
                tm.MATCH_KEY_SOURCE: "同key+同原文",
                tm.MATCH_SOURCE_ONLY: "同原文",
            }[match_mode]
            self.match_label.configure(
                text=self.T(
                    "{basis}在其他 {n} 筆也出現，儲存時會一併套用",
                    basis=self.T(basis_key), n=match_count,
                )
            )
        else:
            self.match_label.configure(text="")

    def _check_writable(self) -> bool:
        """「原始db」唯讀，Ctrl+S快捷鍵/右鍵選單不受按鈕disabled狀態限制，
        所以會寫入資料的入口都要各自檢查一次，不能只靠按鈕視覺上disabled。"""
        if self.current_db_kind == "original":
            messagebox.showerror(self.T("無法編輯"), self.T("目前選擇的是原始參考資料庫，不開放手動編輯。"))
            return False
        return True

    def _on_save(self):
        if not self._check_writable():
            return None
        if self.current_entry_id is None:
            return None
        text = self.translated_text.get("1.0", "end-1c")
        text = text.replace("\r\n", "\n").replace("\n", "\r\n")  # 換行一律正規化成CRLF，配合LOC檔案格式
        removed_invisible = 0
        if self.strip_zw_var.get():
            text, removed_invisible = _strip_invisible(text)
        if removed_invisible:
            # 從外部貼進來的譯文常夾帶零寬字元（U+200B 等），編輯框看不出來，
            # 存檔時一併清掉並同步更新譯文框，避免下次載入又「冒出來」。
            cursor = self.translated_text.index("insert")
            self.translated_text.delete("1.0", "end")
            self.translated_text.insert("1.0", text)
            try:
                self.translated_text.mark_set("insert", cursor)
            except tk.TclError:
                pass
            self._redraw_translated_linenumbers()
            self._refresh_translated_ws()
        status = "translated" if text.strip() else "untranslated"
        self.conn.execute(
            "UPDATE nodes SET translated_value=?, status=? WHERE id=?",
            (text if text else None, status, self.current_entry_id),
        )

        propagated_ids = []
        if text.strip():
            propagated_ids = tm.propagate_translation(
                self.conn,
                self.current_entry_key,
                self.current_entry_source,
                text,
                status,
                exclude_node_id=self.current_entry_id,
                overwrite=self.overwrite_var.get(),
                match_mode=self.match_mode_var.get(),
                category=self.current_entry_category,
            )
        self.conn.commit()

        self._refresh_row_status(status)
        self._refresh_ancestor_rollups(self.current_entry_id)
        for node_id in propagated_ids:
            iid = f"n{node_id}"
            if self.tree.exists(iid):
                self.tree.set(iid, "status", self._status_label(status))
                self.tree.item(iid, tags=(status,))
            self._refresh_ancestor_rollups(node_id)

        msg = self.T("已儲存")
        if removed_invisible:
            msg += self.T("（已去除 {n} 個零寬字元）", n=removed_invisible)
        if propagated_ids:
            msg += self.T("，並同步套用到其他 {n} 筆相同項目", n=len(propagated_ids))
        self.status_bar.configure(text=msg)
        self.match_label.configure(text="")
        self._update_scene_stats()
        return text

    def _on_mark_reviewed(self, status="reviewed"):
        if not self._check_writable():
            return
        if self.current_entry_id is None:
            return
        text = self._on_save()
        self.conn.execute(
            "UPDATE nodes SET status=? WHERE id=?", (status, self.current_entry_id)
        )

        propagated_ids = tm.propagate_review(
            self.conn,
            self.current_entry_key,
            self.current_entry_source,
            text,
            exclude_node_id=self.current_entry_id,
            overwrite=self.overwrite_var.get(),
            status=status,
            match_mode=self.match_mode_var.get(),
                category=self.current_entry_category,
        )
        self.conn.commit()

        self._refresh_row_status(status)
        self._refresh_ancestor_rollups(self.current_entry_id)
        for node_id in propagated_ids:
            iid = f"n{node_id}"
            if self.tree.exists(iid):
                self.tree.set(iid, "status", self._status_label(status))
                self.tree.item(iid, tags=(status,))
            self._refresh_ancestor_rollups(node_id)

        msg = self.T("已標記為已審閱")
        if propagated_ids:
            msg += self.T("，並同步套用到其他 {n} 筆相同項目", n=len(propagated_ids))
        self.status_bar.configure(text=msg)
        self._update_scene_stats()

    def _on_copy_source_as_translation(self):
        if self.current_entry_id is None:
            return
        source = self.source_text.get("1.0", "end-1c")
        self.translated_text.delete("1.0", "end")
        self.translated_text.insert("1.0", source)
        self._redraw_translated_linenumbers()
        self._refresh_translated_ws()
        self._on_mark_reviewed(status="reviewed_same")

    def _on_clear_translation(self):
        if not self._check_writable():
            return
        if self.current_entry_id is None:
            return
        self.translated_text.delete("1.0", "end")
        self._redraw_translated_linenumbers()
        self._refresh_translated_ws()
        self._on_save()

    def _on_focus_tree_shortcut(self, event=None):
        """Shift+Tab：強制把焦點切回變數欄的樹狀清單，讓使用者能直接用上下鍵
        選擇變數（沒有選取項目時，先選第一個，避免上下鍵沒反應）。"""
        self.tree.focus_set()
        sel = self.tree.selection()
        if not sel:
            children = self.tree.get_children()
            if children:
                first = children[0]
                self.tree.selection_set(first)
                self.tree.focus(first)
        return "break"

    def _on_focus_scene_combo_shortcut(self, event=None):
        """Ctrl+Tab：把焦點切到loc下拉選單，focus後上下鍵可直接切換loc。"""
        self.scene_combo.focus_set()
        return "break"

    def _on_undo(self):
        try:
            self.translated_text.edit_undo()
        except tk.TclError:
            pass
        self._redraw_translated_linenumbers()
        self._refresh_translated_ws()

    def _on_tree_right_click(self, event):
        # 右鍵點在哪個節點上，展開/收起就只作用在那個節點的子樹；
        # 點在空白處（沒有節點）則退回作用在整棵樹。
        self._context_target = self.tree.identify_row(event.y) or None
        if self._context_target and self._context_target not in self.tree.selection():
            self.tree.selection_set(self._context_target)
        if self._context_target:
            self.tree_menu.entryconfig(0, label=self.T("展開此項"))
            self.tree_menu.entryconfig(1, label=self.T("收起此項"))
        else:
            self.tree_menu.entryconfig(0, label=self.T("全部展開"))
            self.tree_menu.entryconfig(1, label=self.T("全部收起"))
        try:
            self.tree_menu.tk_popup(event.x_root, event.y_root)
        finally:
            self.tree_menu.grab_release()

    def _set_all_open(self, open_state: bool, root_iid=None):
        def recurse(iid):
            self.tree.item(iid, open=open_state)
            for child in self.tree.get_children(iid):
                recurse(child)

        if root_iid:
            recurse(root_iid)
        else:
            for top in self.tree.get_children(""):
                recurse(top)

    def _on_expand_all(self):
        self._set_all_open(True, root_iid=self._context_target)

    def _on_collapse_all(self):
        self._set_all_open(False, root_iid=self._context_target)

    def _collect_selected_entry_ids(self):
        """把目前樹狀清單選取的節點展開成實際要套用的 entry id 清單。
        選到容器時，連同底下（不受搜尋篩選影響的）所有子 entry 一併納入。"""
        result = []
        seen = set()
        for iid in self.tree.selection():
            node_id = int(iid[1:])
            row = self.conn.execute(
                "SELECT node_type, has_value FROM nodes WHERE id=?", (node_id,)
            ).fetchone()
            if row is None:
                continue
            if row["node_type"] == "container":
                sub_rows = self.conn.execute(
                    """
                    WITH RECURSIVE sub(id) AS (
                        SELECT id FROM nodes WHERE id = ?
                        UNION ALL
                        SELECT n.id FROM nodes n JOIN sub s ON n.parent_id = s.id
                    )
                    SELECT id FROM nodes
                    WHERE id IN (SELECT id FROM sub) AND node_type='entry' AND has_value=1
                    """,
                    (node_id,),
                ).fetchall()
                for r in sub_rows:
                    if r["id"] not in seen:
                        seen.add(r["id"])
                        result.append(r["id"])
            elif row["has_value"]:
                if node_id not in seen:
                    seen.add(node_id)
                    result.append(node_id)
        return result

    def _on_copy_variable(self):
        """把目前樹狀清單選取節點自己的變數（key）複製到剪貼簿，一行一個，
        跟右鍵選單/Ctrl+C共用。選到容器時複製的是容器自己的名稱，不展開成
        底下entry的key（要展開的情境用 _collect_selected_entry_ids()）。"""
        sel = self.tree.selection()
        if not sel:
            return
        node_ids = [int(iid[1:]) for iid in sel]
        placeholders = ",".join("?" * len(node_ids))
        rows = self.conn.execute(
            f"SELECT id, name FROM nodes WHERE id IN ({placeholders})", node_ids
        ).fetchall()
        name_by_id = {r["id"]: r["name"] for r in rows}
        keys = [name_by_id[nid] for nid in node_ids if nid in name_by_id]
        if not keys:
            return
        self.clipboard_clear()
        self.clipboard_append("\n".join(keys))
        self.status_bar.configure(text=self.T("已複製 {n} 個變數", n=len(keys)))

    def _on_bulk_copy_source_as_translation(self):
        if not self._check_writable():
            return
        node_ids = self._collect_selected_entry_ids()
        if not node_ids:
            messagebox.showinfo(self.T("譯文=原文"), self.T("請先選擇要套用的條目"))
            return
        count = len(node_ids)
        if not messagebox.askyesno(
            self.T("確認"), self.T("你確定要將{n}項條目應用譯文=原文，儲存並審閱嗎？", n=count)
        ):
            return

        match_mode = self.match_mode_var.get()
        overwrite = self.overwrite_var.get()
        updated_ids = set()
        for node_id in node_ids:
            row = self.conn.execute(
                "SELECT name, source_value FROM nodes WHERE id=?", (node_id,)
            ).fetchone()
            source_value = row["source_value"] or ""
            self.conn.execute(
                "UPDATE nodes SET translated_value=?, status='reviewed_same' WHERE id=?",
                (source_value, node_id),
            )
            updated_ids.add(node_id)

            # 跟單條目按鈕（_on_copy_source_as_translation -> _on_mark_reviewed）一樣，
            # 依「同步條件」radio選項把結果套用到其他相同項目，不能只處理選取到的條目。
            propagated_ids = tm.propagate_review(
                self.conn,
                row["name"],
                source_value,
                source_value,
                exclude_node_id=node_id,
                overwrite=overwrite,
                status="reviewed_same",
                match_mode=match_mode,
                category=tm.category_path(self.conn, node_id),
            )
            updated_ids.update(propagated_ids)
        self.conn.commit()

        for node_id in updated_ids:
            iid = f"n{node_id}"
            if self.tree.exists(iid):
                self.tree.set(iid, "status", self._status_label("reviewed_same"))
                self.tree.item(iid, tags=("reviewed_same",))
            self._refresh_ancestor_rollups(node_id)

        if self.current_entry_id in updated_ids:
            self._on_tree_select()

        msg = self.T("已將 {n} 項條目套用譯文=原文並標記已審閱", n=count)
        extra = len(updated_ids) - count
        if extra > 0:
            msg += self.T("，並同步套用到其他 {n} 筆相同項目", n=extra)
        self.status_bar.configure(text=msg)
        self._update_scene_stats()

    def _on_bulk_mark_reviewed(self):
        if not self._check_writable():
            return
        node_ids = self._collect_selected_entry_ids()
        if not node_ids:
            messagebox.showinfo(self.T("標記已審閱"), self.T("請先選擇要標記的條目"))
            return
        count = len(node_ids)
        if not messagebox.askyesno(
            self.T("確認"), self.T("你確定要將{n}項條目標記為已審閱嗎？", n=count)
        ):
            return

        match_mode = self.match_mode_var.get()
        overwrite = self.overwrite_var.get()
        updated_ids = set()
        for node_id in node_ids:
            row = self.conn.execute(
                "SELECT name, source_value, translated_value FROM nodes WHERE id=?", (node_id,)
            ).fetchone()
            translated_value = row["translated_value"] or ""
            self.conn.execute(
                "UPDATE nodes SET status='reviewed' WHERE id=?", (node_id,)
            )
            updated_ids.add(node_id)

            propagated_ids = tm.propagate_review(
                self.conn,
                row["name"],
                row["source_value"] or "",
                translated_value,
                exclude_node_id=node_id,
                overwrite=overwrite,
                status="reviewed",
                match_mode=match_mode,
                category=tm.category_path(self.conn, node_id),
            )
            updated_ids.update(propagated_ids)
        self.conn.commit()

        for node_id in updated_ids:
            iid = f"n{node_id}"
            if self.tree.exists(iid):
                self.tree.set(iid, "status", self._status_label("reviewed"))
                self.tree.item(iid, tags=("reviewed",))
            self._refresh_ancestor_rollups(node_id)

        if self.current_entry_id in updated_ids:
            self._on_tree_select()

        msg = self.T("已將 {n} 項條目標記為已審閱", n=count)
        extra = len(updated_ids) - count
        if extra > 0:
            msg += self.T("，並同步套用到其他 {n} 筆相同項目", n=extra)
        self.status_bar.configure(text=msg)
        self._update_scene_stats()

    def _container_rollup_status(self, container_id) -> str:
        row = self.conn.execute(
            """
            WITH RECURSIVE sub(id) AS (
                SELECT id FROM nodes WHERE id = ?
                UNION ALL
                SELECT n.id FROM nodes n JOIN sub s ON n.parent_id = s.id
            )
            SELECT
                COUNT(*) AS total,
                SUM(CASE WHEN status IN ('reviewed','reviewed_same') THEN 1 ELSE 0 END) AS reviewed,
                SUM(CASE WHEN status IN ('translated','reviewed','reviewed_same') THEN 1 ELSE 0 END) AS progressed
            FROM nodes
            WHERE id IN (SELECT id FROM sub) AND node_type='entry' AND has_value=1
            """,
            (container_id,),
        ).fetchone()
        total, reviewed, progressed = row["total"], row["reviewed"] or 0, row["progressed"] or 0
        if total == 0 or progressed == 0:
            return "untranslated"
        if reviewed == total:
            return "reviewed"
        return "translated"

    def _refresh_ancestor_rollups(self, node_id):
        """存檔後往上更新目前可見樹狀清單裡，這個entry所有祖先容器的彙總顏色。"""
        row = self.conn.execute("SELECT parent_id FROM nodes WHERE id=?", (node_id,)).fetchone()
        parent_id = row["parent_id"] if row else None
        while parent_id is not None:
            iid = f"n{parent_id}"
            if self.tree.exists(iid):
                self.tree.item(iid, tags=(self._container_rollup_status(parent_id),))
            row = self.conn.execute("SELECT parent_id FROM nodes WHERE id=?", (parent_id,)).fetchone()
            parent_id = row["parent_id"] if row else None

    def _refresh_row_status(self, status):
        iid = f"n{self.current_entry_id}"
        if self.tree.exists(iid):
            self.tree.set(iid, "status", self._status_label(status))
            self.tree.item(iid, tags=(status,))

    def _on_import(self):
        if not self._check_writable():
            return
        path = filedialog.askopenfilename(
            title=self.T("選擇 .LOC / .txt 檔案"),
            filetypes=[
                ("LOC/TXT files", "*.LOC;*.txt"),
                ("LOC files", "*.LOC"),
                ("TXT files", "*.txt"),
                ("All files", "*.*"),
            ],
        )
        if not path:
            return
        try:
            scene_id = import_loc.import_scene(self.conn, Path(path), game_key=self.current_game)
        except Exception as e:
            messagebox.showerror(self.T("匯入失敗"), str(e))
            return
        self._reload_scenes()
        row = self.conn.execute("SELECT name FROM scenes WHERE id=?", (scene_id,)).fetchone()
        self.scene_var.set(row["name"])
        self._on_scene_selected()
        self.status_bar.configure(text=self.T("已匯入 {name}", name=Path(path).name))

    def _on_import_translated(self):
        """匯入「已翻譯好的LOC/TXT」——跟_on_import（新增LOC，灌入全新未翻譯骨架）不
        同，這是把已經含有中文譯文的檔案合併回資料庫。完整流程見
        import_translated_loc.py docstring：先選目前/新資料庫，再選覆蓋範圍（無論
        目前/新資料庫都會問），既有資料庫還要先驗證這份LOC/TXT確實屬於這個遊戲。"""
        path = filedialog.askopenfilename(
            title=self.T("選擇已翻譯好的 .LOC / .txt 檔案"),
            filetypes=[
                ("LOC/TXT files", "*.LOC;*.txt"),
                ("LOC files", "*.LOC"),
                ("TXT files", "*.txt"),
                ("All files", "*.*"),
            ],
        )
        if not path:
            return
        loc_path = Path(path)

        choice = self._ask_target_database_choice()
        if choice is None:
            return

        try:
            scene_name, translations = itl.validate_and_diff(loc_path, self.current_game)
        except itl.LocValidationError as e:
            messagebox.showerror(self.T("匯入失敗"), str(e))
            return
        except Exception as e:
            messagebox.showerror(self.T("匯入失敗"), self.T("解析LOC/TXT檔案時發生錯誤：{err}", err=e))
            return

        if not translations:
            messagebox.showinfo(
                self.T("匯入已翻譯LOC"), self.T("這份檔案跟遊戲原文相比，沒有偵測到任何翻譯內容。")
            )
            return

        if choice:  # 建立新的資料庫
            new_name = self._ask_new_db_name()
            if new_name is None:
                return
            scope_choice = self._ask_apply_scope(scene_name, len(translations), show_scope=False)
            if scope_choice is None:
                return
            _, overwrite_mode = scope_choice
            try:
                db_path, breakdown = itl.create_database_from_translations(
                    self.current_game, new_name, scene_name, translations, overwrite_mode
                )
            except Exception as e:
                messagebox.showerror(self.T("建立資料庫失敗"), str(e))
                return
            self._reload_db_combo()
            self.db_var.set(new_name)
            self._connect_database(db_path, "custom")
            total = sum(breakdown.values())
            self.status_bar.configure(
                text=self.T("已建立新資料庫「{name}」，並匯入 {n} 筆翻譯", name=new_name, n=total)
            )
            messagebox.showinfo(self.T("匯入完成"), self._format_breakdown_message(breakdown))
            return

        # 使用目前資料庫
        if self.current_db_kind == "original":
            messagebox.showerror(self.T("無法匯入"), self.T("目前選擇的是原始參考資料庫，不開放手動匯入。"))
            return

        scope_choice = self._ask_apply_scope(scene_name, len(translations), show_scope=True)
        if scope_choice is None:
            return
        scope, overwrite_mode = scope_choice
        if scope == "all":
            breakdown = itl.apply_cross_scene(self.conn, translations, overwrite_mode)
        else:
            breakdown = itl.apply_same_scene(self.conn, scene_name, translations, overwrite_mode)
        self.conn.commit()
        self._reload_tree()
        self._update_scene_stats()
        total = sum(breakdown.values())
        self.status_bar.configure(
            text=self.T(
                "已套用 {applied} 筆翻譯（掃描到 {scanned} 筆差異）", applied=total, scanned=len(translations)
            )
        )
        messagebox.showinfo(self.T("匯入完成"), self._format_breakdown_message(breakdown))

    def _format_breakdown_message(self, breakdown: dict) -> str:
        """把 apply_cross_scene/apply_same_scene/create_database_from_translations
        回傳的 {status: 筆數} 統計，組成使用者看得懂的摘要：未翻譯/未審閱/已審閱
        三類固定列出（含0筆，方便確認「已審閱」真的沒被動到）；reviewed 跟
        reviewed_same 在 _STATUS_KEYS 裡本來就共用「已審閱」這個顯示名稱，這裡合併
        成一行避免同一個標籤出現兩次造成混淆。"""
        untranslated = breakdown.get("untranslated", 0)
        unreviewed = breakdown.get("translated", 0)
        reviewed = breakdown.get("reviewed", 0) + breakdown.get("reviewed_same", 0)
        total = untranslated + unreviewed + reviewed
        return self.T(
            "本次共覆蓋 {total} 筆：\n　未翻譯：{untranslated} 筆\n　未審閱：{unreviewed} 筆\n　已審閱：{reviewed} 筆",
            total=total, untranslated=untranslated, unreviewed=unreviewed, reviewed=reviewed,
        )

    def _ask_target_database_choice(self):
        """回傳 True=建立新的資料庫 / False=使用目前資料庫 / None=放棄匯入。"""
        return messagebox.askyesnocancel(
            self.T("匯入已翻譯LOC"),
            self.T(
                "要匯入到目前的資料庫，還是建立一份新的資料庫？\n\n"
                "「是」= 建立新的資料庫\n「否」= 使用目前的資料庫\n「取消」= 放棄匯入"
            ),
        )

    def _ask_new_db_name(self):
        """回傳合法的新資料庫名稱，取消輸入則回傳 None。"""
        while True:
            name = simpledialog.askstring(
                self.T("建立新資料庫"), self.T("請輸入新資料庫名稱（須與現有資料庫不同）："), parent=self
            )
            if name is None:
                return None
            name = name.strip()
            if not name:
                messagebox.showerror(self.T("名稱無效"), self.T("名稱不能是空白"))
                continue
            if dbmod.custom_db_name_exists(self.current_game, name):
                messagebox.showerror(self.T("名稱重複"), self.T("資料庫名稱「{name}」已經存在，請換一個名稱", name=name))
                continue
            return name

    def _ask_apply_scope(self, scene_name: str, count: int, show_scope: bool = True):
        """回傳 (scope, overwrite_mode)。scope 為 "all"（依key套用到全部scene）或
        "scene"（只套用到這份LOC/TXT對應的scene）；show_scope=False（建立新資料庫
        時，只會有這一個scene，沒有scope可選）時scope固定回傳None。overwrite_mode
        是 import_translated_loc.OVERWRITE_ALL/OVERWRITE_SKIP_REVIEWED/
        OVERWRITE_UNTRANSLATED_ONLY 三選一。使用者放棄/關閉視窗則整體回傳 None。"""
        dialog = tk.Toplevel(self)
        dialog.title(self.T("套用已翻譯內容"))
        dialog.transient(self)
        dialog.grab_set()
        dialog.resizable(False, False)

        result = {"value": None}

        ttk.Label(
            dialog,
            text=self.T(
                "掃描到 {count} 筆跟遊戲原文不同的翻譯內容（scene：{scene}）。\n請選擇要如何套用：",
                count=count, scene=scene_name,
            ),
            justify="left",
        ).pack(anchor="w", padx=12, pady=(12, 8))

        scope_var = tk.StringVar(value="all")
        if show_scope:
            ttk.Radiobutton(
                dialog,
                text=self.T("套用匯入的內容至當前符合的全部loc（依相同變數key，套用到目前資料庫全部Scene）"),
                variable=scope_var, value="all",
            ).pack(anchor="w", padx=20)
            ttk.Radiobutton(
                dialog, text=self.T("只對符合的loc應用匯入內容（只套用到這份LOC對應的Scene：{scene}）", scene=scene_name),
                variable=scope_var, value="scene",
            ).pack(anchor="w", padx=20)
            ttk.Radiobutton(
                dialog, text=self.T("放棄匯入"), variable=scope_var, value="abandon",
            ).pack(anchor="w", padx=20, pady=(0, 8))

        ttk.Label(
            dialog,
            text=self.T("覆蓋範圍（以原始資料庫的樹狀結構為準比對差異）："),
            justify="left",
        ).pack(anchor="w", padx=12, pady=(4, 2))

        overwrite_var = tk.StringVar(value=itl.OVERWRITE_SKIP_REVIEWED)
        ttk.Radiobutton(
            dialog, text=self.T("全部匯入（不考慮已審閱或已儲存，全部取代）"),
            variable=overwrite_var, value=itl.OVERWRITE_ALL,
        ).pack(anchor="w", padx=20)
        ttk.Radiobutton(
            dialog, text=self.T("覆蓋未翻譯和未審閱（不覆蓋已審閱）"),
            variable=overwrite_var, value=itl.OVERWRITE_SKIP_REVIEWED,
        ).pack(anchor="w", padx=20)
        ttk.Radiobutton(
            dialog, text=self.T("只覆蓋未翻譯"),
            variable=overwrite_var, value=itl.OVERWRITE_UNTRANSLATED_ONLY,
        ).pack(anchor="w", padx=20, pady=(0, 12))

        btn_row = ttk.Frame(dialog)
        btn_row.pack(fill="x", padx=12, pady=(0, 12))

        def on_confirm():
            if show_scope and scope_var.get() == "abandon":
                result["value"] = None
            else:
                scope = scope_var.get() if show_scope else None
                result["value"] = (scope, overwrite_var.get())
            dialog.destroy()

        def on_cancel():
            result["value"] = None
            dialog.destroy()

        ttk.Button(btn_row, text=self.T("確定"), command=on_confirm).pack(side="right")
        ttk.Button(btn_row, text=self.T("取消"), command=on_cancel).pack(side="right", padx=(0, 6))
        dialog.protocol("WM_DELETE_WINDOW", on_cancel)

        dialog.update_idletasks()
        x = self.winfo_rootx() + (self.winfo_width() - dialog.winfo_width()) // 2
        y = self.winfo_rooty() + (self.winfo_height() - dialog.winfo_height()) // 2
        dialog.geometry(f"+{x}+{y}")

        self.wait_window(dialog)
        return result["value"]

    def _on_export(self):
        if messagebox.askyesno(self.T("批量匯出"), self.T("你要匯出該遊戲全部的檔案嗎？")):
            self._export_all_scenes("loc")
            return
        if self.current_scene_id is None:
            messagebox.showwarning(self.T("尚未選擇 Scene"), self.T("請先選擇要匯出的 Scene"))
            return
        name = self.scene_var.get()
        path = filedialog.asksaveasfilename(
            title=self.T("匯出 .LOC 檔案"),
            defaultextension=".LOC",
            initialfile=f"{name}.LOC",
            filetypes=[("LOC files", "*.LOC")],
        )
        if not path:
            return
        try:
            export_loc.export_scene(self.conn, name, Path(path), game_key=self.current_game)
        except Exception as e:
            messagebox.showerror(self.T("匯出失敗"), str(e))
            return
        self.status_bar.configure(text=self.T("已匯出至 {path}", path=path))

    def _on_export_txt(self):
        """把資料庫譯文打包成 TXT（不呼叫 HitmanBe.exe、不產生 .LOC），
        格式與 HitmanKi.exe 解包出來的格式一致，見 export_loc.export_scene_txt。"""
        if messagebox.askyesno(self.T("批量匯出"), self.T("你要匯出該遊戲全部的檔案嗎？")):
            self._export_all_scenes("txt")
            return
        if self.current_scene_id is None:
            messagebox.showwarning(self.T("尚未選擇 Scene"), self.T("請先選擇要匯出的 Scene"))
            return
        name = self.scene_var.get()
        path = filedialog.asksaveasfilename(
            title=self.T("匯出 TXT 檔案"),
            defaultextension=".txt",
            initialfile=f"{name}.txt",
            filetypes=[("TXT files", "*.txt")],
        )
        if not path:
            return
        try:
            export_loc.export_scene_txt(self.conn, name, Path(path))
        except Exception as e:
            messagebox.showerror(self.T("匯出失敗"), str(e))
            return
        self.status_bar.configure(text=self.T("已匯出至 {path}", path=path))

    def _export_all_scenes(self, kind: str):
        """批量匯出目前連線資料庫底下的全部 scene（kind="loc" 或 "txt"）到
        使用者選擇的資料夾，每個 scene 各自輸出成一個檔案（檔名互不重複，
        不需要用子資料夾分類，見 batch_import.py 的匯入邏輯）。"""
        names = [row["name"] for row in self.conn.execute(
            "SELECT name FROM scenes ORDER BY name"
        ).fetchall()]
        if not names:
            messagebox.showwarning(self.T("沒有可匯出的檔案"), self.T("目前資料庫沒有任何 scene"))
            return
        out_dir = filedialog.askdirectory(title=self.T("選擇批量匯出的資料夾"))
        if not out_dir:
            return
        out_dir = Path(out_dir)
        ext = ".LOC" if kind == "loc" else ".txt"
        ok, failed = [], []
        for name in names:
            out_path = out_dir / f"{name}{ext}"
            try:
                if kind == "loc":
                    export_loc.export_scene(self.conn, name, out_path, game_key=self.current_game)
                else:
                    export_loc.export_scene_txt(self.conn, name, out_path)
                ok.append(name)
            except Exception as e:
                failed.append((name, str(e)))
        summary = self.T("批量匯出完成：{ok}/{total} 個檔案成功。", ok=len(ok), total=len(names))
        self.status_bar.configure(text=summary)
        if failed:
            detail = "\n".join(f"- {name}: {err}" for name, err in failed)
            messagebox.showerror(
                self.T("部分檔案匯出失敗"), self.T("{summary}\n\n失敗清單：\n{detail}", summary=summary, detail=detail)
            )
        else:
            messagebox.showinfo(
                self.T("批量匯出完成"), self.T("{summary}\n已輸出至 {out_dir}", summary=summary, out_dir=out_dir)
            )


if __name__ == "__main__":
    App().mainloop()
