#include "Hook.h"
#include "Log.h"
#include "Config.h"
#include "GlyphAtlas.h"
#include "GlyphHook.h"
#include "ZipPathTrace.h"
#include "TextureManagerReleaseHook.h"
#include "NativeTextureRegistry.h"
#include "LocHook.h"
#include "ToUpperHook.h"
#include "Utf8ReencodeFixHook.h"
#include "SubtitleHook.h"
#include "TrespassHud.h"
#include "WarningHud.h"
#include "MinimapHud.h"
#include "TexDispatchHook.h"
#include "NewsPaperImageWrapHook.h"
#include "NewsPaperSpaceAdvanceFix.h"
#include "NewsPaperLineHeightFix.h"
#include "NewsPaperJustifyFillFix.h"
#include "LabelWidthFixInventory.h"
#include "LabelWidthFixShop.h"
#include <Windows.h>

namespace Hook
{
    // 轉發本身由 linker export forwarding 交給 OS loader（見 binkw32.def），
    // 這裡只做前置驗證：確認 binkw32_real.dll 存在、能載入、9 個匯出都在。
    static void VerifyBinkForwarding()
    {
        HMODULE realDll = LoadLibraryA("binkw32_real.dll");
        if (!realDll)
        {
            Log::Write("[Hook] binkw32_real.dll 載入失敗（err=%lu）！原始Bink影片播放會壞掉，"
                       "請確認真正的binkw32.dll已改名成binkw32_real.dll並放在遊戲目錄", GetLastError());
            return;
        }

        static const char* kExports[] = {
            "_BinkOpen@8", "_BinkClose@4", "_BinkDoFrame@4", "_BinkNextFrame@4",
            "_BinkWait@4", "_BinkCopyToBuffer@28", "_BinkSetSoundSystem@8",
            "_BinkOpenDirectSound@4", "_BinkSetVolume@12",
        };
        int ok = 0;
        for (const char* name : kExports)
        {
            if (GetProcAddress(realDll, name)) ok++;
            else Log::Write("[Hook] binkw32_real.dll 缺少匯出：%s", name);
        }
        Log::Write("[Hook] binkw32_real.dll 轉發前置驗證：%d/9 個匯出存在", ok);
        FreeLibrary(realDll);
    }

    void Init(HMODULE hModule)
    {
        Log::Write("[Hook] Init 開始");

        VerifyBinkForwarding();

        Config::EnsureDefaultIni(hModule);
        // 整份 ini 讀進全域快取，只做這一次；後面各模組的 Config::LoadXxx()
        // 都回傳這份快取（見 Config.cpp「全域設定快取」節）。
        Config::Init(hModule);

        // 兩份 hook 開關設定：hookToggle 只剩報紙 5 欄（ini key 在 [NewsPaper]），
        // generalCfg 對應 [General]；供切原文/譯文重跑時二分排查是哪個 hook
        // 影響排版（見 Config.h 對應 struct 註解）。
        Config::HookToggleConfig hookToggle = Config::LoadHookToggle(hModule);
        Config::GeneralConfig generalCfg = Config::LoadGeneral(hModule);

        // CJK 光柵化引擎（GDI），內部讀 Config::FontConfig 建立 GDI 字型。
        GlyphAtlas::Init(hModule);

        // 報紙分類判斷 IsNewspaperContext()、LocHook 原文/譯文 LOC 路徑推導、
        // SubtitleBrief 場景切換偵測都依賴這個 hook 的 GetLastSceneZipPath()，
        // 不能真的關閉。詳細 trace log 另由 [Debug] DebugZipPathTrace 控制
        // （只影響 log、不影響安裝）。
        if (generalCfg.zipPathTrace) ZipPathTrace::Install(hModule);
        else Log::Write("[Hook] ZipPathTrace 已由[General] ZipPathTrace=0停用，跳過安裝（⚠️報紙分類判斷/NewsPaperImageWrapHook的IsNewspaperContext()、LocHook的原文/譯文LOC路徑推導、SubtitleBrief的場景切換偵測，全部依賴這個hook的GetLastSceneZipPath()，關掉這個會連帶讓這些功能全部失效）");

        // texture reset 清空問題修法：對 native ReleaseTextures(sub_48F190)
        // 做 function-entry inline patch，攔截場景切換自呼叫／引擎關閉解構子
        // 兩條觸發路徑，命中後把 NativeTextureRegistry 兩個字型槽的 nativeIndex
        // 重設 -1，讓下次 BeginScene 重新登記材質，不沿用被 native 清空的舊頁碼。
        // 固定 VA，載入當下即可安裝。屬字體取代管線一環，故隨 [General]
        // NormalFontReplace 一起控制。
        if (generalCfg.normalFontReplace) TextureManagerReleaseHook::Install();
        else Log::Write("[Hook] TextureManagerReleaseHook 已由[General] NormalFontReplace=0停用，跳過安裝");

        // 只讀設定、不做 D3D9/native 呼叫——真正的 vtable patch 由
        // GlyphHook.cpp 的 EnsureBeginSceneHookInstalled() lazy retry 負責。
        // 隨 [General] NormalFontReplace 控制（字體取代管線一環）。
        if (generalCfg.normalFontReplace) NativeTextureRegistry::Install(hModule);
        else Log::Write("[Hook] NativeTextureRegistry 已由[General] NormalFontReplace=0停用，跳過安裝（真正的vtable patch由GlyphHook.cpp的lazy retry驅動，該處也已同步改讀g_synthEnable）");

        // 原文/譯文熱鍵切換：hook sub_465370(DNameNode::GetText slot0)，固定
        // VA、載入當下即可安裝。此 hook 一律安裝，是否切換生效由 runtime 開關
        // [LocSwitch] LocSwitchEnable 控制（見 Config.h LocSwitchConfig::enable）。
        // 場景切換的 LOC 快取失效接在上面的 hook 一起觸發。
        LocHook::Install(hModule);

        // 按鈕/HUD CJK 文字亂碼修復：native 有 5 個呼叫端逐字呼叫 CRT
        // toupper()，其中 sub_463D10 會拿 CJK codepoint(>255) 當參數、超出
        // 定義域造成 UB 弄壞 bytes。改寫 MSVCR71.DLL 的 toupper IAT slot，
        // codepoint>0xFF 時原樣通過，一次修掉 5 個呼叫端。固定 import table
        // 位址，載入當下即可安裝。
        if (generalCfg.toUpperHook) ToUpperHook::Install(hModule);
        else Log::Write("[Hook] ToUpperHook 已由[General] ToUpperHook=0停用，跳過安裝");

        // CJK 翻譯文字雙重編碼亂碼修復（見 Utf8ReencodeFixHook.h）：sub_5A4850
        // 這個共用函式會把已是 UTF-8 的翻譯文字逐 byte 誤當 ANSI 重編碼。修法
        // 是重實作整個函式、加 UTF-8 序列偵測，命中則原樣通過，否則維持原本
        // 逐 codepoint 編碼（保留玩家鍵盤 ANSI 輸入 sub_55C3E0 情境不受影響）。
        // 固定 VA，載入當下即可安裝。
        if (generalCfg.utf8ReencodeFix) Utf8ReencodeFixHook::Install();
        else Log::Write("[Hook] Utf8ReencodeFixHook 已由[General] Utf8ReencodeFix=0停用，跳過安裝");

        // GetGlyph（字型槽 vtable+0x238）呼叫點的 inline patch，同時是字幕
        // compositor（SubtitleRender）lazy retry 的共用驅動點（GlyphHook.cpp
        // ::GlyphCheck()→SubtitleHook::EnsureRenderReady()）。glyphHook=0 時
        // CJK 合成與字幕全部失效，是總電源；NormalFontReplace 只決定 patch
        // 裝好後要不要合成 CJK 字（g_synthEnable），不影響字幕顯示。
        if (generalCfg.glyphHook) GlyphHook::Install(hModule);
        else Log::Write("[Hook] GlyphHook 已由[General] GlyphHook=0停用，跳過安裝（⚠️CJK glyph合成/字幕self-render/座標側錄全部一起關閉，畫面上CJK文字會回到native缺字空白，字幕功能也會失效）");

        // 報紙 IMG 文繞圖失效修復（見 NewsPaperImageWrapHook.h）：sub_559910
        // 內 3 處 sub_5585E0(0) 呼叫改傳目前行真實 Y 座標，只在報紙分類生效。
        // 固定 VA，載入當下即可安裝。
        if (hookToggle.newsPaperImageWrapHook) NewsPaperImageWrapHook::Install();
        else Log::Write("[Hook] NewsPaperImageWrapHook 已由[NewsPaper]停用，跳過安裝");

        // 報紙全篇疊字 bug 的 X 軸修法（見 NewsPaperSpaceAdvanceFix.h）：
        // sub_559910 的 case32(空白) 改成當場重查 GetGlyph(32)，不沿用函式
        // 進入時快取的過期指標，只在報紙分類生效。固定 VA，載入當下即可安裝。
        if (hookToggle.newsPaperSpaceAdvanceFix) NewsPaperSpaceAdvanceFix::Install();
        else Log::Write("[Hook] NewsPaperSpaceAdvanceFix 已由[NewsPaper]停用，跳過安裝");

        // 報紙標題疊字 bug 的 Y 軸修法（見 NewsPaperLineHeightFix.h）：
        // sub_559910 的兩個 v102(行高 running-max) 更新點改查我方探測過的
        // 正確 lineHeight，不沿用可能被污染的 var_998+0x11，只在報紙分類
        // 生效。固定 VA，載入當下即可安裝。
        if (hookToggle.newsPaperLineHeightFix) NewsPaperLineHeightFix::Install();
        else Log::Write("[Hook] NewsPaperLineHeightFix 已由[NewsPaper]停用，跳過安裝");

        // 報紙 <justify fill> 兩端對齊在 CJK 下把行尾文字撐飛（甚至蓋過圖片）
        // 的修法（見 NewsPaperJustifyFillFix.h）：報紙分類下跳過 sub_5586B0
        // (<justify fill> 撐開) 呼叫，等同 <justify left>；非報紙分類不受影響。
        // 固定 VA，載入當下即可安裝。
        if (hookToggle.newsPaperJustifyFillFix) NewsPaperJustifyFillFix::Install(hModule);
        else Log::Write("[Hook] NewsPaperJustifyFillFix 已由[NewsPaper]停用，跳過安裝");

        // 字幕功能總調度：讀 [Subtitle] BriefingSubtitleEnabled /
        // DialogueSubtitleEnabled / OnelinersSubtitleEnabled，依開關安裝
        // SubtitleBrief(+BriefSpeech) / SubtitleGate(+native字幕watchdog側錄)
        // / SubtitleDialogue(sub_6AACA0) / SubtitleOneliners(sub_6A4240＋
        // sub_6BACC0)。三個開關各自對應不同的語音播放函式（見Config.h宣告處
        // 註解）。SubtitleRender 唯一 EndScene compositor 的 lazy retry 由
        // GlyphHook.cpp 每幀呼叫 SubtitleHook::EnsureRenderReady() 驅動。
        SubtitleHook::Install(hModule);

        // 元素A：偽裝錯誤進入限制區時自繪 Trespassing/擅闖 或 Hostile Area/
        // 敵對區域（見TrespassHud.h）。訊號取自 native actor 擅闖旗標 bit，
        // 繪製共用 SubtitleRender 的唯一 EndScene compositor。讀 [Hud]
        // TrespassingEnabled。
        TrespassHud::Install(hModule);

        // 元素B：警衛/NPC 對玩家起疑時自繪 Suspicious/懷疑 或 Alerted/戰鬥
        // （見WarningHud.h）。訊號取自 native ZOSD 警覺條緩動值
        // [[[0x0082083C]+0xA4C]+0x18C]，門檻 75/50。繪製共用 SubtitleRender
        // 唯一 EndScene compositor。讀 [Hud] WarningEnabled。字串與元素A
        // 共用 warningString.txt（WarningStrings）。
        WarningHud::Install(hModule);

        // 小地圖佔位方框（見MinimapHud.h）：讀 [Hud] MinimapEnabled /
        // MinimapPos / MinimapSize，共用 SubtitleRender 唯一 EndScene
        // compositor。註冊在 TrespassHud 之後＝畫在其上層。引擎地圖本體
        // （CMapIconDraw）之後再接。
        MinimapHud::Install(hModule);

        // 方案1已作廢，TexDispatchHook改回停用，避免
        // 巨量[TexDispatchHook]log干擾候選函式診斷log的判讀。程式碼保留，之後若要重查TEX entry名稱對
        // 應關係可以直接取消註解。
        // TexDispatchHook::Install();

        Log::Write("[Hook] CJK glyph/材質hook：Stage C合成邏輯已接上");

        // Inventory 武器檢視面板 label:value 疊字修正＋商店/黑市購買選單
        // Price 欄位同款修正（LabelWidthFixShop.h）。固定 VA，載入當下即可
        // 安裝。疊字只在 CJK 被合成字型輸出時發生，故 NormalFontReplace=0 時
        // 沒有作用對象、不安裝。
        if (generalCfg.normalFontReplace)
        {
            LabelWidthFixInventory::Install(hModule);
            LabelWidthFixShop::Install(hModule);
        }
        else Log::Write("[Hook] LabelWidthFixInventory/LabelWidthFixShop 已由[General] NormalFontReplace=0停用，跳過安裝");

        Log::Write("[Hook] Init 完成");
    }

    void Shutdown()
    {
        Log::Write("[Hook] Shutdown");
    }
}
