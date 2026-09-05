#pragma once
#include <Windows.h>

// 商店/黑市購買選單 Price label:value 疊字修正。
// 跟LabelWidthFixInventory.cpp（見inventory排版修正.md）同一套機制。
// sub_55E550本體共用的呼叫端裡，只有sub_67DDE0（0x67E175）是label:value
// 並排結構（Price標籤+價格數值），跟inventory bug同樣risk；其餘call site
// 量完寬度是拿去定位游標/單一元素，不是並排比對，不會疊字，故不處理。
//
// patch sub_67DDE0裡這1個呼叫sub_55E550(Price label測寬)的call site，
// detour邏輯跟Inventory版完全一致：先呼叫native本體（child list走訪/
// RTTI篩選/padding疊加邏輯不變），再用我方GlyphAtlas實際渲染寬度取代
// native因誤查vanilla西文字寬表而算錯的CJK部分，疊加回*outBuf。
namespace LabelWidthFixShop
{
    // 固定VA，不依賴任何runtime才建立的物件，DLL載入當下就能裝，不需要
    // lazy retry；hModule只用來讀取[Debug] LabelWidthFixDiagEnable這個診斷
    // log開關（跟LabelWidthFixInventory共用同一個診斷開關，同一套機制）。
    // 是否呼叫Install()由Hook.cpp依[General] NormalFontReplace判斷，跟
    // LabelWidthFixInventory同一個if區塊。
    bool Install(HMODULE hModule);
}
