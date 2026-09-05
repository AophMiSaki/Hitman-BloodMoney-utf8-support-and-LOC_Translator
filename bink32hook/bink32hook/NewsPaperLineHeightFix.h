#pragma once
#include <Windows.h>

// 修復報紙標題疊字bug的Y軸根因。與NewsPaperSpaceAdvanceFix同源：native把
// var_998（函式進入/<font N>切換時快取的glyph記錄指標）的+0x11欄位乘1.5
// 當「這一行字高」用，該指標指向的共用buffer會被後續一般字元的GetGlyph
// 查詢覆寫，讀到的不是目前作用中字型的行高而是殘留舊值，偏小導致<br>換行
// 的Y推進量趨近0，兩行標題疊在同一Y。
//
// 修法：inline patch兩個消費點（0x559F10、0x559FB2，各8 bytes，讀
// var_998+0x11）。detour判斷是報紙分類時，重新推導目前字型槽指標
// (fontSlotThis，跟NewsPaperSpaceAdvanceFix.cpp同一套var_7D0[0x210]/
// var_7CC[0x214]/var_610[0x3D0] stack offset)，改查
// GlyphHook::GetNewspaperLineHeightForSlot()——回傳我方在每次<font N>切換時
// 對該槽探測到的native原始+0x11值（未乘1.5，native讀到後自己會乘，銜接
// 一致）。查無/探測失敗(回傳0)或非報紙分類：照native原本兩句指令執行。
namespace NewsPaperLineHeightFix
{
    // 固定VA，不依賴任何runtime才建立的物件，DLL載入當下就能裝，不需要
    // lazy retry。
    bool Install();
}
