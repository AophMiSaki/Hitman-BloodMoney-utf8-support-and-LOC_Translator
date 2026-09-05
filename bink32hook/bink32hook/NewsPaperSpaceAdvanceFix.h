#pragma once
#include <Windows.h>

// 修復報紙全篇疊字/文字散亂bug的X軸根因。
//
// 根因：sub_559910（ZSimpleHTML主排版迴圈）對字面空白字元(U+0020)的處理
// (case 32)直接讀一個「函式進入時（或<font N>標籤切換時）就快取住」的glyph
// 記錄指標(v98，stack offset var_998/[esp+0x48])的+0x10欄位當成X游標推進
// 量，之後整段文字（可能好幾行、幾十個空白）全部共用這一個指標，不會每次
// 空白重新呼叫GetGlyph。反觀一般字元每個都會重新呼叫一次
// GetGlyph(fontSlot, charcode)。native GetGlyph用同一份可覆寫的共用buffer
// 回傳記錄——代表這個一開始快取住的指標，在後續一般字元的GetGlyph呼叫發生
// 後，指向的記憶體內容會被覆寫成「最後一個查詢字元」自己的記錄，之後任何
// 空白字元讀到的並非空白自己的寬度，而是殘留的別人的欄位值。
//
// 修法：inline patch case32的2句讀取指令(0x559DB8起8 bytes)，改成5-byte
// E9 jmp+3-byte NOP跳到detour。detour先判斷是否為報紙分類（ZipPathTrace
// 偵測到_news.zip/_postmission.zip，跟NewsPaperImageWrapHook.cpp同一套判斷
// 邏輯，這裡刻意不共用避免耦合）：
//   - 是報紙分類：原地重跑native在<font>標籤切換時同樣的「找目前字型槽→
//     呼叫GetGlyph(vtable+0x238, 32)」查詢（stack offset var_7D0[esp+0x210]/
//     var_7CC[esp+0x214]/var_610[esp+0x3D0]），拿到「當下真正新鮮」的glyph
//     記錄指標，讀+0x10欄位——不寫回var_998快取（刻意不動，範圍收斂，
//     +0x11行高欄位由NewsPaperLineHeightFix另行處理）。
//   - 非報紙分類：完全照native原本的兩句指令執行，行為不變。
// sub_559910是ZSimpleHTML通用排版函式，不是報紙專用，gating避免影響選單/
// 簡報等其他畫面既有的行為，跟NewsPaperImageWrapHook的既有原則一致。
namespace NewsPaperSpaceAdvanceFix
{
    // 固定VA，不依賴任何runtime才建立的物件，DLL載入當下（跟GlyphHook同一
    // 批）就能裝，不需要lazy retry。
    bool Install();
}
