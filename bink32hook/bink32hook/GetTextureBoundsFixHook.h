#pragma once
#include <Windows.h>

// native GetTexture(ZTextureManagerD3D vtable+0x04 / sub_48F140)內建一個
// int 3除錯中斷點(0x0048F177)：多子頁貼圖的subpage index(a3)超出這格
// redirect表記錄的count(*v4)時，先int3、不管有沒有掛偵錯器都會落到下一行
// 讀v4[a3+1]（越界讀redirect表），算出一個垃圾subpage index去組出材質記錄
// 指標，這個野指標之後被當成合法材質用，最終造成EIP跳進已釋放記憶體崩潰
// （見md\CJK圖集ZBitmap延遲存取崩潰.md 候選A，暫存器特徵Edx=Esi=0x80000003
// =STATUS_BREAKPOINT）。
//
// 我方NativeTextureRegistry.cpp的AllocateSlot呼叫路徑已繞開這個函式，但
// native引擎自己（例如場景重載時的LoadTEXBuffer）仍會呼叫到sub_48F140、
// 仍會踩中同一個陷阱，這個patch直接補在函式本體，兩條路徑（含任何其他
// 未來發現的呼叫路徑）一次堵住。
//
// 改法：0x0048F175起3 bytes（jb short/int3）改成jae short跳到函式既有的
// 「回傳sentinel(unk_90ADC8)」出口＋nop，a3越界時比照其餘無效輸入
// （a2==0/2048/result==0）直接回傳sentinel，不再讀越界記憶體、不再組出
// 野指標；a3合法時的原本redirect讀取完全不受影響。
namespace GetTextureBoundsFixHook
{
    void Install();
}
