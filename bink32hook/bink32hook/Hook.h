#pragma once
#include <Windows.h>

// binkw32 proxy DLL 的中文化 hook 進入點。
// 實際的 vtable 攔截邏輯見 GlyphHook.h。

namespace Hook
{
    void Init(HMODULE hModule);
    void Shutdown();
}
