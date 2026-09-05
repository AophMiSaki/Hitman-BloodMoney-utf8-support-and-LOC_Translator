#include <Windows.h>
#include "Hook.h"
#include "CrashHandler.h"
#include "Log.h"

// binkw32.dll proxy 入口。
// 真正的 binkw32.dll 需改名為 binkw32_real.dll 放在同目錄，
// 本DLL的9個Bink匯出透過 binkw32.def 的 linker export forwarding
// 轉發過去（不需要在這裡手動呼叫），詳見
// md/bloodmoney_utf8_support.md 第6節。
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        Log::Init(hModule);
        CrashHandler::Init(hModule);
        Hook::Init(hModule);
        break;
    case DLL_PROCESS_DETACH:
        Hook::Shutdown();
        break;
    }
    return TRUE;
}
