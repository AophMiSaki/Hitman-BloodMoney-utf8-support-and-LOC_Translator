#include "LabelWidthFixShop.h"
#include "Log.h"
#include "Config.h"
#include "GlyphAtlas.h"
#include "FontCategory.h"
#include "CJKRange.h"
#include "LocHook.h"

namespace LabelWidthFixShop
{
    // 跟LabelWidthFixInventory共用同一個ini開關（[Debug] LabelWidthFixDiagEnable），
    // 同一套機制的診斷log，不另外開一個ini key。
    static bool s_diagEnable = false;

    // native側常數/函式指標。
    // sub_67DDE0 0x67E175: call sub_55E550，前面依序push ebx(padding=5)
    // →lea ecx(outBuf)→push ecx→push edx([esi+0x84]=sourceWidget)，跟
    // Inventory Hook A/B同一種__stdcall(int,float*,int)呼叫慣例，detour
    // 進入點[esp+0/4/8]對應sourceWidget/outBuf/padding。
    static const DWORD kSite = 0x0067E175;

    typedef int (__stdcall *Fn_MeasureLabel)(int, float*, int);
    static const Fn_MeasureLabel NativeMeasureLabel = (Fn_MeasureLabel)0x0055E550;

    typedef int (__cdecl *Fn_HasChild)(int);
    typedef int (__thiscall *Fn_NextChild)(int);
    static const Fn_HasChild  NativeHasChild  = (Fn_HasChild)0x004EB700;
    static const Fn_NextChild NativeNextChild = (Fn_NextChild)0x00431E20;

    typedef DWORD (__thiscall *Fn_GetRttiTag)(void*);
    static DWORD GetRttiTag(void* obj)
    {
        DWORD* vtbl = *(DWORD**)obj;
        Fn_GetRttiTag fn = (Fn_GetRttiTag)(*(DWORD*)((BYTE*)vtbl + 0x34));
        return fn(obj);
    }

    static const DWORD kRttiTagAddr  = 0x009A2B98;
    static const DWORD kRttiMaskAddr = 0x009A2B9C;

    // native逐byte查vanilla西文字寬表，CJK固定被當成查無此字的窄佔位、每個
    // CJK視覺字元少算5px（原文模式下用來補回這個固定低估量）。
    static const int kNativeCjkUnderestimatePerChar = 5;

    // 標準UTF-8變長decode(1~4 bytes)，只取codepoint值，不驗證overlong等
    // edge case——來源是native已在用的合法UTF-8字串，不需要額外防禦。
    static unsigned int DecodeUtf8Next(const unsigned char*& p)
    {
        unsigned char b0 = *p;
        if (b0 < 0x80) { p += 1; return b0; }
        if ((b0 & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80)
        {
            unsigned int cp = ((b0 & 0x1Fu) << 6) | (p[1] & 0x3Fu);
            p += 2; return cp;
        }
        if ((b0 & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80)
        {
            unsigned int cp = ((b0 & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
            p += 3; return cp;
        }
        if ((b0 & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80)
        {
            unsigned int cp = ((b0 & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
            p += 4; return cp;
        }
        p += 1;
        return b0;
    }

    // 單一codepoint「我方真正視覺寬度」——用gm.gmCellIncX(GDI原生advance)
    // 而非cellWidth(atlas貼圖用、含padding，天生偏大)，跟GlyphHook.cpp
    // Stage C實際渲染用的度量一致；GetSpacing()是使用者自訂字距，兩邊各自
    // 另外加，不含在cellWidth/gmCellIncX任一欄位裡。統一用
    // FontCategory::General（商店Price欄位不是報紙/字幕情境）。
    static int OurCharWidth(unsigned int codepoint)
    {
        const GlyphAtlas::Entry* e = GlyphAtlas::GetGlyph(codepoint, FontCategory::General);
        if (!e) return 0;
        return (int)e->gm.gmCellIncX + GlyphAtlas::GetSpacing(FontCategory::General);
    }

    // sub_55E550一進函式就呼叫sub_4E5E20(ecx=widget)，把widget自己的
    // inner=*(widget+4)欄位(+0x24/+0x28/+0x2C)整包寫回目的地——這是native
    // 自帶、每個widget各自獨立的版面基準值，不靠呼叫端stack累加。這裡直接
    // 照offset讀記憶體(純讀取，不呼叫函式本體)。
    static float ReadWidgetOffset(int widget)
    {
        if (widget == 0 || IsBadReadPtr((void*)widget, 8)) return 0.0f;
        int inner = *(int*)(widget + 4);
        if (inner == 0 || IsBadReadPtr((void*)inner, 0x30)) return 0.0f;
        return *(float*)(inner + 0x24);
    }

    // 依F11原文/譯文模式二分：
    //   譯文模式：整段文字都是我方合成字型輸出，逐codepoint(不分CJK/ASCII)
    //     用atlas實際寬度加總；childOffset是native自帶、跟字型無關的固定
    //     偏移，加回去。
    //   原文模式：只有CJK被合成，v5裡ASCII/標點/childOffset的貢獻是對的，
    //     只把CJK部分固定低估的5px/字換成atlas實際寬度。
    static float ComputeOurV5(const char* v8, float childOffset, float v5)
    {
        const unsigned char* p = (const unsigned char*)v8;

        if (LocHook::IsTranslationActive())
        {
            int total = 0;
            while (*p)
            {
                unsigned int cp = DecodeUtf8Next(p);
                total += OurCharWidth(cp);
            }
            return childOffset + (float)total;
        }

        int cjkOurs = 0;
        int cjkNativeUnderestimate = 0;
        while (*p)
        {
            unsigned int cp = DecodeUtf8Next(p);
            if (!BMIsCJKCodepoint(cp)) continue;
            cjkOurs += OurCharWidth(cp);
            cjkNativeUnderestimate += kNativeCjkUnderestimatePerChar;
        }
        return v5 - (float)cjkNativeUnderestimate + (float)cjkOurs;
    }

    // 先呼叫native本體(保留child list走訪/RTTI篩選/padding疊加邏輯不變)，
    // 用ReadWidgetOffset(sourceWidget)反推出乾淨的v5(不受呼叫端stack殘留
    // 值影響)，重新走訪一次child list取label文字+child自帶offset，算出
    // ourV5後用correction=ourV5-v5疊加回*outBuf。
    static void __cdecl FixCore(int sourceWidget, float* outBuf, int padding)
    {
        NativeMeasureLabel(sourceWidget, outBuf, padding);
        float outBufAfterNative = *outBuf;

        float widgetOffset = ReadWidgetOffset(sourceWidget);
        float v5 = outBufAfterNative - widgetOffset - (float)padding;

        DWORD rttiTag  = *(DWORD*)kRttiTagAddr;
        DWORD rttiMask = *(DWORD*)kRttiMaskAddr;

        int matchCount = 0;
        const char* firstCaption = nullptr;
        float firstChildOffset = 0.0f;

        int child = *(int*)(sourceWidget + 0x40);
        if (NativeHasChild(child))
        {
            do
            {
                int c = *(int*)(child + 0x60);
                if (c != 0 && !IsBadReadPtr((void*)c, 0x94) &&
                    (GetRttiTag((void*)c) & rttiMask) == rttiTag)
                {
                    DWORD v7 = *(DWORD*)((BYTE*)c + 0x90);
                    const char* v8 = v7 ? (const char*)(v7 + 0xC) : nullptr;
                    if (v8 && !IsBadReadPtr((void*)v8, 16))
                    {
                        matchCount++;
                        // matchCount一般為2且兩child字串相同（重複widget），
                        // 取第一個match當基準即可；商店畫面同一套widget
                        // class，沿用同一個假設。
                        if (!firstCaption)
                        {
                            firstCaption = v8;
                            firstChildOffset = ReadWidgetOffset(c);
                        }
                    }
                }
                child = NativeNextChild(child);
            } while (NativeHasChild(child));
        }

        if (matchCount == 0 || !firstCaption)
        {
            // 沒有符合條件的child時，*outBuf維持native原樣，不做修正。
            if (s_diagEnable)
                Log::Write("[LabelWidthFixShop] matchCount=0，找不到label文字，維持native原樣(outBuf=%.3f)",
                           *outBuf);
            return;
        }

        float ourV5 = ComputeOurV5(firstCaption, firstChildOffset, v5);

        // ourV5算出負值/明顯不合理時退回v5，correction=0，避免把widget
        // 定位到畫面外。
        if (ourV5 < 0.0f) ourV5 = v5;

        float correction = ourV5 - v5;
        *outBuf = outBufAfterNative + correction;

        if (s_diagEnable)
            Log::Write("[LabelWidthFixShop] matchCount=%d widgetOffset=%.3f v5=%.3f ourV5=%.3f correction=%.3f outBuf=%.3f",
                       matchCount, widgetOffset, v5, ourV5, correction, *outBuf);
    }

    // 進入時ESP跟native在patch點當下完全相同(jmp進來，不是call，沒有
    // push/pop失衡)。
    static __declspec(naked) void Detour()
    {
        __asm
        {
            mov eax, [esp + 0]   ; a1 = sourceWidget([esi+0x84]=Price label widget)
            mov edx, [esp + 4]   ; a2 = outBuf
            mov ecx, [esp + 8]   ; a3 = padding(=5)

            push ecx
            push edx
            push eax
            call FixCore
            add esp, 12          ; cdecl清掉剛才push的3個參數

            add esp, 12          ; 補清掉原本a1/a2/a3

            mov eax, 0x0067E17A  ; kResume：原本call指令後面那條指令
            jmp eax
        }
    }

    static bool PatchSite(DWORD site, const BYTE expected[5], void* detour, const char* label)
    {
        BYTE* p = (BYTE*)site;
        for (int i = 0; i < 5; i++)
        {
            if (p[i] != expected[i])
            {
                Log::Write("[LabelWidthFixShop] %s 0x%08X第%d byte是0x%02X，預期0x%02X"
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

        Log::Write("[LabelWidthFixShop] %s 安裝完成：site=0x%08X detour=%p", label, site, detour);
        return true;
    }

    bool Install(HMODULE hModule)
    {
        s_diagEnable = Config::LoadDebug(hModule).labelWidthFixDiagEnable;

        static const BYTE kExpected[5] = { 0xE8, 0xD6, 0x03, 0xEE, 0xFF };

        bool ok = PatchSite(kSite, kExpected, &Detour, "商店Price欄位(0x67E175)");
        return ok;
    }
}
