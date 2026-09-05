#include "SubtitleDialogue.h"
#include "SubtitleGate.h"
#include "Log.h"
#include <Windows.h>
#include <cstdio>
#include <cstdint>
#include <cmath>

namespace SubtitleDialogue
{
    // ---- sub_6AACA0：組 LOC key 播語音，但完全不呼叫 sub_661FD0 顯示字幕 ----
    //
    // 用 `sprintf("%s%.2d%s", a2, a3, a4)` 組 key、呼叫 `sub_4652A0` 查 LOC key
    // 存在，之後走音效管理員播放路線（`v4=sub_4E5BE0(a1)`→`v4->vtable+0x660`），
    // 全程沒有 `sub_661FD0` 呼叫。是 Dialogue 的觸發點。
    //
    // 修法：hook 函式入口，用 trampoline 呼叫完整原函式本體取得真正回傳值，
    // 成功（非 0）才組出 LOC key，交給 SubtitleGate::DispatchSubtitleLine 決定
    // 顯示位置，完全不動 `v4->vtable+0x660` 那個角色專屬語音播放邏輯。
    //
    // 0x6AACA0 起 6 bytes（`sub esp,304h` 剛好一條完整指令），5-byte JMP patch
    // 後剩 1 byte 補 NOP，續行點對齊到下一條指令(`push esi`)。
    static const DWORD kVA_ExtraLineSite = 0x006AACA0;
    static const DWORD kVA_ExtraLineContinue = 0x006AACA6;
    static const BYTE kExpectedExtraLine[6] = { 0x81, 0xEC, 0x04, 0x03, 0x00, 0x00 };

    static void __cdecl OnExtraLineCall(int a1, const char* a2, int a3, const char* a4, int origResult)
    {
        char locKey[256];
        _snprintf_s(locKey, _TRUNCATE, "%s%.2d%s", a2 ? a2 : "", a3, a4 ? a4 : "");

        if (!origResult) return;

        void* actorPtr = (void*)SubtitleGate::ResolveActorHandle(a1);
        float distSq = 0.0f;
        bool inRange = SubtitleGate::IsNpcWithinHearRange(actorPtr, &distSq);
        if (SubtitleGate::DiagEnabled() && actorPtr && SubtitleGate::NpcRadiusFilterEnabled())
        {
            Log::Write("[SubtitleDialogue] Distance key=\"%s\" dist=%.1f threshold=%.1f %s",
                       locKey, sqrtf(distSq), sqrtf(SubtitleGate::NpcHearRadiusSq()), inRange ? "顯示" : "擋下");
        }
        if (!inRange) return;

        SubtitleGate::DispatchSubtitleLine("Dialogue", a2 ? a2 : "", locKey);
    }

    // 執行被覆蓋的原指令(sub esp,304h)後跳回續行點，讓原函式本體完整跑完
    // （cdecl，retn 時彈回呼叫者，也就是下面 Detour_ExtraLineEntry 裡
    // `call Trampoline_ExtraLine` 的下一行）。
    static __declspec(naked) void Trampoline_ExtraLine()
    {
        __asm
        {
            sub esp, 304h
            jmp kVA_ExtraLineContinue
        }
    }

    // 進入時(cdecl)：[esp]=returnAddr，[esp+4]=a1，[esp+8]=a2(topic)，
    // [esp+0Ch]=a3(index)，[esp+10h]=a4(role)。先呼叫 Trampoline_ExtraLine
    // 執行完整原函式本體取得真正回傳值，之後視情況補叫字幕顯示，最後把原
    // 函式的回傳值原樣還給真正呼叫者（cdecl，呼叫者自己清理 4 個參數，這裡
    // 完全不動它的堆疊）。
    static __declspec(naked) void Detour_ExtraLineEntry()
    {
        __asm
        {
            push ebp
            mov ebp, esp
            sub esp, 4                     ; local: [ebp-4] = 原函式回傳值

            push dword ptr [ebp+14h]       ; a4
            push dword ptr [ebp+10h]       ; a3
            push dword ptr [ebp+0Ch]       ; a2
            push dword ptr [ebp+8]         ; a1
            call Trampoline_ExtraLine
            add esp, 16

            movzx eax, al
            mov [ebp-4], eax

            push eax                       ; origResult
            push dword ptr [ebp+14h]       ; a4
            push dword ptr [ebp+10h]       ; a3
            push dword ptr [ebp+0Ch]       ; a2
            push dword ptr [ebp+8]         ; a1
            call OnExtraLineCall
            add esp, 20

            mov eax, [ebp-4]
            mov esp, ebp
            pop ebp
            ret
        }
    }

    void Install()
    {
        BYTE* p = (BYTE*)kVA_ExtraLineSite;
        for (int i = 0; i < 6; i++)
        {
            if (p[i] != kExpectedExtraLine[i])
            {
                Log::Write("[SubtitleDialogue] 0x%08X第%d byte是0x%02X，預期0x%02X"
                           "——版本不符或已被其他patch動過，放棄安裝sub_6AACA0字幕補完hook", kVA_ExtraLineSite, i, p[i], kExpectedExtraLine[i]);
                return;
            }
        }

        DWORD oldProt = 0;
        VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &oldProt);
        p[0] = 0xE9;
        *(INT32*)(p + 1) = (INT32)((DWORD)&Detour_ExtraLineEntry - (DWORD)(p + 5));
        p[5] = 0x90;
        VirtualProtect(p, 6, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), p, 6);

        Log::Write("[SubtitleDialogue] sub_6AACA0字幕補完hook安裝完成：site=0x%08X detour=%p"
                   "（成功組出LOC key且原函式判定存在時，交給SubtitleGate狀態機決定顯示位置，"
                   "不影響原本語音播放邏輯）",
                   kVA_ExtraLineSite, &Detour_ExtraLineEntry);
    }
}
