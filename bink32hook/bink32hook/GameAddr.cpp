#include "GameAddr.h"
#include "Log.h"
#include <stdio.h>
#include <string.h>

namespace GameAddr
{
    static Build g_build = Unknown;
    static bool g_missingLogged[AddressTable::Count];

    static const char* BuildName(Build b)
    {
        return b == Gog ? "GOG" : (b == Steam ? "Steam" : "Unknown");
    }

    void Init()
    {
        const BYTE* base = (const BYTE*)GetModuleHandleA(NULL);
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)base;
        const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        DWORD stamp = nt->FileHeader.TimeDateStamp;
        DWORD size = nt->OptionalHeader.SizeOfImage;

        if (stamp == AddressTable::kSteamBuild.timeDateStamp && size == AddressTable::kSteamBuild.sizeOfImage) g_build = Steam;
        else if (stamp == AddressTable::kGogBuild.timeDateStamp && size == AddressTable::kGogBuild.sizeOfImage) g_build = Gog;
        else g_build = Unknown;

        Log::Write("[GameAddr] 版本=%s TimeDateStamp=0x%08X SizeOfImage=0x%X（Steam=0x%08X/0x%X GOG=0x%08X/0x%X）%s",
                   BuildName(g_build), stamp, size,
                   AddressTable::kSteamBuild.timeDateStamp, AddressTable::kSteamBuild.sizeOfImage,
                   AddressTable::kGogBuild.timeDateStamp, AddressTable::kGogBuild.sizeOfImage,
                   g_build == Unknown ? " 版本不明，Addr()全部回傳0" : "");
    }

    Build GetBuild()
    {
        return g_build;
    }

    bool IsSupported()
    {
        return g_build == Gog || g_build == Steam;
    }

    DWORD Addr(AddressTable::Id id)
    {
        if (id < 0 || id >= AddressTable::Count) return 0;

        DWORD va = 0;
        if (g_build == Gog) va = AddressTable::kEntries[id].gog;
        else if (g_build == Steam) va = AddressTable::kEntries[id].steam;
        else return 0;

        if (va == 0 && !g_missingLogged[id])
        {
            g_missingLogged[id] = true;
            Log::Write("[GameAddr] 位址查詢失敗：AddressTable 第%d項在 %s 版的欄位為0", (int)id, BuildName(g_build));
        }
        return va;
    }

    DWORD ReadDword(AddressTable::Id id)
    {
        DWORD va = Addr(id);
        if (!va || IsBadReadPtr((const void*)va, sizeof(DWORD))) return 0;
        return *(const DWORD*)va;
    }

    void HexAt(DWORD addr, size_t n, char* out, size_t cap)
    {
        if (cap == 0) return;
        out[0] = '\0';
        if (!addr || IsBadReadPtr((const void*)addr, n))
        {
            strcpy_s(out, cap, "(無法讀取)");
            return;
        }
        const BYTE* p = (const BYTE*)addr;
        size_t len = 0;
        for (size_t i = 0; i < n && len + 4 <= cap; i++)
        {
            len += (size_t)_snprintf_s(out + len, cap - len, _TRUNCATE, "%02X ", p[i]);
        }
    }

    bool MatchMasked(DWORD addr, const BYTE* expected, size_t n, const Wild* wild, size_t nWild)
    {
        if (IsBadReadPtr((const void*)addr, n)) return false;
        const BYTE* p = (const BYTE*)addr;
        for (size_t i = 0; i < n; i++)
        {
            bool skip = false;
            for (size_t w = 0; w < nWild; w++)
            {
                if ((int)i >= wild[w].off && (int)i < wild[w].off + wild[w].len)
                {
                    skip = true;
                    break;
                }
            }
            if (!skip && p[i] != expected[i]) return false;
        }
        return true;
    }
}
