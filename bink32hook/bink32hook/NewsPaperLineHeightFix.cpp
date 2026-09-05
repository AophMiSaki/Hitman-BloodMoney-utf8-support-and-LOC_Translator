#include "NewsPaperLineHeightFix.h"
#include "Log.h"
#include "ZipPathTrace.h"
#include "GlyphHook.h"
#include <cstring>

namespace NewsPaperLineHeightFix
{
    static const DWORD kSiteA = 0x00559F10; // 讀var_998+0x11的第一處，回跳0x559F18
    static const DWORD kSiteB = 0x00559FB2; // 第二處，回跳0x559FBA

    // 跟NewsPaperSpaceAdvanceFix.cpp同一套判斷，這裡刻意不共用——修行為的
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

    // 對GlyphHook::GetNewspaperLineHeightForSlot()的cdecl包裝——MSVC naked
    // __asm可以直接用未修飾的檔案內static函式名稱當call目標，不用處理
    // C++命名空間修飾符號。
    static int __cdecl GetLineHeightWrapper(void* fontSlotThis)
    {
        return GlyphHook::GetNewspaperLineHeightForSlot(fontSlotThis);
    }

    // jmp進入，ESP與native在patch點一致，[esp+0x210]/[esp+0x214]/[esp+0x3D0]/
    // [esp+0x48] (var_7D0/var_7CC/var_610/var_998) 直接沿用。
    //
    // 報紙分類分支：重新推導目前字型槽指標(fontSlotThis)，查詢我方已探測的
    // 正確lineHeight，成功則取代原本讀var_998+0x11的值（回傳未乘1.5的原始
    // 值，native緊接著自己會乘，銜接一致）。探測失敗(回傳0)或非報紙分類：
    // 照native原本兩句指令執行。
    static __declspec(naked) void Detour_LineHeightA()
    {
        __asm
        {
            call IsNewspaperContext
            test eax, eax
            jz   NotNewspaperA

            mov  ecx, [esp + 0x210]      ; ecx = var_7D0（字型堆疊深度）
            lea  eax, [ecx - 1]
            cmp  eax, ecx
            jb   short BoundsOkA
            int  3                       ; 理論上不會發生，照native原本的防呆邊界檢查保留
        BoundsOkA:
            mov  eax, [esp + eax * 4 + 0x214]  ; eax = var_7CC[depth-1]
            mov  ecx, [esp + 0x3D0]      ; ecx = var_610（字型槽指標陣列基底）
            mov  ecx, [ecx + eax * 4]    ; ecx = 目前字型槽物件(fontSlotThis)

            push ecx
            call GetLineHeightWrapper
            add  esp, 4
            test eax, eax
            jz   UseNativeA
            mov  edx, eax
            jmp  ResumeA

        UseNativeA:
        NotNewspaperA:
            mov  ecx, [esp + 0x48]       ; ecx = var_998（函式進入/<font>切換時快取的舊指標）
            movsx edx, byte ptr [ecx + 0x11]

        ResumeA:
            mov  eax, 0x559F18
            jmp  eax
        }
    }

    static __declspec(naked) void Detour_LineHeightB()
    {
        __asm
        {
            call IsNewspaperContext
            test eax, eax
            jz   NotNewspaperB

            mov  ecx, [esp + 0x210]
            lea  eax, [ecx - 1]
            cmp  eax, ecx
            jb   short BoundsOkB
            int  3
        BoundsOkB:
            mov  eax, [esp + eax * 4 + 0x214]
            mov  ecx, [esp + 0x3D0]
            mov  ecx, [ecx + eax * 4]

            push ecx
            call GetLineHeightWrapper
            add  esp, 4
            test eax, eax
            jz   UseNativeB
            mov  edx, eax
            jmp  ResumeB

        UseNativeB:
        NotNewspaperB:
            mov  ecx, [esp + 0x48]
            movsx edx, byte ptr [ecx + 0x11]

        ResumeB:
            mov  eax, 0x559FBA
            jmp  eax
        }
    }

    static bool PatchSite(DWORD site, const BYTE expected[8], void* detour, const char* label)
    {
        BYTE* p = (BYTE*)site;
        for (int i = 0; i < 8; i++)
        {
            if (p[i] != expected[i])
            {
                Log::Write("[NewsPaperLineHeightFix] %s 0x%08X第%d byte是0x%02X，預期0x%02X"
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

        Log::Write("[NewsPaperLineHeightFix] %s 安裝完成：site=0x%08X detour=%p", label, site, detour);
        return true;
    }

    bool Install()
    {
        // 原始byte（兩處相同）。
        static const BYTE kExpected[8] = { 0x8B, 0x4C, 0x24, 0x48, 0x0F, 0xBE, 0x51, 0x11 };

        bool okA = PatchSite(kSiteA, kExpected, &Detour_LineHeightA, "case10路徑v102更新點A(0x559F10)");
        bool okB = PatchSite(kSiteB, kExpected, &Detour_LineHeightB, "case10路徑v102更新點B(0x559FB2)");
        Log::Write("[NewsPaperLineHeightFix] 安裝結果：A=%d B=%d", okA, okB);
        return okA && okB;
    }
}
