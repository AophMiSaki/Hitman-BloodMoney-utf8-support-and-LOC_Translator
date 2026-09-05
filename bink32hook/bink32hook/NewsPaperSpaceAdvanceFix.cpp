#include "NewsPaperSpaceAdvanceFix.h"
#include "Log.h"
#include "ZipPathTrace.h"
#include <cstring>

namespace NewsPaperSpaceAdvanceFix
{
    static const DWORD kSite = 0x559DB8; // case32(literal space)的v98快取讀取點
    // 回跳位址0x559DC0＝緊接的mov[esp+20h],ecx，patch長度8 bytes

    // 跟NewsPaperImageWrapHook.cpp同一套判斷，這裡刻意不共用——修行為的
    // hook各自獨立判斷，避免耦合。
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

    // jmp進入，ESP與native在0x559DB8一致，[esp+0x210]/[esp+0x214]/[esp+0x3D0]/
    // [esp+0x48] (var_7D0/var_7CC/var_610/var_998) 直接沿用。
    //
    // 報紙分類分支：原地重跑native在<font>標籤切換時同樣的「找目前字型槽→
    // 呼叫GetGlyph(vtable+0x238,32)」查詢，拿到「當下真正新鮮」的glyph記錄
    // 指標讀+0x10欄位。不照抄native那句mov edi,eax——edi在case32這裡是目前
    // 字元碼(=32)，不能被借用覆寫。
    //
    // 非報紙分類分支：完全照native原本兩句指令執行，行為不變。
    static __declspec(naked) void Detour_SpaceAdvance()
    {
        __asm
        {
            call IsNewspaperContext     ; cdecl無參數，call/ret自平衡esp，且保留ebx/esi/edi/ebp（含edi=目前字元碼32）
            test eax, eax
            jz   NotNewspaper

            ; ── 報紙分類：重新查詢，取得新鮮的glyph記錄 ──
            mov  ecx, [esp + 0x210]      ; ecx = v112（字型堆疊深度，var_7D0）
            lea  eax, [ecx - 1]
            cmp  eax, ecx
            jb   short BoundsOk
            int  3                       ; 理論上不會發生，照native原本的防呆邊界檢查保留
        BoundsOk:
            mov  eax, [esp + eax * 4 + 0x214]  ; eax = v113[v112-1]（var_7CC陣列）
            mov  ecx, [esp + 0x3D0]      ; ecx = v126（字型槽指標陣列基底，var_610）
            mov  ecx, [ecx + eax * 4]    ; ecx = 目前字型槽物件
            mov  edx, [ecx]              ; edx = vtable
            push 0x20
            call dword ptr [edx + 0x238] ; eax = 新鮮的glyph記錄指標（不寫回var_998，刻意收斂影響範圍，見.h說明）
            movsx ecx, byte ptr [eax + 0x10]
            jmp  Resume

        NotNewspaper:
            ; ── 非報紙分類：完全照native原本兩句指令 ──
            mov  eax, [esp + 0x48]       ; eax = v98（var_998，函式進入/<font>切換時快取的舊指標）
            movsx ecx, byte ptr [eax + 0x10]

        Resume:
            mov  edx, 0x559DC0
            jmp  edx
        }
    }

    static bool PatchSite(DWORD site, const BYTE expected[8], void* detour, const char* label)
    {
        BYTE* p = (BYTE*)site;
        for (int i = 0; i < 8; i++)
        {
            if (p[i] != expected[i])
            {
                Log::Write("[NewsPaperSpaceAdvanceFix] %s 0x%08X第%d byte是0x%02X，預期0x%02X"
                           "——版本不符或已被其他patch動過，放棄安裝",
                           label, site, i, p[i], expected[i]);
                return false;
            }
        }

        DWORD oldProt = 0;
        VirtualProtect(p, 8, PAGE_EXECUTE_READWRITE, &oldProt);
        p[0] = 0xE9;
        *(INT32*)(p + 1) = (INT32)((DWORD)detour - (DWORD)(p + 5));
        p[5] = p[6] = p[7] = 0x90;
        VirtualProtect(p, 8, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), p, 8);

        Log::Write("[NewsPaperSpaceAdvanceFix] %s 安裝完成：site=0x%08X detour=%p", label, site, detour);
        return true;
    }

    bool Install()
    {
        // 原始byte。
        static const BYTE kExpected[8] = { 0x8B, 0x44, 0x24, 0x48, 0x0F, 0xBE, 0x48, 0x10 };

        bool ok = PatchSite(kSite, kExpected, &Detour_SpaceAdvance, "case32空白advance修正點(0x559DB8)");
        Log::Write("[NewsPaperSpaceAdvanceFix] 安裝結果：%d", ok);
        return ok;
    }
}
