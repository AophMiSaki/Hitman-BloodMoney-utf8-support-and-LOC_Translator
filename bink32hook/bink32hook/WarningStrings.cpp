#include "WarningStrings.h"
#include "Log.h"
#include "Paths.h"
#include "LocHook.h"
#include "LocTxtParser.h"
#include <map>
#include <string>
#include <cstdio>

namespace WarningStrings
{
    // ---- 字串表（warningString.txt 攤平成 category/key -> value）----
    static std::map<std::string, std::string> g_strings;
    static bool g_loaded = false;

    // 內建預設。trans 值依 md\小地圖與非法入侵顯示.md §1：擅闖／敵對區域、
    // 懷疑／戰鬥。key 用 orig/trans 通用命名（非特定語言代碼），因為
    // IsTranslationActive() 只是原文／譯文二選一開關，非語言選擇。
    // 手建的非括號舊檔會解析失敗→退回此表。
    static const char kDefaultWarningTxt[] =
        "[\n"
        "  \"TRESPASSING\" [\n"
        "    \"orig\" = \"Trespassing\"\n"
        "    \"trans\" = \"擅闖\"\n"
        "  ]\n"
        "  \"HOSTILE\" [\n"
        "    \"orig\" = \"Hostile Area\"\n"
        "    \"trans\" = \"敵對區域\"\n"
        "  ]\n"
        "  \"SUSPICIOUS\" [\n"
        "    \"orig\" = \"Suspicious\"\n"
        "    \"trans\" = \"懷疑\"\n"
        "  ]\n"
        "  \"ALERTED\" [\n"
        "    \"orig\" = \"Alerted\"\n"
        "    \"trans\" = \"戰鬥\"\n"
        "  ]\n"
        "]\n";

    static void GetWarningTxtPath(HMODULE hModule, char* outBuf, size_t bufSize)
    {
        char dir[MAX_PATH];
        Paths::GetDataDir(hModule, dir, sizeof(dir));
        _snprintf_s(outBuf, bufSize, _TRUNCATE, "%swarningString.txt", dir);
    }

    // 缺檔時寫入 kDefaultWarningTxt。
    static void EnsureDefaultWarningTxt(HMODULE hModule)
    {
        char path[MAX_PATH];
        GetWarningTxtPath(hModule, path, sizeof(path));
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return;

        Paths::EnsureDataDir(hModule);
        HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            Log::Write("[WarningStrings] warningString.txt 建立失敗（err=%lu）：%s", GetLastError(), path);
            return;
        }
        DWORD written = 0;
        WriteFile(h, kDefaultWarningTxt, (DWORD)(sizeof(kDefaultWarningTxt) - 1), &written, nullptr);
        CloseHandle(h);
        Log::Write("[WarningStrings] 已建立預設 warningString.txt：%s", path);
    }

    static void LoadStrings(HMODULE hModule)
    {
        g_strings.clear();

        char path[MAX_PATH];
        GetWarningTxtPath(hModule, path, sizeof(path));

        HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        std::string raw;
        if (h != INVALID_HANDLE_VALUE)
        {
            DWORD size = GetFileSize(h, nullptr);
            if (size != INVALID_FILE_SIZE && size > 0)
            {
                raw.resize(size);
                DWORD got = 0;
                if (ReadFile(h, &raw[0], size, &got, nullptr))
                {
                    raw.resize(got);
                }
                else
                {
                    Log::Write("[WarningStrings] warningString.txt 讀取失敗（err=%lu）：%s", GetLastError(), path);
                    raw.clear();
                }
            }
            CloseHandle(h);
        }

        // 去掉 UTF-8 BOM，否則 LocTxtParser 會因首 token 不是 '[' 而失敗。
        if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF &&
            (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF)
            raw.erase(0, 3);

        // 開檔或解析失敗都退回內建預設字串，功能不因缺字串而失效。
        const char* text = raw.empty() ? kDefaultWarningTxt : raw.c_str();
        size_t len = raw.empty() ? (sizeof(kDefaultWarningTxt) - 1) : raw.size();

        std::string err;
        if (!LocTxtParser::Parse(text, len, g_strings, err))
        {
            g_strings.clear();
            std::string err2;
            LocTxtParser::Parse(kDefaultWarningTxt, sizeof(kDefaultWarningTxt) - 1, g_strings, err2);
            Log::Write("[WarningStrings] warningString.txt 解析失敗（%s），改用內建預設字串", err.c_str());
        }
    }

    void EnsureLoaded(HMODULE hModule)
    {
        if (g_loaded) return;
        g_loaded = true;
        EnsureDefaultWarningTxt(hModule);
        LoadStrings(hModule);
        Log::Write("[WarningStrings] 載入完成：字串數=%zu", g_strings.size());
    }

    std::string Pick(const char* category)
    {
        const char* lang = LocHook::IsTranslationActive() ? "trans" : "orig";
        std::string key = std::string(category) + "/" + lang;
        std::map<std::string, std::string>::const_iterator it = g_strings.find(key);
        if (it != g_strings.end()) return it->second;

        std::string keyOrig = std::string(category) + "/orig";
        it = g_strings.find(keyOrig);
        return it != g_strings.end() ? it->second : std::string(category);
    }

    size_t Count()
    {
        return g_strings.size();
    }
}
