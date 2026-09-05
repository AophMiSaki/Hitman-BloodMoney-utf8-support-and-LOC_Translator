#include "TextureManagerReleaseHook.h"
#include "NativeTextureRegistry.h"
#include "LocHook.h"
#include "Log.h"

namespace TextureManagerReleaseHook
{
    // sub_48F190 = ZTextureManagerD3D::ReleaseTextures。函式開頭5 bytes
    // （push ebx / push esi / mov ebx,ecx / push edi）剛好是乾淨的指令邊界，
    // 5-byte jmp正好蓋滿。進入時ecx=this（ZTextureManagerD3D實例本身），
    // 純thiscall、無stack參數。kExpected為這5 bytes，安裝前比對防呆。
    static const DWORD kVA_Entry = 0x0048F190;
    static const DWORD kVA_ReturnTo = 0x0048F195;
    static const BYTE kExpected[5] = { 0x53, 0x56, 0x8B, 0xD9, 0x57 };

    static DWORD g_thisStash = 0;

    // __cdecl，供naked detour呼叫。只做輕量工作：重設NativeTextureRegistry
    // 兩個字型槽的nativeIndex/pushedGeneration，不做任何native/D3D呼叫
    // （重量級的AllocateSlot+DecodeBitmapPtr留給下次Hook_BeginScene的
    // Update()自然觸發）。這支函式可能在LoadTEXBuffer場景切換或
    // ZTextureManagerD3D解構子（引擎關閉/裝置重建）兩條路徑下被觸發，兩種
    // 情境這裡做的事都一樣安全。
    static void __cdecl OnReleaseTextures(DWORD thisPtr)
    {
        NativeTextureRegistry::InvalidateAllSlots();

        // 原文/譯文熱鍵切換功能的LOC快取失效，比照texture reset用同一觸發
        // 點。純設flag+clear map，不做檔案I/O，這裡呼叫安全。
        LocHook::InvalidateCache();

        Log::Write("[TextureManagerReleaseHook] ReleaseTextures命中：this=0x%08X，已清除NativeTextureRegistry字型槽狀態＋LOC快取",
                   thisPtr);
    }

    // 進入時：ecx=this。pushad/popad確保ecx在呼叫OnReleaseTextures前後完全
    // 不變，重跑被偷走的4句指令時this仍然正確。
    static __declspec(naked) void ReleaseTexturesDetour()
    {
        __asm
        {
            mov  [g_thisStash], ecx

            pushad
            push dword ptr [g_thisStash]
            call OnReleaseTextures
            add  esp, 4
            popad

            ; 原樣重跑被覆蓋的4句指令（此時ecx跟進入時相同）。
            push ebx
            push esi
            mov  ebx, ecx
            push edi

            jmp  kVA_ReturnTo
        }
    }

    void Install()
    {
        BYTE* p = (BYTE*)kVA_Entry;
        for (int i = 0; i < 5; i++)
        {
            if (p[i] != kExpected[i])
            {
                Log::Write("[TextureManagerReleaseHook] 0x%08X第%d byte是0x%02X，預期0x%02X——版本不符或已被其他patch動過，放棄安裝",
                           kVA_Entry, i, p[i], kExpected[i]);
                return;
            }
        }

        DWORD oldProt = 0;
        VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &oldProt);
        *p = 0xE9;
        *(INT32*)(p + 1) = (INT32)((DWORD)&ReleaseTexturesDetour - (DWORD)(p + 5));
        VirtualProtect(p, 5, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), p, 5);

        Log::Write("[TextureManagerReleaseHook] sub_48F190 entry hook安裝完成：site=0x%08X detour=%p", kVA_Entry, &ReleaseTexturesDetour);
    }
}
