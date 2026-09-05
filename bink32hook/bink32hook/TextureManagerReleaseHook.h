#pragma once
#include <Windows.h>

// texture reset清空問題的修法。
//
// 根因：native ReleaseTextures（sub_48F190，ZTextureManagerD3D vtable+0x0C）
// 無差別walk全部2048個材質槽並memset歸零，CJK atlas材質跟native材質共用同一
// 張表、毫無區別標記，每次場景切換（LoadTEXBuffer自呼叫）或引擎關閉
// （ZTextureManagerD3D解構子直接呼叫）都會把我們登記的頁碼一起清掉，但
// NativeTextureRegistry的SlotState.nativeIndex沒有偵測機制，會繼續沿用一個
// 已經失效的頁碼。
//
// 解法：對ReleaseTextures本體做function-entry inline patch。不patch兩個呼叫
// 端是因為LoadTEXBuffer那端是虛擬派發、靜態查不到call site，解構子那端只在
// 物件銷毀時觸發；patch本體則兩條觸發路徑都能攔到。手法為偷開頭幾個bytes塞
// jmp、detour裡先做我們自己的事、再原地重跑被偷走的指令、jmp回原函式續跑。
//
// 函式開頭剛好5 bytes就是乾淨的指令邊界（push ebx / push esi / mov ebx,ecx /
// push edi），5-byte jmp正好蓋滿。呼叫慣例純thiscall，進入時ecx=this
// （ZTextureManagerD3D實例本身），無stack參數。
namespace TextureManagerReleaseHook
{
    void Install();
}
