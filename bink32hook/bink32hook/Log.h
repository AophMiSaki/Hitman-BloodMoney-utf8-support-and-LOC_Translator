#pragma once
#include <Windows.h>

// 統一log出口，寫到 bink32hook\bink32hook.log（見Paths.h）。刻意不設任何全域
// 筆數上限或靜默機制，確保需要的log不會被吃掉。

namespace Log
{
    void Init(HMODULE hModule);
    void Write(const char* fmt, ...);

    // 暫時性除錯開關：narrow trace window。LocHook::LookupText偵測到
    // key=="PickupSPC"時開啟，下一次任何LookupText呼叫時自動關閉，用來把log
    // 範圍收斂到PickupSPC那一段呼叫堆疊。除錯完成後可移除。
    void SetTraceWindow(bool active);
    bool IsTraceWindowActive();
}
