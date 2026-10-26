#include "GetTextureBoundsFixHook.h"
#include "Log.h"
#include "GameAddr.h"

namespace GetTextureBoundsFixHook
{
    // sub_48F140內，0x0048F175起3 bytes（IDA反組譯逐byte確認）：
    //   0048F175  72 01   jb  short loc_48F178   ; a3 < count 才跳過int3
    //   0048F177  CC      int 3
    // 改成：
    //   0048F175  73 10   jae short loc_48F187   ; a3 >= count 直接跳到
    //                                              sentinel出口(unk_90ADC8)
    //   0048F177  90      nop                     ; a3 < count 原本路徑不變
    //                                              （緊接的0x0048F178起
    //                                              redirect讀取完全不動）
    static DWORD g_vaPatch = 0;
    static const BYTE kExpected[3] = { 0x72, 0x01, 0xCC };
    static const BYTE kPatched[3]  = { 0x73, 0x10, 0x90 };

    void Install()
    {
        g_vaPatch = GameAddr::Addr(AddressTable::GetTextureBounds_BreakpointSite);
        if (!g_vaPatch)
        {
            Log::Write("[GetTextureBoundsFixHook] 位址查詢失敗（版本不明或位址表缺項），放棄安裝");
            return;
        }

        BYTE* p = (BYTE*)g_vaPatch;
        if (IsBadReadPtr(p, 3))
        {
            Log::Write("[GetTextureBoundsFixHook] 0x%08X無法讀取，放棄安裝", g_vaPatch);
            return;
        }
        for (int i = 0; i < 3; i++)
        {
            if (p[i] != kExpected[i])
            {
                Log::Write("[GetTextureBoundsFixHook] 0x%08X第%d byte是0x%02X，預期0x%02X——版本不符或已被其他patch動過，放棄安裝",
                           g_vaPatch, i, p[i], kExpected[i]);
                return;
            }
        }

        DWORD oldProt = 0;
        VirtualProtect(p, 3, PAGE_EXECUTE_READWRITE, &oldProt);
        p[0] = kPatched[0];
        p[1] = kPatched[1];
        p[2] = kPatched[2];
        VirtualProtect(p, 3, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), p, 3);

        Log::Write("[GetTextureBoundsFixHook] sub_48F140越界subpage崩潰修法安裝完成：site=0x%08X（72 01 CC -> 73 10 90）", g_vaPatch);
    }
}
