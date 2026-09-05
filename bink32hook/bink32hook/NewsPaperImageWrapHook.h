#pragma once
#include <Windows.h>

// 修復報紙IMG文繞圖失效（文字蓋到TargetPic/PoliceSketch圖片）。
//
// 根因：sub_559910（ZSimpleHTML主排版迴圈）裡3處呼叫sub_5585E0（決定
// 「這一行可用多寬」）全部硬編碼傳參數0，導致繞圖判斷永遠只看<img>解析
// 當下寫入的固定快照，跟目前這一行實際畫到的Y座標（本地變數v122）脫節。
// 姊妹函式sub_558340用同一套算式但呼叫端有正確傳入v122、運作正常。
//
// 修法：inline patch這3個call site（5-byte E8 call改成E9 jmp到detour），
// detour內判斷目前是否為報紙分類（ZipPathTrace偵測_news.zip/_postmission
// .zip），是的話把傳入sub_5585E0的參數從0改成(int)v122（用game本身的
// __ftol2轉換）；非報紙分類完全不動——sub_5585E0是ZSimpleHTML通用函式，
// gating避免影響選單/簡報等其他畫面既有行為。
namespace NewsPaperImageWrapHook
{
    // 固定VA，不依賴任何runtime才建立的物件，DLL載入當下（跟GlyphHook同一
    // 批）就能裝，不需要lazy retry。
    bool Install();
}
