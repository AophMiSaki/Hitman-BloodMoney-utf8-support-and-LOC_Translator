#include "NewsPaperImageWrapHook.h"
#include "Log.h"
#include "ZipPathTrace.h"
#include <cstring>

namespace NewsPaperImageWrapHook
{
    // sub_559910（ZSimpleHTML主排版迴圈，0x559910）裡3個
    // 「call sub_5585E0」的call site，全部原本硬編碼傳參數0——見
    // NewsPaperImageWrapHook.h頂部說明。
    // 下面3個detour的inline asm一律直接寫死hex literal（不引用這幾個
    // C++常數），比照GlyphHook.cpp既有慣例，避免MSVC inline asm對
    // static const DWORD當immediate還是memory operand處理的不確定性。
    // 這幾個常數只用來給PatchCallSite()當site位址參數。
    static const DWORD kSite_CaseBr    = 0x559C9E; // case10(<br>)換欄X位移計算，回跳位址0x559CA3(=+5)
    static const DWORD kSite_MainCheck = 0x55A0E4; // 逐字元寬度溢出檢查（主要觸發點，每字元都會跑到），回跳位址0x55A0E9(=+5)
    static const DWORD kSite_WrapBr    = 0x55A334; // 自動換行溢出時的換欄X位移計算，回跳位址0x55A339(=+5)
    // sub_5585E0＝0x5585E0，game本身的__ftol2＝0x7286AC（float截斷成int，
    // 跟native其他地方轉換v122用同一個helper）——這兩個位址直接寫死在下面
    // 3個detour的inline asm裡。

    static bool ZipPathEndsWith(const char* path, const char* suffix)
    {
        size_t pathLen = strlen(path);
        size_t sufLen  = strlen(suffix);
        if (pathLen < sufLen) return false;
        return _stricmp(path + pathLen - sufLen, suffix) == 0;
    }

    // __cdecl，無參數，回傳int(0/1)不用bool——inline asm只`test eax,eax`
    // 檢查整個暫存器，用int確保MSVC把完整32-bit 0/1寫進eax，不用擔心bool
    // 只保證AL、上面24 bit是垃圾值。zip副檔名判斷式跟GlyphHook.cpp的
    // DetermineCategory()相同，但這裡特意不呼叫那邊的版本——這裡只是唯讀
    // gating判斷，不需要牽動DetermineCategory()裡EnsureCategoryReady()那些
    // 字型合成lazy init副作用，保持本檔職責單純（修排版位置，不碰字型合成）。
    static int __cdecl IsNewspaperContext()
    {
        const char* zipPath = ZipPathTrace::GetLastSceneZipPath();
        if (!zipPath || !zipPath[0]) return 0;
        return (ZipPathEndsWith(zipPath, "_news.zip") || ZipPathEndsWith(zipPath, "_postmission.zip")) ? 1 : 0;
    }

    // v122（sub_559910本地變數，目前這一行的Y座標，float）在3個call site
    // 被patch的瞬間esp相對offset皆相同（call前都恰好1個pending push）：
    //   pushad前：pushed參數(0)在[esp+0]，v122在[esp+0x3C4]
    //   pushad後（32 bytes）：pushed參數在[esp+0x20]，v122在[esp+0x3E4]
    // 3個detour內容完全相同，只有結尾jmp回去的位址不同（各自的call
    // site+5），比照GlyphHook.cpp「內容重複、各自對應不同site」寫法，
    // 不共用trampoline以免增加複雜度。
    static __declspec(naked) void Detour_CaseBr()
    {
        __asm
        {
            pushad
            call IsNewspaperContext
            test eax, eax
            jz   short skip_fix
            fld  dword ptr [esp + 0x3E4]
            mov  eax, 0x7286AC   ; __ftol2
            call eax
            mov  [esp + 0x20], eax
        skip_fix:
            popad
            mov  eax, 0x5585E0   ; sub_5585E0
            call eax
            mov  edx, 0x559CA3   ; 回跳位址＝kSite_CaseBr(0x559C9E)+5
            jmp  edx
        }
    }

    static __declspec(naked) void Detour_MainCheck()
    {
        __asm
        {
            pushad
            call IsNewspaperContext
            test eax, eax
            jz   short skip_fix
            fld  dword ptr [esp + 0x3E4]
            mov  eax, 0x7286AC   ; __ftol2
            call eax
            mov  [esp + 0x20], eax
        skip_fix:
            popad
            mov  eax, 0x5585E0   ; sub_5585E0
            call eax
            mov  edx, 0x55A0E9   ; 回跳位址＝kSite_MainCheck(0x55A0E4)+5
            jmp  edx
        }
    }

    static __declspec(naked) void Detour_WrapBr()
    {
        __asm
        {
            pushad
            call IsNewspaperContext
            test eax, eax
            jz   short skip_fix
            fld  dword ptr [esp + 0x3E4]
            mov  eax, 0x7286AC   ; __ftol2
            call eax
            mov  [esp + 0x20], eax
        skip_fix:
            popad
            mov  eax, 0x5585E0   ; sub_5585E0
            call eax
            mov  edx, 0x55A339   ; 回跳位址＝kSite_WrapBr(0x55A334)+5
            jmp  edx
        }
    }

    // 「byte比對+改成jmp detour」邏輯，手法同GlyphHook.cpp的PatchCallSite，
    // 但這裡patch的是5-byte「E8 call」直接呼叫，檢查完整5-byte E8+rel32，
    // 跟GlyphHook那邊的6-byte vtable間接呼叫pattern不同，不共用同一份函式。
    static bool PatchCallSite(DWORD site, const BYTE expected5[5], void* detour, const char* label)
    {
        BYTE* p = (BYTE*)site;
        for (int i = 0; i < 5; i++)
        {
            if (p[i] != expected5[i])
            {
                Log::Write("[NewsPaperImageWrapHook] %s 0x%08X第%d byte是0x%02X，預期0x%02X"
                           "——版本不符或已被其他patch動過，放棄安裝",
                           label, site, i, p[i], expected5[i]);
                return false;
            }
        }

        DWORD oldProt = 0;
        VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &oldProt);
        *p = 0xE9;
        *(INT32*)(p + 1) = (INT32)((DWORD)detour - (DWORD)(p + 5));
        VirtualProtect(p, 5, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), p, 5);

        Log::Write("[NewsPaperImageWrapHook] %s 安裝完成：site=0x%08X detour=%p", label, site, detour);
        return true;
    }

    bool Install()
    {
        // 原始byte：都是「E8 <rel32指向0x5585E0>」。
        static const BYTE kExpected_CaseBr[5]    = { 0xE8, 0x3D, 0xE9, 0xFF, 0xFF };
        static const BYTE kExpected_MainCheck[5] = { 0xE8, 0xF7, 0xE4, 0xFF, 0xFF };
        static const BYTE kExpected_WrapBr[5]    = { 0xE8, 0xA7, 0xE2, 0xFF, 0xFF };

        bool ok1 = PatchCallSite(kSite_CaseBr,    kExpected_CaseBr,    &Detour_CaseBr,    "case10換欄X位移(0x559C9E)");
        bool ok2 = PatchCallSite(kSite_MainCheck, kExpected_MainCheck, &Detour_MainCheck, "逐字元寬度溢出檢查(0x55A0E4)");
        bool ok3 = PatchCallSite(kSite_WrapBr,    kExpected_WrapBr,    &Detour_WrapBr,    "自動換行溢出換欄(0x55A334)");

        bool allOk = ok1 && ok2 && ok3;
        return allOk;
    }
}
