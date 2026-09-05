#include "Log.h"
#include "Paths.h"
#include <cstdio>
#include <cstdarg>

namespace Log
{
    static char g_logPath[MAX_PATH];
    static CRITICAL_SECTION g_cs;
    static bool g_ready = false;
    static volatile bool g_traceWindowActive = false;

    void Init(HMODULE hModule)
    {
        Paths::EnsureDataDir(hModule);
        Paths::GetLogPath(hModule, g_logPath, sizeof(g_logPath));
        InitializeCriticalSection(&g_cs);
        g_ready = true;

        // 每次啟動蓋掉舊log重寫，避免無限累積。開頭補UTF-8 BOM（EF BB BF）：檔案
        // 沒有BOM時記事本等工具可能誤判成ANSI(950 Big5)顯示亂碼，要在檔案層級
        // 標記編碼。用"wb"二進位模式避免CRLF轉換動到BOM這三個raw bytes。
        FILE* f = nullptr;
        fopen_s(&f, g_logPath, "wb");
        if (f)
        {
            static const unsigned char kUtf8Bom[3] = { 0xEF, 0xBB, 0xBF };
            fwrite(kUtf8Bom, 1, sizeof(kUtf8Bom), f);
            fclose(f);
        }

        Write("[bink32hook] Log 初始化完成，log路徑=%s", g_logPath);
    }

    void Write(const char* fmt, ...)
    {
        if (!g_ready) return;

        char msg[1024];
        va_list args;
        va_start(args, fmt);
        _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, args);
        va_end(args);

        SYSTEMTIME st;
        GetLocalTime(&st);

        EnterCriticalSection(&g_cs);
        FILE* f = nullptr;
        fopen_s(&f, g_logPath, "a");
        if (f)
        {
            fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] %s\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
            fclose(f);
        }
        LeaveCriticalSection(&g_cs);
    }

    void SetTraceWindow(bool active)
    {
        g_traceWindowActive = active;
    }

    bool IsTraceWindowActive()
    {
        return g_traceWindowActive;
    }
}
