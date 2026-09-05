#include "GlyphHook.h"
#include "Log.h"
#include "Config.h"
#include "CJKRange.h"
#include "GlyphAtlas.h"
#include "NativeTextureRegistry.h"
#include "SubtitleHook.h"
#include "TrespassHud.h"
#include "WarningHud.h"
#include "MinimapHud.h"
#include "ZipPathTrace.h"
#include "FontCategory.h"
#include "LocHook.h"
#include <unordered_map>
#include <array>
#include <cstring>
#include <cstdio>

namespace GlyphHook
{
    static const DWORD kVA_CallSite = 0x0055A0A0;  // `call dword ptr [eax+238h]`，6 bytes: FF 90 38 02 00 00
    // 回跳位址0x55A0A6（=kVA_CallSite+6）寫死在GlyphDetour的inline asm裡
    // （MASM inline asm無法乾淨引用C++ static const symbol的值）。

    // 另兩個vtable+0x238呼叫點：push的charcode是寫死常數0x20（空白），用途是查
    // 「空白字元寬度」給斷行演算法用；vtable在edx（不是eax），指令一樣是
    // 6 bytes: FF 92 38 02 00 00。SpaceCheck借這兩點在每次<font N>切換時探測報紙字級。
    static const DWORD kVA_CallSiteSpaceA = 0x00559A21;  // 回跳位址0x559A27
    static const DWORD kVA_CallSiteSpaceB = 0x00559AFC;  // 回跳位址0x559B02

    // 0x550BE4：sub_5509D0（結構與sub_559910幾乎一樣的另一個獨立ZSimpleHTML排版
    // 函式，主選單實際使用）裡字元驅動的GetGlyph呼叫。暫存器結構同0x55A0A0
    // （eax=vtable/ecx=this），只差charcode在ebp（不是edi）。
    static const DWORD kVA_CallSiteMenu = 0x00550BE4;  // 回跳位址0x550BEA

    // 0x5555C9：sub_555590（字串寬度量測輔助函式）裡的GetGlyph呼叫。暫存器結構
    // 不同——vtable在ebp、charcode在eax（push前先call sub_4390C0取得）、
    // this=ecx(=esi)。
    static const DWORD kVA_CallSiteMeasure = 0x005555C9;  // 回跳位址0x5555CF

    // 0x555A68：sub_555960（多行文字自動換行核心函式，engine\zwindows\zlineobj.cpp）
    // 裡的GetGlyph呼叫，逐字元取寬度累加判斷斷行位置，call後讀回傳記錄的
    // advance欄位(+0x10)。vtable在edx、charcode在eax（非callee-saved，call後被
    // 回傳值蓋掉，需stash；但vtable在edx不能借edx當scratch覆寫[esp]的charcode，
    // 改借eax本身）。呼叫端與主渲染sub_559910/sub_5509D0是不同呼叫鏈，這條hook
    // 只覆蓋這個獨立的換行路徑。
    static const DWORD kVA_CallSiteWrap = 0x00555A68;  // 回跳位址0x555A6E

    static bool  g_diagEnable = false;
    static int   g_diagCap    = 300;
    static DWORD g_hits       = 0;

    // [Debug] GlyphOverrunProbe 快取值。DetermineCategory() 每次依「本旗標 &&
    // 目前在報紙/postmission 畫面」呼叫 GlyphAtlas::SetOverrunProbe()。
    static bool  g_overrunProbeCfg = false;

    // NativeTextureRegistry 的 hook 安裝、以及 SubtitleRender 唯一 EndScene
    // compositor 的安裝（透過 SubtitleHook::EnsureRenderReady()），都靠 GlyphCheck()
    // 每次命中 GetGlyph 時 lazy retry 驅動；旗標快取在對應模組內。Install() 被
    // [General] GlyphHook=0 跳過時 GlyphCheck 不會被呼叫，字幕功能雖與 CJK 字型
    // 取代無關卻共用這個開關當驅動點，是預期的連帶效果。
    // g_synthEnable 宣告放這裡（GlyphCheck 先用到，須先宣告）；實際值在 Install()
    // 從 [General] NormalFontReplace 指定。
    static bool g_synthEnable = false;

    // 報紙分類專用診斷（不受GlyphDiagEnable限制、永遠開著）。g_newsHits上限只是
    // 避免長時間停在報紙畫面時無限增長；g_newsLineHits統計charcode==0x0A（換行）
    // 出現次數，當「實際顯示行數」的proxy。
    static DWORD g_newsHits     = 0;
    static const DWORD kNewsDiagCap = 4000;
    static DWORD g_newsLineHits = 0;

    // CJK字型分類（通用/字幕/報紙三分法）——native GetGlyph這5個call site要判斷
    // 目前是不是報紙畫面，才知道要查哪份GlyphAtlas實例/native材質頁。判斷訊號＝
    // ZipPathTrace::GetLastSceneZipPath()結尾是"_news.zip"或"_postmission.zip"
    // （兩者都會渲染報紙版面，缺一不可）。Install()時把hModule存起來，供第一次
    // 偵測到報紙zip時lazy呼叫GlyphAtlas::EnsureCategoryReady()用
    // （Config::LoadForCategory需要它找到bink32hook.ini路徑）。
    static HMODULE g_hModule = nullptr;

    static bool ZipPathEndsWith(const char* path, const char* suffix)
    {
        size_t pathLen = strlen(path);
        size_t sufLen  = strlen(suffix);
        if (pathLen < sufLen) return false;
        return _stricmp(path + pathLen - sufLen, suffix) == 0;
    }

    // __cdecl、無參數。在native GetGlyph（與GlyphCheck）呼叫前執行，讓GlyphCheck
    // 的報紙專用log能收到分類參數，Stage C共用同一份g_categoryStash。回傳
    // (int)FontCategory，供inline asm存進g_categoryStash——enum class跨asm邊界
    // 當純int處理。
    //
    // 此刻native GetGlyph還沒呼叫、量不到font物件的blackBoxH，無法判斷font1~4
    // 哪一級，回傳粗略預設FontCategory::NewspaperFont4（最常見的內文級別），只供
    // GlyphCheck的[News]log與EnsureCategoryReady觸發判斷用。真正精確分類移到
    // SynthesizeCJKGlyphRecord，等native回傳、有blackBoxH可量測後才做
    // （見該函式內ProbeNewspaperFontSize()）。
    static int __cdecl DetermineCategory()
    {
        const char* zipPath = ZipPathTrace::GetLastSceneZipPath();
        bool news = zipPath && zipPath[0] &&
                    (ZipPathEndsWith(zipPath, "_news.zip") || ZipPathEndsWith(zipPath, "_postmission.zip"));

        // [Debug] GlyphOverrunProbe：只有「開關開 && 目前在報紙/postmission 畫面」
        // 才讓下游字圖合成路徑印側錄，進報紙前完全安靜。
        GlyphAtlas::SetOverrunProbe(g_overrunProbeCfg && news);

        if (news)
        {
            // lazy init：第一次偵測到報紙zip時，4個級別全部先觸發一次（冪等，
            // 已Ready/Failed時內部直接return）——此刻還不知道會量到哪一級，全部
            // 先建好GDI字型確保atlas就緒。材質提前登記移到SynthesizeCJKGlyphRecord
            // （需精確分類後才知道登記哪一份）。
            GlyphAtlas::EnsureCategoryReady(g_hModule, FontCategory::NewspaperFont1);
            GlyphAtlas::EnsureCategoryReady(g_hModule, FontCategory::NewspaperFont2);
            GlyphAtlas::EnsureCategoryReady(g_hModule, FontCategory::NewspaperFont3);
            GlyphAtlas::EnsureCategoryReady(g_hModule, FontCategory::NewspaperFont4);

            return (int)FontCategory::NewspaperFont4;
        }
        return (int)FontCategory::General;
    }

    // 跨pushad/popad暫存DetermineCategory()回傳值，同g_placeholderResult
    // 手法——call cdecl函式一定會蓋掉eax，popad後才拿得到乾淨的暫存器狀態。
    static DWORD g_categoryStash = 0;

    // 缺字佔位符機制：native對CJK碼位統一回傳共用的「缺字」記錄、呼叫端跳過材質
    // 註冊、畫面空白。此機制在呼叫native GetGlyph前把charcode偷換成一個ASCII符號，
    // 讓native查到真正存在的字圖，呼叫端當正常字元處理、正常觸發+0x108/+0x130，
    // 畫面顯示該符號而非空白。純粹借用native既有ASCII資源。
    static bool  g_placeholderEnable = false;
    static const int kPlaceholderChar = '@';  // 0x40
    static DWORD g_placeholderResult = 0;     // 給detour暫存PreparePlaceholder回傳值用（跨pushad/popad）

    // __cdecl。charcode是「解碼後的原始碼位」，fontSlotThis沿用GlyphCheck/
    // GlyphResultCheck已經在印的同一個值，天然就是「這個字屬於哪段文字/UI
    // 元件」的識別碼，不用另外設計字串ID機制。回傳值：不受影響的原charcode，
    // 或CJK碼位時的佔位符。
    static int __cdecl PreparePlaceholder(int charcode, void* fontSlotThis)
    {
        if (!g_placeholderEnable) return charcode;
        if (!BMIsCJKCodepoint((unsigned int)charcode)) return charcode;
        Log::Write("[GlyphHook] CJK缺字佔位替換：U+%04X -> '@' fontSlotThis=%p（這個值可以拿來分組同一段文字的所有缺字）",
                   (unsigned int)charcode, fontSlotThis);
        return kPlaceholderChar;
    }

    // siteId: 0=主(0x55A0A0)，1=選單(0x550BE4)，2=量測(0x5555C9)，
    // 3=換行(0x555A68)。仍供GlyphResultCheck的log標籤使用。
    static const char* const kSiteLabel[4] = { "主(0x55A0A0)", "選單(0x550BE4)", "量測(0x5555C9)", "換行(0x555A68)" };

    // __cdecl，供下面的naked detour呼叫。目前純diagnostic：偵測到CJK
    // codepoint時印一行log（受g_diagCap節流，避免掃過整個選單畫面洗版）。
    // 不回傳任何東西、不影響呼叫端行為——native GetGlyph
    // 呼叫是GlyphDetour自己另外inline重跑一次完成的，這裡只是側錄。
    static void __cdecl GlyphCheck(void* fontSlotThis, int charcode, int siteId, int categoryInt)
    {
        // 下面幾個lazy retry各自檢查對應的[HookToggle]旗標，關閉時該模組不會被安裝。

        // g_synthEnable時安裝輕量BeginScene vtable hook（成本只有一次slot比對）；
        // 真正的重量級native ReserveTexture/UpdateTexture（256KB stack alloca，見
        // NativeTextureRegistry.h/.cpp頂部註解）不能從GlyphDetour深層呼叫鏈直接
        // 觸發，改在每幀BeginScene（呼叫深度很淺）觸發。
        if (g_synthEnable) NativeTextureRegistry::EnsureBeginSceneHookInstalled();

        // 字幕統一 compositor（Brief/Dialogue/Oneliners 共用的唯一 EndScene
        // hook，見 SubtitleRender.h）：同款 lazy retry，裝置未就緒前每次
        // GetGlyph 命中都重試一次。開關全關時 EnsureRenderReady() 內部直接
        // return，成本只有一次 bool 旗標比對。
        SubtitleHook::EnsureRenderReady();

        // 元素A擅闖字（TrespassHud）跟字幕共用同一個唯一 EndScene compositor，
        // 字幕全關時也要能驅動其 lazy 安裝。停用時內部直接 return。
        TrespassHud::EnsureRenderReady();

        // 元素B懷疑/戰鬥字（WarningHud）同款共用同一個 compositor。
        WarningHud::EnsureRenderReady();

        // 小地圖佔位方框（MinimapHud）同款共用同一個 compositor，同樣需要
        // 驅動其 lazy 安裝。停用時內部直接 return。
        MinimapHud::EnsureRenderReady();

        if (!g_diagEnable) return;
        if (!BMIsCJKCodepoint((unsigned int)charcode)) return;
        if (g_hits >= (DWORD)g_diagCap) return;
        g_hits++;
        Log::Write("[GlyphHook] CJK codepoint觸及native GetGlyph：U+%04X fontSlotThis=%p（診斷階段，仍交給native處理）",
                   (unsigned int)charcode, fontSlotThis);
    }

    // CJK codepoint有進到GetGlyph，但畫面顯示空白、+0x108/+0x130未被觸發——代表
    // native對未知CJK碼位的回傳值讓呼叫端跳過材質註冊。這裡補抓native呼叫後的
    // eax（thiscall回傳值＝glyph記錄指標），確認是NULL還是某種特殊sentinel記錄。
    static DWORD g_resultHits = 0;

    // 報紙font 1~4實際字級量測用，不受GlyphDiagEnable限制，只在Newspaper分類+
    // ASCII字元才印（僅供下方已註解的[FontSize] side-record）。
    static DWORD g_newsFontSizeHits = 0;
    static const DWORD kNewsFontSizeDiagCap = 500;

    // 與報紙font1~4對照的一般選單基準值——site=1(0x550BE4)、ASCII範圍，不受
    // g_diagEnable/IsNewspaperCategory限制（僅供下方已註解的[Menu][FontSize]
    // side-record）。
    static DWORD g_menuFontSizeHits = 0;
    static const DWORD kMenuFontSizeDiagCap = 500;

    // 讀native <font 1>~<font 4>（報紙headline/內文各段）實際渲染的blackbox寬高，
    // 判斷CJK字型分類要不要依font slot再拆分。rec[0x0C]/[0x0D]＝native blackbox
    // 寬高欄位，這裡讀Stage C覆寫前的原始回傳值，對ASCII有效（CJK缺字sentinel
    // 記錄的數值不能拿來當字級參考）。
    static void __cdecl GlyphResultCheck(int charcode, int siteId, void* result, void* fontSlotThis)
    {
        // 失敗案例（NULL/不可讀指標）不受g_diagEnable限制、一律印出——
        // GlyphDiagEnable=0時也應該看得到失敗訊號。只有下面正常情況的完整
        // hex dump（純資訊性）才受g_diagEnable/g_diagCap節流。
        if (!result)
        {
            Log::Write("[GlyphHook] native GetGlyph回傳值 site=%s U+%04X fontSlotThis=%p -> NULL（無glyph記錄）",
                       kSiteLabel[siteId], (unsigned int)charcode, fontSlotThis);
            return;
        }
        if (IsBadReadPtr(result, 0x13))
        {
            Log::Write("[GlyphHook] native GetGlyph回傳值 site=%s U+%04X fontSlotThis=%p -> record=%p（不可讀，可能是特殊sentinel值）",
                       kSiteLabel[siteId], (unsigned int)charcode, fontSlotThis, result);
            return;
        }

        // 報紙內文空白字元X游標異常追查：SynthesizeCJKGlyphRecord對非CJK
        // codepoint（含空白0x20）一律直接return nativeResult，所以sub_559910
        // 讀到的異常rec[0x10]是native原始記錄、非我方算的。不受g_diagEnable
        // 限制，只在報紙分類+空白類codepoint才印。
        /*
        if (IsNewspaperCategory((FontCategory)g_categoryStash) &&
            (charcode == 0x20 || charcode == 0x09 || charcode == 0x0D || charcode == 0x0A))
        {
            BYTE wsRaw[0x13];
            memcpy(wsRaw, result, 0x13);
            char wsHex[0x13 * 3 + 1] = {};
            for (int i = 0; i < 0x13; i++)
                sprintf_s(wsHex + i * 3, sizeof(wsHex) - i * 3, "%02X ", wsRaw[i]);
            Log::Write("[GlyphHook][News][WhitespaceRecord] site=%s U+%04X fontSlotThis=%p record=%p +0x10=%d(signed) raw[0x00-0x12]=%s",
                       kSiteLabel[siteId], (unsigned int)charcode, fontSlotThis, result, (signed char)*((BYTE*)result + 0x10), wsHex);
        }*/

        // 報紙font1~4逐字元(0x20~0x7E)native量測 side-record，補對照
        // ProbeNewspaperFontSize的[NativeProbe]（後者只有'@'單一樣本）。
        // 由使用者指示註解掉，保留供之後重新對照時取消註解。
        /*
        if (IsNewspaperCategory((FontCategory)g_categoryStash) &&
            charcode >= 0x20 && charcode <= 0x7E &&
            g_newsFontSizeHits < kNewsFontSizeDiagCap)
        {
            g_newsFontSizeHits++;
            BYTE fsW = *((BYTE*)result + 0x0C);
            BYTE fsH = *((BYTE*)result + 0x0D);
            signed char fsOriginX = (signed char)*((BYTE*)result + 0x0E);
            signed char fsOriginY = (signed char)*((BYTE*)result + 0x0F);
            BYTE fsAdvance = *((BYTE*)result + 0x10);
            BYTE fsLineHeight = *((BYTE*)result + 0x11);
            // 跟ProbeNewspaperFontSize同一組門檻(28/20/12)分類font1~4。
            int fsFontLevel;
            if (fsH >= 28) fsFontLevel = 1;
            else if (fsH >= 20) fsFontLevel = 2;
            else if (fsH >= 12) fsFontLevel = 3;
            else fsFontLevel = 4;
            Log::Write("[GlyphHook][News][FontSize] font%d U+%04X fontSlotThis=%p blackBoxW=%d blackBoxH=%d originX=%d originY=%d advance=%d lineHeight(+0x11)=%d",
                       fsFontLevel, (unsigned int)charcode, fontSlotThis, (int)fsW, (int)fsH,
                       (int)fsOriginX, (int)fsOriginY, (int)fsAdvance, (int)fsLineHeight);
        }
        */

        // 一般選單基準值side-record（對照報紙font1~4 blackBoxH）。由使用者
        // 指示註解掉，保留供之後重新比對時取消註解。
        /*
        if (siteId == 1 && charcode >= 0x20 && charcode <= 0x7E &&
            g_menuFontSizeHits < kMenuFontSizeDiagCap)
        {
            g_menuFontSizeHits++;
            BYTE mfW = *((BYTE*)result + 0x0C);
            BYTE mfH = *((BYTE*)result + 0x0D);
            signed char mfOriginX = (signed char)*((BYTE*)result + 0x0E);
            signed char mfOriginY = (signed char)*((BYTE*)result + 0x0F);
            BYTE mfAdvance = *((BYTE*)result + 0x10);
            BYTE mfLineHeight = *((BYTE*)result + 0x11);
            Log::Write("[GlyphHook][Menu][FontSize] U+%04X fontSlotThis=%p blackBoxW=%d blackBoxH=%d originX=%d originY=%d advance=%d lineHeight(+0x11)=%d",
                       (unsigned int)charcode, fontSlotThis, (int)mfW, (int)mfH,
                       (int)mfOriginX, (int)mfOriginY, (int)mfAdvance, (int)mfLineHeight);
        }
        */

        if (!g_diagEnable) return;
        // 暫時放寬成連ASCII都記錄（CJK一律是共用sentinel、無per-glyph差異可比
        // 對材質頁欄位）；找到隨材質頁變化的欄位後應改回CJK-only，避免log量過大。
        if (g_resultHits >= (DWORD)g_diagCap) return;
        g_resultHits++;

        DWORD glyphIndex = *(DWORD*)((BYTE*)result + 0x04);
        BYTE  m8  = *((BYTE*)result + 0x08);
        BYTE  m10 = *((BYTE*)result + 0x10);
        BYTE  m11 = *((BYTE*)result + 0x11);
        // +0x0C/+0x0D＝native blackbox寬高（quad size），ASCII字元是真正烘焙字圖
        // 的實際pixel尺寸，可比較不同fontSlotThis（=<font N>）之間的字級差異。
        BYTE  blackBoxW = *((BYTE*)result + 0x0C);
        BYTE  blackBoxH = *((BYTE*)result + 0x0D);

        // 完整dump 0x00~0x12（19 bytes），供比對不同字型槽/codepoint下哪個欄位
        // 的值會隨材質頁（TEX entry的#N）變化。
        BYTE raw[0x13];
        memcpy(raw, result, 0x13);
        char hex[0x13 * 3 + 1] = {};
        for (int i = 0; i < 0x13; i++)
            sprintf_s(hex + i * 3, sizeof(hex) - i * 3, "%02X ", raw[i]);

        Log::Write("[GlyphHook] native GetGlyph回傳值 site=%s U+%04X fontSlotThis=%p -> record=%p glyphIndex=0x%08X blackBoxW=%d blackBoxH=%d +0x08=%d +0x10=%d +0x11=%d raw[0x00-0x12]=%s",
                   kSiteLabel[siteId], (unsigned int)charcode, fontSlotThis, result, glyphIndex, blackBoxW, blackBoxH, m8, m10, m11, hex);
    }

    // Stage C：CJK glyph記錄合成。native/consumer只讀取+0x00~+0x12共19 bytes。
    // 策略是拿native這次呼叫的回傳值（真字元記錄或「缺字」sentinel，byte layout
    // 一樣、都合法可讀）當byte-template整段複製，只覆寫已知欄位，+0x11/+0x12維持
    // template原值不動（未知語意，照抄比亂猜安全）。
    //
    // 快取key：std::unordered_map保證既有entry的參照/指標在後續insert不會失效
    // （只有iterator可能失效），所以回傳cache裡的data()指標是安全的。key用
    // bit-packed (codepoint<<3)|category——同一個字在不同分類下光柵化結果
    // （atlas頁碼/UV座標）不同，不能共用同一筆cache；category一律用
    // GlyphAtlas::ResolveCategory()後的「有效分類」，保證跟GetGlyph()實際查詢到
    // 的atlas內容一致。
    static std::unordered_map<unsigned int, std::array<BYTE, 24>> g_synthCache;

    static unsigned int MakeSynthCacheKey(unsigned int codepoint, FontCategory effectiveCategory)
    {
        return (codepoint << 3) | (unsigned int)effectiveCategory;
    }

    // 報紙依<font N>分級：fontSlotThis(=<font N>物件)第一次遇到時才探測分類，之後
    // 同一個fontSlotThis直接用快取結果，確保整段文字視覺一致（避免anti-alias
    // 四捨五入讓邊界字元誤判到不同bucket）。跟g_synthCache一起在
    // InvalidateSynthCache()清空——fontSlotThis位址每次進報紙都可能不同。
    static std::unordered_map<void*, FontCategory> g_newspaperSizeCache;

    // 跟g_newspaperSizeCache同一次探測順便存下來，供GetNewspaperLineHeightForSlot()
    // 查詢——sub_559910內var_998的+0x11欄位會被共用buffer的後續字元query污染、
    // 與這個fontSlotThis真正該有的行高對不上。存的是native原始+0x11 byte值，未乘1.5。
    static std::unordered_map<void*, int> g_newspaperLineHeightCache;

    typedef void* (__thiscall* ProbeGetGlyphFn)(void* thisPtr, int charcode);

    // CJK codepoint一律拿到native共用的「缺字」sentinel記錄，尺寸語意與真正字型
    // 物件無關（只有ASCII有意義），不能直接讀這次呼叫的nativeResult來分級。改成
    // 對fontSlotThis額外探測一次：用同一個font物件的vtable+0x238查詢'@'（native
    // 必有烘焙ASCII），用這筆查詢回傳的真正度量分級。探測獨立於呼叫端要顯示的
    // 字元（nativeResult不受影響），只有fontSlotThis第一次出現時多呼叫一次，
    // 之後走g_newspaperSizeCache快取。
    static FontCategory ProbeNewspaperFontSize(void* fontSlotThis)
    {
        if (!fontSlotThis || IsBadReadPtr(fontSlotThis, 4))
            return FontCategory::NewspaperFont4;  // 無法探測，退回最常見的內文級別

        DWORD* vtable = *(DWORD**)fontSlotThis;
        if (!vtable || IsBadReadPtr(vtable, 0x238 + 4))
            return FontCategory::NewspaperFont4;

        ProbeGetGlyphFn getGlyph = (ProbeGetGlyphFn)vtable[0x238 / 4];
        void* probeResult = getGlyph(fontSlotThis, '@');
        if (!probeResult || IsBadReadPtr(probeResult, 0x0E))
            return FontCategory::NewspaperFont4;

        BYTE blackBoxH = *((BYTE*)probeResult + 0x0D);
        BYTE lineHeight = *((BYTE*)probeResult + 0x11);
        // 用lineHeight(+0x11)而非blackBoxH(+0x0D)分級：blackBoxH受單一字母字形
        // （升降部）影響，探測字元'@'會讓中段級卡在門檻下緣誤判成font3；lineHeight
        // 是native整行推進高度，不受單一字母形狀影響、較穩定。門檻40/19/12對應
        // native實測的5組字級聚類（54/26/22/16/13）取中點。這是native既有行為的
        // 特徵，與我方CJK替換字型要用多大px（ini font1+固定offset）無關，門檻
        // 不跟著font1的ini值連動。
        FontCategory bucket;
        if (lineHeight >= 40) bucket = FontCategory::NewspaperFont1;
        else if (lineHeight >= 19) bucket = FontCategory::NewspaperFont2;
        else if (lineHeight >= 12) bucket = FontCategory::NewspaperFont3;
        else bucket = FontCategory::NewspaperFont4;

        // 探測呼叫本身就是native GetGlyph('@')的原始回傳值（Stage C合成還沒介入），
        // 一次印出已知有明確語意的全部欄位供備查（下方[NativeProbe]已註解）：
        // blackBoxW/H（quad尺寸）、originX/Y（+0x0E/+0x0F，signed，筆刷位移）、
        // advance（+0x10）、+0x11（native原始行高欄位）、+0x12（語意未知）。
        int fontLevel = (int)bucket - (int)FontCategory::NewspaperFont1 + 1;
        BYTE blackBoxW  = *((BYTE*)probeResult + 0x0C);
        signed char originX = (signed char)*((BYTE*)probeResult + 0x0E);
        signed char originY = (signed char)*((BYTE*)probeResult + 0x0F);
        BYTE advance    = *((BYTE*)probeResult + 0x10);
        BYTE field12    = *((BYTE*)probeResult + 0x12);
        /*
        Log::Write("[GlyphHook][News][NativeProbe] fontSlotThis=%p probe='@' -> font%d blackBoxW=%d blackBoxH=%d originX=%d originY=%d advance=%d +0x11=%d +0x12=%d",
                   fontSlotThis, fontLevel, (int)blackBoxW, (int)blackBoxH,
                   (int)originX, (int)originY, (int)advance, (int)lineHeight, (int)field12);
                   */
        // 順便存進g_newspaperLineHeightCache，供GetNewspaperLineHeightForSlot()查詢。
        g_newspaperLineHeightCache[fontSlotThis] = (int)lineHeight;

        return bucket;
    }

    int GetNewspaperLineHeightForSlot(void* fontSlotThis)
    {
        auto it = g_newspaperLineHeightCache.find(fontSlotThis);
        if (it != g_newspaperLineHeightCache.end())
            return it->second;

        // 還沒探測過（理論上GlyphDetourSpaceA/B在每次<font N>切換時都會
        // 提前探測，這裡是保險：萬一漏接就現場補探測一次）。
        ProbeNewspaperFontSize(fontSlotThis);
        it = g_newspaperLineHeightCache.find(fontSlotThis);
        return (it != g_newspaperLineHeightCache.end()) ? it->second : 0;
    }

    static void* __cdecl SynthesizeCJKGlyphRecord(unsigned int codepoint, void* nativeResult, int categoryInt, void* fontSlotThis)
    {
        if (!g_synthEnable) return nativeResult;
        // 譯文模式（F11）下不分CJK/ASCII一律用同一套合成字型輸出，讓整段文字
        // 度量基準一致（否則同段譯文的英數字與CJK字型不同、行高基準不一致，會
        // 造成疊字）。原文模式維持CJK-only，避免原文英文UI被強制套用CJK字型。
        bool forceAll = LocHook::IsTranslationActive();
        if (!forceAll && !BMIsCJKCodepoint(codepoint)) return nativeResult;

        FontCategory category = (FontCategory)categoryInt;

        // 報紙依<font N>分級——category此刻是DetermineCategory()給的粗略預設值，
        // 這裡才是精確分類的地方。fontSlotThis第一次遇到時才探測，之後用快取。
        if (IsNewspaperCategory(category))
        {
            auto cachedBucket = g_newspaperSizeCache.find(fontSlotThis);
            if (cachedBucket != g_newspaperSizeCache.end())
                category = cachedBucket->second;
            else
            {
                category = ProbeNewspaperFontSize(fontSlotThis);
                g_newspaperSizeCache[fontSlotThis] = category;
            }
        }

        FontCategory effective = GlyphAtlas::ResolveCategory(category);
        unsigned int cacheKey = MakeSynthCacheKey(codepoint, effective);

        auto cached = g_synthCache.find(cacheKey);
        if (cached != g_synthCache.end())
            return cached->second.data();

        const GlyphAtlas::Entry* e = GlyphAtlas::GetGlyph(codepoint, category);
        if (!e || e->width == 0 || e->height == 0)
        {
            // 光柵化失敗（GDI錯誤/atlas已滿）或空白類零寬字元：不合成，
            // 維持native原本回傳值（通常是「缺字」sentinel，畫面空白，安全）。
            return nativeResult;
        }

        if (!nativeResult || IsBadReadPtr(nativeResult, 0x13))
        {
            Log::Write("[GlyphHook] Stage C合成放棄：U+%04X native回傳值不可用(result=%p)，沒有byte-template可複製",
                       codepoint, nativeResult);
            return nativeResult;
        }

        // glyphIndex用NativeTextureRegistry登記到的真正native材質頁碼。
        // GetRegisteredIndex純讀取快取值、不做native呼叫，可安全從這個深層呼叫鏈
        // 呼叫（真正的ReserveTexture/UpdateTexture在BeginScene觸發）。還沒在某次
        // BeginScene完成登記時維持native原本回傳值、不合成，避免產生一筆
        // glyphIndex查無材質的record；atlas內容也要等下一次BeginScene才推上native
        // 材質，畫面上可能有一格延遲。

        // 報紙材質提前登記：報紙文章排版一次性同步跑完，等下一次BeginScene才登記
        // 太慢（CJK會全部合成失敗且不重新排版）。移到這裡是因為只有解析出精確分類
        // (effective)後才知道哪一級需要登記，避免沒用到的級別白白佔一份材質槽。
        // 只在GetRegisteredIndex()<0時呼叫，已登記時只是一次陣列讀取；Update()冪等。
        // 風險：從GetGlyph深層呼叫鏈呼叫重量級native函式（見NativeTextureRegistry
        // crash歷史），尚未實機驗證。
        if (IsNewspaperCategory(effective) &&
            GlyphAtlas::IsCategoryReady(effective) &&
            NativeTextureRegistry::GetRegisteredIndex(effective) < 0)
        {
            NativeTextureRegistry::Update(effective);
        }

        int nativePage = NativeTextureRegistry::GetRegisteredIndex(effective);
        if (nativePage < 0)
            return nativeResult;

        std::array<BYTE, 24> rec = {};
        memcpy(rec.data(), nativeResult, 0x13);

        // [Debug] GlyphOverrunProbe：報紙字圖合成的關鍵輸入——native 記錄前 0x13
        // bytes、GetGlyph 回傳的 Entry 尺寸欄位、nativePage。純側錄。
        if (GlyphAtlas::OverrunProbeOn())
        {
            const BYTE* nr = (const BYTE*)nativeResult;
            Log::Write("[GlyphOverrunProbe][Synth] U+%04X cat=%d eff=%d nativeResult=%p nativePage=%d e.wh=%dx%d e.cell=%dx%d e.atlas=(%d,%d) nr[0..12]=%02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X%02X %02X%02X%02X",
                       codepoint, (int)category, (int)effective, nativeResult, nativePage,
                       e->width, e->height, e->cellWidth, e->cellHeight, e->atlasX, e->atlasY,
                       nr[0], nr[1], nr[2], nr[3], nr[4], nr[5], nr[6], nr[7], nr[8], nr[9],
                       nr[10], nr[11], nr[12], nr[13], nr[14], nr[15], nr[16], nr[17], nr[18]);
        }

        // ReHitman社群SDK的ZFONT.h `SCharInfo`（native GetGlyph/vtable+0x238的
        // 回傳型別）：5個DWORD共20 bytes——
        //   charCode(+0x00) / unk0(+0x04 glyphIndex) / unk1(+0x08~0x0B UV) /
        //   unk2(+0x0C~0x0F 尺寸/origin) / unk3(+0x10~0x13 advance/lineHeight)。
        // +0x00~0x03是charCode欄位本身。native「缺字」sentinel是共用buffer、其
        // charCode大概率不等於要查的CJK codepoint；consumer可能拿charCode跟查詢
        // codepoint比對判斷「有沒有找到」（不符就畫方框），所以這裡寫入真正的
        // codepoint讓欄位語意正確。
        *(DWORD*)(rec.data() + 0x00) = codepoint;

        DWORD glyphIndex = (DWORD)nativePage;
        *(DWORD*)(rec.data() + 0x04) = glyphIndex;

        // atlas是正方形，UV = pixel / atlasSize；native存的是UV×128的
        // signed byte（consumer端×flt_763BF0=1/128換算回紋理座標），這裡
        // 反向：算出UV後×128四捨五入存回byte。atlas座標必定在0~1024內、
        // UV必定在0~1之間，理論上不會超出signed byte範圍。
        float atlasSize = (float)GlyphAtlas::GetAtlasWidth();
        auto toUVByte = [atlasSize](int px) -> BYTE
        {
            float uv = (float)px / atlasSize;
            int   v  = (int)(uv * 128.0f + 0.5f);
            if (v < -128) v = -128;
            if (v > 127)  v = 127;
            return (BYTE)v;
        };
        // 右/下邊界用e->cellWidth/cellHeight（已對齊UV量化格線）而非緊貼字形的
        // e->width/e->height——1個UV單位=8px、CJK字圖僅10~12px寬高，用緊貼尺寸算
        // UV右下角會四捨五入裁掉字形內容。cell是量化格線的整數倍，不會有裁切誤差。
        rec[0x08] = toUVByte(e->atlasX);
        rec[0x09] = toUVByte(e->atlasY);
        rec[0x0A] = toUVByte(e->atlasX + e->cellWidth);
        rec[0x0B] = toUVByte(e->atlasY + e->cellHeight);

        // +0x0C/+0x0D＝quad的寬高：unsigned byte直接cast成float、無縮放係數，
        // consumer拿它跟x/y做「pos + size*0.5」置中運算。用e->cellWidth/cellHeight
        // （而非緊貼字形的gmBlackBoxX/Y），讓quad尺寸與UV涵蓋範圍維持1:1——UV那邊
        // 已放大到量化格線的cell尺寸，quad若還用緊貼尺寸會把較大的UV範圍硬塞進
        // 較小的quad、字形被擠壓變形。cell略大出的是透明像素，不影響字距
        // （advance仍用真正的gmCellIncX）。
        rec[0x0C] = (BYTE)e->cellWidth;
        rec[0x0D] = (BYTE)e->cellHeight;
        // +0x0E/+0x0F：未經縮放、直接加到筆刷位置的X/Y位移（+0x0E加到x；+0x0F由
        // consumer取負）。對應GDI gmptGlyphOrigin（x非斜體字型常態為0；y存負值，
        // 語意是「筆刷原點到黑盒頂端的垂直距離」，consumer統一取負套用）。
        rec[0x0E] = (BYTE)e->gm.gmptGlyphOrigin.x;
        rec[0x0F] = (BYTE)(-e->gm.gmptGlyphOrigin.y);
        // +0x10：前進寬度(advance) = native GDI量到的gmCellIncX + 使用者自訂字距
        // (ini FontSpacing，見GlyphAtlas::GetSpacing()頂部註解)。用effective（=取得
        // e這個Entry用的同一個分類，含fallback判斷後）取spacing，不用category，
        // 確保兩者一致。clamp到signed byte範圍，避免CJK大字級+大spacing溢位變負值
        // （native讀成有號byte，一旦變負advance會讓排版游標倒退）。
        {
            int advancePx = (int)e->gm.gmCellIncX + GlyphAtlas::GetSpacing(effective);
            if (advancePx > 127)  advancePx = 127;
            if (advancePx < -128) advancePx = -128;
            rec[0x10] = (BYTE)advancePx;
        }
        // +0x11＝排版函式消費的「行高」欄位（v101，×1.5後推進Y游標）。必須寫入，
        // 否則照抄native「缺字」sentinel殘值會讓CJK自動換行行距塌陷疊字。用
        // 「cellHeight + glyph origin Y偏移(取正值)」試填，跟+0x0C~+0x10同樣是
        // 未經縮放的原始px cast進byte；clamp到0~127避免CJK大字級時signed byte
        // 溢位變負（native讀成char，變負會讓v101/v102的max比較完全失效）。
        // 量級是否為native期望值尚未實機驗證。
        {
            int lineHeightPx = e->cellHeight + e->gm.gmptGlyphOrigin.y;
            if (lineHeightPx > 127) lineHeightPx = 127;
            if (lineHeightPx < 0)   lineHeightPx = 0;
            rec[0x11] = (BYTE)lineHeightPx;
        }

        // 成功案例的合成完成log受g_diagEnable限制；只有GlyphResultCheck那種
        // 「native GetGlyph回傳異常值」的失敗案例才不受開關限制、一律印出。
        if (g_diagEnable)
            Log::Write("[GlyphHook] Stage C合成完成：U+%04X category=%d(effective=%d) glyphIndex=0x%08X UV=(%d,%d,%d,%d) origin*2=(%d,%d) yDisp=%d advance=%d lineHeight=%d",
                       codepoint, (int)category, (int)effective, *(DWORD*)(rec.data() + 0x04),
                       (signed char)rec[0x08], (signed char)rec[0x09], (signed char)rec[0x0A], (signed char)rec[0x0B],
                       (signed char)rec[0x0C], (signed char)rec[0x0D], (signed char)rec[0x0F], (signed char)rec[0x10], (signed char)rec[0x11]);

        auto& stored = g_synthCache[cacheKey];
        stored = rec;
        return stored.data();
    }

    // 見GlyphHook.h宣告處註解——由NativeTextureRegistry::InvalidateAllSlots()
    // 呼叫，避免已合成過的字永遠帶著已失效的舊nativeIndex。
    void InvalidateSynthCache()
    {
        size_t n = g_synthCache.size();
        g_synthCache.clear();
        // 報紙字級分類快取跟g_synthCache一起清空——fontSlotThis位址每次進報紙
        // 都可能不同。
        size_t m = g_newspaperSizeCache.size();
        g_newspaperSizeCache.clear();
        Log::Write("[GlyphHook] InvalidateSynthCache：清空%zu筆已合成CJK glyph快取、%zu筆報紙字級分類快取，下次命中會用當下nativeIndex/blackBoxH重新合成/分類", n, m);
    }

    // fontSlotThis跨native call的暫存：detour進入當下(native call之前)ecx=
    // fontSlotThis天然正確，但native GetGlyph與cdecl的SynthesizeCJKGlyphRecord
    // 都不保證保留ecx，所以在detour最前面就存起來，供native call結束後才呼叫的
    // GlyphResultCheck/SynthesizeCJKGlyphRecord使用——跟g_measureCharcodeStash/
    // g_placeholderResult同一種「暫存器可能被清掉，改用static變數跨越」的手法。
    // 主/選單/量測/換行4個detour都會寫入。
    static DWORD g_selfRenderFontSlotThisStash = 0;

    static void __cdecl SpaceCheck(void* fontSlotThis, int siteId)
    {
        // 這兩個call site是native每次<font N>切換（含函式一開始的初始字型）都會
        // 經過的點，比CJK觸發的探測時機（ProbeNewspaperFontSize只在遇到CJK字元
        // 才跑）更早、更全——確保font1~4只要在文章裡出現過<font N>就會被探測到。
        // 只在報紙分類才探測，避免其他html畫面的<font>切換白跑一次GetGlyph('@')。
        if (IsNewspaperCategory((FontCategory)DetermineCategory()) &&
            g_newspaperSizeCache.find(fontSlotThis) == g_newspaperSizeCache.end())
        {
            FontCategory bucket = ProbeNewspaperFontSize(fontSlotThis);
            g_newspaperSizeCache[fontSlotThis] = bucket;
        }
    }

    // 進入時暫存器狀態跟原本0x55A0A0那條被覆蓋掉的`call dword ptr [eax+238h]`
    // 指令執行前完全一樣：eax=字型槽vtable指標，ecx=this(字型槽物件)，
    // edi=charcode，[esp]=charcode（native在0x55A09F的`push edi`已經執行過，
    // 我們的patch只覆蓋接下來的call指令本身，不動push edi）。
    //
    // pushad/popad對稱地保留了[esp]那個pushed charcode，所以popad跑完後，
    // 暫存器與堆疊狀態跟「detour剛進入時」完全一致——這時候直接原樣重新
    // 執行一次`call dword ptr [eax+238h]`，語意上跟原本沒被patch時一模一樣
    // （呼叫真正的native GetGlyph，thiscall慣例下該函式自己retn 4把pushed
    // charcode清掉，esp淨變化為0，eax=回傳的glyph記錄指標）。不需要另外
    // 配置trampoline——沒有動到callee本體，只是把call site換成先繞去自己
    // 的診斷函式，再原樣把同一個call補回來，因此不會有指令中段被切斷的
    // 風險，也不需要額外的指令長度解碼。
    static __declspec(naked) void GlyphDetour()
    {
        __asm
        {
            ; ecx=fontSlotThis此刻天然正確，先存起來，native call跟後面呼叫的
            ; cdecl函式都不保證保留ecx，等真的要用（native call結束後）它
            ; 可能已經是別的值了。單純mov，不影響任何暫存器/flags，可以安全
            ; 放在最前面。
            mov  [g_selfRenderFontSlotThisStash], ecx

            ; 分類判斷提前到native call之前，供GlyphCheck的報紙專用log使用。
            ; DetermineCategory()只看zip路徑字串、不依賴native call結果；下面
            ; Stage C共用這裡寫進g_categoryStash的值，不再算一次。
            pushad
            call DetermineCategory
            mov  [g_categoryStash], eax
            popad

            pushad
            push dword ptr [g_categoryStash]  ; category
            push 0               ; siteId=主
            push edi            ; charcode
            push ecx            ; fontSlotThis
            call GlyphCheck
            add  esp, 16
            popad

            ; 缺字佔位符：[esp]目前是native已經push好的charcode（call前的
            ; thiscall堆疊參數），呼叫PreparePlaceholder算出（可能被替換的）
            ; 值，存進static變數（跨pushad/popad安全），popad後用edx（此刻
            ; 不需要保留的暫存器）覆寫[esp]。
            pushad
            push ecx              ; fontSlotThis
            push edi              ; charcode
            call PreparePlaceholder
            add  esp, 8
            mov  [g_placeholderResult], eax
            popad
            mov  edx, [g_placeholderResult]
            mov  [esp], edx

            call dword ptr [eax + 238h]

            pushad
            push dword ptr [g_selfRenderFontSlotThisStash] ; fontSlotThis
            push eax             ; result（edi=charcode是callee-saved，call不會動到）
            push 0               ; siteId=主
            push edi             ; charcode（原始碼位，不是佔位符，方便log對照）
            call GlyphResultCheck
            add  esp, 16
            popad

            ; Stage C：CJK codepoint合成假glyph記錄。category沿用detour最前面
            ; 存好的g_categoryStash。不用pushad/popad包住——要保留eax（cdecl
            ; 回傳值）當jmp回native前的「glyph記錄指標」。fontSlotThis參數供
            ; 報紙依<font N>分級用，g_selfRenderFontSlotThisStash在最前面已存好。
            push dword ptr [g_selfRenderFontSlotThisStash] ; fontSlotThis
            push dword ptr [g_categoryStash]  ; category
            push eax             ; nativeResult
            push edi             ; codepoint
            call SynthesizeCJKGlyphRecord
            add  esp, 16

            mov  edx, 0x55A0A6
            jmp  edx
        }
    }

    // 暫存器狀態同GlyphDetour，差別只在vtable在edx（不是eax）、charcode是native
    // 已push好的常數0x20（不是變數）。siteId（1或2）硬編碼在各自的inline asm裡。
    static __declspec(naked) void GlyphDetourSpaceA()
    {
        __asm
        {
            pushad
            push 1              ; siteId
            push ecx            ; fontSlotThis
            call SpaceCheck
            add  esp, 8
            popad

            call dword ptr [edx + 238h]

            mov  eax, 0x559A27
            jmp  eax
        }
    }

    static __declspec(naked) void GlyphDetourSpaceB()
    {
        __asm
        {
            pushad
            push 2              ; siteId
            push ecx            ; fontSlotThis
            call SpaceCheck
            add  esp, 8
            popad

            call dword ptr [edx + 238h]

            mov  eax, 0x559B02
            jmp  eax
        }
    }

    // sub_5509D0裡的GetGlyph呼叫（0x550BE4）——跟主call site同一種暫存器
    // 結構（eax=vtable/ecx=this），只差charcode在ebp。進入時ebp已經是
    // native的charcode（push ebp已執行過，ebp暫存器本身沒被push動到），
    // pushad/popad一樣天然保留，detour結束後原樣重新執行[eax+238h]呼叫。
    static __declspec(naked) void GlyphDetourMenu()
    {
        __asm
        {
            ; 同主detour，先把ecx=fontSlotThis存起來（見主detour同一段註解）。
            mov  [g_selfRenderFontSlotThisStash], ecx

            ; 分類判斷提前，同主detour。
            pushad
            call DetermineCategory
            mov  [g_categoryStash], eax
            popad

            pushad
            push dword ptr [g_categoryStash]  ; category
            push 1               ; siteId=選單
            push ebp            ; charcode
            push ecx            ; fontSlotThis
            call GlyphCheck
            add  esp, 16
            popad

            pushad
            push ecx              ; fontSlotThis
            push ebp              ; charcode
            call PreparePlaceholder
            add  esp, 8
            mov  [g_placeholderResult], eax
            popad
            mov  edx, [g_placeholderResult]
            mov  [esp], edx

            call dword ptr [eax + 238h]

            pushad
            push dword ptr [g_selfRenderFontSlotThisStash] ; fontSlotThis
            push eax             ; result（ebp=charcode是callee-saved，call不會動到）
            push 1               ; siteId=選單
            push ebp             ; charcode（原始碼位）
            call GlyphResultCheck
            add  esp, 16
            popad

            ; Stage C：category沿用detour最前面存好的g_categoryStash，不用
            ; pushad/popad包住，保留eax給jmp回去用。fontSlotThis參數同主detour。
            push dword ptr [g_selfRenderFontSlotThisStash] ; fontSlotThis
            push dword ptr [g_categoryStash]  ; category
            push eax             ; nativeResult
            push ebp             ; codepoint
            call SynthesizeCJKGlyphRecord
            add  esp, 16

            mov  edx, 0x550BEA
            jmp  edx
        }
    }

    // charcode在這個call site是eax（非callee-saved），native呼叫一結束就被回傳值
    // 蓋掉——不能像主/選單detour那樣call完沿用暫存器，改存進static變數
    // （單執行緒渲染路徑，無重入/競爭問題）。
    static DWORD g_measureCharcodeStash = 0;

    // sub_555590（字串寬度量測輔助函式）裡的GetGlyph呼叫（0x5555C9）——
    // 暫存器結構不同：vtable在ebp（不是eax）、charcode在eax、this=ecx(=esi)。
    static __declspec(naked) void GlyphDetourMeasure()
    {
        __asm
        {
            ; 借用fontSlotThis stash機制，讓後面GlyphResultCheck（native call已把
            ; ecx/esi清成別的值）還能拿到正確的fontSlotThis，供font 1~4字級量測用。
            mov  [g_selfRenderFontSlotThisStash], ecx

            ; 分類判斷提前，同主detour。此刻eax還是native charcode，pushad/popad
            ; 包住不受DetermineCategory影響。
            pushad
            call DetermineCategory
            mov  [g_categoryStash], eax
            popad

            pushad
            push dword ptr [g_categoryStash]  ; category
            push 2               ; siteId=量測
            push eax            ; charcode
            push ecx            ; fontSlotThis(=esi)
            call GlyphCheck
            add  esp, 16
            popad

            mov  [g_measureCharcodeStash], eax   ; call前charcode還在eax，先存起來

            pushad
            push ecx              ; fontSlotThis(=esi)
            push eax              ; charcode
            call PreparePlaceholder
            add  esp, 8
            mov  [g_placeholderResult], eax
            popad
            mov  edx, [g_placeholderResult]
            mov  [esp], edx

            call dword ptr [ebp + 238h]

            pushad
            push dword ptr [g_selfRenderFontSlotThisStash] ; fontSlotThis
            push eax                                ; result
            push 2                                   ; siteId=量測
            push dword ptr [g_measureCharcodeStash]  ; charcode
            call GlyphResultCheck
            add  esp, 16
            popad

            ; Stage C：category沿用detour最前面存好的g_categoryStash，不用
            ; pushad/popad包住，保留eax給jmp回去用。fontSlotThis參數同主detour。
            push dword ptr [g_selfRenderFontSlotThisStash] ; fontSlotThis
            push dword ptr [g_categoryStash]         ; category
            push eax                                ; nativeResult
            push dword ptr [g_measureCharcodeStash]  ; codepoint
            call SynthesizeCJKGlyphRecord
            add  esp, 16

            mov  edx, 0x5555CF
            jmp  edx
        }
    }

    // 跟g_measureCharcodeStash同理：charcode在eax、call後被回傳值蓋掉，
    // 單執行緒渲染路徑，不會有重入/競爭問題。
    static DWORD g_wrapCharcodeStash = 0;

    // sub_555960（多行文字自動換行核心函式）裡的GetGlyph呼叫
    // （0x555A68）——vtable在edx、charcode在eax、this=ecx。跟量測
    // call site同樣charcode非callee-saved，但vtable在edx，不能像量測
    // 那樣借edx當scratch去覆寫[esp]的charcode，改借eax（此刻eax已經
    // 存進stash，call前把它蓋成placeholder值不影響後面）。
    // 這是純寬度量測用途（判斷斷行位置，不實際畫字），跟量測call site
    // 一樣不呼叫GlyphResultCheck/SynthesizeCJKGlyphRecord以外的側錄函式。
    static __declspec(naked) void GlyphDetourWrap()
    {
        __asm
        {
            ; 借用fontSlotThis stash機制，讓後面GlyphResultCheck還能拿到正確的
            ; fontSlotThis，供font 1~4字級量測用。
            mov  [g_selfRenderFontSlotThisStash], ecx

            ; 分類判斷提前，同主detour。此刻eax還是native charcode，pushad/popad
            ; 包住不受DetermineCategory影響。
            pushad
            call DetermineCategory
            mov  [g_categoryStash], eax
            popad

            pushad
            push dword ptr [g_categoryStash]  ; category
            push 3               ; siteId=換行
            push eax            ; charcode
            push ecx            ; fontSlotThis
            call GlyphCheck
            add  esp, 16
            popad

            mov  [g_wrapCharcodeStash], eax   ; call前charcode還在eax，先存起來

            pushad
            push ecx              ; fontSlotThis
            push eax              ; charcode
            call PreparePlaceholder
            add  esp, 8
            mov  [g_placeholderResult], eax
            popad
            mov  eax, [g_placeholderResult]
            mov  [esp], eax        ; 覆寫native已push好的charcode（vtable在edx不能借，改用eax）

            call dword ptr [edx + 238h]

            pushad
            push dword ptr [g_selfRenderFontSlotThisStash] ; fontSlotThis
            push eax                                ; result
            push 3                                   ; siteId=換行
            push dword ptr [g_wrapCharcodeStash]     ; charcode
            call GlyphResultCheck
            add  esp, 16
            popad

            ; Stage C：category沿用detour最前面存好的g_categoryStash，不用
            ; pushad/popad包住，保留eax給jmp回去用。fontSlotThis參數同主detour。
            push dword ptr [g_selfRenderFontSlotThisStash] ; fontSlotThis
            push dword ptr [g_categoryStash]         ; category
            push eax                                ; nativeResult
            push dword ptr [g_wrapCharcodeStash]     ; codepoint
            call SynthesizeCJKGlyphRecord
            add  esp, 16

            mov  edx, 0x555A6E
            jmp  edx
        }
    }

    // 共用的「byte比對+改成jmp detour」邏輯。modrmByte是call指令第2個byte，
    // 隨vtable來源暫存器不同（eax=0x90、edx=0x92、ebp=0x95）；各call site都是
    // 6-byte `FF <modrm> 38 02 00 00`結構。
    static bool PatchCallSite(DWORD site, BYTE modrmByte, void* detour, const char* label)
    {
        BYTE* p = (BYTE*)site;
        const BYTE kExpected[6] = { 0xFF, modrmByte, 0x38, 0x02, 0x00, 0x00 };
        for (int i = 0; i < 6; i++)
        {
            if (p[i] != kExpected[i])
            {
                Log::Write("[GlyphHook] %s 0x%08X第%d byte是0x%02X，預期0x%02X——版本不符或已被其他patch動過，放棄安裝",
                           label, site, i, p[i], kExpected[i]);
                return false;
            }
        }

        DWORD oldProt = 0;
        VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &oldProt);
        *p = 0xE9;
        *(INT32*)(p + 1) = (INT32)((DWORD)detour - (DWORD)(p + 5));
        p[5] = 0x90;  // NOP，補6-byte原指令跟5-byte jmp之間差的1 byte
        VirtualProtect(p, 6, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), p, 6);

        Log::Write("[GlyphHook] %s 安裝完成：site=0x%08X detour=%p", label, site, detour);
        return true;
    }

    bool Install(HMODULE hModule)
    {
        g_hModule = hModule;  // DetermineCategory()偵測到報紙zip時lazy init要用

        Config::DebugConfig dbg = Config::LoadDebug(hModule);
        g_diagEnable = dbg.glyphDiagEnable;
        g_diagCap    = dbg.glyphDiagCap;
        g_placeholderEnable = dbg.cjkPlaceholderEnable;
        g_overrunProbeCfg   = dbg.glyphOverrunProbe;
        // 這裡（native GetGlyph 通用/報紙合成）只看 [General] NormalFontReplace；
        // 字幕開關在 SubtitleHook 內部快取。
        g_synthEnable = Config::LoadGeneral(hModule).normalFontReplace;

        bool mainOk = PatchCallSite(kVA_CallSite, 0x90, &GlyphDetour, "主call site(0x55A0A0)");
        // 這兩個是額外診斷點，失敗只記log（PatchCallSite內部已處理），不影響
        // 主hook的安裝結果——即使main site版本不符要放棄，還是想知道這兩個
        // 空白候選點的狀況。
        PatchCallSite(kVA_CallSiteSpaceA, 0x92, &GlyphDetourSpaceA, "空白候選點A(0x559A21)");
        PatchCallSite(kVA_CallSiteSpaceB, 0x92, &GlyphDetourSpaceB, "空白候選點B(0x559AFC)");

        // sub_5509D0(主選單排版)、sub_555590(字串寬度量測)、sub_555960(自動換行)
        // 的GetGlyph呼叫點。同樣獨立安裝、失敗不影響mainOk。
        PatchCallSite(kVA_CallSiteMenu, 0x90, &GlyphDetourMenu, "選單call site(0x550BE4)");
        PatchCallSite(kVA_CallSiteMeasure, 0x95, &GlyphDetourMeasure, "寬度量測call site(0x5555C9)");
        PatchCallSite(kVA_CallSiteWrap, 0x92, &GlyphDetourWrap, "換行call site(0x555A68)");

        if (!mainOk) return false;

        Log::Write("[GlyphHook] 主hook diagEnable=%d diagCap=%d placeholderEnable=%d synthEnable=%d overrunProbe=%d",
                   g_diagEnable, g_diagCap, g_placeholderEnable, g_synthEnable, g_overrunProbeCfg);
        return true;
    }
}
