#include "CrashHandler.h"
#include "Paths.h"
#include "Log.h"
#include <DbgHelp.h>
#define PSAPI_VERSION 2
#include <Psapi.h>

#pragma comment(lib, "Dbghelp.lib")

namespace CrashHandler
{
    static char g_dataDir[MAX_PATH];

    static LONG WINAPI Filter(EXCEPTION_POINTERS* ep)
    {
        Log::Write("[CrashHandler] 未處理例外：code=0x%08X addr=%p",
            ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);

        MEMORYSTATUSEX ms = { sizeof(ms) };
        if (GlobalMemoryStatusEx(&ms))
        {
            Log::Write("[CrashHandler] 系統記憶體：使用率=%lu%% 可用實體=%lluMB 總實體=%lluMB 可用虛擬=%lluMB",
                ms.dwMemoryLoad,
                (unsigned long long)(ms.ullAvailPhys / 1048576),
                (unsigned long long)(ms.ullTotalPhys / 1048576),
                (unsigned long long)(ms.ullAvailVirtual / 1048576));
        }

        PROCESS_MEMORY_COUNTERS pmc = { sizeof(pmc) };
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        {
            Log::Write("[CrashHandler] 行程記憶體：WorkingSet=%luMB PeakWorkingSet=%luMB PagefileUsage=%luMB",
                (unsigned long)(pmc.WorkingSetSize / 1048576),
                (unsigned long)(pmc.PeakWorkingSetSize / 1048576),
                (unsigned long)(pmc.PagefileUsage / 1048576));
        }

        SYSTEMTIME st;
        GetLocalTime(&st);
        char dumpPath[MAX_PATH];
        wsprintfA(dumpPath, "%sbink32hook_%04d%02d%02d_%02d%02d%02d.dmp",
            g_dataDir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

        HANDLE hFile = CreateFileA(dumpPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE)
        {
            MINIDUMP_EXCEPTION_INFORMATION mei;
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = ep;
            mei.ClientPointers = FALSE;

            MINIDUMP_TYPE type = (MINIDUMP_TYPE)(
                MiniDumpWithDataSegs |
                MiniDumpWithHandleData |
                MiniDumpWithUnloadedModules |
                MiniDumpWithFullMemoryInfo |
                MiniDumpWithThreadInfo);

            BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, type, &mei, NULL, NULL);
            Log::Write("[CrashHandler] Minidump %s：%s", ok ? "寫入成功" : "寫入失敗", dumpPath);
            CloseHandle(hFile);
        }
        else
        {
            Log::Write("[CrashHandler] 無法建立dump檔案：%s（err=%lu）", dumpPath, GetLastError());
        }

        return EXCEPTION_CONTINUE_SEARCH;
    }

    void Init(HMODULE hModule)
    {
        Paths::GetDataDir(hModule, g_dataDir, sizeof(g_dataDir));
        SetUnhandledExceptionFilter(Filter);
        Log::Write("[CrashHandler] 已安裝 SetUnhandledExceptionFilter");
    }
}
