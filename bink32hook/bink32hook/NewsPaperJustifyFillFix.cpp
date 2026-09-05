#include "NewsPaperJustifyFillFix.h"
#include "Log.h"
#include "ZipPathTrace.h"
#include <cstring>

namespace NewsPaperJustifyFillFix
{
    static const DWORD kSite = 0x55A22C;    // sub_559910內唯一一處呼叫sub_5586B0(<justify fill>撐開函式)的call
    // sub_5586B0本體＝0x5586B0，回跳位址＝0x55A231。detour內這兩個位址用
    // literal直接寫、不宣告成具名const，避免inline asm對具名const是否被當
    // immediate展開的疑慮（比照NewsPaperImageLineHeightFix.cpp既有寫法）。

    // 跟其他NewsPaper*Fix.cpp同一套判斷，這裡刻意不共用——修行為的hook各自
    // 獨立判斷，避免耦合（見[[feedback_verify_path_before_fix]]既有慣例）。
    static bool ZipPathEndsWith(const char* path, const char* suffix)
    {
        size_t pathLen = strlen(path);
        size_t sufLen = strlen(suffix);
        if (pathLen < sufLen) return false;
        return _stricmp(path + pathLen - sufLen, suffix) == 0;
    }

    static int __cdecl IsNewspaperContext()
    {
        const char* zipPath = ZipPathTrace::GetLastSceneZipPath();
        if (!zipPath || !zipPath[0]) return 0;
        return (ZipPathEndsWith(zipPath, "_news.zip") || ZipPathEndsWith(zipPath, "_postmission.zip")) ? 1 : 0;
    }

    // 進入時的機器狀態＝原本call sub_5586B0那一刻：呼叫端已push好4個dword
    // 參數，ecx已設成pExceptionObject（原樣保留、不去動它）。sub_5586B0是
    // callee-clean(retn 10h)，所以「跳過」分支必須自己補一個`add esp,16`
    // 清掉這4個已push的參數，否則堆疊會多16 bytes沒被清、後面全部亂掉。
    static __declspec(naked) void Detour_SkipJustifyFill()
    {
        __asm
        {
            push ecx                    ; 保護ecx（IsNewspaperContext是cdecl，可能弄髒eax/ecx/edx）
            call IsNewspaperContext
            pop  ecx                    ; 還原ecx，pop不影響flags，test結果仍有效
            test eax, eax
            jz   DoOriginal

            add  esp, 16                ; 報紙分類：跳過撐開呼叫，自行清掉4個已push參數，等同<justify left>
            mov  eax, 0x55A231          ; kResume，用literal避免inline asm對具名const的解讀疑慮（比照既有NewsPaperImageLineHeightFix.cpp慣例）
            jmp  eax

        DoOriginal:
            mov  eax, 0x5586B0          ; kTarget＝sub_5586B0本體
            call eax                    ; 非報紙分類：完全照native原本行為呼叫，callee自己清stack(retn 10h)
            mov  eax, 0x55A231          ; kResume
            jmp  eax
        }
    }

    static bool PatchSite(DWORD site, const BYTE expected[5], void* detour, const char* label)
    {
        BYTE* p = (BYTE*)site;
        for (int i = 0; i < 5; i++)
        {
            if (p[i] != expected[i])
            {
                Log::Write("[NewsPaperJustifyFillFix] %s 0x%08X第%d byte是0x%02X，預期0x%02X"
                           "——版本不符或已被其他patch動過，放棄安裝",
                           label, site, i, p[i], expected[i]);
                return false;
            }
        }

        DWORD oldProt = 0;
        VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &oldProt);
        p[0] = 0xE9;
        *(INT32*)(p + 1) = (INT32)((DWORD)detour - (DWORD)(p + 5));
        VirtualProtect(p, 5, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), p, 5);

        Log::Write("[NewsPaperJustifyFillFix] %s 安裝完成：site=0x%08X detour=%p", label, site, detour);
        return true;
    }

    bool Install(HMODULE hModule)
    {
        (void)hModule;

        // 原始byte＝call sub_5586B0的5-byte call rel32。
        static const BYTE kExpected[5] = { 0xE8, 0x7F, 0xE4, 0xFF, 0xFF };

        bool ok = PatchSite(kSite, kExpected, &Detour_SkipJustifyFill, "報紙<justify fill>改跳過撐開(0x55A22C)");
        Log::Write("[NewsPaperJustifyFillFix] 安裝結果：%d", ok);
        return ok;
    }
}
