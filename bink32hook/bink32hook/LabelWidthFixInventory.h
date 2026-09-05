#pragma once
#include <Windows.h>

// Inventory武器檢視面板 label:value 疊字修正。
// 完整root cause/設計見G:\bloodmoney\md\inventory排版修正.md。
//
// patch sub_681F10(UpdateItemInfo)內2個呼叫sub_55E550(label測寬)的call
// site，detour一律先呼叫native本體（child list走訪/RTTI篩選/padding疊加
// 邏輯完全不變），再用我方GlyphAtlas實際渲染寬度取代native因誤查vanilla
// 西文字寬表而算錯的CJK部分，疊加回*outBuf，讓value widget不會蓋住CJK
// label尾端。
namespace LabelWidthFixInventory
{
    // 固定VA，不依賴任何runtime才建立的物件，DLL載入當下就能裝，不需要lazy
    // retry；hModule只用來讀取[Debug] LabelWidthFixDiagEnable這個診斷log開關。
    // 是否呼叫Install()由Hook.cpp依[General] NormalFontReplace判斷（=0時沒有
    // 合成字、疊字不會發生，整個不安裝）。
    bool Install(HMODULE hModule);
}
