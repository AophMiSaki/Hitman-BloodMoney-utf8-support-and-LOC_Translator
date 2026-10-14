#include "AllocateSlotStaleRedirectFixHook.h"
#include "Log.h"

namespace AllocateSlotStaleRedirectFixHook
{
    // sub_48F560(AllocateSlot)找到空閒slot後的尾段（IDA反組譯逐byte確認）：
    //   0048F57B  8B D0        mov edx, eax        ; edx = idx
    //   0048F57D  6B D2 44     imul edx, 44h        ; edx = idx*68
    //   0048F580  C7 44 0A 30 01 00 00 00
    //                          mov dword ptr [edx+ecx+30h], 1   ; +0x2C=1(占用)
    //   0048F588  C3           retn
    // 偷開頭5 bytes(8B D0 6B D2 44)塞jmp，detour裡重跑這2句指令拿到
    // edx=idx*68、ecx=this，接著呼叫OnAllocateSlot檢查/清掉這格的+0x30，
    // 再jmp回0x0048F580讓原本「+0x2C=1;retn」照常執行。
    static const DWORD kVA_Entry = 0x0048F57B;
    static const DWORD kVA_ReturnTo = 0x0048F580;
    static const BYTE kExpected[5] = { 0x8B, 0xD0, 0x6B, 0xD2, 0x44 };

    // sub_4466D0：native的釋放函式，__cdecl、單一參數(要釋放的指標)，呼叫端
    // 自己add esp,4清堆疊（見sub_48F190/sub_48F690既有呼叫方式）。
    typedef void (__cdecl* FreeFn)(void*);
    static const DWORD kVA_Free = 0x004466D0;

    static DWORD g_clearedLogCount = 0;

    // __cdecl，供naked detour呼叫。thisPtr=ZTextureManagerD3D實例，
    // idxTimes68=剛配置到的slot index*68。textureDef = thisPtr+idxTimes68+4，
    // +0x30欄位地址 = textureDef+0x30。非0代表這格「上一個占用者」是多子頁
    // 貼圖、殘留了舊的count/redirect表：呼叫native釋放函式正確free掉、再
    // 清成0，避免接手的新貼圖繼承到跟自己內容對不上號的舊count。
    static void __cdecl OnAllocateSlot(DWORD thisPtr, DWORD idxTimes68)
    {
        DWORD* redirectField = (DWORD*)(thisPtr + idxTimes68 + 4 + 0x30);
        if (*redirectField)
        {
            FreeFn freeFn = (FreeFn)kVA_Free;
            freeFn((void*)*redirectField);
            *redirectField = 0;

            if (g_clearedLogCount < 20)
            {
                g_clearedLogCount++;
                Log::Write("[AllocateSlotStaleRedirectFixHook] 清除殘留redirect表(#%lu)：slot這格的+0x30原本非0，已free並歸零，避免下個接手貼圖繼承錯誤count",
                           g_clearedLogCount);
            }
        }
    }

    // 進入時：eax=result(slot index)，ecx=this。pushad/popad確保eax/ecx/edx
    // 在呼叫OnAllocateSlot前後完全不變，重跑被偷走的2句指令、jmp回原函式
    // 時暫存器狀態跟native原本的執行流程一致。
    static __declspec(naked) void AllocateSlotDetour()
    {
        __asm
        {
            mov  edx, eax
            imul edx, 44h

            pushad
            push edx
            push ecx
            call OnAllocateSlot
            add  esp, 8
            popad

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
                Log::Write("[AllocateSlotStaleRedirectFixHook] 0x%08X第%d byte是0x%02X，預期0x%02X——版本不符或已被其他patch動過，放棄安裝",
                           kVA_Entry, i, p[i], kExpected[i]);
                return;
            }
        }

        DWORD oldProt = 0;
        VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &oldProt);
        *p = 0xE9;
        *(INT32*)(p + 1) = (INT32)((DWORD)&AllocateSlotDetour - (DWORD)(p + 5));
        VirtualProtect(p, 5, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), p, 5);

        Log::Write("[AllocateSlotStaleRedirectFixHook] sub_48F560殘留+0x30清除hook安裝完成：site=0x%08X detour=%p", kVA_Entry, &AllocateSlotDetour);
    }
}
