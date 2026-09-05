#pragma once
#include <Windows.h>

// GetGlyph（字型槽vtable+0x238）呼叫點：
// - 0x55A0A0，位於sub_559910（ZSimpleHTML排版主迴圈）
// - 0x550BE4，位於sub_5509D0（結構相同的另一個獨立排版函式，主選單文字實際走這個）
// - 0x5555C9，位於sub_555590（字串寬度量測輔助函式）
// - 0x555A68，位於sub_555960（多行文字自動換行核心函式，engine\zwindows\zlineobj.cpp，
//   逐字元GetGlyph取寬度累加判斷斷行位置，與上面三個是獨立呼叫鏈）
// 另有兩處硬編碼查空白寬度（0x559A21/0x559AFC），由SpaceCheck用來探測報紙字級。
//
// native對未知CJK碼位一律回傳共用「缺字」sentinel記錄（安全、不crash、只是空白）；
// GlyphAtlas能把codepoint光柵化成點陣圖+GDI度量。
//
// Stage C（ini [General] NormalFontReplace控制、預設啟用）：CJK codepoint命中
// GlyphAtlas成功時，用native回傳值(sentinel或真字元記錄，byte layout一樣)當
// byte-template，覆寫glyphIndex/UV/位移/前進寬度等已知欄位，合成一份假glyph記錄
// 取代native回傳指標，讓下游材質註冊(+0x108/+0x130)正常觸發。細節見
// GlyphHook.cpp的SynthesizeCJKGlyphRecord。

namespace GlyphHook
{
    bool Install(HMODULE hModule);

    // g_synthCache（SynthesizeCJKGlyphRecord的合成結果快取，key見GlyphHook.cpp
    // MakeSynthCacheKey()）一合成就永久保留、從不主動失效。但NativeTextureRegistry
    // 在native ReleaseTextures後重新AllocateSlot時不保證拿到同一個頁碼——已合成過
    // 的字若不重跑SynthesizeCJKGlyphRecord，會永遠帶著舊頁碼去取樣一張已清空、
    // 之後也不再更新的材質頁，畫面呈現空白方框。由
    // NativeTextureRegistry::InvalidateAllSlots()呼叫，讓下一次同一個字cache miss、
    // 重新讀取當下真正的nativeIndex。
    void InvalidateSynthCache();

    // 供NewsPaperLineHeightFix.cpp呼叫。sub_559910內判斷<br>/自動換行Y推進量用的
    // var_998（空白字元GetGlyph查詢結果快取），其+0x11(lineHeight)欄位會被同一個
    // 共用buffer的後續字元查詢污染、不可信。這裡改回傳我方對這個fontSlotThis探測過
    // 的native原始+0x11值（未乘1.5，native讀到後才乘）——沒有快取記錄時現場呼叫
    // ProbeNewspaperFontSize探測一次（順便填好g_newspaperSizeCache）。找不到/探測
    // 失敗回傳0，呼叫端應視為「沒有可信數值」，維持native原本行為。
    int GetNewspaperLineHeightForSlot(void* fontSlotThis);
}
