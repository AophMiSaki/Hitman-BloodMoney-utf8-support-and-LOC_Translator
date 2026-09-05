#pragma once
#include <Windows.h>
#include <d3d9.h>
#include "Config.h"
#include "FontCategory.h"

// Stage B：CJK/ASCII光柵化引擎。用GDI（CreateFontIndirectA+GetGlyphOutlineW）
// 把字元即時轉成8bpp灰階點陣圖，用shelf-pack packing進一張系統記憶體atlas。
//
// CJK字型分三類（FontCategory::General/Subtitle/Newspaper），各自獨立字型/
// atlas/快取。General在Init()無條件建立，狀態必為Ready，是所有fallback的
// 最終目標。Subtitle/Newspaper為lazy init（EnsureCategoryReady），成功或失敗
// 都只嘗試一次，不會每次GetGlyph重試。GetGlyph()對非Ready分類自動fallback成
// General，確保ini字型設定有問題時畫面上仍能正常輸出文字、不開天窗。呼叫端要
// 準備UV/atlas buffer/材質時，須呼叫ResolveCategory()取得跟GetGlyph()內部一致
// 的分類判斷，避免兩邊fallback邏輯分歧、UV座標搭到錯誤的atlas buffer。
//
// 只負責「codepoint -> 點陣圖+GDI度量」這一層，不碰D3D9（材質建立/上傳是
// 後續階段的工作）。

namespace GlyphAtlas
{
    struct Entry
    {
        int width;          // 點陣圖寬度（pixel，緊貼字形黑盒），0表示空白/零寬字元
        int height;         // 點陣圖高度（pixel，緊貼字形黑盒）
        int cellWidth;       // atlas裡實際配置給這個字的cell寬度——向上取整對齊到
                              // 「UV byte量化格線」（見GlyphAtlas.cpp kUVGranularity），
                              // 一定≥width。Stage C合成UV矩形/quad尺寸要用這個而不是
                              // width，否則UV byte精度不足會把字形邊緣裁掉。
        int cellHeight;       // 同上，高度
        int atlasX;         // 在atlas緩衝區裡的左上角X（已對齊UV量化格線）
        int atlasY;         // 在atlas緩衝區裡的左上角Y（已對齊UV量化格線）
        GLYPHMETRICS gm;    // GDI原生度量：gmBlackBoxX/Y、gmptGlyphOrigin.x/y、gmCellIncX/Y
    };

    // 讀取Config::FontConfig，建立通用分類（FontCategory::General）的GDI
    // 字型與量測用DC。程式一開機就呼叫，General狀態必為Ready，是所有
    // fallback的最終目標。
    void Init(HMODULE hModule);

    // 字幕/報紙分類的lazy init：第一次真的用到該分類前呼叫（
    // SubtitleBrief第一次要畫字幕前、native GetGlyph call site偵測到
    // 目前zip是報紙時），冪等——已經是Ready或Failed狀態時直接return，不會
    // 每次GetGlyph命中都重試一次昂貴的GDI字型建立。對General呼叫這個函式
    // 是no-op（General一律走Init()）。
    void EnsureCategoryReady(HMODULE hModule, FontCategory category);

    // 該分類是否真的是Ready狀態（不是fallback到General）——供
    // NativeTextureRegistry/TextureUpload判斷要不要花一個額外材質槽/材質
    // 登記這份atlas內容。
    bool IsCategoryReady(FontCategory category);

    // 求出GetGlyph(codepoint, category)實際會使用的分類：category本身
    // Ready就回傳category，否則回傳FontCategory::General。呼叫端要準備
    // UV/atlas buffer/材質時務必呼叫這個函式取得跟GetGlyph()內部一致的
    // 判斷結果，不要自己另外判斷IsCategoryReady()，避免兩邊fallback邏輯
    // 分歧、UV座標搭配到錯誤的atlas buffer。
    FontCategory ResolveCategory(FontCategory category);

    // 查詢/光柵化指定codepoint字圖（ASCII/CJK共用）。category未Ready時
    // 自動fallback成General（見上方ResolveCategory()）。成功回傳非null
    // （指標由GlyphAtlas內部持有，呼叫端不用/不可free）；atlas空間耗盡或
    // GDI呼叫失敗回傳null。目前每個分類只有單一atlas頁（1024x1024 A8），
    // 滿了不會自動擴充第二頁——見GlyphAtlas.cpp頂部註解。
    const Entry* GetGlyph(unsigned int codepoint, FontCategory category = FontCategory::General);

    // 指定分類的atlas緩衝區本身（8bpp灰階，A8語意），Stage D上傳D3D9材質
    // 時直接整塊讀取。呼叫端應先用ResolveCategory()決定實際要讀哪個分類。
    const BYTE* GetAtlasBuffer(FontCategory category = FontCategory::General);
    // atlas尺寸是所有分類共用的編譯期常數，不分分類。
    int GetAtlasWidth();
    int GetAtlasHeight();

    // 指定分類每次有新glyph被光柵化進atlas就遞增，Stage D可以用來判斷要不要
    // 重新上傳材質（只在有變動時才做UpdateTexture/LockRect，避免每幀都重傳
    // 整張atlas）。
    DWORD GetGeneration(FontCategory category = FontCategory::General);

    // 取得指定分類目前的「字距」設定(Config::FontConfig.spacing，ini
    // FontSpacing=)，已經套用該分類當下的render scale（跟RebuildFont()建字型
    // 時lfWidth的縮放邏輯一致，見GlyphAtlas.cpp GetRenderScaleFor()）。單位是
    // 最終裝置pixel，呼叫端直接加進自己的width/advance計算即可，不用重複算
    // scale。category未Ready時比照GetGlyph()一樣fallback成General的設定值。
    int GetSpacing(FontCategory category = FontCategory::General);

    // D3D9裝置指標存取，供TextureUpload等其他模組共用，不用各自重複解同一條
    // this鏈。裝置尚未建立（DLL剛載入、遊戲還沒進入D3D9初始化階段）時回傳
    // nullptr，呼叫端lazy retry。全部分類共用同一個全域D3D9裝置，不分slot。
    IDirect3DDevice9* GetD3D9Device();

    // [Debug] GlyphOverrunProbe：報紙 CJK 字圖合成溢位定位側錄。
    // GlyphHook::DetermineCategory() 每次依「開關 && 目前在報紙/postmission
    // 畫面」呼叫 SetOverrunProbe()；GetGlyphInSlot/BlitGray8 與 GlyphHook 端
    // 據 OverrunProbeOn() 決定要不要印。純側錄。
    void SetOverrunProbe(bool on);
    bool OverrunProbeOn();
}
