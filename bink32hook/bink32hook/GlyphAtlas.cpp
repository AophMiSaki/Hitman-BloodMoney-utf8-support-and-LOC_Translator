#include "GlyphAtlas.h"
#include "Log.h"
#include "Config.h"
#include <unordered_map>
#include <vector>
#include <cstring>
#include <d3d9.h>

// 設計取捨：
// - 光柵化用GDI GetGlyphOutlineW(GGO_GRAY8_BITMAP)，回傳0~64灰階，線性縮放成
//   0~255當A8 alpha。不用FreeType/stb_truetype。
// - 字型來源是使用者系統已安裝字型（Config::FontConfig.face），不隨附TTF。
// - 每個分類只有單一1024x1024 atlas頁，shelf-pack（逐排塞、塞不下換下一排），
//   滿了不會自動擴充第二頁。
// - 解析度動態縮放：GDI字型大小以桌面螢幕像素為單位，但遊戲實際渲染解析度
//   （D3D9 viewport）可能不同（視窗模式、非原生解析度的exclusive fullscreen），
//   用「D3D9目前viewport高度 / 桌面螢幕高度」動態算縮放係數。
// - g_slots[FontCategory::Count]陣列，每分類一份狀態。General（index 0）在Init()
//   無條件建立、必為Ready；Subtitle/Newspaper為lazy init，只嘗試一次；GetGlyph
//   對非Ready分類自動fallback回General。

namespace GlyphAtlas
{
    static const int kAtlasSize = 1024;
    static const int kGlyphPadding = 1;  // 相鄰glyph間至少留1px空隙，避免材質bilinear取樣滲色

    // native消費UV byte時固定×1/128換算成float，代表這個byte格式的定址精度是
    // 「atlas尺寸/128」px/單位（1024atlas=8px/單位）。shelf-pack配置cell時要
    // 對齊到這個格線，UV編碼時才不會因為四捨五入把字形內容裁掉（CJK字圖只有
    // 10~12px寬高，未對齊時裁切非常明顯）。
    static const int kUVGranularity = kAtlasSize / 128;

    static int AlignUp(int v, int granularity)
    {
        return ((v + granularity - 1) / granularity) * granularity;
    }

    // 每個FontCategory各自的初始化狀態。NotLoaded=從未嘗試過（Subtitle/
    // Newspaper初始值）；Ready=可以正常使用；Failed=嘗試過但初始化本身失敗
    // （例如CreateCompatibleDC失敗）。GDI對不存在的face name不會回傳失敗、
    // 只會靜默替換字型，沒有可靠手段偵測這種「字型被替換」情形，所以
    // RebuildFont成功執行完就視為Ready，Failed只涵蓋更前面的DC建立失敗這種
    // 明確錯誤。
    enum class AtlasState { NotLoaded, Ready, Failed };

    struct SlotState
    {
        BYTE atlas[kAtlasSize * kAtlasSize] = {};
        DWORD generation = 0;

        HFONT font = nullptr;
        HDC   dc = nullptr;

        Config::FontConfig baseCfg = {};  // ini讀到的原始（未經解析度縮放）設定
        float lastScale = -1.0f;          // 上次套用的縮放係數，-1代表尚未建立過字型

        int shelfY = 0;
        int shelfHeight = 0;
        int cursorX = 0;
        bool full = false;

        AtlasState state = AtlasState::NotLoaded;

        std::unordered_map<unsigned int, Entry> cache;
    };

    static SlotState g_slots[(size_t)FontCategory::Count];

    static SlotState& SlotFor(FontCategory category) { return g_slots[(size_t)category]; }

    // [Debug] GlyphOverrunProbe：GlyphHook::DetermineCategory() 依「開關 && 目前
    // 在報紙/postmission 畫面」每次設定；GetGlyphInSlot/BlitGray8 印溢位定位側錄用。
    static bool g_overrunProbe = false;

    // 0x90AF0C是一個wrapper物件的指標，wrapper+0x04才是真正的
    // IDirect3DDevice9*。DLL載入當下這個裝置必定還不存在，要lazy retry（每次
    // GetGlyph呼叫都重新檢查）。整個引擎共用單一全域裝置，3個字型分類共用同
    // 一份GetRenderScale()結果。
    static const DWORD kVA_D3D9WrapperPtr = 0x0090AF0C;

    IDirect3DDevice9* GetD3D9Device()
    {
        DWORD wrapper = *(DWORD*)kVA_D3D9WrapperPtr;
        if (!wrapper) return nullptr;  // wrapper物件還沒建立
        return *(IDirect3DDevice9**)(wrapper + 4);
    }

    static float GetRenderScale()
    {
        IDirect3DDevice9* device = GetD3D9Device();
        if (!device) return 1.0f;  // 裝置還沒建立，先用1.0（不縮放），下次呼叫再檢查

        D3DVIEWPORT9 vp = {};
        if (FAILED(device->GetViewport(&vp)) || vp.Height == 0)
            return 1.0f;

        int desktopHeight = GetSystemMetrics(SM_CYSCREEN);
        if (desktopHeight <= 0) return 1.0f;

        return (float)vp.Height / (float)desktopHeight;
    }

    // native排版參考高度是固定量，不隨遊戲目前解析度縮放；報紙分類的CJK字型
    // 如果再套用GetRenderScale()，實際pixel字級會隨解析度飄動、跟native參考值
    // 脫鉤，讓ini端clamp的下限失去意義。報紙分類固定用scale=1.0（ini設的
    // FontSize直接就是最終GDI字級），通用/字幕分類維持解析度縮放行為。
    static float GetRenderScaleFor(FontCategory category)
    {
        if (IsNewspaperCategory(category)) return 1.0f;
        return GetRenderScale();
    }

    static const char* CategoryLabel(FontCategory category)
    {
        switch (category)
        {
        case FontCategory::General:         return "通用";
        case FontCategory::Subtitle:        return "字幕";
        case FontCategory::NewspaperFont1: return "報紙font1";
        case FontCategory::NewspaperFont2: return "報紙font2";
        case FontCategory::NewspaperFont3: return "報紙font3";
        case FontCategory::NewspaperFont4: return "報紙font4";
        default:                             return "?";
        }
    }

    // 依目前縮放係數(重新)建立GDI字型。scale改變時（含裝置剛就緒、或使用者
    // 遊戲內切換解析度）才會被呼叫——不同scale下glyph實際pixel大小不同，
    // 舊atlas內容/快取全部作廢，清空重來。呼叫前s.baseCfg必須已經設好。
    static void RebuildFont(FontCategory category, float scale)
    {
        SlotState& s = SlotFor(category);

        // 錯誤修正：s.font此刻仍被選入s.dc（上次RebuildFont的SelectObject留下的），
        // GDI不允許刪除目前被DC選中的物件，直接DeleteObject會silent失敗、舊HFONT
        // 變孤兒控制代碼洩漏——每次scale變動（裝置剛就緒/切解析度/切全螢幕）都會
        // 洩漏一個。刪除前先把DC選回一個不需要釋放的stock字型，讓s.font先被deselect。
        if (s.font)
        {
            SelectObject(s.dc, GetStockObject(SYSTEM_FONT));
            DeleteObject(s.font);
        }

        // 報紙分類套用每font級獨立的最小pixel下限（Config::
        // NewspaperMinRenderSizeFor()），避免縮放後字級過小。
        int pxHeight = (int)(s.baseCfg.size * scale + 0.5f);
        if (IsNewspaperCategory(category))
        {
            const int minSize = Config::NewspaperMinRenderSizeFor(category);
            if (pxHeight < minSize) pxHeight = minSize;
        }

        LOGFONTA lf = {};
        lf.lfHeight = -pxHeight;
        lf.lfWidth  = (int)(s.baseCfg.width * scale + 0.5f);
        lf.lfWeight = s.baseCfg.weight;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfOutPrecision = OUT_TT_ONLY_PRECIS;
        lf.lfQuality = ANTIALIASED_QUALITY;
        (void)lstrcpynA(lf.lfFaceName, s.baseCfg.face, LF_FACESIZE);

        s.font = CreateFontIndirectA(&lf);
        SelectObject(s.dc, s.font);

        // GDI對不存在的字型名稱不會回傳失敗，而是靜默替換成其他字型——印出
        // 實際選中的字型名稱，方便比對ini設定的face是否真的生效。
        char actualFace[LF_FACESIZE] = {};
        GetTextFaceA(s.dc, LF_FACESIZE, actualFace);

        // GetTextFaceA回傳系統ANSI字碼頁(CP_ACP) bytes，不是UTF-8——選中的
        // 字型若有中文名稱（CJK字型常見），直接塞進Log::Write的%s會跟log檔其
        // 餘UTF-8內容混在一起變亂碼。先轉成UTF-8再log。
        char actualFaceUtf8[LF_FACESIZE * 3] = {};
        {
            wchar_t wbuf[LF_FACESIZE] = {};
            int wlen = MultiByteToWideChar(CP_ACP, 0, actualFace, -1, wbuf, LF_FACESIZE);
            if (wlen > 0)
                WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, actualFaceUtf8, sizeof(actualFaceUtf8), nullptr, nullptr);
        }

        memset(s.atlas, 0, sizeof(s.atlas));
        s.cache.clear();
        s.shelfY = s.shelfHeight = s.cursorX = 0;
        s.full = false;
        s.generation++;

        Log::Write("[GlyphAtlas] RebuildFont[%s]：scale=%.3f 要求face=%s -> 實際face=%s size=%d(原始%d) width=%d(原始%d) weight=%d",
                   CategoryLabel(category), scale, s.baseCfg.face, actualFaceUtf8,
                   -lf.lfHeight, s.baseCfg.size, lf.lfWidth, s.baseCfg.width, s.baseCfg.weight);

        s.lastScale = scale;
    }

    void Init(HMODULE hModule)
    {
        SlotState& s = SlotFor(FontCategory::General);
        s.baseCfg = Config::Load(hModule);

        s.dc = CreateCompatibleDC(nullptr);  // 只用來當GetGlyphOutlineW的裝置內容，不需要實際繪圖表面
        SetMapMode(s.dc, MM_TEXT);

        // D3D9裝置這時候幾乎必定還沒建立，先用scale=1.0建立一版字型讓GetGlyph
        // 至少能動作；GetGlyph每次呼叫都會重新檢查scale，裝置就緒後會自動用
        // 正確scale重建字型跟atlas。
        RebuildFont(FontCategory::General, 1.0f);
        s.state = AtlasState::Ready;

        Log::Write("[GlyphAtlas] Init完成：atlasSize=%dx%d（實際scale會在D3D9裝置就緒後自動校正，見RebuildFont log）",
                   kAtlasSize, kAtlasSize);
    }

    void EnsureCategoryReady(HMODULE hModule, FontCategory category)
    {
        if (category == FontCategory::General) return;  // General一律走Init()

        SlotState& s = SlotFor(category);
        if (s.state != AtlasState::NotLoaded) return;  // 已經Ready或Failed，不重試

        s.baseCfg = Config::LoadForCategory(hModule, category);
        s.dc = CreateCompatibleDC(nullptr);
        if (!s.dc)
        {
            s.state = AtlasState::Failed;
            Log::Write("[GlyphAtlas] EnsureCategoryReady[%s]失敗：CreateCompatibleDC失敗，fallback回通用分類",
                       CategoryLabel(category));
            return;
        }
        SetMapMode(s.dc, MM_TEXT);

        RebuildFont(category, GetRenderScaleFor(category));
        // CreateFontIndirectA對不存在的face會靜默替換、不會回傳NULL/失敗，
        // 目前沒有可靠手段偵測「字型真的建立失敗」，先一律視為成功——
        // 之後如果找到判定方式（例如比對actualFace跟要求face完全不同時
        // 判定為替換失敗），再補上Failed分支。
        s.state = AtlasState::Ready;

        Log::Write("[GlyphAtlas] EnsureCategoryReady[%s]完成，狀態=Ready", CategoryLabel(category));
    }

    bool IsCategoryReady(FontCategory category)
    {
        return SlotFor(category).state == AtlasState::Ready;
    }

    FontCategory ResolveCategory(FontCategory category)
    {
        if (category == FontCategory::General) return FontCategory::General;
        return IsCategoryReady(category) ? category : FontCategory::General;
    }

    // 把GDI GGO_GRAY8_BITMAP輸出（每pixel 0~64灰階值）轉成0~255 alpha，寫進
    // 指定字型槽atlas緩衝區的(dstX, dstY)位置。srcPitch是GDI回傳的列距
    // （4-byte對齊）。
    static void BlitGray8(SlotState& s, const BYTE* src, int srcPitch, int width, int height, int dstX, int dstY)
    {
        // 兩道防呆，任一命中就整塊跳過（該字圖不顯示，安全）：
        //   1. src==nullptr 或 width/height 非正：來源點陣圖是退化結果。GDI
        //      GetGlyphOutlineW 對某些字元會回 size=0（沒有點陣圖 bytes），
        //      呼叫端配出的 std::vector<BYTE>(0) 其 .data() 為 nullptr，這裡
        //      若往下讀 srow[x] 就是解參考 null。實機觸發字元：U+000B(VT)。
        //   2. dst 落點越界：會寫穿 s.atlas（g_slots[] 全域陣列）踩壞相鄰 slot。
        if (src == nullptr || width <= 0 || height <= 0 ||
            dstX < 0 || dstY < 0 ||
            dstX + width > kAtlasSize || dstY + height > kAtlasSize)
        {
            if (g_overrunProbe)
                Log::Write("[GlyphOverrunProbe][BlitGray8] 落點/來源異常 dst=(%d,%d) wh=%dx%d pitch=%d src=%p atlas=%d — 跳過",
                           dstX, dstY, width, height, srcPitch, (const void*)src, kAtlasSize);
            return;
        }
        for (int y = 0; y < height; y++)
        {
            const BYTE* srow = src + y * srcPitch;
            BYTE* drow = s.atlas + (dstY + y) * kAtlasSize + dstX;
            for (int x = 0; x < width; x++)
            {
                BYTE v = srow[x];
                if (v > 64) v = 64;                // GGO_GRAY8_BITMAP規格保證0~64，防呆用
                drow[x] = (BYTE)((v * 255) / 64);   // 線性縮放到0~255
            }
        }
    }

    // 預設可忽略／零寬格式字元。字型多半沒有對應glyph，GetGlyphOutlineW會回
    // .notdef（畫面上是方框），且blackBox非0會繞過下方零墨跡防呆。實機：
    // U+200B在Noto Sans Mono CJK TC畫成16x15方框（實機驗證過）。這些
    // 字元一律當零寬零墨跡處理，不進atlas、advance記0。
    static bool IsZeroWidthFormatChar(unsigned int cp)
    {
        return cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0x200E || cp == 0x200F
            || (cp >= 0x202A && cp <= 0x202E)
            || cp == 0x2060 || cp == 0xFEFF;
    }

    // 真正的單一分類光柵化實作，category必須已經是ResolveCategory()解析後的
    // 有效分類（Ready狀態，或呼叫端明知是General）。GetGlyph()負責fallback
    // 判斷，這裡不重複判斷。
    static const Entry* GetGlyphInSlot(FontCategory category, unsigned int codepoint)
    {
        SlotState& s = SlotFor(category);

        float scale = GetRenderScaleFor(category);
        if (scale != s.lastScale)
            RebuildFont(category, scale);

        auto it = s.cache.find(codepoint);
        if (it != s.cache.end())
            return &it->second;

        if (s.full)
            return nullptr;  // atlas已滿，之前已經記過一次log，不再重複嘗試

        // 零寬格式字元（U+200B 等，見 IsZeroWidthFormatChar）：零寬零墨跡，
        // 不進atlas、advance記0，避免GDI回.notdef方框被畫出來。
        if (IsZeroWidthFormatChar(codepoint))
        {
            Entry& e = s.cache[codepoint];
            e.width = 0; e.height = 0; e.cellWidth = 0; e.cellHeight = 0;
            e.atlasX = 0; e.atlasY = 0;
            e.gm = GLYPHMETRICS{};
            return &e;
        }

        // 空白／控制字元沒有可見墨跡，直接用GetCharWidth32W查前進寬度即可，不
        // 呼叫GetGlyphOutlineW（GGO_GRAY8_BITMAP對零墨跡字元的行為在GDI上不
        // 穩定：實機證實 U+0020 會整個崩潰；U+000B 會回 size=0 但 blackBox=1x1，
        // 繞過下方的零寬防呆，導致用 nullptr 來源 blit）。
        // 排除範圍：所有 C0 控制字元 U+0000–U+001F（含 TAB 0x09 / LF 0x0A /
        // CR 0x0D / VT 0x0B / FF 0x0C 等）、半形空格 U+0020、DEL 0x7F。
        if (codepoint <= 0x20 || codepoint == 0x7F)
        {
            INT advance = 0;
            GetCharWidth32W(s.dc, codepoint, codepoint, &advance);

            // 側錄診斷：印出空白字元的raw advance、其byte低8位元、此刻s.dc實際
            // 選中的字型與字級，用來判斷報紙內文空白字元X游標異常回跳時，是
            // GetCharWidth32W查出異常advance，還是s.dc當下選錯字型/字級。
            {
                char actualFaceDiag[LF_FACESIZE] = {};
                GetTextFaceA(s.dc, LF_FACESIZE, actualFaceDiag);
                char actualFaceDiagUtf8[LF_FACESIZE * 3] = {};
                wchar_t wbufDiag[LF_FACESIZE] = {};
                int wlenDiag = MultiByteToWideChar(CP_ACP, 0, actualFaceDiag, -1, wbufDiag, LF_FACESIZE);
                if (wlenDiag > 0)
                    WideCharToMultiByte(CP_UTF8, 0, wbufDiag, -1, actualFaceDiagUtf8, sizeof(actualFaceDiagUtf8), nullptr, nullptr);
                // 用scale_x1000整數格式，避免執行期間locale被設成danish時%f
                // 被CRT locale-aware格式化成逗號小數點。
                Log::Write("[GlyphAtlas][WhitespaceAdvanceDiag] category=%s U+%04X advance=%d (byte低8位元=%d) 目前DC選中字型=%s baseCfg.size=%d scale_x1000=%d",
                           CategoryLabel(category), codepoint, advance, (BYTE)advance, actualFaceDiagUtf8, s.baseCfg.size, (int)(scale * 1000.0f + 0.5f));
            }

            Entry& e = s.cache[codepoint];
            e.width = 0; e.height = 0; e.cellWidth = 0; e.cellHeight = 0;
            e.atlasX = 0; e.atlasY = 0;
            e.gm = GLYPHMETRICS{};
            e.gm.gmCellIncX = advance;
            return &e;
        }

        GLYPHMETRICS gm = {};
        MAT2 mat = { {0,1},{0,0},{0,0},{0,1} };  // 單位矩陣，不做額外縮放/斜切

        DWORD size = GetGlyphOutlineW(s.dc, codepoint, GGO_GRAY8_BITMAP, &gm, 0, nullptr, &mat);
        if (size == GDI_ERROR)
        {
            if (Log::IsTraceWindowActive())
                Log::Write("[GlyphAtlas] GetGlyphOutlineW量測失敗：U+%04X（字型可能不含這個字）", codepoint);
            return nullptr;
        }

        int width  = (int)gm.gmBlackBoxX;
        int height = (int)gm.gmBlackBoxY;

        if (size == 0 || width == 0 || height == 0)
        {
            // 沒有點陣圖內容的字元：不佔atlas空間，度量（尤其gmCellIncX，斷行/
            // 前進寬度會用到）仍然有效。三種情況：
            //   - 全形空白（U+3000 等）：blackBox 為 0x0（U+200B 等零寬格式字元
            //     已在上方 IsZeroWidthFormatChar 先攔掉）。
            //   - GDI 退化結果：size=0 但 blackBox 非 0（實機 U+000B 回 size=0
            //     blackBox=1x1）。若往下走會配一個 0-byte 的 std::vector，
            //     .data()==nullptr，blit 時解參考 null 崩潰。
            //   - 上方 codepoint<=0x20 / 0x7F 已先攔掉控制字元，這裡是第二道。
            Entry& e = s.cache[codepoint];
            e.width = 0; e.height = 0; e.cellWidth = 0; e.cellHeight = 0;
            e.atlasX = 0; e.atlasY = 0; e.gm = gm;
            return &e;
        }

        // cell尺寸向上取整對齊到kUVGranularity（見上方常數註解），保證
        // atlasX/atlasY、以及atlasX+cellWidth/atlasY+cellHeight這兩個UV
        // 矩形端點換算成UV byte時完全落在量化格線上、沒有四捨五入誤差，
        // 不會裁掉字形內容——代價是cell比緊貼字形bounding box稍大，多出
        // 的邊界是atlas裡未寫入內容的透明像素，畫面上只是glyph周圍多一點
        // 看不見的留白，不影響字形本身。
        int cellWidth  = AlignUp(width  + kGlyphPadding, kUVGranularity);
        int cellHeight = AlignUp(height + kGlyphPadding, kUVGranularity);

        // shelf-pack：目前shelf塞不下就換行。用cellWidth/cellHeight（已含
        // padding、已對齊格線）算，換行時shelfY只加cellHeight，下一排的shelfY
        // 才會依然停在格線上。
        // 邊界用>=而非>：toUVByte把UV(0~1)×128存成signed byte，上限127對應
        // uv=127/128，無法精確表示uv=1.0（會被clamp成127）。若cell右/下緣剛好
        // 落在kAtlasSize（atlasX+cellWidth==kAtlasSize），uv正好是1.0、被clamp
        // 掉最後一個kUVGranularity單位，造成UV取樣範圍比quad顯示尺寸窄、字形
        // 被拉伸變糊。用>=提前換行/判滿，讓cell右/下緣永遠不會頂到kAtlasSize。
        if (s.cursorX + cellWidth >= kAtlasSize)
        {
            s.shelfY += s.shelfHeight;
            s.shelfHeight = 0;
            s.cursorX = 0;
        }
        if (s.shelfY + cellHeight >= kAtlasSize)
        {
            if (Log::IsTraceWindowActive())
                Log::Write("[GlyphAtlas] atlas空間耗盡[%s]（%dx%d），U+%04X起無法再新增glyph",
                           CategoryLabel(category), kAtlasSize, kAtlasSize, codepoint);
            s.full = true;
            return nullptr;
        }

        std::vector<BYTE> bitmap(size);
        DWORD written = GetGlyphOutlineW(s.dc, codepoint, GGO_GRAY8_BITMAP, &gm, size, bitmap.data(), &mat);
        if (written == GDI_ERROR)
        {
            if (Log::IsTraceWindowActive())
                Log::Write("[GlyphAtlas] GetGlyphOutlineW取點陣圖失敗：U+%04X", codepoint);
            return nullptr;
        }

        int srcPitch = (width + 3) & ~3;  // GGO_GRAY8_BITMAP每列4-byte對齊

        int dstX = s.cursorX;
        int dstY = s.shelfY;

        // [Debug] GlyphOverrunProbe：報紙字圖合成溢位定位——印量測 size、blackBox、
        // 對齊後 cell、srcPitch、dst 落點與 kAtlasSize 的關係。純側錄。
        if (g_overrunProbe)
            Log::Write("[GlyphOverrunProbe][GetGlyph] U+%04X cat=%d size=%u bbox=%dx%d wh=%dx%d cell=%dx%d pitch=%d dst=(%d,%d) dstEnd=(%d,%d) atlas=%d cursorX=%d shelfY=%d shelfH=%d bmp=%zu",
                       codepoint, (int)category, size, (int)gm.gmBlackBoxX, (int)gm.gmBlackBoxY,
                       width, height, cellWidth, cellHeight, srcPitch,
                       dstX, dstY, dstX + width, dstY + height, kAtlasSize,
                       s.cursorX, s.shelfY, s.shelfHeight, bitmap.size());

        BlitGray8(s, bitmap.data(), srcPitch, width, height, dstX, dstY);

        s.cursorX += cellWidth;
        if (cellHeight > s.shelfHeight) s.shelfHeight = cellHeight;

        Entry& e = s.cache[codepoint];
        e.width = width; e.height = height;
        e.cellWidth = cellWidth; e.cellHeight = cellHeight;
        e.atlasX = dstX; e.atlasY = dstY;
        e.gm = gm;
        s.generation++;

        if (Log::IsTraceWindowActive())
            Log::Write("[GlyphAtlas] 光柵化完成[%s]：U+%04X -> atlas(%d,%d) %dx%d cell=%dx%d blackBox=%dx%d origin=(%d,%d) cellInc=(%d,%d)",
                       CategoryLabel(category), codepoint, dstX, dstY, width, height, cellWidth, cellHeight,
                       (int)gm.gmBlackBoxX, (int)gm.gmBlackBoxY, gm.gmptGlyphOrigin.x, gm.gmptGlyphOrigin.y,
                       gm.gmCellIncX, gm.gmCellIncY);

        return &e;
    }

    const Entry* GetGlyph(unsigned int codepoint, FontCategory category)
    {
        return GetGlyphInSlot(ResolveCategory(category), codepoint);
    }

    void SetOverrunProbe(bool on) { g_overrunProbe = on; }
    bool OverrunProbeOn() { return g_overrunProbe; }

    // 見GlyphAtlas.h宣告處註解。用ResolveCategory()fallback到跟GetGlyph()一致
    // 的分類，再用該分類目前的scale縮放baseCfg.spacing，邏輯跟RebuildFont()裡
    // lfWidth的縮放算法對齊。
    int GetSpacing(FontCategory category)
    {
        FontCategory resolved = ResolveCategory(category);
        SlotState& s = SlotFor(resolved);
        float scale = GetRenderScaleFor(resolved);
        return (int)(s.baseCfg.spacing * scale + 0.5f);
    }

    const BYTE* GetAtlasBuffer(FontCategory category) { return SlotFor(ResolveCategory(category)).atlas; }
    int GetAtlasWidth()  { return kAtlasSize; }
    int GetAtlasHeight() { return kAtlasSize; }
    DWORD GetGeneration(FontCategory category) { return SlotFor(ResolveCategory(category)).generation; }
}
