#include "Utf8ReencodeFixHook.h"
#include "Log.h"

namespace Utf8ReencodeFixHook
{
    // native sub_438FE0：通用codepoint->UTF-8編碼器。a1=輸出buffer(至少
    // 4+1 bytes)，a2=codepoint，回傳寫入的byte數1~4。
    typedef int(__cdecl* EncodeCodepointFn)(BYTE* out, unsigned int codepoint);
    static const EncodeCodepointFn sub_438FE0 = (EncodeCodepointFn)0x00438FE0;

    static const DWORD kVA_5A4850 = 0x005A4850;
    static const BYTE kExpected5A4850[5] = { 0x83,0xEC,0x08, 0x53, 0x55 }; // sub esp,8 / push ebx / push ebp

    static bool IsUtf8Continuation(BYTE b)
    {
        return (b & 0xC0) == 0x80;
    }

    // src指向的位置開始，判斷是不是一段完整、合法的UTF-8多byte序列
    // （lead byte + 對應數量的continuation byte）。是的話回傳序列長度
    // (2~4)；不是的話（純ASCII、或不構成合法序列的孤立高位byte、或字串
    // 提早結束）回傳0，代表呼叫端應該照native原本邏輯把*src當單一
    // codepoint重新編碼。continuation byte檢查在遇到不符合的byte（含
    // 字串結尾的0x00）時立刻回傳，不會讀到字串NUL終止byte以外的記憶體。
    static int Utf8SeqLenAt(const BYTE* src)
    {
        BYTE lead = src[0];
        if (lead < 0xC2 || lead > 0xF4) return 0; // ASCII、continuation byte、或0xC0/0xC1/0xF5+這類UTF-8不合法lead byte
        int need = (lead < 0xE0) ? 1 : (lead < 0xF0) ? 2 : 3;
        for (int i = 1; i <= need; i++)
        {
            if (!IsUtf8Continuation(src[i]))
                return 0;
        }
        return need + 1;
    }

    // sub_5A4850的完整重新實作，介面/邊界行為（maxlen、提早補0收尾）跟
    // native原版一致，差別只在「已經是合法UTF-8序列的部分原樣複製，不重
    // 新編碼」。cdecl呼叫慣例下args由呼叫端清stack，這裡直接被jmp導入
    // 取代整個原函式，不需要跳回原本函式本體。
    static char __cdecl Reencode(BYTE* dst, const BYTE* src, unsigned int maxlen)
    {
        unsigned int pos = 0;
        if (*src)
        {
            for (;;)
            {
                if (pos + 1 >= maxlen)
                {
                    dst[pos] = 0;
                    return (char)(DWORD)dst;
                }

                int seqLen = Utf8SeqLenAt(src);
                // sub_438FE0在4-byte分支會多寫一個a1[4]=0終止byte（native
                // 呼叫端用_DWORD v8[2]即8 bytes buffer），這裡比照同樣留
                // 8 bytes，避免只留4 bytes在4-byte codepoint情況下越界寫。
                BYTE encoded[8];
                const BYTE* outPtr;
                int outLen;
                if (seqLen > 0)
                {
                    outPtr = src;
                    outLen = seqLen;
                }
                else
                {
                    outLen = sub_438FE0(encoded, (unsigned char)*src);
                    outPtr = encoded;
                }

                if ((unsigned int)outLen + pos + 1 >= maxlen)
                    break;

                for (int i = 0; i < outLen; i++)
                    dst[pos + i] = outPtr[i];
                pos += outLen;
                src += (seqLen > 0) ? seqLen : 1;
                if (!*src)
                {
                    dst[pos] = 0;
                    return (char)(DWORD)dst;
                }
            }
            dst[pos] = 0;
        }
        else
        {
            *dst = 0;
        }
        return (char)(DWORD)dst;
    }

    void Install()
    {
        BYTE* p = (BYTE*)kVA_5A4850;
        for (int i = 0; i < 5; i++)
        {
            if (p[i] != kExpected5A4850[i])
            {
                Log::Write("[Utf8ReencodeFixHook] sub_5A4850 0x%08X第%d byte是0x%02X，預期0x%02X"
                           "——版本不符或已被其他patch動過，放棄安裝", kVA_5A4850, i, p[i], kExpected5A4850[i]);
                return;
            }
        }

        DWORD oldProt = 0;
        VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &oldProt);
        p[0] = 0xE9;
        *(INT32*)(p + 1) = (INT32)((DWORD)&Reencode - (DWORD)(p + 5));
        VirtualProtect(p, 5, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), p, 5);

        Log::Write("[Utf8ReencodeFixHook] sub_5A4850 inline hook安裝完成：site=0x%08X detour=%p",
                   kVA_5A4850, &Reencode);
    }
}
