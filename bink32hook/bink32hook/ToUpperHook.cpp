#include "ToUpperHook.h"
#include "Log.h"
#include "GameAddr.h"
#include <cstring>

namespace ToUpperHook
{
    typedef int (__cdecl* ToUpperFn)(int);
    static ToUpperFn g_origToUpper = nullptr;

    // 判斷式用UTF-8編碼規則、不用codepoint範圍：ASCII一定是單一byte且最高
    // 位元=0（0x00~0x7F），非ASCII（UTF-8 lead byte、continuation byte，或
    // 完整codepoint）一定>=0x80，對「逐raw byte呼叫」跟「先解碼成codepoint」
    // 兩種情況都正確。相關呼叫端的英文原文都沒有需要轉大寫的重音字母，
    // blanket bypass不會誤傷Danish locale的合法轉換需求。
    static int __cdecl Hook_ToUpper(int c)
    {
        if ((unsigned int)c >= 0x80)
            return c;
        return g_origToUpper(c);
    }

    // ---- sub_463D10 switch分派入口 inline hook ----
    //
    // IAT hook只能攔到實際呼叫toupper()的分支；sub_463D10內部另有一條處理
    // Danish特殊字元的分支（輸入落在[0xDF,0xFC]）直接算術減0x20轉大寫、
    // 不呼叫toupper()，IAT hook攔不到。在switch分派入口(0x463D30)插入跟
    // Hook_ToUpper相同的">=0x80原樣通過"判斷：>=0x80時直接跳到case後方的
    // 重新編碼步驟(0x463D54)，跳過[0xDF,0xFC]算術分支與toupper()兩者；
    // <0x80時原樣重跑被覆蓋的兩句指令(lea ecx,[eax-0DFh] / cmp ecx,1Dh)
    // 後跳回原本的switch跳轉指令，不影響ASCII的轉大寫行為。覆蓋9 bytes
    //（5-byte jmp + 4 NOP）。
    static DWORD g_vaSwitchSite = 0;
    static DWORD g_vaSwitchResume = 0;  // "ja short def_463D42"起點
    static DWORD g_vaSwitchBypass = 0;  // case後方重新編碼步驟("push eax"起點)
    static const BYTE kExpectedSwitchSite[9] = { 0x8D,0x88,0x21,0xFF,0xFF,0xFF, 0x83,0xF9,0x1D };

    static __declspec(naked) void Detour_SwitchEntry()
    {
        __asm
        {
            cmp   eax, 80h
            jae   bypass
            lea   ecx, [eax - 0DFh]
            cmp   ecx, 1Dh
            jmp   dword ptr [g_vaSwitchResume]
        bypass:
            jmp   dword ptr [g_vaSwitchBypass]
        }
    }

    static void InstallSwitchBypass()
    {
        g_vaSwitchSite = GameAddr::Addr(AddressTable::ToUpper_SwitchSite);
        g_vaSwitchResume = GameAddr::Addr(AddressTable::ToUpper_SwitchResume);
        g_vaSwitchBypass = GameAddr::Addr(AddressTable::ToUpper_SwitchBypass);
        if (!g_vaSwitchSite || !g_vaSwitchResume || !g_vaSwitchBypass)
        {
            Log::Write("[ToUpperHook] sub_463D10 switch入口位址查詢失敗（site=0x%08X resume=0x%08X bypass=0x%08X），放棄安裝",
                       g_vaSwitchSite, g_vaSwitchResume, g_vaSwitchBypass);
            return;
        }

        BYTE* p = (BYTE*)g_vaSwitchSite;
        if (IsBadReadPtr(p, 9))
        {
            Log::Write("[ToUpperHook] sub_463D10 0x%08X無法讀取，放棄安裝", g_vaSwitchSite);
            return;
        }
        for (int i = 0; i < 9; i++)
        {
            if (p[i] != kExpectedSwitchSite[i])
            {
                Log::Write("[ToUpperHook] sub_463D10 0x%08X第%d byte是0x%02X，預期0x%02X——版本不符或已被其他patch動過，放棄安裝",
                           g_vaSwitchSite, i, p[i], kExpectedSwitchSite[i]);
                return;
            }
        }

        DWORD oldProt = 0;
        VirtualProtect(p, 9, PAGE_EXECUTE_READWRITE, &oldProt);
        p[0] = 0xE9;
        *(INT32*)(p + 1) = (INT32)((DWORD)&Detour_SwitchEntry - (DWORD)(p + 5));
        p[5] = p[6] = p[7] = p[8] = 0x90;
        VirtualProtect(p, 9, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), p, 9);

        Log::Write("[ToUpperHook] sub_463D10 switch分派入口inline hook安裝完成：site=0x%08X detour=%p"
                   "（修正[0xDF,0xFC]算術分支繞過toupper()的CJK亂碼bug）",
                   g_vaSwitchSite, &Detour_SwitchEntry);
    }

    // 標準IAT hook：改寫main exe匯入表裡指定匯入函式的指標，改成指向我們
    // 的替身函式。
    static bool PatchIat(HMODULE targetModule, const char* importDll, const char* importFunc,
        void* detour, void** origOut)
    {
        BYTE* base = (BYTE*)targetModule;
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

        IMAGE_DATA_DIRECTORY& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (importDir.VirtualAddress == 0) return false;

        IMAGE_IMPORT_DESCRIPTOR* desc = (IMAGE_IMPORT_DESCRIPTOR*)(base + importDir.VirtualAddress);
        for (; desc->Name != 0; desc++)
        {
            const char* dllName = (const char*)(base + desc->Name);
            if (_stricmp(dllName, importDll) != 0) continue;

            IMAGE_THUNK_DATA* nameThunk = desc->OriginalFirstThunk
                ? (IMAGE_THUNK_DATA*)(base + desc->OriginalFirstThunk)
                : (IMAGE_THUNK_DATA*)(base + desc->FirstThunk);
            IMAGE_THUNK_DATA* iatThunk = (IMAGE_THUNK_DATA*)(base + desc->FirstThunk);

            for (; nameThunk->u1.AddressOfData != 0; nameThunk++, iatThunk++)
            {
                if (IMAGE_SNAP_BY_ORDINAL32(nameThunk->u1.Ordinal)) continue; // 只處理具名匯入
                IMAGE_IMPORT_BY_NAME* byName = (IMAGE_IMPORT_BY_NAME*)(base + nameThunk->u1.AddressOfData);
                if (strcmp((const char*)byName->Name, importFunc) != 0) continue;

                DWORD oldProt = 0;
                VirtualProtect(&iatThunk->u1.Function, sizeof(DWORD), PAGE_READWRITE, &oldProt);
                if (origOut) *origOut = (void*)(DWORD_PTR)iatThunk->u1.Function;
                iatThunk->u1.Function = (DWORD_PTR)detour;
                VirtualProtect(&iatThunk->u1.Function, sizeof(DWORD), oldProt, &oldProt);
                return true;
            }
        }
        return false;
    }

    // 錯誤修正：Steam版殼重建了匯入表，PatchIat走DataDirectory[IMPORT]找不到MSVCR71.DLL!toupper。
    // 改為直接改寫IAT槽位（位址來自AddressTable::ToUpperIatSlot）；槽位目前值必須等於
    // MSVCR71.DLL實際匯出的toupper才寫入，否則不動並記錄兩個值。
    static bool PatchIatSlot(DWORD slotVa, void* detour, void** origOut)
    {
        if (!slotVa || IsBadReadPtr((void*)slotVa, sizeof(DWORD)))
        {
            Log::Write("[ToUpperHook] toupper IAT槽位0x%08X無法讀取", slotVa);
            return false;
        }
        HMODULE crt = GetModuleHandleA("MSVCR71.DLL");
        DWORD real = crt ? (DWORD)GetProcAddress(crt, "toupper") : 0;
        DWORD* slot = (DWORD*)slotVa;
        if (!real || *slot != real)
        {
            Log::Write("[ToUpperHook] toupper IAT槽位0x%08X目前值0x%08X，MSVCR71.DLL!toupper=0x%08X（模組=%p），不相符，不改寫",
                       slotVa, *slot, real, crt);
            return false;
        }
        DWORD oldProt = 0;
        VirtualProtect(slot, sizeof(DWORD), PAGE_READWRITE, &oldProt);
        if (origOut) *origOut = (void*)(DWORD_PTR)*slot;
        *slot = (DWORD)detour;
        VirtualProtect(slot, sizeof(DWORD), oldProt, &oldProt);
        return true;
    }

    void Install(HMODULE hModule)
    {
        if (!GameAddr::IsSupported())
        {
            Log::Write("[ToUpperHook] 遊戲版本不明，放棄安裝");
            return;
        }

        HMODULE exeModule = GetModuleHandleA(NULL);

        void* orig = nullptr;
        bool iatOk = (GameAddr::GetBuild() == GameAddr::Steam)
            ? PatchIatSlot(GameAddr::Addr(AddressTable::ToUpperIatSlot), (void*)&Hook_ToUpper, &orig)
            : PatchIat(exeModule, "MSVCR71.DLL", "toupper", (void*)&Hook_ToUpper, &orig);
        if (iatOk)
        {
            g_origToUpper = (ToUpperFn)orig;
            Log::Write("[ToUpperHook] toupper IAT hook安裝完成，orig=%p", orig);
        }
        else
        {
            Log::Write("[ToUpperHook] toupper IAT hook安裝失敗（GOG版：main exe匯入表找不到MSVCR71.DLL!toupper；"
                       "Steam版：IAT槽位值與MSVCR71.DLL!toupper不符，見上一行；按鈕/HUD CJK文字亂碼問題不會被修復）");
        }

        // IAT hook只涵蓋toupper()路徑，sub_463D10的[0xDF,0xFC]算術分支繞過
        // toupper()攔不到，需要另外inline hook。
        InstallSwitchBypass();
    }
}
