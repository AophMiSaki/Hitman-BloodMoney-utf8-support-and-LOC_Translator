r"""
bink32hook_iniEdit.py — Hitman Blood Money 中文化 binkw32.dll hook 的設定工具

部署時跟編譯好的 bink32hook_iniEdit.exe 一起放進遊戲目錄下的 "bink32hook\"
子資料夾（跟 bink32hook.ini / bink32hook.log 同一層，由 binkw32.dll 執行期
自己建立）。

讀寫用 ctypes 直接呼叫 Get/WritePrivateProfileStringW，跟C++端(Config.cpp)
用的 GetPrivateProfileStringA 是同一套WinAPI ini函式家族、同樣依賴系統目前的
ANSI codepage做編碼轉換，兩邊不會有UTF-8/ANSI編碼落差問題。

外層四分頁 Notebook：「字型設定」／「選項開關」／「位置設定」／「DEBUG開關」，
底部單一「儲存」按鈕一次寫入全部分頁的設定。字型分3類
[FontCjk]/[FontSubtitle]/[FontNewspaper]（欄位相同，報紙FontSize是固定
lineHeight值、不是px、不隨解析度縮放，font2~4由font1自動推算，見
build_slot_tab()）；語系字串抽到lang/<code>.json，預設zh-Hant，檔案不存在
時用內建DEFAULT_STRINGS產生一份。

異動歷史見 md\bink32hook_iniEdit工具.md，不寫在這裡。
"""

import ctypes
import ctypes.wintypes
import json
import sys
from pathlib import Path
import tkinter as tk
import tkinter.font as tkfont
from tkinter import ttk, messagebox

# 錯誤修正：打包成單一exe（PyInstaller onefile）執行時__file__指向TEMP底下的
# 解壓暫存目錄，不是exe實際位置，導致ini/lang/minimaptest.png全部被找到TEMP去；
# frozen狀態改用sys.executable（永遠是exe真實路徑）取代__file__。
if getattr(sys, "frozen", False):
    _BASE_DIR = Path(sys.executable).resolve().parent
else:
    _BASE_DIR = Path(__file__).resolve().parent

INI_PATH = _BASE_DIR / "bink32hook.ini"

# ── 0. 語系（UI 字串抽到 lang/<code>.json，目前只做 zh-Hant）────────────────

LANG_DIR = _BASE_DIR / "lang"
DEFAULT_LANG = "zh-Hant"

# 內建預設語系（繁體中文）。lang/<code>.json 不存在時用這份產生一份；存在時
# 讀 json、缺的 key 用這份補齊。之後要加別的語言，複製 lang/zh-Hant.json 改
# 檔名跟內容即可，程式不用動（把 DEFAULT_LANG 改成新代碼，或日後做語言選單）。
DEFAULT_STRINGS = {
    "_meta.language_name": "繁體中文",
    "_meta.language_code": "zh-Hant",

    "app.title": "bink32hook 設定工具",
    "app.ini_path": "ini 路徑：{path}",
    "app.lang_label": "介面語言：",
    "tab.font": "字型設定",
    "tab.options": "選項開關",
    "tab.layout": "位置設定",
    "tab.debug": "DEBUG 開關",
    "btn.save": "儲存",
    "btn.apply_font": "套用選取",
    "btn.detect_key": "偵測按鍵",
    "btn.detect_key.active": "按下要設定的按鍵…",

    "msg.save_done.title": "完成",
    "msg.save_done.body": "設定已儲存，重啟遊戲後生效。",
    "msg.error.title": "錯誤",
    "msg.info.title": "提示",
    "msg.pick_font": "請在左側列表點選字體後自動套用。",

    "err.font_numeric": "[{section}] 字級／字寬／字距／Y偏移必須是數字",
    "err.ini_write": "寫入 ini 失敗：{path} [{section}]",
    "err.ini_write_key": "寫入 ini 失敗：{path} [{section}] {key}",
    "err.hotkey_numeric": "[LocSwitch] 熱鍵 VK code 必須是數字",
    "err.npc_radius_numeric": "[Subtitle] NpcHearRadius 必須是數字",
    "err.tvradio_radius_numeric": "[Subtitle] TvRadioHearRadius 必須是數字",
    "err.subtitle_coord": "[Subtitle] {key} 座標必須是數字",
    "err.hud_coord": "[Hud] {key} 座標必須是數字",
    "err.debug_numeric": "[Debug] {key} 必須是數字",
    "err.newspaper_interval": "[NewsPaper] SoftBreakCjkInterval 必須是數字",
    "err.minimap_size_numeric": "[Hud] MinimapSize 必須是 0~100 的整數",
    "err.minimap_zoom_numeric": "[Hud] MinimapZoom 必須是數字",

    "font.list_title": "系統字型清單",
    "font.slot.FontCjk": "通用",
    "font.slot.FontSubtitle": "字幕",
    "font.slot.FontNewspaper": "報紙",
    "font.field.face": "字型：",
    "font.field.size": "字級(px)：",
    "font.field.size.newspaper": "字級(固定，非 px)：",
    "font.field.weight": "粗細：",
    "font.weight.0": "不指定 (0)",
    "font.weight.300": "細 (300)",
    "font.weight.400": "正常 (400)",
    "font.weight.500": "中等 (500)",
    "font.weight.700": "粗體 (700)",
    "font.field.width": "字寬微調(px，0=自動)：",
    "font.field.spacing": "字距微調(px)：",
    "font.field.yoffset": "Y偏移微調(px)：",
    "font.preview": "預覽",
    "font.preview.cantload": "(字型無法載入：{face})",
    "font.newspaper.note": (
        "報紙字級是固定值：不是 px、也不會隨遊戲解析度縮放，畫面放大縮小都維持"
        "同一個大小。這個數字約當 native 的 lineHeight（供分類門檻比對，非精確"
        "換算）；font2~4 會自動以 font1 為基準遞減（−14／−28／−30，下限 22／10／8），"
        "face／粗細／字寬／字距／Y偏移 font2~4 一律沿用 font1、不能個別調整。"
        "實測 38～42 之間 CJK 不會疊字。"
    ),

    "preview.sample.FontCjk": "血錢中文化預覽\n刺客47文字測試……AaBbCc123",
    "preview.sample.FontSubtitle": "任務簡報字幕預覽\n「目標已確認，準備行動。」",
    "preview.sample.FontNewspaper": "號外：城市頭條新聞\n刺客案偵辦露出曙光",

    "opt.general.frame": "[General] 功能總開關",
    "opt.locswitch.frame": "[LocSwitch] 原文／譯文熱鍵切換",
    "opt.subtitle.frame": "[Subtitle] 字幕補完",
    "opt.subtitle.place.frame": "[Subtitle] 字幕位置",
    "opt.newspaper.frame": "[NewsPaper] 報紙排版修正",
    "opt.hud.frame": "[Hud] 小地圖／擅闖／警告 HUD",

    "general.NormalFontReplace": "通用／報紙自訂 CJK 字型",
    "general.NormalFontReplace.desc": (
        "關閉時 native GetGlyph 遇到 CJK 一律維持缺字，通用選單／簡報 Objective／"
        "報紙全部顯示空白，不套用 [FontCjk]／[FontNewspaper]；連帶不安裝材質頁登記、"
        "場景切換材質刷新、Inventory／商店標籤寬度修正。字幕走另一條 self-render，"
        "不受影響。"
    ),
    "general.Utf8ReencodeFix": "UTF-8 雙重編碼修復",
    "general.Utf8ReencodeFix.desc": (
        "修復共用函式 sub_5A4850 把已是 UTF-8 的譯文逐 byte 誤當 ANSI 重新編碼"
        "造成的亂碼。一般保持開啟。"
    ),
    "general.GlyphHook": "CJK 字圖合成／字幕總電源",
    "general.GlyphHook.desc": (
        "GetGlyph 呼叫點的 inline patch，是 CJK 字圖合成與字幕 self-render 的總"
        "開關。關閉時畫面 CJK 回到 native 缺字空白，且字幕功能一併失效。"
    ),
    "general.ToUpperHook": "toupper() CJK 亂碼修復",
    "general.ToUpperHook.desc": (
        "修復 native 對 CJK 碼位呼叫 CRT toupper() 造成的 byte 損壞（按鈕／HUD／"
        "鍵位選單文字亂碼）。一般保持開啟。"
    ),
    "general.ZipPathTrace": "場景 zip 路徑側錄（必要）",
    "general.ZipPathTrace.desc": (
        "側錄最近一次場景 zip 路徑，是原文／譯文 LOC 路徑推導、簡報字幕場景偵測、"
        "報紙分類判斷共用的必要資料來源。關掉會讓中文譯文／原文、簡報字幕、報紙"
        "分類全部失效，正常不要關。逐次 trace log 另由 [Debug] DebugZipPathTrace "
        "控制。"
    ),

    "locswitch.enable": "啟用原文／譯文切換",
    "locswitch.hotkey": "熱鍵 VK code：",
    "locswitch.hotkey.hint": "(預設 122＝VK_F11)",
    "locswitch.hotkey.hint.capturing": "(按 Esc 取消)",
    "locswitch.hotkey.desc": (
        "遊戲中按這個鍵在畫面上切換顯示 LOC 原文／中文譯文（按下瞬間觸發，長按"
        "不會連續切換）。"
    ),

    "subtitle.BriefingSubtitleEnabled": "簡報開場旁白字幕",
    "subtitle.BriefingSubtitleEnabled.desc": (
        "補顯任務簡報開場旁白字幕（native 這條播放路徑不顯示字幕）。同時控制語音"
        "開始訊號側錄與字幕文字顯示。"
    ),
    "subtitle.DialogueSubtitleEnabled": "NPC 對話字幕（Dialogue）",
    "subtitle.DialogueSubtitleEnabled.desc": (
        "補顯雙人對話（sub_6AACA0）字幕。native 這條補顯路徑從設計上就沒有字幕"
        "步驟。"
    ),
    "subtitle.OnelinersSubtitleEnabled": "背景喊話字幕（Oneliners）",
    "subtitle.OnelinersSubtitleEnabled.desc": (
        "補顯單人事件反應喊話（警報／發現屍體／閒聊，sub_6A4240）與 M04 詐死彩蛋"
        "台詞（sub_6BACC0）字幕。"
    ),
    "subtitle.NpcHearRadius": "NPC 距離門檻：",
    "subtitle.NpcHearRadius.desc": (
        "NPC 對話／喊話與玩家角色距離超過此值就不顯示字幕（避免顯示聽不到的背景"
        "對話）。單位未確認、需實機調整；0＝停用過濾（全地圖都顯示）。預設 1000。"
    ),
    "subtitle.WalkieSubtitleEnabled": "對講機字幕（Walkie）",
    "subtitle.WalkieSubtitleEnabled.desc": (
        "補顯關卡內NPC對講機傳輸（sub_6C0220）的字幕。"
    ),
    "subtitle.TvRadioSubtitleEnabled": "電視／收音機字幕（TvRadio）",
    "subtitle.TvRadioSubtitleEnabled.desc": (
        "補顯關卡內電視新聞／收音機廣播（sub_4C5E10）的字幕。"
    ),
    "subtitle.TvRadioHearRadius": "電視／收音機距離門檻：",
    "subtitle.TvRadioHearRadius.desc": (
        "廣播音效發聲點與玩家角色距離超過此值就不顯示字幕（廣播關卡載入即全區"
        "循環播放，音量通常穿多個房間，門檻比 NPC 對話大）。單位同 NpcHearRadius；"
        "0＝停用過濾（全地圖都顯示）。預設 3000。"
    ),

    "subtitle.place.SubtitlePlaceFirst": "首選字幕位置",
    "subtitle.place.SubtitlePlaceFirst.desc": (
        "首選字幕欄位的螢幕位置（Dialogue／Oneliners 首選，以及開場簡報字幕共用）。"
        "X／Y 為螢幕百分比，0＝最左／最上，100＝最右／最下，超出自動 clamp。"
        "預設 50,80。"
    ),
    "subtitle.place.SubtitlePlaceThird": "第三字幕位置",
    "subtitle.place.SubtitlePlaceThird.desc": (
        "第三字幕顯示點的螢幕位置（8 態狀態機在首選＋次選都被佔用時使用）。次選"
        "沿用 native 原本字幕位置、沒有對應設定。預設 60,90。"
    ),
    "subtitle.place.x": "X%",
    "subtitle.place.y": "Y%",

    "newspaper.NewsPaperImageWrapFix": "圖片文繞修正",
    "newspaper.NewsPaperImageWrapFix.desc": (
        "修復報紙 <img> 文繞圖：native 算圖片旁縮窄寬度／下緣時讀到未初始化堆疊值，"
        "導致 CJK 版圖片位置往上跑。只在報紙分類生效。"
    ),
    "newspaper.NewsPaperSpaceAdvanceFix": "全篇疊字修正（空白前進量）",
    "newspaper.NewsPaperSpaceAdvanceFix.desc": (
        "修復報紙全篇疊字：空白字元改成當場重查 GetGlyph(32)，不沿用函式進入時的"
        "過期指標。只在報紙分類生效。"
    ),
    "newspaper.NewsPaperLineHeightFix": "標題疊字修正（行高）",
    "newspaper.NewsPaperLineHeightFix.desc": (
        "修復報紙標題疊字：行高 running-max 改用我方探測到的正確 lineHeight，不沿用"
        "可能被污染的欄位。只在報紙分類生效。"
    ),
    "newspaper.NewsPaperJustifyFillFix": "兩端對齊撐飛修正",
    "newspaper.NewsPaperJustifyFillFix.desc": (
        "報紙 <justify fill> 兩端對齊在 CJK 下會把行尾文字撐飛（甚至蓋過圖片），"
        "開啟時報紙分類改當 <justify left>。非報紙分類不受影響。"
    ),
    "newspaper.NewsPaperSoftBreakFix": "CJK 軟斷行（插入空白）",
    "newspaper.NewsPaperSoftBreakFix.desc": (
        "在報紙 CJK 譯文裡每隔數個字插入一個空白當換行斷點候選，把換行判斷密度拉回"
        "接近英文版，避免擠字／超版。只套用報紙 CJK 譯文，不動 F11 原文。"
    ),
    "newspaper.SoftBreakCjkInterval": "軟斷行間隔：",
    "newspaper.SoftBreakCjkInterval.desc": (
        "上一個開關的插入間隔：每隔幾個連續 CJK 字元插一個空白。預設 6，0＝不插入。"
    ),

    "hud.TrespassingEnabled": "擅闖／敵對區域自繪字",
    "hud.TrespassingEnabled.desc": "顯示玩家進入擅闖／敵對區域時的自繪提示文字。",
    "hud.TrespassPos": "擅闖文字位置",
    "hud.TrespassPos.desc": "螢幕百分比座標，0＝最左／最上，100＝最右／最下。預設 16,85。",
    "hud.WarningEnabled": "AI 警覺／戰鬥提示字",
    "hud.WarningEnabled.desc": "顯示 AI 懷疑（SUSPICIOUS）／警戒（ALERTED）狀態提示文字。",
    "hud.WarningPos": "警覺提示文字位置",
    "hud.WarningPos.desc": "螢幕百分比座標，畫面上方置中。預設 50,5。",
    "hud.MinimapEnabled": "小地圖",
    "hud.MinimapEnabled.desc": "顯示小地圖佔位方框。",
    "hud.MinimapPos": "小地圖位置",
    "hud.MinimapPos.desc": "小地圖左上角螢幕百分比座標。預設 5.0,60.0。",
    "hud.MinimapSize": "小地圖邊長倍數：",
    "hud.MinimapSize.desc": (
        "實際像素邊長＝此數值×10，範圍 0~100（0＝不繪製）。只接受純十進位整數，"
        "格式錯誤會在遊戲內退回預設值 15。預設 15。"
    ),
    "hud.MinimapZoom": "小地圖縮放：",
    "hud.MinimapZoom.desc": (
        "小地圖方框內橫向可見的world單位數，數值越小越放大、以玩家為中心跟隨"
        "捲動；0＝整層等比縮放塞滿方框（看全貌、不跟隨）。負值遊戲內會退回0。"
        "預設 4000。"
    ),

    "layout.preview.frame": "畫面預覽",
    "layout.preview.warning": "懷疑",
    "layout.preview.trespass": "擅闖",
    "layout.preview.minimap": "小地圖",
    "layout.preview.subtitle_first": "字幕(首選)",
    "layout.preview.subtitle_third": "字幕(第三)",
    "layout.preview.zoom_label": "縮放 {value}",

    "debug.header": (
        "[Debug] 診斷開關 — 除了「CJK 缺字佔位符號」以外，其餘"
        "只影響 log 輸出量，不影響顯示行為。"
    ),
    "debug.cap_label": "上限次數：",
    "debug.GlyphDiagEnable": "字型合成診斷 log",
    "debug.GlyphDiagEnable.desc": "每次 CJK 字圖合成印一行細節（含上限次數，達上限後靜音）。",
    "debug.CJKPlaceholderEnable": "CJK 缺字佔位符號（'@'）",
    "debug.CJKPlaceholderEnable.desc": (
        "把 CJK 碼位在呼叫 native GetGlyph 前換成 '@'。★唯一會改變顯示行為的診斷"
        "開關，一般保持關閉。"
    ),
    "debug.TexUploadDiagEnable": "材質上傳診斷 log",
    "debug.TexUploadDiagEnable.desc": "GDI 點陣圖上傳成 D3D9 材質的細節 log。",
    "debug.LocSwitchDiagEnable": "原文／譯文查表診斷 log",
    "debug.LocSwitchDiagEnable.desc": "每筆 LOC 查表命中／miss 都印一行，量很大。",
    "debug.ToUpperDiagEnable": "toupper() codepoint 診斷 log",
    "debug.ToUpperDiagEnable.desc": "記錄被 hook 的 toupper() 收到的 codepoint。",
    "debug.NativeTexRegDiagEnable": "材質頁登記診斷 log",
    "debug.NativeTexRegDiagEnable.desc": "native 材質頁槽登記／失效的細節 log。",
    "debug.DebugSubtitleDiagEnable": "字幕細節診斷 log",
    "debug.DebugSubtitleDiagEnable.desc": (
        "簡報／Dialogue／Oneliners 共用的字幕細節 log（含 NPC 距離校準）。安裝"
        "成功／失敗 log 不受影響。"
    ),
    "debug.LabelWidthFixDiagEnable": "標籤寬度修正診斷 log",
    "debug.LabelWidthFixDiagEnable.desc": (
        "Inventory 武器面板／商店 Price 的 label:value 寬度修正細節 log。"
    ),
    "debug.DebugZipPathTrace": "場景 zip 路徑 trace log",
    "debug.DebugZipPathTrace.desc": (
        "場景 zip 路徑側錄的逐次 trace log（是否安裝側錄由 [General] ZipPathTrace "
        "控制，不是這個）。"
    ),
    "debug.HudDiagEnable": "擅闖／警戒 HUD 診斷 log",
    "debug.HudDiagEnable.desc": "TrespassHud 每次判定命中時印zoneKind→級別的細節 log。",
    "debug.MinimapFloorProbe": "小地圖選樓層側錄",
    "debug.MinimapFloorProbe.desc": (
        "每秒 dump 一次玩家座標／目前選到的樓層／zone 節點名等，供離線比對小地圖"
        "選樓層邏輯用，純側錄不改繪製。走完對照後應關閉。"
    ),
    "debug.GlyphOverrunProbe": "報紙字圖溢位側錄",
    "debug.GlyphOverrunProbe.desc": (
        "報紙 CJK 字圖合成路徑的 stack/atlas 溢位定位側錄，只在報紙畫面印，純側錄。"
    ),
}


def load_strings(lang: str = DEFAULT_LANG) -> dict:
    """讀 lang/<lang>.json；不存在就用 DEFAULT_STRINGS 產生一份。json 解析失敗
    或缺 key 時一律 fallback 到 DEFAULT_STRINGS，不讓工具打不開。"""
    merged = dict(DEFAULT_STRINGS)
    try:
        LANG_DIR.mkdir(exist_ok=True)
    except OSError:
        return merged

    path = LANG_DIR / f"{lang}.json"
    if not path.exists():
        try:
            path.write_text(
                json.dumps(DEFAULT_STRINGS, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
        except OSError:
            return merged
        return merged

    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return merged

    if isinstance(data, dict):
        merged.update({k: v for k, v in data.items() if isinstance(v, str)})
        # json 比內建版本少 key（工具更新後）時，補寫一份完整的回去方便翻譯者
        if set(DEFAULT_STRINGS) - set(data):
            try:
                path.write_text(
                    json.dumps(merged, ensure_ascii=False, indent=2) + "\n",
                    encoding="utf-8",
                )
            except OSError:
                pass
    return merged


CURRENT_LANG_PATH = LANG_DIR / "current.txt"


def load_current_lang() -> str:
    """讀上次選過的語言代碼（存在 lang/current.txt）；沒有就用 DEFAULT_LANG。"""
    try:
        code = CURRENT_LANG_PATH.read_text(encoding="utf-8").strip()
    except OSError:
        return DEFAULT_LANG
    return code or DEFAULT_LANG


def save_current_lang(code: str) -> None:
    try:
        LANG_DIR.mkdir(exist_ok=True)
        CURRENT_LANG_PATH.write_text(code, encoding="utf-8")
    except OSError:
        pass


def available_languages() -> list:
    """掃 lang/ 目錄下所有 <code>.json，回傳 [(code, language_name), ...]（依
    language_name排序）。日後要加新語言，複製一份 zh-Hant.json 改檔名跟內容、
    改好 _meta.language_code/_meta.language_name 放進 lang/ 就會自動出現在
    下拉選單，不用改程式。current.txt 不是語言檔，略過。"""
    result = []
    try:
        paths = LANG_DIR.glob("*.json")
    except OSError:
        paths = []
    for path in paths:
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        if not isinstance(data, dict):
            continue
        code = data.get("_meta.language_code", path.stem)
        name = data.get("_meta.language_name", code)
        result.append((code, name))
    result.sort(key=lambda item: item[1])
    return result


CURRENT_LANG = load_current_lang()
S = load_strings(CURRENT_LANG)


# ── 1. 字型（3分類，對應Config.cpp的kSectionCjk/kSectionSubtitle/kSectionNewspaper）──

FONT_SECTIONS = ["FontCjk", "FontSubtitle", "FontNewspaper"]

FONT_DEFAULTS = {
    "FontFace": "Microsoft JhengHei",
    "FontWeight": "0",
    "FontWidth": "0",
    "FontSpacing": "0",
    "FontYOffset": "0",
}

# FontSize fallback 依 section：通用/字幕 16、報紙 38（＝Config.cpp
# NewspaperMinRenderSizeFor(NewspaperFont1)，同時是 EnsureDefaultIni 寫入值）。
FONT_SIZE_DEFAULTS = {
    "FontCjk": "16",
    "FontSubtitle": "16",
    "FontNewspaper": "38",
}

WEIGHT_CODES = ["0", "300", "400", "500", "700"]


def build_weight_options() -> list:
    """(顯示文字, ini值) 清單，顯示文字走 lang json，語言切換後要重call這個
    函式重建，不能只讀一次快取。"""
    return [(S[f"font.weight.{code}"], code) for code in WEIGHT_CODES]


WEIGHT_OPTIONS = build_weight_options()

# ── 2. 選項開關（對應 Config.cpp 的 LoadGeneral/LoadLocSwitch/LoadSubtitle*/
#      LoadHookToggle/LoadNewsSoftBreakInterval）────────────────────────────────

GENERAL_SECTION = "General"
GENERAL_BOOL_FIELDS = [
    # (iniKey, 預設值)。顯示文字/說明在 lang json 的 general.<key>[.desc]。
    ("NormalFontReplace", "1"),
    ("Utf8ReencodeFix", "1"),
    ("GlyphHook", "1"),
    ("ToUpperHook", "1"),
    ("ZipPathTrace", "1"),
]

LOCSWITCH_SECTION = "LocSwitch"

SUBTITLE_SECTION = "Subtitle"
SUBTITLE_BOOL_FIELDS = [
    ("BriefingSubtitleEnabled", "1"),
    ("DialogueSubtitleEnabled", "1"),
    ("OnelinersSubtitleEnabled", "1"),
    ("WalkieSubtitleEnabled", "1"),
    ("TvRadioSubtitleEnabled", "1"),
]
# ini 存單一字串 "X,Y"（螢幕百分比 0~100），UI 拆成 X/Y 兩個欄位編輯。次選沿用
# native 原位，沒有 SubtitlePlaceSecond。
SUBTITLE_PLACE_FIELDS = [
    ("SubtitlePlaceFirst", "50,80"),
    ("SubtitlePlaceThird", "60,90"),
]

NEWSPAPER_SECTION = "NewsPaper"
NEWSPAPER_BOOL_FIELDS = [
    ("NewsPaperImageWrapFix", "1"),
    ("NewsPaperSpaceAdvanceFix", "1"),
    ("NewsPaperLineHeightFix", "1"),
    ("NewsPaperJustifyFillFix", "1"),
    ("NewsPaperSoftBreakFix", "1"),
]

HUD_SECTION = "Hud"
HUD_BOOL_FIELDS = [
    ("TrespassingEnabled", "1"),
    ("WarningEnabled", "1"),
    ("MinimapEnabled", "1"),
]
# ini 存單一字串 "X,Y"（螢幕百分比），UI 拆成 X/Y 兩個欄位編輯，比照
# SUBTITLE_PLACE_FIELDS／_parse_place()同一套解析＋clamp規則。
HUD_POS_FIELDS = [
    ("TrespassPos", "16,85"),
    ("WarningPos", "50,5"),
    ("MinimapPos", "5.0,60.0"),
]


def _parse_place(raw: str, default_x: float = 50.0, default_y: float = 80.0) -> tuple[str, str]:
    """比照 Config.cpp LoadPlaceConfigImpl() 的 sscanf_s("%f,%f")+clamp(0~100)
    邏輯，格式錯誤時 fallback 成該欄位自己的預設值。回傳字串 tuple 供 UI 欄位
    初始化用。"""
    try:
        x_str, y_str = raw.split(",", 1)
        x = max(0.0, min(100.0, float(x_str)))
        y = max(0.0, min(100.0, float(y_str)))
    except ValueError:
        x, y = default_x, default_y
    return f"{x:.1f}", f"{y:.1f}"


# ── 3. DEBUG開關（[Debug]，對應 Config.cpp LoadDebugImpl 的 12 個 key）─────────

# (iniKey, 對應的次數上限key或None, 預設值"0"/"1", 上限預設值或None)
DEBUG_FIELDS = [
    ("GlyphDiagEnable", "GlyphDiagCap", "0", "300"),
    ("CJKPlaceholderEnable", None, "0", None),
    ("TexUploadDiagEnable", None, "0", None),
    ("LocSwitchDiagEnable", None, "0", None),
    ("ToUpperDiagEnable", None, "0", None),
    ("NativeTexRegDiagEnable", None, "0", None),
    ("DebugSubtitleDiagEnable", None, "0", None),
    ("LabelWidthFixDiagEnable", None, "0", None),
    ("DebugZipPathTrace", None, "0", None),
    ("HudDiagEnable", None, "0", None),
    ("MinimapFloorProbe", None, "0", None),
    ("GlyphOverrunProbe", None, "0", None),
]

DEBUG_SECTION = "Debug"


def ini_get(section: str, key: str, default: str) -> str:
    buf = ctypes.create_unicode_buffer(256)
    ctypes.windll.kernel32.GetPrivateProfileStringW(
        section, key, default, buf, len(buf), str(INI_PATH)
    )
    return buf.value


def ini_set(section: str, key: str, value: str) -> bool:
    return bool(
        ctypes.windll.kernel32.WritePrivateProfileStringW(
            section, key, value, str(INI_PATH)
        )
    )


def _is_int(s: str) -> bool:
    try:
        int(s)
        return True
    except ValueError:
        return False


def _is_float(s: str) -> bool:
    try:
        float(s)
        return True
    except ValueError:
        return False


def make_scrollable(parent: ttk.Frame) -> ttk.Frame:
    """在 parent 裡建立一個垂直可捲動區域，回傳可放內容的內層 Frame。
    內容比視窗高時右側出現捲軸、滑鼠滾輪可捲動；內層寬度永遠跟著外層。"""
    parent.columnconfigure(0, weight=1)
    parent.rowconfigure(0, weight=1)

    canvas = tk.Canvas(parent, highlightthickness=0)
    vsb = ttk.Scrollbar(parent, orient="vertical", command=canvas.yview)
    canvas.configure(yscrollcommand=vsb.set)
    canvas.grid(column=0, row=0, sticky="nsew")
    vsb.grid(column=1, row=0, sticky="ns")

    inner = ttk.Frame(canvas)
    inner_id = canvas.create_window((0, 0), window=inner, anchor="nw")

    inner.bind("<Configure>",
               lambda _e: canvas.configure(scrollregion=canvas.bbox("all")))
    canvas.bind("<Configure>",
                lambda e: canvas.itemconfigure(inner_id, width=e.width))

    def _wheel(e):
        canvas.yview_scroll(-1 if e.delta > 0 else 1, "units")

    canvas.bind("<Enter>", lambda _e: canvas.bind_all("<MouseWheel>", _wheel))
    canvas.bind("<Leave>", lambda _e: canvas.unbind_all("<MouseWheel>"))

    return inner


# ── 系統字體列舉（跟obCJK_iniEdit.py同一套EnumFontFamiliesExW作法）──────────────

class LOGFONTW(ctypes.Structure):
    _fields_ = [
        ("lfHeight",         ctypes.wintypes.LONG),
        ("lfWidth",          ctypes.wintypes.LONG),
        ("lfEscapement",     ctypes.wintypes.LONG),
        ("lfOrientation",    ctypes.wintypes.LONG),
        ("lfWeight",         ctypes.wintypes.LONG),
        ("lfItalic",         ctypes.c_ubyte),
        ("lfUnderline",      ctypes.c_ubyte),
        ("lfStrikeOut",      ctypes.c_ubyte),
        ("lfCharSet",        ctypes.c_ubyte),
        ("lfOutPrecision",   ctypes.c_ubyte),
        ("lfClipPrecision",  ctypes.c_ubyte),
        ("lfQuality",        ctypes.c_ubyte),
        ("lfPitchAndFamily", ctypes.c_ubyte),
        ("lfFaceName",       ctypes.c_wchar * 32),
    ]


FONTENUMPROC = ctypes.WINFUNCTYPE(
    ctypes.c_int,
    ctypes.POINTER(LOGFONTW),
    ctypes.c_void_p,
    ctypes.wintypes.DWORD,
    ctypes.wintypes.LPARAM,
)

DEFAULT_CHARSET = 0x01


def enum_fonts() -> list[str]:
    gdi32 = ctypes.windll.gdi32
    user32 = ctypes.windll.user32
    hdc = user32.GetDC(None)
    found: list[str] = []

    def _cb(lplf, _tm, _ft, _lp):
        name = lplf.contents.lfFaceName
        if not name.startswith("@") and name not in found:
            found.append(name)
        return 1

    cb = FONTENUMPROC(_cb)
    lf = LOGFONTW()
    lf.lfCharSet = DEFAULT_CHARSET
    lf.lfFaceName = ""
    gdi32.EnumFontFamiliesExW(hdc, ctypes.byref(lf), cb, 0, 0)
    user32.ReleaseDC(None, hdc)
    return sorted(found)


# ── 分頁1：字型設定 ─────────────────────────────────────────────────────────

def _render_preview(canvas: tk.Canvas, section: str, vars_: dict, face_override: str = None) -> None:
    """依目前欄位值即時渲染預覽文字。跟obCJK_iniEdit.py的_update_preview()同
    一種近似手法：tkinter canvas text item無法重新縮放字形本身，FontWidth
    只能近似成「每字水平步進距離」而非真正拉伸字寬（GDI lfWidth的效果）；
    FontYOffset近似成整段文字統一的垂直位移。face_override：在左側全域字型
    清單瀏覽/點選時，不用先套用進欄位就能立即預覽該字型。"""
    canvas.delete("all")

    face = (face_override or vars_["face"].get().strip()) or FONT_DEFAULTS["FontFace"]
    try:
        size = max(6, min(int(vars_["size"].get()), 72))
    except ValueError:
        size = int(FONT_SIZE_DEFAULTS.get(section, "16"))
    weight_val = next(
        (val for label, val in WEIGHT_OPTIONS if label == vars_["weight"].get()), "0"
    )
    tk_weight = "bold" if int(weight_val or 0) >= 700 else "normal"
    try:
        width = int(vars_["width"].get())
    except ValueError:
        width = 0
    try:
        spacing = int(vars_["spacing"].get())
    except ValueError:
        spacing = 0
    try:
        yoffset = int(vars_["yOffset"].get())
    except ValueError:
        yoffset = 0

    try:
        fnt = tkfont.Font(family=face, size=-size, weight=tk_weight)
    except tk.TclError:
        canvas.create_text(6, 6, text=S["font.preview.cantload"].format(face=face), anchor="nw")
        return

    line_h = fnt.metrics("linespace")

    def _advance(ch: str) -> int:
        return width if width > 0 else fnt.measure(ch)

    sample = S.get(f"preview.sample.{section}", "AaBbCc 123")
    y = 6 + yoffset
    for line in sample.split("\n"):
        x = 6
        for ch in line:
            adv = _advance(ch)
            canvas.create_text(x, y, text=ch, font=fnt, anchor="nw")
            x += adv + spacing
        y += line_h + 2

    unit = "" if section == "FontNewspaper" else "px"
    canvas.create_text(6, y + 4, text=f"({face}, {size}{unit})", anchor="nw", fill="#888888")


def build_slot_tab(parent: ttk.Frame, section: str, on_apply_request) -> tuple[dict, tk.Canvas]:
    """建立單一字型分類(通用/字幕/報紙)的欄位＋即時預覽，回傳(vars_, canvas)。

    字型欄位不再各自帶一份完整系統字型Combobox，改成Entry＋「套用選取」
    按鈕——實際字型清單是左側全域共用的Listbox，按下按鈕會把左側清單目前
    選取的字型套進這個欄位；清單沒有選取項目時彈提示訊息。"""
    parent.columnconfigure(0, weight=1)
    parent.rowconfigure(0, weight=1)
    frm = ttk.Frame(parent, padding=12)
    frm.grid(sticky="nsew")
    frm.rowconfigure(7, weight=1)

    vars_: dict = {}

    ttk.Label(frm, text=S["font.field.face"]).grid(column=0, row=0, sticky="w")
    face_var = tk.StringVar(value=ini_get(section, "FontFace", FONT_DEFAULTS["FontFace"]))
    ttk.Entry(frm, textvariable=face_var, width=26).grid(column=1, row=0, sticky="w")
    ttk.Button(frm, text=S["btn.apply_font"], command=on_apply_request).grid(column=2, row=0, sticky="w", padx=(4, 0))
    vars_["face"] = face_var

    # 報紙那格的 FontSize 是固定 lineHeight 值、不是 px、不隨解析度縮放（見
    # 下方 font.newspaper.note 說明），標籤跟著改；存進 ini／CreateFont 用的仍
    # 是同一個數字，預覽畫布直接當數值用、不做轉換。
    size_label = S["font.field.size.newspaper"] if section == "FontNewspaper" else S["font.field.size"]
    ttk.Label(frm, text=size_label).grid(column=0, row=1, sticky="w")
    size_default = FONT_SIZE_DEFAULTS.get(section, "16")
    size_var = tk.StringVar(value=ini_get(section, "FontSize", size_default))
    ttk.Spinbox(frm, from_=8, to=72, textvariable=size_var, width=6).grid(column=1, row=1, sticky="w")
    vars_["size"] = size_var

    ttk.Label(frm, text=S["font.field.weight"]).grid(column=0, row=2, sticky="w")
    cur_weight = ini_get(section, "FontWeight", FONT_DEFAULTS["FontWeight"])
    weight_var = tk.StringVar(
        value=next((label for label, val in WEIGHT_OPTIONS if val == cur_weight), WEIGHT_OPTIONS[1][0])
    )
    ttk.Combobox(
        frm, textvariable=weight_var, values=[label for label, _ in WEIGHT_OPTIONS],
        state="readonly", width=12,
    ).grid(column=1, row=2, sticky="w")
    vars_["weight"] = weight_var

    ttk.Label(frm, text=S["font.field.width"]).grid(column=0, row=3, sticky="w")
    width_var = tk.StringVar(value=ini_get(section, "FontWidth", FONT_DEFAULTS["FontWidth"]))
    ttk.Spinbox(frm, from_=0, to=72, textvariable=width_var, width=6).grid(column=1, row=3, sticky="w")
    vars_["width"] = width_var

    ttk.Label(frm, text=S["font.field.spacing"]).grid(column=0, row=4, sticky="w")
    spacing_var = tk.StringVar(value=ini_get(section, "FontSpacing", FONT_DEFAULTS["FontSpacing"]))
    ttk.Spinbox(frm, from_=-20, to=20, textvariable=spacing_var, width=6).grid(column=1, row=4, sticky="w")
    vars_["spacing"] = spacing_var

    ttk.Label(frm, text=S["font.field.yoffset"]).grid(column=0, row=5, sticky="w")
    yoffset_var = tk.StringVar(value=ini_get(section, "FontYOffset", FONT_DEFAULTS["FontYOffset"]))
    ttk.Spinbox(frm, from_=-20, to=20, textvariable=yoffset_var, width=6).grid(column=1, row=5, sticky="w")
    vars_["yOffset"] = yoffset_var

    if section == "FontNewspaper":
        ttk.Label(frm, text=S["font.newspaper.note"], foreground="#666666",
                  wraplength=520, justify="left").grid(
            column=0, row=6, columnspan=3, sticky="w", pady=(12, 0))

    # ── 預覽畫布：放在字型欄位下方、橫跨整個寬度 ────────────────────────────
    preview_lf = ttk.LabelFrame(frm, text=S["font.preview"])
    preview_lf.grid(column=0, row=7, columnspan=3, sticky="nsew", pady=(16, 0))
    canvas = tk.Canvas(preview_lf, height=260, bg="white",
                        highlightthickness=1, highlightbackground="#999999")
    canvas.pack(padx=6, pady=6, fill="both", expand=True)

    preview_state = {"after_id": None}

    def _schedule_preview(*_args) -> None:
        if preview_state["after_id"] is not None:
            canvas.after_cancel(preview_state["after_id"])
        preview_state["after_id"] = canvas.after(
            150, lambda: _render_preview(canvas, section, vars_)
        )

    for key in ("face", "size", "weight", "width", "spacing", "yOffset"):
        vars_[key].trace_add("write", _schedule_preview)

    _render_preview(canvas, section, vars_)

    return vars_, canvas


def build_font_tab(parent: ttk.Frame) -> dict:
    """建立「字型設定」分頁：左側全域共用系統字型清單＋右側3個字型分類子分頁。
    回傳slot_vars（section名稱→欄位vars_字典），供on_save()寫回ini。"""
    parent.columnconfigure(1, weight=1)
    parent.rowconfigure(0, weight=1)

    list_frame = ttk.LabelFrame(parent, text=S["font.list_title"])
    list_frame.grid(column=0, row=0, sticky="ns", padx=(0, 12), pady=(0, 0))
    lb = tk.Listbox(list_frame, width=30, height=14, exportselection=False)
    sb = ttk.Scrollbar(list_frame, command=lb.yview)
    lb.configure(yscrollcommand=sb.set)
    lb.pack(side="left", fill="both", expand=True, padx=(4, 0), pady=4)
    sb.pack(side="right", fill="y", pady=4)
    for f in enum_fonts():
        lb.insert("end", f)

    notebook = ttk.Notebook(parent)
    notebook.grid(column=1, row=0, sticky="nsew")

    slot_vars: dict[str, dict] = {}
    slot_canvas: dict[str, tk.Canvas] = {}
    pending = {"section": None}    # 等待套用中的分頁（按過「套用選取」但清單還沒選）
    active_section = {"value": FONT_SECTIONS[0]}  # 目前顯示中的分頁，供清單預覽用

    def _request_apply(section: str) -> None:
        sel = lb.curselection()
        if sel:
            slot_vars[section]["face"].set(lb.get(sel[0]))
        else:
            pending["section"] = section
            messagebox.showinfo(S["msg.info.title"], S["msg.pick_font"])

    def _on_font_select(_event) -> None:
        sel = lb.curselection()
        if not sel:
            return
        font_name = lb.get(sel[0])
        if pending["section"] is not None:
            slot_vars[pending["section"]]["face"].set(font_name)
            pending["section"] = None
        sec = active_section["value"]
        _render_preview(slot_canvas[sec], sec, slot_vars[sec], face_override=font_name)

    lb.bind("<<ListboxSelect>>", _on_font_select)

    for section in FONT_SECTIONS:
        tab = ttk.Frame(notebook)
        notebook.add(tab, text=S[f"font.slot.{section}"])
        vars_, canvas = build_slot_tab(tab, section, lambda sec=section: _request_apply(sec))
        slot_vars[section] = vars_
        slot_canvas[section] = canvas

    def _on_tab_changed(_event) -> None:
        cur = notebook.select()
        if not cur:
            return
        idx = notebook.index(cur)
        active_section["value"] = FONT_SECTIONS[idx]

    notebook.bind("<<NotebookTabChanged>>", _on_tab_changed)

    return slot_vars


# ── 分頁2：選項開關 ─────────────────────────────────────────────────────────

def build_options_tab(parent: ttk.Frame) -> dict:
    """建立「選項開關」分頁：[General] / [LocSwitch] / [Subtitle] / [NewsPaper]。
    回傳vars_字典，key為(section, iniKey)，供on_save()寫回ini。
    值型別：BooleanVar＝開關；StringVar＝數值欄位；{"x","y"}dict＝字幕位置。"""
    frm = ttk.Frame(make_scrollable(parent), padding=12)
    frm.grid(sticky="nsew")
    frm.columnconfigure(0, weight=1)

    vars_: dict = {}
    r = 0

    # ── [General] 功能總開關 ──────────────────────────────────────────────
    lf1 = ttk.LabelFrame(frm, text=S["opt.general.frame"])
    lf1.grid(column=0, row=r, sticky="ew", pady=(0, 10)); r += 1
    lf1.columnconfigure(1, weight=1)
    for i, (key, default) in enumerate(GENERAL_BOOL_FIELDS):
        var = tk.BooleanVar(value=ini_get(GENERAL_SECTION, key, default) == "1")
        ttk.Checkbutton(lf1, text=S[f"general.{key}"], variable=var).grid(
            column=0, row=i, sticky="nw", padx=6, pady=4)
        ttk.Label(lf1, text=S[f"general.{key}.desc"], foreground="#666666",
                  wraplength=560, justify="left").grid(column=1, row=i, sticky="w", padx=(6, 6))
        vars_[(GENERAL_SECTION, key)] = var

    # ── [LocSwitch] 原文/譯文熱鍵切換 ────────────────────────────────────
    lf2 = ttk.LabelFrame(frm, text=S["opt.locswitch.frame"])
    lf2.grid(column=0, row=r, sticky="ew", pady=(0, 10)); r += 1
    lf2.columnconfigure(3, weight=1)

    enable_var = tk.BooleanVar(value=ini_get(LOCSWITCH_SECTION, "LocSwitchEnable", "1") == "1")
    ttk.Checkbutton(lf2, text=S["locswitch.enable"], variable=enable_var).grid(
        column=0, row=0, sticky="w", padx=6, pady=4)
    vars_[(LOCSWITCH_SECTION, "LocSwitchEnable")] = enable_var

    ttk.Label(lf2, text=S["locswitch.hotkey"]).grid(column=0, row=1, sticky="w", padx=6)
    hotkey_var = tk.StringVar(value=ini_get(LOCSWITCH_SECTION, "Hotkey", "122"))
    ttk.Entry(lf2, textvariable=hotkey_var, width=8).grid(column=1, row=1, sticky="w")
    vars_[(LOCSWITCH_SECTION, "Hotkey")] = hotkey_var

    capture_btn = ttk.Button(lf2, text=S["btn.detect_key"])
    capture_btn.grid(column=2, row=1, sticky="w", padx=(6, 0))
    hint_var = tk.StringVar(value=S["locswitch.hotkey.hint"])
    ttk.Label(lf2, textvariable=hint_var, foreground="#666666").grid(
        column=3, row=1, sticky="w", padx=(6, 0))

    ttk.Label(lf2, text=S["locswitch.hotkey.desc"], foreground="#666666",
              wraplength=560, justify="left").grid(
        column=0, row=2, columnspan=4, sticky="w", padx=6, pady=(2, 4))

    capture_state = {"active": False, "bind_id": None}

    def _stop_capture() -> None:
        top = lf2.winfo_toplevel()
        if capture_state["bind_id"] is not None:
            top.unbind("<KeyPress>", capture_state["bind_id"])
            capture_state["bind_id"] = None
        capture_state["active"] = False
        capture_btn.configure(text=S["btn.detect_key"])
        hint_var.set(S["locswitch.hotkey.hint"])

    def _on_key_captured(event: tk.Event) -> None:
        if event.keysym != "Escape":
            hotkey_var.set(str(event.keycode))
        _stop_capture()

    def _start_capture() -> None:
        if capture_state["active"]:
            _stop_capture()
            return
        top = lf2.winfo_toplevel()
        top.focus_set()
        capture_state["active"] = True
        capture_btn.configure(text=S["btn.detect_key.active"])
        hint_var.set(S["locswitch.hotkey.hint.capturing"])
        capture_state["bind_id"] = top.bind("<KeyPress>", _on_key_captured)

    capture_btn.configure(command=_start_capture)

    # ── [Subtitle] 字幕補完（三個開關 + NpcHearRadius）───────────────────
    lf3 = ttk.LabelFrame(frm, text=S["opt.subtitle.frame"])
    lf3.grid(column=0, row=r, sticky="ew", pady=(0, 10)); r += 1
    lf3.columnconfigure(1, weight=1)
    for i, (key, default) in enumerate(SUBTITLE_BOOL_FIELDS):
        var = tk.BooleanVar(value=ini_get(SUBTITLE_SECTION, key, default) == "1")
        ttk.Checkbutton(lf3, text=S[f"subtitle.{key}"], variable=var).grid(
            column=0, row=i, sticky="nw", padx=6, pady=4)
        ttk.Label(lf3, text=S[f"subtitle.{key}.desc"], foreground="#666666",
                  wraplength=540, justify="left").grid(column=1, row=i, sticky="w", padx=(6, 6))
        vars_[(SUBTITLE_SECTION, key)] = var

    nr = len(SUBTITLE_BOOL_FIELDS)
    radius_row = ttk.Frame(lf3)
    radius_row.grid(column=0, row=nr, sticky="nw", padx=6, pady=4)
    ttk.Label(radius_row, text=S["subtitle.NpcHearRadius"]).pack(side="left")
    radius_var = tk.StringVar(value=ini_get(SUBTITLE_SECTION, "NpcHearRadius", "1000"))
    ttk.Spinbox(radius_row, from_=0, to=100000, textvariable=radius_var, width=8).pack(side="left")
    vars_[(SUBTITLE_SECTION, "NpcHearRadius")] = radius_var
    ttk.Label(lf3, text=S["subtitle.NpcHearRadius.desc"], foreground="#666666",
              wraplength=540, justify="left").grid(column=1, row=nr, sticky="w", padx=(6, 6))

    tr = nr + 1
    tvradio_row = ttk.Frame(lf3)
    tvradio_row.grid(column=0, row=tr, sticky="nw", padx=6, pady=4)
    ttk.Label(tvradio_row, text=S["subtitle.TvRadioHearRadius"]).pack(side="left")
    tvradio_var = tk.StringVar(value=ini_get(SUBTITLE_SECTION, "TvRadioHearRadius", "3000"))
    ttk.Spinbox(tvradio_row, from_=0, to=100000, textvariable=tvradio_var, width=8).pack(side="left")
    vars_[(SUBTITLE_SECTION, "TvRadioHearRadius")] = tvradio_var
    ttk.Label(lf3, text=S["subtitle.TvRadioHearRadius.desc"], foreground="#666666",
              wraplength=540, justify="left").grid(column=1, row=tr, sticky="w", padx=(6, 6))

    # ── [NewsPaper] 報紙排版修正 ────────────────────────────────────────
    lf5 = ttk.LabelFrame(frm, text=S["opt.newspaper.frame"])
    lf5.grid(column=0, row=r, sticky="ew", pady=(0, 10)); r += 1
    lf5.columnconfigure(1, weight=1)
    for i, (key, default) in enumerate(NEWSPAPER_BOOL_FIELDS):
        var = tk.BooleanVar(value=ini_get(NEWSPAPER_SECTION, key, default) == "1")
        ttk.Checkbutton(lf5, text=S[f"newspaper.{key}"], variable=var).grid(
            column=0, row=i, sticky="nw", padx=6, pady=4)
        ttk.Label(lf5, text=S[f"newspaper.{key}.desc"], foreground="#666666",
                  wraplength=540, justify="left").grid(column=1, row=i, sticky="w", padx=(6, 6))
        vars_[(NEWSPAPER_SECTION, key)] = var

    ni = len(NEWSPAPER_BOOL_FIELDS)
    interval_row = ttk.Frame(lf5)
    interval_row.grid(column=0, row=ni, sticky="nw", padx=6, pady=4)
    ttk.Label(interval_row, text=S["newspaper.SoftBreakCjkInterval"]).pack(side="left")
    interval_var = tk.StringVar(value=ini_get(NEWSPAPER_SECTION, "SoftBreakCjkInterval", "6"))
    ttk.Spinbox(interval_row, from_=0, to=100, textvariable=interval_var, width=6).pack(side="left")
    vars_[(NEWSPAPER_SECTION, "SoftBreakCjkInterval")] = interval_var
    ttk.Label(lf5, text=S["newspaper.SoftBreakCjkInterval.desc"], foreground="#666666",
              wraplength=540, justify="left").grid(column=1, row=ni, sticky="w", padx=(6, 6))

    return vars_


# ── 分頁3：位置設定 ─────────────────────────────────────────────────────────

LAYOUT_PREVIEW_W = 1024
LAYOUT_PREVIEW_H = 768
LAYOUT_PREVIEW_REF_W = 1024  # MinimapSize換算預覽框像素用的參考寬度
MINIMAP_SAMPLE_PATH = _BASE_DIR / "minimaptest.png"
MINIMAP_ZOOM_BASELINE = 4000

_minimap_sample_cache: dict = {"photo": None, "tried": False}


def _get_minimap_sample() -> tk.PhotoImage | None:
    """惰性載入小地圖示意圖(minimaptest.png)，找不到檔案時回傳None(預覽改畫空框)。"""
    if not _minimap_sample_cache["tried"]:
        _minimap_sample_cache["tried"] = True
        try:
            _minimap_sample_cache["photo"] = tk.PhotoImage(file=str(MINIMAP_SAMPLE_PATH))
        except tk.TclError:
            _minimap_sample_cache["photo"] = None
    return _minimap_sample_cache["photo"]


def _scaled_photo(base: tk.PhotoImage, scale: float) -> tk.PhotoImage:
    """用PhotoImage.zoom()/subsample()的整數倍率近似任意縮放比例。"""
    scale = max(0.05, min(scale, 4.0))
    if scale >= 1.0:
        factor = max(1, round(scale))
        return base.zoom(factor, factor) if factor > 1 else base
    factor = max(1, round(1 / scale))
    return base.subsample(factor, factor)


def _render_layout_preview(canvas: tk.Canvas, vars_: dict, mini_state: dict) -> None:
    """依目前[Hud]/[Subtitle]位置欄位畫面預覽：4個文字標記＋小地圖示意框（框內圖
    依MinimapZoom裁放模擬縮放效果）。"""
    canvas.delete("all")
    if mini_state["mini_canvas"] is not None:
        mini_state["mini_canvas"].destroy()
        mini_state["mini_canvas"] = None

    def _pos(section, key, default_x, default_y):
        raw = vars_[(section, key)]
        try:
            return (max(0.0, min(100.0, float(raw["x"].get()))),
                    max(0.0, min(100.0, float(raw["y"].get()))))
        except ValueError:
            return default_x, default_y

    def _marker(x_pct, y_pct, label):
        px, py = x_pct / 100 * LAYOUT_PREVIEW_W, y_pct / 100 * LAYOUT_PREVIEW_H
        canvas.create_text(px, py, text=label, fill="#ffcc66", anchor="center")

    if vars_[(HUD_SECTION, "WarningEnabled")].get():
        _marker(*_pos(HUD_SECTION, "WarningPos", 50.0, 5.0), S["layout.preview.warning"])
    if vars_[(HUD_SECTION, "TrespassingEnabled")].get():
        _marker(*_pos(HUD_SECTION, "TrespassPos", 16.0, 85.0), S["layout.preview.trespass"])
    _marker(*_pos(SUBTITLE_SECTION, "SubtitlePlaceFirst", 50.0, 80.0), S["layout.preview.subtitle_first"])
    _marker(*_pos(SUBTITLE_SECTION, "SubtitlePlaceThird", 60.0, 90.0), S["layout.preview.subtitle_third"])

    if vars_[(HUD_SECTION, "MinimapEnabled")].get():
        mx, my = _pos(HUD_SECTION, "MinimapPos", 5.0, 60.0)
        try:
            size_val = int(vars_[(HUD_SECTION, "MinimapSize")].get())
        except ValueError:
            size_val = 15
        try:
            zoom_val = float(vars_[(HUD_SECTION, "MinimapZoom")].get())
        except ValueError:
            zoom_val = MINIMAP_ZOOM_BASELINE

        box_left = mx / 100 * LAYOUT_PREVIEW_W
        box_top = my / 100 * LAYOUT_PREVIEW_H
        box_edge = int(max(4, min(LAYOUT_PREVIEW_W, LAYOUT_PREVIEW_H,
                                   size_val * 10 * LAYOUT_PREVIEW_W / LAYOUT_PREVIEW_REF_W)))

        mini = tk.Canvas(canvas, width=box_edge, height=box_edge, bg="#141414",
                          highlightthickness=2, highlightbackground="#c9a227")
        sample = _get_minimap_sample()
        if sample is not None:
            fit_scale = min(box_edge / sample.width(), box_edge / sample.height())
            if zoom_val <= 0:
                scale = fit_scale
            else:
                multiplier = (MINIMAP_ZOOM_BASELINE / zoom_val) ** 3
                scale = fit_scale * max(1.0, min(8.0, multiplier))
            photo = _scaled_photo(sample, scale)
            mini.create_image(box_edge / 2, box_edge / 2, anchor="center", image=photo)
            mini.image = photo  # 保留參照避免被GC回收
        cx = cy = box_edge / 2
        mini.create_rectangle(cx - 2, cy - 2, cx + 2, cy + 2, fill="#e2231a", outline="")
        canvas.create_window(box_left, box_top, anchor="nw", window=mini)
        canvas.create_text(
            box_left, box_top + box_edge + 4, anchor="nw", fill="#c9a227",
            text=S["layout.preview.minimap"] + " " +
            S["layout.preview.zoom_label"].format(value=int(zoom_val)),
        )
        mini_state["mini_canvas"] = mini


def build_layout_tab(parent: ttk.Frame) -> dict:
    """建立「位置設定」分頁：[Hud]小地圖／擅闖／警告(含啟用開關)＋[Subtitle]字幕
    位置，底部即時畫面預覽。回傳vars_字典(section,key)→變數，供on_save()寫回ini
    （結構同build_options_tab，key不重複，寫入時直接跟options_vars合併處理）。"""
    frm = ttk.Frame(make_scrollable(parent), padding=12)
    frm.grid(sticky="nsew")
    frm.columnconfigure(0, weight=1)

    vars_: dict = {}
    r = 0

    # ── [Hud] 小地圖／擅闖／警告 ─────────────────────────────────────────
    lf1 = ttk.LabelFrame(frm, text=S["opt.hud.frame"])
    lf1.grid(column=0, row=r, sticky="ew", pady=(0, 10)); r += 1
    lf1.columnconfigure(1, weight=1)
    hr = 0
    for key, default in HUD_BOOL_FIELDS:
        var = tk.BooleanVar(value=ini_get(HUD_SECTION, key, default) == "1")
        ttk.Checkbutton(lf1, text=S[f"hud.{key}"], variable=var).grid(
            column=0, row=hr, sticky="nw", padx=6, pady=4)
        ttk.Label(lf1, text=S[f"hud.{key}.desc"], foreground="#666666",
                  wraplength=540, justify="left").grid(column=1, row=hr, sticky="w", padx=(6, 6))
        vars_[(HUD_SECTION, key)] = var
        hr += 1

    for key, default in HUD_POS_FIELDS:
        dx, dy = (float(v) for v in default.split(","))
        raw = ini_get(HUD_SECTION, key, default)
        x_str, y_str = _parse_place(raw, dx, dy)
        ttk.Label(lf1, text=S[f"hud.{key}"] + "：").grid(
            column=0, row=hr, sticky="w", padx=6, pady=(4, 0))
        pos_row = ttk.Frame(lf1)
        pos_row.grid(column=1, row=hr, sticky="w", padx=(6, 6))
        x_var = tk.StringVar(value=x_str)
        ttk.Label(pos_row, text=S["subtitle.place.x"]).pack(side="left")
        ttk.Spinbox(pos_row, from_=0, to=100, increment=0.5, textvariable=x_var, width=6).pack(
            side="left", padx=(2, 8))
        y_var = tk.StringVar(value=y_str)
        ttk.Label(pos_row, text=S["subtitle.place.y"]).pack(side="left")
        ttk.Spinbox(pos_row, from_=0, to=100, increment=0.5, textvariable=y_var, width=6).pack(
            side="left", padx=(2, 0))
        vars_[(HUD_SECTION, key)] = {"x": x_var, "y": y_var}
        hr += 1
        ttk.Label(lf1, text=S[f"hud.{key}.desc"], foreground="#666666",
                  wraplength=520, justify="left").grid(
            column=0, row=hr, columnspan=2, sticky="w", padx=6, pady=(0, 4))
        hr += 1

    size_row = ttk.Frame(lf1)
    size_row.grid(column=0, row=hr, sticky="nw", padx=6, pady=4)
    ttk.Label(size_row, text=S["hud.MinimapSize"]).pack(side="left")
    size_var = tk.StringVar(value=ini_get(HUD_SECTION, "MinimapSize", "15"))
    ttk.Spinbox(size_row, from_=0, to=100, textvariable=size_var, width=6).pack(side="left")
    vars_[(HUD_SECTION, "MinimapSize")] = size_var
    ttk.Label(lf1, text=S["hud.MinimapSize.desc"], foreground="#666666",
              wraplength=540, justify="left").grid(column=1, row=hr, sticky="w", padx=(6, 6))
    hr += 1

    zoom_row = ttk.Frame(lf1)
    zoom_row.grid(column=0, row=hr, sticky="nw", padx=6, pady=4)
    ttk.Label(zoom_row, text=S["hud.MinimapZoom"]).pack(side="left")
    zoom_var = tk.StringVar(value=ini_get(HUD_SECTION, "MinimapZoom", "4000"))
    ttk.Spinbox(zoom_row, from_=0, to=100000, increment=100, textvariable=zoom_var, width=8).pack(side="left")
    vars_[(HUD_SECTION, "MinimapZoom")] = zoom_var
    ttk.Label(lf1, text=S["hud.MinimapZoom.desc"], foreground="#666666",
              wraplength=540, justify="left").grid(column=1, row=hr, sticky="w", padx=(6, 6))

    # ── [Subtitle] 字幕位置 ─────────────────────────────────────────────
    lf2 = ttk.LabelFrame(frm, text=S["opt.subtitle.place.frame"])
    lf2.grid(column=0, row=r, sticky="ew", pady=(0, 10)); r += 1
    lf2.columnconfigure(5, weight=1)
    for i, (key, default) in enumerate(SUBTITLE_PLACE_FIELDS):
        dx, dy = (float(v) for v in default.split(","))
        raw = ini_get(SUBTITLE_SECTION, key, default)
        x_str, y_str = _parse_place(raw, dx, dy)
        ttk.Label(lf2, text=S[f"subtitle.place.{key}"] + "：").grid(
            column=0, row=i * 2, sticky="w", padx=6, pady=(4, 0))
        x_var = tk.StringVar(value=x_str)
        ttk.Label(lf2, text=S["subtitle.place.x"]).grid(column=1, row=i * 2, sticky="e")
        ttk.Spinbox(lf2, from_=0, to=100, increment=0.5, textvariable=x_var, width=6).grid(
            column=2, row=i * 2, sticky="w", padx=(2, 8))
        y_var = tk.StringVar(value=y_str)
        ttk.Label(lf2, text=S["subtitle.place.y"]).grid(column=3, row=i * 2, sticky="e")
        ttk.Spinbox(lf2, from_=0, to=100, increment=0.5, textvariable=y_var, width=6).grid(
            column=4, row=i * 2, sticky="w", padx=(2, 8))
        ttk.Label(lf2, text=S[f"subtitle.place.{key}.desc"], foreground="#666666",
                  wraplength=520, justify="left").grid(
            column=0, row=i * 2 + 1, columnspan=6, sticky="w", padx=6, pady=(0, 4))
        vars_[(SUBTITLE_SECTION, key)] = {"x": x_var, "y": y_var}

    # ── 畫面預覽 ─────────────────────────────────────────────────────────
    preview_lf = ttk.LabelFrame(frm, text=S["layout.preview.frame"])
    preview_lf.grid(column=0, row=r, sticky="nsew"); r += 1
    canvas = tk.Canvas(preview_lf, width=LAYOUT_PREVIEW_W, height=LAYOUT_PREVIEW_H,
                        bg="#0a0e14", highlightthickness=1, highlightbackground="#999999")
    canvas.pack(padx=6, pady=6)

    mini_state = {"mini_canvas": None}
    preview_state = {"after_id": None}

    def _schedule_preview(*_args) -> None:
        if preview_state["after_id"] is not None:
            canvas.after_cancel(preview_state["after_id"])
        preview_state["after_id"] = canvas.after(
            150, lambda: _render_layout_preview(canvas, vars_, mini_state)
        )

    for v in vars_.values():
        if isinstance(v, dict):
            v["x"].trace_add("write", _schedule_preview)
            v["y"].trace_add("write", _schedule_preview)
        else:
            v.trace_add("write", _schedule_preview)

    _render_layout_preview(canvas, vars_, mini_state)

    return vars_


# ── 分頁4：DEBUG開關 ────────────────────────────────────────────────────────

def build_debug_tab(parent: ttk.Frame) -> dict:
    """建立「DEBUG開關」分頁：[Debug]全部診斷開關。回傳vars_字典，key為
    (section, iniKey)，供on_save()寫回ini。"""
    frm = ttk.Frame(make_scrollable(parent), padding=12)
    frm.grid(sticky="nsew")
    frm.columnconfigure(3, weight=1)

    ttk.Label(
        frm, text=S["debug.header"],
        foreground="#666666", wraplength=620, justify="left",
    ).grid(column=0, row=0, columnspan=4, sticky="w", pady=(0, 10))

    vars_: dict = {}
    row = 1
    for key, cap_key, default, cap_default in DEBUG_FIELDS:
        var = tk.BooleanVar(value=ini_get(DEBUG_SECTION, key, default) == "1")
        ttk.Checkbutton(frm, text=S[f"debug.{key}"], variable=var).grid(
            column=0, row=row, sticky="w", pady=(4, 0))
        vars_[(DEBUG_SECTION, key)] = var

        if cap_key:
            ttk.Label(frm, text=S["debug.cap_label"]).grid(
                column=1, row=row, sticky="e", padx=(16, 4))
            cap_var = tk.StringVar(value=ini_get(DEBUG_SECTION, cap_key, cap_default))
            ttk.Spinbox(frm, from_=1, to=100000, textvariable=cap_var, width=8).grid(
                column=2, row=row, sticky="w")
            vars_[(DEBUG_SECTION, cap_key)] = cap_var

        row += 1
        ttk.Label(frm, text=S[f"debug.{key}.desc"], foreground="#666666",
                  wraplength=620, justify="left").grid(
            column=0, row=row, columnspan=4, sticky="w", padx=(24, 0), pady=(0, 4))
        row += 1

    return vars_


def _auto_fit_screen(root: tk.Tk, target_w: int = 1024, target_h: int = 860) -> None:
    """比照G:\\bloodmoney\\HitmanLOCTranslator\\python_gui實作注意事項.md
    第1~5點：視窗開啟時避開工作列、預設視窗化、扣除標題列/邊框(chrome)實際
    佔用尺寸。呼叫時機固定在UI都build完之後，且要先minsize()蓋掉Tk依內容
    自然大小反推出的隱性最小視窗尺寸。

    ★開啟時固定用 target_w x target_h（含標題列/邊框的整個視窗實際尺寸）置中
    開啟，但不超過目前工作區可用範圍。"""
    root.minsize(480, 360)
    root.update_idletasks()

    SPI_GETWORKAREA = 0x0030
    rect = ctypes.wintypes.RECT()
    ctypes.windll.user32.SystemParametersInfoW(SPI_GETWORKAREA, 0, ctypes.byref(rect), 0)
    work_x, work_y = rect.left, rect.top
    work_w, work_h = rect.right - rect.left, rect.bottom - rect.top

    outer_w = min(target_w, work_w)
    outer_h = min(target_h, work_h)
    x = work_x + max((work_w - outer_w) // 2, 0)
    y = work_y + max((work_h - outer_h) // 2, 0)

    root.geometry(f"{outer_w}x{outer_h}+{x}+{y}")
    root.update()

    GA_ROOT = 2
    hwnd = ctypes.windll.user32.GetAncestor(root.winfo_id(), GA_ROOT)
    outer_rect = ctypes.wintypes.RECT()
    ctypes.windll.user32.GetWindowRect(hwnd, ctypes.byref(outer_rect))
    chrome_w = max((outer_rect.right - outer_rect.left) - root.winfo_width(), 0)
    chrome_h = max((outer_rect.bottom - outer_rect.top) - root.winfo_height(), 0)

    client_w = max(outer_w - chrome_w, 1)
    client_h = max(outer_h - chrome_h, 1)
    x = work_x + max((work_w - (client_w + chrome_w)) // 2, 0)
    y = work_y + max((work_h - (client_h + chrome_h)) // 2, 0)
    root.geometry(f"{client_w}x{client_h}+{x}+{y}")


# ── main ────────────────────────────────────────────────────────────────────

def main():
    root = tk.Tk()
    root.resizable(True, True)
    root.columnconfigure(0, weight=1)
    root.rowconfigure(0, weight=1)

    def build_ui():
        """建立/重建整個視窗內容。切換語言下拉選單時會重call這個函式：清掉
        root底下所有子元件重蓋一次，讀取的是重call當下的全域S／WEIGHT_OPTIONS，
        不用重開視窗。ini欄位值一律重新from ini讀，跟語言切換無關。"""
        global CURRENT_LANG, S, WEIGHT_OPTIONS

        for child in root.winfo_children():
            child.destroy()

        root.title(S["app.title"])

        outer = ttk.Frame(root, padding=12)
        outer.grid(sticky="nsew")
        outer.columnconfigure(0, weight=1)
        outer.rowconfigure(1, weight=1)

        top_row = ttk.Frame(outer)
        top_row.grid(column=0, row=0, sticky="ew", pady=(0, 8))
        top_row.columnconfigure(0, weight=1)

        ttk.Label(top_row, text=S["app.ini_path"].format(path=INI_PATH)).grid(
            column=0, row=0, sticky="w")

        langs = available_languages()
        lang_names = [name for _, name in langs]
        cur_name = next(
            (name for code, name in langs if code == CURRENT_LANG),
            lang_names[0] if lang_names else "",
        )
        ttk.Label(top_row, text=S["app.lang_label"]).grid(
            column=1, row=0, sticky="e", padx=(12, 4))
        lang_var = tk.StringVar(value=cur_name)
        lang_combo = ttk.Combobox(
            top_row, textvariable=lang_var, values=lang_names,
            state="readonly", width=14,
        )
        lang_combo.grid(column=2, row=0, sticky="e")

        def on_lang_change(event=None):
            global CURRENT_LANG, S, WEIGHT_OPTIONS
            chosen = next((code for code, name in langs if name == lang_var.get()), CURRENT_LANG)
            if chosen == CURRENT_LANG:
                return
            CURRENT_LANG = chosen
            save_current_lang(chosen)
            S = load_strings(chosen)
            WEIGHT_OPTIONS = build_weight_options()
            build_ui()

        lang_combo.bind("<<ComboboxSelected>>", on_lang_change)

        top_notebook = ttk.Notebook(outer)
        top_notebook.grid(column=0, row=1, sticky="nsew")

        font_tab = ttk.Frame(top_notebook)
        options_tab = ttk.Frame(top_notebook)
        layout_tab = ttk.Frame(top_notebook)
        debug_tab = ttk.Frame(top_notebook)
        top_notebook.add(font_tab, text=S["tab.font"])
        top_notebook.add(options_tab, text=S["tab.options"])
        top_notebook.add(layout_tab, text=S["tab.layout"])
        top_notebook.add(debug_tab, text=S["tab.debug"])

        slot_vars = build_font_tab(font_tab)
        options_vars = build_options_tab(options_tab)
        layout_vars = build_layout_tab(layout_tab)
        debug_vars = build_debug_tab(debug_tab)

        def on_save():
            # ── 字型（3分類）─────────────────────────────────────────────────
            for section, vars_ in slot_vars.items():
                weight_val = next(
                    (val for label, val in WEIGHT_OPTIONS if label == vars_["weight"].get()), "0"
                )
                try:
                    size_val = str(int(vars_["size"].get()))
                    width_val = str(int(vars_["width"].get()))
                    spacing_val = str(int(vars_["spacing"].get()))
                    yoffset_val = str(int(vars_["yOffset"].get()))
                except ValueError:
                    messagebox.showerror(S["msg.error.title"],
                                         S["err.font_numeric"].format(section=section))
                    return

                ok = all([
                    ini_set(section, "FontFace", vars_["face"].get().strip() or FONT_DEFAULTS["FontFace"]),
                    ini_set(section, "FontSize", size_val),
                    ini_set(section, "FontWeight", weight_val),
                    ini_set(section, "FontWidth", width_val),
                    ini_set(section, "FontSpacing", spacing_val),
                    ini_set(section, "FontYOffset", yoffset_val),
                ])
                if not ok:
                    messagebox.showerror(S["msg.error.title"],
                                         S["err.ini_write"].format(path=INI_PATH, section=section))
                    return

            # ── 選項開關（[General]/[LocSwitch]/[Subtitle]/[NewsPaper]）＋
            #    位置設定（[Hud]/[Subtitle]位置，跟選項開關共用同一套驗證規則）──
            for (section, key), var in {**options_vars, **layout_vars}.items():
                if isinstance(var, dict):  # 字幕位置／Hud位置 X,Y
                    try:
                        x_val = max(0.0, min(100.0, float(var["x"].get())))
                        y_val = max(0.0, min(100.0, float(var["y"].get())))
                    except ValueError:
                        err_key = "err.hud_coord" if section == HUD_SECTION else "err.subtitle_coord"
                        messagebox.showerror(S["msg.error.title"],
                                             S[err_key].format(key=key))
                        return
                    value = f"{x_val:.1f},{y_val:.1f}"
                elif isinstance(var, tk.BooleanVar):
                    value = "1" if var.get() else "0"
                else:
                    raw = var.get().strip()
                    if (section, key) == (LOCSWITCH_SECTION, "Hotkey"):
                        if not _is_int(raw):
                            messagebox.showerror(S["msg.error.title"], S["err.hotkey_numeric"])
                            return
                        value = str(int(raw))
                    elif (section, key) == (SUBTITLE_SECTION, "NpcHearRadius"):
                        if not _is_float(raw):
                            messagebox.showerror(S["msg.error.title"], S["err.npc_radius_numeric"])
                            return
                        value = raw
                    elif (section, key) == (SUBTITLE_SECTION, "TvRadioHearRadius"):
                        if not _is_float(raw):
                            messagebox.showerror(S["msg.error.title"], S["err.tvradio_radius_numeric"])
                            return
                        value = raw
                    elif (section, key) == (NEWSPAPER_SECTION, "SoftBreakCjkInterval"):
                        if not _is_int(raw):
                            messagebox.showerror(S["msg.error.title"], S["err.newspaper_interval"])
                            return
                        value = str(int(raw))
                    elif (section, key) == (HUD_SECTION, "MinimapSize"):
                        if not _is_int(raw) or not (0 <= int(raw) <= 100):
                            messagebox.showerror(S["msg.error.title"], S["err.minimap_size_numeric"])
                            return
                        value = str(int(raw))
                    elif (section, key) == (HUD_SECTION, "MinimapZoom"):
                        if not _is_float(raw):
                            messagebox.showerror(S["msg.error.title"], S["err.minimap_zoom_numeric"])
                            return
                        value = str(max(0.0, float(raw)))
                    else:
                        value = raw
                if not ini_set(section, key, value):
                    messagebox.showerror(S["msg.error.title"],
                                         S["err.ini_write_key"].format(path=INI_PATH, section=section, key=key))
                    return

            # ── DEBUG開關（[Debug]）─────────────────────────────────────────
            for (section, key), var in debug_vars.items():
                if isinstance(var, tk.BooleanVar):
                    value = "1" if var.get() else "0"
                else:
                    try:
                        value = str(int(var.get()))
                    except ValueError:
                        messagebox.showerror(S["msg.error.title"],
                                             S["err.debug_numeric"].format(key=key))
                        return
                if not ini_set(section, key, value):
                    messagebox.showerror(S["msg.error.title"],
                                         S["err.ini_write_key"].format(path=INI_PATH, section=section, key=key))
                    return

            messagebox.showinfo(S["msg.save_done.title"], S["msg.save_done.body"])

        ttk.Button(outer, text=S["btn.save"], command=on_save).grid(column=0, row=2, pady=(12, 0))

        _auto_fit_screen(root)

    build_ui()
    root.mainloop()


if __name__ == "__main__":
    main()
