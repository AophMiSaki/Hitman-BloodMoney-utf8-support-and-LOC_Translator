#pragma once
#include <Windows.h>
#include <cstring>
#include <cstdio>

// 路徑規則：binkw32.dll跟HitmanBloodMoney.exe放同一層，設定/log/dump統一放在
// 同層底下的"bink32hook\"子資料夾，用DLL自己的module路徑(hModule)反推遊戲目錄，
// 不寫死絕對路徑。跟[[obcjk_project]]的obCJK_Path.h是同樣的設計精神，但obCJK是
// OBSE plugin掛在"Data\OBSE\Plugins\obCJK\"這個固定相對路徑下，這裡沒有OBSE可
// 依附，只能用GetModuleFileNameA實際反推。

namespace Paths
{
    static void GetDataDir(HMODULE hModule, char* outBuf, size_t bufSize)
    {
        char modulePath[MAX_PATH] = {};
        GetModuleFileNameA(hModule, modulePath, MAX_PATH);
        char* lastSlash = strrchr(modulePath, '\\');
        if (lastSlash) *(lastSlash + 1) = '\0';
        else modulePath[0] = '\0';
        _snprintf_s(outBuf, bufSize, _TRUNCATE, "%sbink32hook\\", modulePath);
    }

    static bool EnsureDataDir(HMODULE hModule)
    {
        char dir[MAX_PATH];
        GetDataDir(hModule, dir, sizeof(dir));

        size_t len = strlen(dir);
        if (len > 0 && dir[len - 1] == '\\') dir[len - 1] = '\0';

        BOOL ok = CreateDirectoryA(dir, NULL);
        return ok || GetLastError() == ERROR_ALREADY_EXISTS;
    }

    static void GetIniPath(HMODULE hModule, char* outBuf, size_t bufSize)
    {
        char dir[MAX_PATH];
        GetDataDir(hModule, dir, sizeof(dir));
        _snprintf_s(outBuf, bufSize, _TRUNCATE, "%sbink32hook.ini", dir);
    }

    static void GetLogPath(HMODULE hModule, char* outBuf, size_t bufSize)
    {
        char dir[MAX_PATH];
        GetDataDir(hModule, dir, sizeof(dir));
        _snprintf_s(outBuf, bufSize, _TRUNCATE, "%sbink32hook.log", dir);
    }
}
