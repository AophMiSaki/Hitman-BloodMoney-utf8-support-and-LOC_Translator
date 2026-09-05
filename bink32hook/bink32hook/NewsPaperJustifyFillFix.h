#pragma once
#include <Windows.h>

// 修復報紙<justify fill>在CJK下把行尾文字推遠、甚至蓋過圖片的問題。
//
// 根因：sub_559910主排版迴圈裡，一行觸發自動換行且JUSTIFY狀態帶0x800
// (<justify fill>)時，會呼叫sub_5586B0把剩餘寬度平分到行內每個空白間隙。
// 其「目前Y是否還在圖片範圍內」的判斷讀的欄位在圖片Y範圍算錯時會誤判，
// 算出錯誤的撐開目標寬度。英文一行有十幾個空白、誤差被稀釋看不出來；
// CJK一行常只有1個空白，整筆誤差全塞進那個空白，把後半段文字推走。
//
// 修法：報紙分類下讓<justify fill>直接跳過撐開、等同<justify left>（維持
// word-wrap後的自然位置）。非報紙分類（選單/簡報等）不受影響，維持native
// 兩端對齊行為。
//
// patch site 0x55A22C：sub_559910內唯一一處呼叫sub_5586B0的call，5 bytes。
// sub_5586B0是callee-clean(retn 10h)，改成jmp進detour後：
//   - 報紙分類：不呼叫sub_5586B0，用`add esp,16`自行清掉呼叫端已push的
//     4個參數，跳回原本call之後(0x55A231)。
//   - 非報紙分類：照原樣呼叫sub_5586B0，不改變native行為。
// detour內用push/pop ecx保護，避免IsNewspaperContext()這個cdecl呼叫用掉
// ecx，確保「非報紙」分支呼叫sub_5586B0時ecx仍是native設好的值。
//
// 代價：報紙CJK正文行尾不再兩端切齊、會參差不齊，換取不再位移錯亂。
namespace NewsPaperJustifyFillFix
{
    bool Install(HMODULE hModule);
}
