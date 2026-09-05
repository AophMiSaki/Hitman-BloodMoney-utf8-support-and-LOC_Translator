#include "Config.h"
#include "Paths.h"
#include "Log.h"
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace Config
{
    // ASCII字元也查[FontCjk]這份設定；字幕/報紙各自獨立section。section
    // 名稱沿用"FontCjk"不改名，保留使用者既有ini的自訂設定。見Config.h
    // 頂部註解。
    static const char kSectionCjk[]       = "FontCjk";
    static const char kSectionSubtitle[]  = "FontSubtitle";
    // 報紙依<font N>分級——單一[FontNewspaper] section只設定font1(headline)，
    // font2~4從font1推算，見LoadNewspaperFont1~4()。
    static const char kSectionNewspaper[] = "FontNewspaper";
    // [General] section——必要功能總開關，不是診斷開關，見Config.h
    // GeneralConfig宣告處註解。
    static const char kSectionGeneral[]   = "General";
    static const char kDebugSection[]     = "Debug";
    static const char kLocSwitchSection[] = "LocSwitch";
    // SubtitleGate 三欄位(首選/次選/第三)字幕位置機制。
    static const char kSubtitleSection[]  = "Subtitle";
    // 報紙patch開關＋SoftBreakCjkInterval共用的section。
    static const char kSectionNewsPaper[] = "NewsPaper";
    // 小地圖/擅闖字/AI警告的必要功能開關與位置。
    static const char kSectionHud[]       = "Hud";

    static void WriteDefaultFontSection(const char* section, const char* iniPath, const char* defaultSize, BOOL* results)
    {
        results[0] = WritePrivateProfileStringA(section, "FontFace",   "Microsoft JhengHei", iniPath);
        results[1] = WritePrivateProfileStringA(section, "FontSize",   defaultSize,  iniPath);
        results[2] = WritePrivateProfileStringA(section, "FontWeight", "0",   iniPath);
        results[3] = WritePrivateProfileStringA(section, "FontWidth",  "0",   iniPath);
        results[4] = WritePrivateProfileStringA(section, "FontSpacing", "0",  iniPath);
        results[5] = WritePrivateProfileStringA(section, "FontYOffset", "0",  iniPath);
    }

    void EnsureDefaultIni(HMODULE hModule)
    {
        char iniPath[MAX_PATH];
        Paths::GetIniPath(hModule, iniPath, sizeof(iniPath));

        if (GetFileAttributesA(iniPath) != INVALID_FILE_ATTRIBUTES)
        {
            Log::Write("[Config] ini已存在，略過建立預設值：%s", iniPath);
            return;
        }

        BOOL wCjk[6]   = {};
        WriteDefaultFontSection(kSectionCjk,   iniPath, "16", wCjk);
        BOOL wSub[6] = {};
        WriteDefaultFontSection(kSectionSubtitle, iniPath, "16", wSub);
        // 報紙只寫font1的預設值（38，=下限值），font2~4不寫進ini，執行期
        // 從font1推算（見LoadNewspaperFont2~4()）。
        BOOL wNews[6] = {};
        WriteDefaultFontSection(kSectionNewspaper, iniPath, "38", wNews);

        // [General]：必要功能總開關，預設啟用（跟[Debug]底下預設關閉的純
        // 診斷開關性質不同）。
        BOOL wGenA = WritePrivateProfileStringA(kSectionGeneral, "NormalFontReplace", "1", iniPath);
        BOOL wGenB = WritePrivateProfileStringA(kSectionGeneral, "Utf8ReencodeFix", "1", iniPath);
        BOOL wGenC = WritePrivateProfileStringA(kSectionGeneral, "GlyphHook", "1", iniPath);
        BOOL wGenD = WritePrivateProfileStringA(kSectionGeneral, "ToUpperHook", "1", iniPath);
        BOOL wGenE = WritePrivateProfileStringA(kSectionGeneral, "ZipPathTrace", "1", iniPath);

        BOOL w7  = WritePrivateProfileStringA(kDebugSection, "GlyphDiagEnable", "0",   iniPath);
        BOOL w8  = WritePrivateProfileStringA(kDebugSection, "GlyphDiagCap",    "300", iniPath);
        BOOL w12 = WritePrivateProfileStringA(kDebugSection, "CJKPlaceholderEnable", "0", iniPath);
        BOOL w17 = WritePrivateProfileStringA(kDebugSection, "TexUploadDiagEnable", "0", iniPath);
        BOOL w20 = WritePrivateProfileStringA(kDebugSection, "LocSwitchDiagEnable", "0", iniPath);
        BOOL w21 = WritePrivateProfileStringA(kDebugSection, "ToUpperDiagEnable", "0", iniPath);
        BOOL w22 = WritePrivateProfileStringA(kDebugSection, "NativeTexRegDiagEnable", "0", iniPath);
        BOOL w23 = WritePrivateProfileStringA(kDebugSection, "DebugSubtitleDiagEnable", "0", iniPath);
        BOOL w25 = WritePrivateProfileStringA(kDebugSection, "LabelWidthFixDiagEnable", "0", iniPath);
        // DebugZipPathTrace：純log開關，預設0。見Config.h DebugConfig註解。
        BOOL w26 = WritePrivateProfileStringA(kDebugSection, "DebugZipPathTrace", "0", iniPath);
        BOOL w28 = WritePrivateProfileStringA(kDebugSection, "HudDiagEnable", "0", iniPath);
        // MinimapFloorProbe：小地圖選樓層定案用的每秒側錄開關，預設0
        // （見Config.h DebugConfig註解、MinimapHud.cpp FloorProbeTick）。
        BOOL w30 = WritePrivateProfileStringA(kDebugSection, "MinimapFloorProbe", "0", iniPath);
        // GlyphOverrunProbe：報紙 CJK 字圖合成溢位定位側錄，預設0
        // （見Config.h DebugConfig註解）。
        BOOL w31 = WritePrivateProfileStringA(kDebugSection, "GlyphOverrunProbe", "0", iniPath);

        // [Subtitle]：字幕self-render疊加位置（X,Y，0~100，0,0是左上角，見
        // Config.h SubtitlePlaceConfig註解）。SubtitlePlaceFirst(首選欄位，
        // SubtitleGate首選字幕+SubtitleBrief開場簡報字幕共用，預設50,80)、
        // SubtitlePlaceThird(第三顯示點，8態狀態機用，預設60,90)；次選沿用
        // native原位、無對應key。
        BOOL wSubPlaceFirst = WritePrivateProfileStringA(kSubtitleSection, "SubtitlePlaceFirst", "50,80", iniPath);
        BOOL wSubPlaceThird = WritePrivateProfileStringA(kSubtitleSection, "SubtitlePlaceThird", "60,90", iniPath);
        // 見Config.h LoadNpcHearRadius()註解，預設值是未經驗證的猜測（假設
        // 引擎座標單位接近公分，1000≈10公尺），需要使用者實機測試調整。
        // Dialogue跟Oneliners共用同一個門檻。
        BOOL wNpcHearRadius = WritePrivateProfileStringA(kSubtitleSection, "NpcHearRadius", "1000", iniPath);
        // 見Config.h LoadTvRadioHearRadius()註解，預設3000（>NpcHearRadius，
        // 廣播穿多個房間）。需使用者實機調整。
        BOOL wTvRadioHearRadius = WritePrivateProfileStringA(kSubtitleSection, "TvRadioHearRadius", "3000", iniPath);
        // 見Config.h LoadBriefingSubtitleEnabled()註解，同時控制語音訊號
        // 側錄跟字幕文字顯示，1=啟用。對應「簡報旁白」播放函式。
        BOOL wBriefSubEnable = WritePrivateProfileStringA(kSubtitleSection, "BriefingSubtitleEnabled", "1", iniPath);
        // 見Config.h LoadDialogueSubtitleEnabled()/
        // LoadOnelinersSubtitleEnabled()註解，各自對應Dialogue(sub_6AACA0)/
        // Oneliners(sub_6A4240＋sub_6BACC0)播放函式，1=啟用。
        BOOL wDialogueSubEnable = WritePrivateProfileStringA(kSubtitleSection, "DialogueSubtitleEnabled", "1", iniPath);
        BOOL wOnelinersSubEnable = WritePrivateProfileStringA(kSubtitleSection, "OnelinersSubtitleEnabled", "1", iniPath);
        // 對講機（sub_6C0220）傳輸字幕補完，1=啟用。
        BOOL wWalkieSubEnable = WritePrivateProfileStringA(kSubtitleSection, "WalkieSubtitleEnabled", "1", iniPath);
        // 電視新聞／收音機廣播（sub_4C5E10）字幕補完，1=啟用。
        BOOL wTvRadioSubEnable = WritePrivateProfileStringA(kSubtitleSection, "TvRadioSubtitleEnabled", "1", iniPath);

        // [LocSwitch]：原文/譯文熱鍵切換，見bloodmoney_utf8_support.md第7節
        // 第2點。Hotkey預設122=VK_F11。原文/譯文切換由LocSwitchEnable的
        // runtime值決定（見Config.h LocSwitchConfig::enable註解）。
        BOOL wL1 = WritePrivateProfileStringA(kLocSwitchSection, "LocSwitchEnable", "1", iniPath);
        BOOL wL2 = WritePrivateProfileStringA(kLocSwitchSection, "Hotkey", "122", iniPath);

        // 報紙patch開關寫在[NewsPaper]（見Config.h HookToggleConfig註解）。
        static const char* const kNewsPaperFixKeys[] = {
            "NewsPaperImageWrapFix", "NewsPaperSpaceAdvanceFix", "NewsPaperLineHeightFix", "NewsPaperJustifyFillFix", "NewsPaperSoftBreakFix",
        };
        BOOL wNewsPaperToggle = TRUE;
        for (const char* key : kNewsPaperFixKeys)
            wNewsPaperToggle &= WritePrivateProfileStringA(kSectionNewsPaper, key, "1", iniPath);

        // 見Config.h LoadNewsSoftBreakInterval()註解。
        BOOL wNewsSoftBreak = WritePrivateProfileStringA(kSectionNewsPaper, "SoftBreakCjkInterval", "2", iniPath);

        // [Hud]：元素A擅闖/敵對區域自繪字（見TrespassHud.h）＋小地圖佔位
        // 方框（見MinimapHud.h）。座標皆為螢幕百分比。MinimapSize為10x10px
        // 基礎點的倍數（邊長=值*10px）。
        BOOL wHudA = WritePrivateProfileStringA(kSectionHud, "TrespassingEnabled", "1", iniPath);
        BOOL wHudB = WritePrivateProfileStringA(kSectionHud, "TrespassPos", "16,85", iniPath);
        BOOL wHudF = WritePrivateProfileStringA(kSectionHud, "WarningEnabled", "1", iniPath);
        BOOL wHudG = WritePrivateProfileStringA(kSectionHud, "WarningPos", "50,5", iniPath);
        BOOL wHudC = WritePrivateProfileStringA(kSectionHud, "MinimapEnabled", "1", iniPath);
        BOOL wHudD = WritePrivateProfileStringA(kSectionHud, "MinimapPos", "5.0,60.0", iniPath);
        BOOL wHudE = WritePrivateProfileStringA(kSectionHud, "MinimapSize", "15", iniPath);
        // 小地圖顯示範圍（world單位）：>0＝以47為中心的局部視窗、0＝整層看全貌。見MinimapHud.h。
        BOOL wHudJ = WritePrivateProfileStringA(kSectionHud, "MinimapZoom", "4000", iniPath);

        Log::Write("[Config] 建立預設ini：%s（FontCjk=%d%d%d%d%d%d FontSubtitle=%d%d%d%d%d%d "
            "FontNewspaper(font1)=%d%d%d%d%d%d "
            "General=%d%d%d%d Debug=%d%d%d%d%d%d%d%d%d%d%d%d%d Subtitle=%d%d%d%d%d%d%d LocSwitch=%d%d NewsPaperToggle=%d Hud=%d%d%d%d%d%d%d%d, err=%lu）",
            iniPath,
            wCjk[0], wCjk[1], wCjk[2], wCjk[3], wCjk[4], wCjk[5],
            wSub[0], wSub[1], wSub[2], wSub[3], wSub[4], wSub[5],
            wNews[0], wNews[1], wNews[2], wNews[3], wNews[4], wNews[5],
            wGenA, wGenB, wGenC, wGenD,
            w7, w8, w12, w17, w20, w21, w22, w23, w25, w26, w28, w30, w31,
            wSubPlaceFirst, wSubPlaceThird, wBriefSubEnable, wDialogueSubEnable, wOnelinersSubEnable, wWalkieSubEnable, wTvRadioSubEnable,
            wL1, wL2, wNewsPaperToggle, wHudA, wHudB, wHudF, wHudG, wHudC, wHudD, wHudE, wHudJ, GetLastError());
    }

    // [FontCjk]/[FontSubtitle]/[FontNewspaper]三個section共用同一套讀取/
    // log/fallback行為，只由下面Init()呼叫一次，見本檔案「全域設定快取」
    // 節說明。
    static FontConfig LoadSectionImpl(HMODULE hModule, const char* section, int defaultSize = 16)
    {
        char iniPath[MAX_PATH];
        Paths::GetIniPath(hModule, iniPath, sizeof(iniPath));

        FontConfig cfg = {};
        bool iniExists = GetFileAttributesA(iniPath) != INVALID_FILE_ATTRIBUTES;
        if (!iniExists)
            Log::Write("[Config] 讀取失敗：ini不存在（%s），[%s]全部使用內建預設值", iniPath, section);

        GetPrivateProfileStringA(section, "FontFace", "Microsoft JhengHei", cfg.face, sizeof(cfg.face), iniPath);
        cfg.size    = GetPrivateProfileIntA(section, "FontSize", defaultSize, iniPath);
        cfg.weight  = GetPrivateProfileIntA(section, "FontWeight", 0, iniPath);
        cfg.width   = GetPrivateProfileIntA(section, "FontWidth", 0, iniPath);
        cfg.spacing = GetPrivateProfileIntA(section, "FontSpacing", 0, iniPath);
        cfg.yOffset = GetPrivateProfileIntA(section, "FontYOffset", 0, iniPath);

        Log::Write("[Config] 讀取%s[%s]：FontFace=%s FontSize=%d FontWeight=%d FontWidth=%d FontSpacing=%d FontYOffset=%d",
            iniExists ? "成功" : "使用預設值", section, cfg.face, cfg.size, cfg.weight, cfg.width, cfg.spacing, cfg.yOffset);

        return cfg;
    }

    // font2~4以font1為基準分別減14/28/30px，下限clamp用各自的
    // NewspaperMinRenderSizeFor()（同一份常數也是ini預設值）——font1設很小
    // 時font4不會掉到不合理的小字。
    static FontConfig DeriveNewspaperLevelImpl(const FontConfig& font1, int level, int stepDown, int minSize)
    {
        FontConfig cfg = font1;
        cfg.size -= stepDown;
        if (cfg.size < minSize) cfg.size = minSize;
        Log::Write("[Config] 報紙font%d字級由font1(%d)推算：%d（-%dpx，下限%dpx，其餘face/weight/width/spacing/yOffset沿用font1）",
                   level, font1.size, cfg.size, stepDown, minSize);
        return cfg;
    }

    // font1的下限跟ini預設值統一用NewspaperMinRenderSizeFor()（見Config.h
    // 註解），38px。
    static FontConfig LoadNewspaperFont1Impl(HMODULE hModule)
    {
        const int minSize = NewspaperMinRenderSizeFor(FontCategory::NewspaperFont1);
        FontConfig cfg = LoadSectionImpl(hModule, kSectionNewspaper, minSize);
        if (cfg.size < minSize)
        {
            Log::Write("[Config] 報紙font1 FontSize=%d低於下限%dpx，clamp為%d",
                       cfg.size, minSize, minSize);
            cfg.size = minSize;
        }
        return cfg;
    }

    static GeneralConfig LoadGeneralImpl(HMODULE hModule)
    {
        char iniPath[MAX_PATH];
        Paths::GetIniPath(hModule, iniPath, sizeof(iniPath));

        GeneralConfig cfg = {};
        cfg.normalFontReplace = GetPrivateProfileIntA(kSectionGeneral, "NormalFontReplace", 1, iniPath) != 0;
        cfg.utf8ReencodeFix   = GetPrivateProfileIntA(kSectionGeneral, "Utf8ReencodeFix", 1, iniPath) != 0;
        cfg.glyphHook         = GetPrivateProfileIntA(kSectionGeneral, "GlyphHook", 1, iniPath) != 0;
        cfg.toUpperHook       = GetPrivateProfileIntA(kSectionGeneral, "ToUpperHook", 1, iniPath) != 0;
        cfg.zipPathTrace      = GetPrivateProfileIntA(kSectionGeneral, "ZipPathTrace", 1, iniPath) != 0;
        Log::Write("[Config] 讀取[General]：NormalFontReplace=%d Utf8ReencodeFix=%d GlyphHook=%d ToUpperHook=%d ZipPathTrace=%d", cfg.normalFontReplace, cfg.utf8ReencodeFix, cfg.glyphHook, cfg.toUpperHook, cfg.zipPathTrace);
        return cfg;
    }

    // SubtitlePlaceFirst/SubtitlePlaceThird共用同一套讀取/clamp/log邏輯，
    // 只有ini key名稱跟預設值不同，抽成共用static helper供下面兩個函式呼叫。
    static SubtitlePlaceConfig LoadPlaceConfigImpl(HMODULE hModule, const char* key, float defX, float defY)
    {
        char iniPath[MAX_PATH];
        Paths::GetIniPath(hModule, iniPath, sizeof(iniPath));

        char defStr[32];
        _snprintf_s(defStr, _TRUNCATE, "%g,%g", defX, defY);

        char raw[32];
        GetPrivateProfileStringA(kSubtitleSection, key, defStr, raw, sizeof(raw), iniPath);

        float x = defX, y = defY;
        if (sscanf_s(raw, "%f,%f", &x, &y) != 2)
        {
            Log::Write("[Config] [Subtitle] %s格式錯誤(\"%s\")，使用預設值%g,%g", key, raw, defX, defY);
            x = defX;
            y = defY;
        }
        if (x < 0.0f) x = 0.0f;
        if (x > 100.0f) x = 100.0f;
        if (y < 0.0f) y = 0.0f;
        if (y > 100.0f) y = 100.0f;

        SubtitlePlaceConfig cfg = {};
        cfg.xPercent = x;
        cfg.yPercent = y;
        Log::Write("[Config] 讀取[Subtitle] %s=%.1f,%.1f", key, cfg.xPercent, cfg.yPercent);
        return cfg;
    }

    // 見Config.h宣告處註解。GetPrivateProfileIntA不支援小數，改用
    // GetPrivateProfileStringA+atof（跟LoadPlaceConfigImpl的X,Y字串解析
    // 同一套讀取慣例）。
    static float LoadNpcHearRadiusImpl(HMODULE hModule)
    {
        char iniPath[MAX_PATH];
        Paths::GetIniPath(hModule, iniPath, sizeof(iniPath));

        char buf[64] = {};
        GetPrivateProfileStringA(kSubtitleSection, "NpcHearRadius", "1000", buf, sizeof(buf), iniPath);
        float radius = (float)atof(buf);
        if (radius < 0.0f) radius = 0.0f;

        Log::Write("[Config] 讀取[Subtitle] NpcHearRadius=%.1f", radius);
        return radius;
    }

    // 讀取/clamp 慣例同 LoadNpcHearRadiusImpl，只是不同 key／預設值。
    static float LoadTvRadioHearRadiusImpl(HMODULE hModule)
    {
        char iniPath[MAX_PATH];
        Paths::GetIniPath(hModule, iniPath, sizeof(iniPath));

        char buf[64] = {};
        GetPrivateProfileStringA(kSubtitleSection, "TvRadioHearRadius", "3000", buf, sizeof(buf), iniPath);
        float radius = (float)atof(buf);
        if (radius < 0.0f) radius = 0.0f;

        Log::Write("[Config] 讀取[Subtitle] TvRadioHearRadius=%.1f", radius);
        return radius;
    }

    static DebugConfig LoadDebugImpl(HMODULE hModule)
    {
        char iniPath[MAX_PATH];
        Paths::GetIniPath(hModule, iniPath, sizeof(iniPath));

        DebugConfig cfg = {};
        cfg.glyphDiagEnable = GetPrivateProfileIntA(kDebugSection, "GlyphDiagEnable", 0, iniPath) != 0;
        cfg.glyphDiagCap    = GetPrivateProfileIntA(kDebugSection, "GlyphDiagCap", 300, iniPath);
        cfg.cjkPlaceholderEnable = GetPrivateProfileIntA(kDebugSection, "CJKPlaceholderEnable", 0, iniPath) != 0;
        cfg.texUploadDiagEnable     = GetPrivateProfileIntA(kDebugSection, "TexUploadDiagEnable", 0, iniPath) != 0;
        cfg.locSwitchDiagEnable     = GetPrivateProfileIntA(kDebugSection, "LocSwitchDiagEnable", 0, iniPath) != 0;
        cfg.toUpperDiagEnable       = GetPrivateProfileIntA(kDebugSection, "ToUpperDiagEnable", 0, iniPath) != 0;
        cfg.nativeTexRegDiagEnable      = GetPrivateProfileIntA(kDebugSection, "NativeTexRegDiagEnable", 0, iniPath) != 0;
        cfg.subtitleDiagEnable          = GetPrivateProfileIntA(kDebugSection, "DebugSubtitleDiagEnable", 0, iniPath) != 0;
        cfg.labelWidthFixDiagEnable     = GetPrivateProfileIntA(kDebugSection, "LabelWidthFixDiagEnable", 0, iniPath) != 0;
        // debugZipPathTrace：純log開關，預設關閉（見Config.h DebugConfig
        // 註解）。
        cfg.debugZipPathTrace          = GetPrivateProfileIntA(kDebugSection, "DebugZipPathTrace", 0, iniPath) != 0;
        cfg.hudDiagEnable              = GetPrivateProfileIntA(kDebugSection, "HudDiagEnable", 0, iniPath) != 0;
        // minimapFloorProbe：小地圖選樓層定案用的每秒側錄開關，預設關閉
        // （見Config.h DebugConfig註解）。
        cfg.minimapFloorProbe         = GetPrivateProfileIntA(kDebugSection, "MinimapFloorProbe", 0, iniPath) != 0;
        // glyphOverrunProbe：報紙 CJK 字圖合成溢位定位側錄，預設關閉
        // （見Config.h DebugConfig註解）。
        cfg.glyphOverrunProbe         = GetPrivateProfileIntA(kDebugSection, "GlyphOverrunProbe", 0, iniPath) != 0;

        Log::Write("[Config] 讀取[Debug]：GlyphDiagEnable=%d GlyphDiagCap=%d CJKPlaceholderEnable=%d TexUploadDiagEnable=%d LocSwitchDiagEnable=%d ToUpperDiagEnable=%d NativeTexRegDiagEnable=%d DebugSubtitleDiagEnable=%d LabelWidthFixDiagEnable=%d DebugZipPathTrace=%d "
            "HudDiagEnable=%d MinimapFloorProbe=%d GlyphOverrunProbe=%d",
            cfg.glyphDiagEnable, cfg.glyphDiagCap, cfg.cjkPlaceholderEnable, cfg.texUploadDiagEnable, cfg.locSwitchDiagEnable, cfg.toUpperDiagEnable, cfg.nativeTexRegDiagEnable, cfg.subtitleDiagEnable, cfg.labelWidthFixDiagEnable, cfg.debugZipPathTrace,
            cfg.hudDiagEnable, cfg.minimapFloorProbe, cfg.glyphOverrunProbe);

        return cfg;
    }

    static LocSwitchConfig LoadLocSwitchImpl(HMODULE hModule)
    {
        char iniPath[MAX_PATH];
        Paths::GetIniPath(hModule, iniPath, sizeof(iniPath));

        LocSwitchConfig cfg = {};
        cfg.enable = GetPrivateProfileIntA(kLocSwitchSection, "LocSwitchEnable", 1, iniPath) != 0;
        cfg.hotkeyVK = GetPrivateProfileIntA(kLocSwitchSection, "Hotkey", 122 /* VK_F11 */, iniPath);

        Log::Write("[Config] 讀取[LocSwitch]：LocSwitchEnable=%d Hotkey=%d(0x%02X)",
            cfg.enable, cfg.hotkeyVK, cfg.hotkeyVK);

        return cfg;
    }

    static HookToggleConfig LoadHookToggleImpl(HMODULE hModule)
    {
        char iniPath[MAX_PATH];
        Paths::GetIniPath(hModule, iniPath, sizeof(iniPath));

        HookToggleConfig cfg = {};

        // 報紙patch開關，實際ini key在[NewsPaper]（見Config.h
        // HookToggleConfig註解）。
        cfg.newsPaperImageWrapHook    = GetPrivateProfileIntA(kSectionNewsPaper, "NewsPaperImageWrapFix", 1, iniPath) != 0;
        cfg.newsPaperSpaceAdvanceFix  = GetPrivateProfileIntA(kSectionNewsPaper, "NewsPaperSpaceAdvanceFix", 1, iniPath) != 0;
        cfg.newsPaperLineHeightFix    = GetPrivateProfileIntA(kSectionNewsPaper, "NewsPaperLineHeightFix", 1, iniPath) != 0;
        cfg.newsPaperJustifyFillFix   = GetPrivateProfileIntA(kSectionNewsPaper, "NewsPaperJustifyFillFix", 1, iniPath) != 0;
        cfg.newsPaperSoftBreakFix     = GetPrivateProfileIntA(kSectionNewsPaper, "NewsPaperSoftBreakFix", 1, iniPath) != 0;

        Log::Write("[Config] 讀取[NewsPaper]：NewsPaperImageWrapFix=%d NewsPaperSpaceAdvanceFix=%d NewsPaperLineHeightFix=%d NewsPaperJustifyFillFix=%d NewsPaperSoftBreakFix=%d",
            cfg.newsPaperImageWrapHook, cfg.newsPaperSpaceAdvanceFix, cfg.newsPaperLineHeightFix, cfg.newsPaperJustifyFillFix, cfg.newsPaperSoftBreakFix);

        return cfg;
    }

    // 見Config.h LoadBriefingSubtitleEnabled()宣告處註解。
    static bool LoadBriefingSubtitleEnabledImpl(HMODULE hModule)
    {
        char iniPath[MAX_PATH];
        Paths::GetIniPath(hModule, iniPath, sizeof(iniPath));

        bool enable = GetPrivateProfileIntA(kSubtitleSection, "BriefingSubtitleEnabled", 1, iniPath) != 0;
        Log::Write("[Config] 讀取[Subtitle] BriefingSubtitleEnabled=%d", enable);
        return enable;
    }

    // 見Config.h宣告處註解。各自對應Dialogue(sub_6AACA0)/
    // Oneliners(sub_6A4240＋sub_6BACC0)播放函式。
    static bool LoadDialogueSubtitleEnabledImpl(HMODULE hModule)
    {
        char iniPath[MAX_PATH];
        Paths::GetIniPath(hModule, iniPath, sizeof(iniPath));

        bool enable = GetPrivateProfileIntA(kSubtitleSection, "DialogueSubtitleEnabled", 1, iniPath) != 0;
        Log::Write("[Config] 讀取[Subtitle] DialogueSubtitleEnabled=%d", enable);
        return enable;
    }

    static bool LoadOnelinersSubtitleEnabledImpl(HMODULE hModule)
    {
        char iniPath[MAX_PATH];
        Paths::GetIniPath(hModule, iniPath, sizeof(iniPath));

        bool enable = GetPrivateProfileIntA(kSubtitleSection, "OnelinersSubtitleEnabled", 1, iniPath) != 0;
        Log::Write("[Config] 讀取[Subtitle] OnelinersSubtitleEnabled=%d", enable);
        return enable;
    }

    // 見Config.h宣告處註解。對應對講機傳輸播放函式sub_6C0220。
    static bool LoadWalkieSubtitleEnabledImpl(HMODULE hModule)
    {
        char iniPath[MAX_PATH];
        Paths::GetIniPath(hModule, iniPath, sizeof(iniPath));

        bool enable = GetPrivateProfileIntA(kSubtitleSection, "WalkieSubtitleEnabled", 1, iniPath) != 0;
        Log::Write("[Config] 讀取[Subtitle] WalkieSubtitleEnabled=%d", enable);
        return enable;
    }

    // 見Config.h宣告處註解。對應電視新聞／收音機廣播播放路徑sub_4C5E10。
    static bool LoadTvRadioSubtitleEnabledImpl(HMODULE hModule)
    {
        char iniPath[MAX_PATH];
        Paths::GetIniPath(hModule, iniPath, sizeof(iniPath));

        bool enable = GetPrivateProfileIntA(kSubtitleSection, "TvRadioSubtitleEnabled", 1, iniPath) != 0;
        Log::Write("[Config] 讀取[Subtitle] TvRadioSubtitleEnabled=%d", enable);
        return enable;
    }

    // 見Config.h LoadNewsSoftBreakInterval()宣告處註解。
    static int LoadNewsSoftBreakIntervalImpl(HMODULE hModule)
    {
        char iniPath[MAX_PATH];
        Paths::GetIniPath(hModule, iniPath, sizeof(iniPath));

        int v = GetPrivateProfileIntA(kSectionNewsPaper, "SoftBreakCjkInterval", 2, iniPath);
        if (v < 0) v = 0;
        Log::Write("[Config] 讀取[NewsPaper] SoftBreakCjkInterval=%d", v);
        return v;
    }

    // "X,Y"百分比字串解析＋clamp 0~100，格式錯誤退回 defX,defY 並記log。
    // TrespassPos/MinimapPos 共用（key在[Hud]、不在[Subtitle]，故沒沿用
    // LoadPlaceConfigImpl）。
    static void ParseHudPos(const char* raw, const char* key, float defX, float defY, float& outX, float& outY)
    {
        float x = defX, y = defY;
        if (sscanf_s(raw, "%f,%f", &x, &y) != 2)
        {
            Log::Write("[Config] [Hud] %s格式錯誤(\"%s\")，使用預設值%g,%g", key, raw, defX, defY);
            x = defX;
            y = defY;
        }
        if (x < 0.0f) x = 0.0f;
        if (x > 100.0f) x = 100.0f;
        if (y < 0.0f) y = 0.0f;
        if (y > 100.0f) y = 100.0f;
        outX = x;
        outY = y;
    }

    // MinimapSize 嚴格解析：只接受純十進位整數字串、範圍0~100。非數字、含
    // 小數點/正負號、位數>3（最大有效值"100"）、超過100一律無效並記log、
    // 退回預設15（不clamp）。0 視為有效但邊長為0＝不繪製。
    static int ParseMinimapSize(const char* raw)
    {
        const int kDefault = 15;
        if (!raw) return kDefault;

        const char* s = raw;
        while (*s == ' ' || *s == '\t') s++;
        const char* e = s + strlen(s);
        while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) e--;

        size_t len = (size_t)(e - s);
        if (len == 0)
        {
            Log::Write("[Config] [Hud] MinimapSize為空，使用預設值%d", kDefault);
            return kDefault;
        }
        if (len > 3)
        {
            Log::Write("[Config] [Hud] MinimapSize=\"%s\"位數過長(>3)，使用預設值%d", raw, kDefault);
            return kDefault;
        }
        for (const char* p = s; p < e; p++)
        {
            if (*p < '0' || *p > '9')
            {
                Log::Write("[Config] [Hud] MinimapSize=\"%s\"含非數字字元，使用預設值%d", raw, kDefault);
                return kDefault;
            }
        }
        int v = 0;
        for (const char* p = s; p < e; p++) v = v * 10 + (*p - '0');

        if (v > 100)
        {
            Log::Write("[Config] [Hud] MinimapSize=%d超過上限100，使用預設值%d", v, kDefault);
            return kDefault;
        }
        if (v == 0)
            Log::Write("[Config] [Hud] MinimapSize=0：小地圖邊長為0，等同不繪製");
        return v;
    }

    // 見Config.h LoadHud()宣告處註解。
    static HudConfig LoadHudImpl(HMODULE hModule)
    {
        char iniPath[MAX_PATH];
        Paths::GetIniPath(hModule, iniPath, sizeof(iniPath));

        HudConfig cfg = {};
        cfg.trespassingEnabled = GetPrivateProfileIntA(kSectionHud, "TrespassingEnabled", 1, iniPath) != 0;

        char raw[32];
        GetPrivateProfileStringA(kSectionHud, "TrespassPos", "16,85", raw, sizeof(raw), iniPath);
        ParseHudPos(raw, "TrespassPos", 16.0f, 85.0f, cfg.trespassPosX, cfg.trespassPosY);

        cfg.warningEnabled = GetPrivateProfileIntA(kSectionHud, "WarningEnabled", 1, iniPath) != 0;

        GetPrivateProfileStringA(kSectionHud, "WarningPos", "50,5", raw, sizeof(raw), iniPath);
        ParseHudPos(raw, "WarningPos", 50.0f, 5.0f, cfg.warningPosX, cfg.warningPosY);

        cfg.minimapEnabled = GetPrivateProfileIntA(kSectionHud, "MinimapEnabled", 1, iniPath) != 0;

        GetPrivateProfileStringA(kSectionHud, "MinimapPos", "5.0,60.0", raw, sizeof(raw), iniPath);
        ParseHudPos(raw, "MinimapPos", 5.0f, 60.0f, cfg.minimapPosX, cfg.minimapPosY);

        GetPrivateProfileStringA(kSectionHud, "MinimapSize", "15", raw, sizeof(raw), iniPath);
        cfg.minimapSizeMul = ParseMinimapSize(raw);

        GetPrivateProfileStringA(kSectionHud, "MinimapZoom", "4000", raw, sizeof(raw), iniPath);
        cfg.minimapZoom = (float)atof(raw);
        if (cfg.minimapZoom < 0.0f)
        {
            Log::Write("[Config] [Hud] MinimapZoom=\"%s\" 無效（負值），退回0（顯示整層）", raw);
            cfg.minimapZoom = 0.0f;
        }

        Log::Write("[Config] 讀取[Hud]：TrespassingEnabled=%d TrespassPos=%.1f,%.1f WarningEnabled=%d WarningPos=%.1f,%.1f MinimapEnabled=%d MinimapPos=%.1f,%.1f MinimapSize=%d(=%dpx) MinimapZoom=%.1f",
            cfg.trespassingEnabled, cfg.trespassPosX, cfg.trespassPosY,
            cfg.warningEnabled, cfg.warningPosX, cfg.warningPosY,
            cfg.minimapEnabled, cfg.minimapPosX, cfg.minimapPosY, cfg.minimapSizeMul, cfg.minimapSizeMul * 10,
            cfg.minimapZoom);
        return cfg;
    }

    // ================= 全域設定快取 =================
    // ini在遊戲執行期間不會變。Init()在Hook::Init()一開始（EnsureDefaultIni()
    // 之後）呼叫一次，把所有section讀進下面這些全域變數並各自印一次log；下面
    // 公開的LoadXxx()全部單純回傳快取，不重新讀檔/印log。呼叫Init()之前不可
    // 呼叫任何LoadXxx()（會回傳未初始化的空值）——全部呼叫端都在Hook::Init()
    // 流程之後才會用到，見Hook.cpp。
    static FontConfig g_fontCjk;
    static FontConfig g_fontSubtitle;
    static FontConfig g_fontNews1;
    static FontConfig g_fontNews2;
    static FontConfig g_fontNews3;
    static FontConfig g_fontNews4;
    static GeneralConfig g_general;
    static DebugConfig g_debug;
    static LocSwitchConfig g_locSwitch;
    static HookToggleConfig g_hookToggle;
    static SubtitlePlaceConfig g_subtitlePlaceFirst;
    static SubtitlePlaceConfig g_subtitlePlaceThird;
    static float g_npcHearRadius;
    static float g_tvRadioHearRadius;
    static bool g_briefingSubtitleEnabled;
    static bool g_dialogueSubtitleEnabled;
    static bool g_onelinersSubtitleEnabled;
    static bool g_walkieSubtitleEnabled;
    static bool g_tvRadioSubtitleEnabled;
    static int g_newsSoftBreakInterval;
    static HudConfig g_hud;

    void Init(HMODULE hModule)
    {
        g_fontCjk      = LoadSectionImpl(hModule, kSectionCjk);
        g_fontSubtitle = LoadSectionImpl(hModule, kSectionSubtitle);
        g_fontNews1 = LoadNewspaperFont1Impl(hModule);
        g_fontNews2 = DeriveNewspaperLevelImpl(g_fontNews1, 2, 14, NewspaperMinRenderSizeFor(FontCategory::NewspaperFont2));
        g_fontNews3 = DeriveNewspaperLevelImpl(g_fontNews1, 3, 28, NewspaperMinRenderSizeFor(FontCategory::NewspaperFont3));
        g_fontNews4 = DeriveNewspaperLevelImpl(g_fontNews1, 4, 30, NewspaperMinRenderSizeFor(FontCategory::NewspaperFont4));

        g_general   = LoadGeneralImpl(hModule);
        g_debug     = LoadDebugImpl(hModule);
        g_locSwitch = LoadLocSwitchImpl(hModule);
        g_hookToggle = LoadHookToggleImpl(hModule);

        g_subtitlePlaceFirst = LoadPlaceConfigImpl(hModule, "SubtitlePlaceFirst", 50.0f, 80.0f);
        g_subtitlePlaceThird = LoadPlaceConfigImpl(hModule, "SubtitlePlaceThird", 60.0f, 90.0f);
        g_npcHearRadius = LoadNpcHearRadiusImpl(hModule);
        g_tvRadioHearRadius = LoadTvRadioHearRadiusImpl(hModule);
        g_briefingSubtitleEnabled  = LoadBriefingSubtitleEnabledImpl(hModule);
        g_dialogueSubtitleEnabled  = LoadDialogueSubtitleEnabledImpl(hModule);
        g_onelinersSubtitleEnabled = LoadOnelinersSubtitleEnabledImpl(hModule);
        g_walkieSubtitleEnabled    = LoadWalkieSubtitleEnabledImpl(hModule);
        g_tvRadioSubtitleEnabled   = LoadTvRadioSubtitleEnabledImpl(hModule);
        g_newsSoftBreakInterval    = LoadNewsSoftBreakIntervalImpl(hModule);
        g_hud                      = LoadHudImpl(hModule);
    }

    // ---- 以下全部回傳全域快取，hModule參數保留只為了不動全部呼叫端簽章 ----
    FontConfig Load(HMODULE) { return g_fontCjk; }
    FontConfig LoadSubtitle(HMODULE) { return g_fontSubtitle; }
    FontConfig LoadNewspaperFont1(HMODULE) { return g_fontNews1; }
    FontConfig LoadNewspaperFont2(HMODULE) { return g_fontNews2; }
    FontConfig LoadNewspaperFont3(HMODULE) { return g_fontNews3; }
    FontConfig LoadNewspaperFont4(HMODULE) { return g_fontNews4; }

    FontConfig LoadForCategory(HMODULE, FontCategory category)
    {
        switch (category)
        {
        case FontCategory::Subtitle:       return g_fontSubtitle;
        case FontCategory::NewspaperFont1: return g_fontNews1;
        case FontCategory::NewspaperFont2: return g_fontNews2;
        case FontCategory::NewspaperFont3: return g_fontNews3;
        case FontCategory::NewspaperFont4: return g_fontNews4;
        case FontCategory::General:
        default:                            return g_fontCjk;
        }
    }

    GeneralConfig LoadGeneral(HMODULE) { return g_general; }
    DebugConfig LoadDebug(HMODULE) { return g_debug; }
    LocSwitchConfig LoadLocSwitch(HMODULE) { return g_locSwitch; }
    HookToggleConfig LoadHookToggle(HMODULE) { return g_hookToggle; }
    SubtitlePlaceConfig LoadSubtitlePlaceFirst(HMODULE) { return g_subtitlePlaceFirst; }
    SubtitlePlaceConfig LoadSubtitlePlaceThird(HMODULE) { return g_subtitlePlaceThird; }
    float LoadNpcHearRadius(HMODULE) { return g_npcHearRadius; }
    float LoadTvRadioHearRadius(HMODULE) { return g_tvRadioHearRadius; }
    bool LoadBriefingSubtitleEnabled(HMODULE) { return g_briefingSubtitleEnabled; }
    bool LoadDialogueSubtitleEnabled(HMODULE) { return g_dialogueSubtitleEnabled; }
    bool LoadOnelinersSubtitleEnabled(HMODULE) { return g_onelinersSubtitleEnabled; }
    bool LoadWalkieSubtitleEnabled(HMODULE) { return g_walkieSubtitleEnabled; }
    bool LoadTvRadioSubtitleEnabled(HMODULE) { return g_tvRadioSubtitleEnabled; }
    int LoadNewsSoftBreakInterval(HMODULE) { return g_newsSoftBreakInterval; }
    HudConfig LoadHud(HMODULE) { return g_hud; }
}
