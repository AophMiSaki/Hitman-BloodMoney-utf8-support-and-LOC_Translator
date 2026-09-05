#include "ZipPathTrace.h"
#include "Log.h"
#include "Config.h"
#include <cstring>

namespace ZipPathTrace
{
    // sub_441920 (engine\system\sysfile.cpp)：__thiscall int Open(this,
    // const char* name, char forceCreate)。前兩句指令：
    //   push 0FFFFFFFFh              ; 6A FF
    //   push offset SEH_441920       ; 68 BA AA 72 00 (=0x0072AABA)
    // 共7 bytes，足夠塞5-byte jmp，在這兩句上做patch。
    static const DWORD kVA_Entry = 0x00441920;
    // 回跳位址=kVA_Entry+7（前兩句原始指令的總長度），對應第三句指令
    // mov eax, large fs:0 的起始位址。
    static const DWORD kVA_ReturnTo = 0x00441927;
    static const DWORD kSehHandler = 0x0072AABA;

    static DWORD g_thisStash = 0;
    static DWORD g_nameStash = 0;
    static DWORD g_modeStash = 0;
    static DWORD g_retAddrStash = 0;
    static DWORD g_hitCount = 0;

    // LocHook.cpp用來推導中文LOC路徑，只記最近一次.zip請求（場景切換
    // 時舊值自然被新場景蓋掉，不需要額外清空邏輯）。
    static char g_lastZipPath[MAX_PATH] = {};

    // 只控制下面CheckPathResolve()逐次trace log要不要印（見[Debug]
    // DebugZipPathTrace），不影響g_lastZipPath側錄與Install()安裝log。
    static bool g_diagEnable = false;

    // sub_441920內部會把name的副檔名改寫成.zip才繼續往下開檔，代表進到這裡
    // 的name在改寫之前還是.loc/.tex（或其他資源副檔名）；一併比對.zip以防
    // 呼叫端直接傳已經是.zip的路徑進來。
    static bool IsRelevantPath(const char* path)
    {
        size_t len = strlen(path);
        if (len < 4) return false;
        const char* ext = path + len - 4;
        return _stricmp(ext, ".loc") == 0 || _stricmp(ext, ".tex") == 0 || _stricmp(ext, ".zip") == 0;
    }

    // __cdecl，供naked detour呼叫。記錄呼叫端return address。
    static void __cdecl CheckPathResolve(const char* name, int mode, void* returnAddr, void* thisPtr)
    {
        if (!name) return;
        if (!IsRelevantPath(name)) return;
        g_hitCount++;
        if (g_diagEnable)
            Log::Write("[ZipPathTrace] sub_441920路徑解析請求#%lu：name=%s mode=%d this=%p 呼叫端returnAddr=0x%08X",
                       g_hitCount, name, mode, thisPtr, (DWORD)returnAddr);

        size_t len = strlen(name);
        if (len >= 4 && _stricmp(name + len - 4, ".zip") == 0)
            strncpy_s(g_lastZipPath, name, _TRUNCATE);
    }

    // 進入時：ecx=this，[esp]=呼叫端returnAddr，[esp+4]=name(a2)，
    // [esp+8]=forceCreate(a3，char但棧上佔4 bytes)。先原樣收集，pushad
    // 後再組call參數。
    static __declspec(naked) void ZipPathTraceDetour()
    {
        __asm
        {
            mov  eax, [esp]
            mov  [g_retAddrStash], eax
            mov  eax, [esp+4]
            mov  [g_nameStash], eax
            mov  eax, [esp+8]
            mov  [g_modeStash], eax
            mov  [g_thisStash], ecx

            pushad
            push dword ptr [g_thisStash]
            push dword ptr [g_retAddrStash]
            push dword ptr [g_modeStash]
            push dword ptr [g_nameStash]
            call CheckPathResolve
            add  esp, 16
            popad

            ; 原樣重跑被覆蓋的前兩句指令，語意上跟完全沒被patch時一模一樣
            ; （純立即值push，不依賴進入時的暫存器/旗標狀態）。
            push 0FFFFFFFFh
            push kSehHandler

            mov  edx, kVA_ReturnTo
            jmp  edx
        }
    }

    void Install(HMODULE hModule)
    {
        g_diagEnable = Config::LoadDebug(hModule).debugZipPathTrace;

        BYTE* p = (BYTE*)kVA_Entry;
        const BYTE kExpected[7] = { 0x6A, 0xFF, 0x68, 0xBA, 0xAA, 0x72, 0x00 };
        for (int i = 0; i < 7; i++)
        {
            if (p[i] != kExpected[i])
            {
                // 安裝失敗，不受g_diagEnable限制，一律印出。
                Log::Write("[ZipPathTrace] 0x%08X第%d byte是0x%02X，預期0x%02X——版本不符或已被其他patch動過，放棄安裝",
                           kVA_Entry, i, p[i], kExpected[i]);
                return;
            }
        }

        DWORD oldProt = 0;
        VirtualProtect(p, 7, PAGE_EXECUTE_READWRITE, &oldProt);
        *p = 0xE9;
        *(INT32*)(p + 1) = (INT32)((DWORD)&ZipPathTraceDetour - (DWORD)(p + 5));
        p[5] = 0x90;  // NOP
        p[6] = 0x90;  // NOP，補7-byte原指令跟5-byte jmp之間差的2 bytes
        VirtualProtect(p, 7, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), p, 7);

        // 安裝成功，不受g_diagEnable限制，一律印出。
        Log::Write("[ZipPathTrace] sub_441920 entry hook安裝完成：site=0x%08X detour=%p", kVA_Entry, &ZipPathTraceDetour);
    }

    const char* GetLastSceneZipPath()
    {
        return g_lastZipPath;
    }
}
