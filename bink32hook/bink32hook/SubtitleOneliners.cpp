#include "SubtitleOneliners.h"
#include "SubtitleGate.h"
#include "Log.h"
#include <Windows.h>
#include <cstdio>
#include <cstdint>
#include <cmath>

namespace SubtitleOneliners
{
    // ---- sub_6BACC0：M04 專屬寫死彩蛋台詞（詐死動作用錯時的吐槽），跟
    // sub_6AACA0 一樣完全不呼叫 sub_661FD0 顯示字幕 ----
    //
    // 跟 sub_6AACA0 不同，這裡的 LOC key 是完全寫死的常數字串，沒有
    // topic/index/role 三個動態參數，函式本身也是 cdecl 無參數。guard 條件是
    // sub_51D680(actorPtr)（this=`*(dword_82083C)+0xA40`）回傳非 0 代表還在
    // 冷卻/已觸發過，這次呼叫其實不會真的播——原函式回傳值在 skip 分支跟成功
    // 分支不是同一個值域，直接比對容易誤判，所以改成在 detour 裡自己重放同一
    // 個 guard 呼叫（sub_51D680 是唯讀查詢，重放一次不影響原函式接下來還會
    // 再呼叫一次的真正行為）來判斷這次呼叫是否真的會觸發。
    //
    // 0x6BACC0 起 5 bytes（`mov eax,dword_82083C` 剛好一條完整指令），5-byte
    // JMP patch 不需要補 NOP，續行點對齊到下一條指令 `sub esp,204h`。
    static const DWORD kVA_FeignDeathEggSite = 0x006BACC0;
    static const DWORD kVA_FeignDeathEggContinue = 0x006BACC5;
    static const BYTE kExpectedFeignDeathEgg[5] = { 0xA1, 0x3C, 0x20, 0x82, 0x00 };

    static const char* kFeignDeathEggLocKey = "M04/Oneliners/HMUsesFeignDeathWrong/M04_HMUFDW_01HM";
    static const char* kFeignDeathEggCategory = "HMUsesFeignDeathWrong";

    // thiscall sub_51D680(void* actorPtr) -> bool（只看 AL）。
    typedef char (__thiscall* CheckStillWaitingFn)(void* thisPtr);
    static const CheckStillWaitingFn NativeCheckStillWaiting = (CheckStillWaitingFn)0x0051D680;

    static void __cdecl OnFeignDeathEggEntry()
    {
        DWORD base = *(const DWORD*)0x0082083C;
        if (!base) return;
        DWORD actorPtr = *(const DWORD*)(base + 0xA40);
        if (!actorPtr) return;

        if (NativeCheckStillWaiting((void*)actorPtr)) return;

        SubtitleGate::DispatchSubtitleLine("Oneliners", kFeignDeathEggCategory, kFeignDeathEggLocKey);
    }

    static __declspec(naked) void Detour_FeignDeathEggEntry()
    {
        __asm
        {
            pushfd
            pushad
            call OnFeignDeathEggEntry
            popad
            popfd

            mov eax, dword ptr ds:[0082083Ch]
            jmp kVA_FeignDeathEggContinue
        }
    }

    static void InstallFeignDeathEggHook()
    {
        BYTE* p = (BYTE*)kVA_FeignDeathEggSite;
        for (int i = 0; i < 5; i++)
        {
            if (p[i] != kExpectedFeignDeathEgg[i])
            {
                Log::Write("[SubtitleOneliners] 0x%08X第%d byte是0x%02X，預期0x%02X"
                           "——版本不符或已被其他patch動過，放棄安裝sub_6BACC0字幕補完hook", kVA_FeignDeathEggSite, i, p[i], kExpectedFeignDeathEgg[i]);
                return;
            }
        }

        DWORD oldProt = 0;
        VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &oldProt);
        p[0] = 0xE9;
        *(INT32*)(p + 1) = (INT32)((DWORD)&Detour_FeignDeathEggEntry - (DWORD)(p + 5));
        VirtualProtect(p, 5, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), p, 5);

        Log::Write("[SubtitleOneliners] sub_6BACC0(M04詐死彩蛋台詞)字幕補完hook安裝完成：site=0x%08X detour=%p",
                   kVA_FeignDeathEggSite, &Detour_FeignDeathEggEntry);
    }

    // ---- sub_6A4240：真正的 Oneliners（單人事件反應喊話，如警報/發現屍體/
    // 電梯閒聊等）觸發函式，同樣完全不呼叫 sub_661FD0 顯示字幕 ----
    //
    // sub_6A4240 跟 sub_6AACA0 幾乎同一套模板（sub_4E5BE0(a1) 解析 ZHM3Actor→
    // sub_4652C0/sub_464F10 查 LOC→vtable+0x660 播語音），但**沒有 sprintf 組
    // key**，呼叫端直接傳完整 LOC key 字串進來（String2），比 sub_6AACA0 單純。
    //
    // 呼叫端是資料驅動、runtime 才決定 index 的間接呼叫，靜態找不到，所以
    // 直接在函式入口裝 hook。沒有動態 topic/index/role 參數可組 category，直接
    // 用完整 LOC key 字串同時當 category 跟 locKey——Oneliners 是一次性單句
    // 喊話，不像雙人 Dialogue 需要「同一輪對話延續同一欄位」的分類語意。
    //
    // 0x6A4240 起 10 bytes（`mov eax,[esp+4]` 4 bytes ＋ `sub esp,204h` 6 bytes
    // 共兩條完整指令），5-byte JMP patch 後剩 5 bytes 補 NOP，續行點對齊到
    // 下一條指令(`push esi`)。
    static const DWORD kVA_OnelinerSite = 0x006A4240;
    static const DWORD kVA_OnelinerContinue = 0x006A424A;
    static const BYTE kExpectedOneliner[10] = { 0x8B, 0x44, 0x24, 0x04, 0x81, 0xEC, 0x04, 0x02, 0x00, 0x00 };

    // NPC/玩家距離過濾（SubtitleGate::IsNpcWithinHearRange 等，sub_6AACA0 跟
    // 這裡共用同一套）。
    static void __cdecl OnOnelinerCall(int a1, const char* String2, int origResult)
    {
        if (!origResult) return;
        if (!String2 || !String2[0]) return;

        void* actorPtr = (void*)SubtitleGate::ResolveActorHandle(a1);
        float distSq = 0.0f;
        bool inRange = SubtitleGate::IsNpcWithinHearRange(actorPtr, &distSq);
        if (SubtitleGate::DiagEnabled() && actorPtr && SubtitleGate::NpcRadiusFilterEnabled())
        {
            Log::Write("[SubtitleOneliners] Distance key=\"%s\" dist=%.1f threshold=%.1f %s",
                       String2, sqrtf(distSq), sqrtf(SubtitleGate::NpcHearRadiusSq()), inRange ? "顯示" : "擋下");
        }
        if (!inRange) return;

        SubtitleGate::DispatchSubtitleLine("Oneliners", String2, String2);
    }

    // 執行被覆蓋的原指令(mov eax,[esp+4]; sub esp,204h)後跳回續行點，讓原
    // 函式本體完整跑完（cdecl，retn 時彈回呼叫者，也就是下面
    // Detour_OnelinerEntry 裡 `call Trampoline_OnelinerEntry` 的下一行）。
    static __declspec(naked) void Trampoline_OnelinerEntry()
    {
        __asm
        {
            mov eax, [esp+4]
            sub esp, 204h
            jmp kVA_OnelinerContinue
        }
    }

    // 進入時(cdecl)：[esp]=returnAddr，[esp+4]=a1，[esp+8]=String2(完整 LOC
    // key)。先呼叫 Trampoline_OnelinerEntry 執行完整原函式本體取得真正回傳
    // 值，非 0（LOC key 存在、原生已照常播放語音）才補叫字幕顯示，最後把原
    // 函式的回傳值原樣還給真正呼叫者（cdecl，呼叫者自己清理 2 個參數，這裡
    // 完全不動它的堆疊）。
    static __declspec(naked) void Detour_OnelinerEntry()
    {
        __asm
        {
            push ebp
            mov ebp, esp
            sub esp, 4                     ; local: [ebp-4] = 原函式回傳值

            push dword ptr [ebp+0Ch]       ; String2
            push dword ptr [ebp+8]         ; a1
            call Trampoline_OnelinerEntry
            add esp, 8

            mov [ebp-4], eax

            push eax                       ; origResult
            push dword ptr [ebp+0Ch]       ; String2
            push dword ptr [ebp+8]         ; a1
            call OnOnelinerCall
            add esp, 12

            mov eax, [ebp-4]
            mov esp, ebp
            pop ebp
            ret
        }
    }

    static void InstallOnelinerHook()
    {
        BYTE* p = (BYTE*)kVA_OnelinerSite;
        for (int i = 0; i < 10; i++)
        {
            if (p[i] != kExpectedOneliner[i])
            {
                Log::Write("[SubtitleOneliners] 0x%08X第%d byte是0x%02X，預期0x%02X"
                           "——版本不符或已被其他patch動過，放棄安裝sub_6A4240(Oneliners)字幕補完hook", kVA_OnelinerSite, i, p[i], kExpectedOneliner[i]);
                return;
            }
        }

        DWORD oldProt = 0;
        VirtualProtect(p, 10, PAGE_EXECUTE_READWRITE, &oldProt);
        p[0] = 0xE9;
        *(INT32*)(p + 1) = (INT32)((DWORD)&Detour_OnelinerEntry - (DWORD)(p + 5));
        p[5] = 0x90;
        p[6] = 0x90;
        p[7] = 0x90;
        p[8] = 0x90;
        p[9] = 0x90;
        VirtualProtect(p, 10, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), p, 10);

        Log::Write("[SubtitleOneliners] sub_6A4240(Oneliners)字幕補完hook安裝完成：site=0x%08X detour=%p"
                   "（成功時直接用完整LOC key字串當category+locKey交給SubtitleGate狀態機，"
                   "不影響原本語音播放邏輯）",
                   kVA_OnelinerSite, &Detour_OnelinerEntry);
    }

    void Install()
    {
        InstallFeignDeathEggHook();
        InstallOnelinerHook();
    }
}
