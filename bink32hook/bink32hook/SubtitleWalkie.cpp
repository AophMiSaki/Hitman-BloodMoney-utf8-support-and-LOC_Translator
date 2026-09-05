#include "SubtitleWalkie.h"
#include "SubtitleGate.h"
#include "Log.h"
#include <Windows.h>
#include <cstdint>
#include <cstring>

namespace SubtitleWalkie
{
    // sub_6C0220：對講機傳輸播放函式（thiscall）
    //   int __thiscall sub_6C0220(radioCtrl* this, int a2, int a3, char* String2, int a5)
    // 入口：ecx=this，[esp+4]=a2，[esp+8]=a3（直接收訊者），[esp+0xC]=String2
    // （完整 LOC key path），[esp+0x10]=a5（說話者）。開頭三道唯讀 guard：
    // *(dword_820820+0xB0C)==0、sub_4E5BE0(a5)==0、sub_4E5BE0(a2)==0 任一成立
    // 時原函式 return 0 不播語音。
    // 0x6C0220 起 6 bytes 是 `mov eax, large fs:0`（64 A1 00 00 00 00，SEH
    // prologue 首指令），5-byte JMP patch＋1 NOP，續行 0x6C0226。detour 在 SEH
    // frame 建立前執行、只讀記憶體。
    static const DWORD kVA_WalkieSite     = 0x006C0220;
    static const DWORD kVA_WalkieContinue = 0x006C0226;
    static const BYTE  kExpectedWalkie[6] = { 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00 };

    static DWORD PlayerActor()
    {
        DWORD gameData = *(const DWORD*)0x0082083C;
        if (!gameData) return 0;
        return *(const DWORD*)(gameData + 0xA40);
    }

    // 每通對講機傳輸呼叫一次。不判斷距離——比照原版對講機音訊 participant/channel
    // 制照播，只要 sub_6C0220 會真的播就補字幕。只決定補哪一版文字：玩家＝直接
    // 收訊者 a3 → String2（原聲版）；否則 → String2+"R"（旁人聽到的無線電失真
    // 變體）。a3／玩家解析不到時退回 String2。（字幕功能.md §3.5）
    static void __cdecl OnWalkieCall(const char* locKey, int a2, int a3, int a5)
    {
        if (!locKey || !locKey[0]) return;

        // 重放 sub_6C0220 開頭三道唯讀 guard：任一不成立時原函式不播語音，不補字幕。
        DWORD gameData = *(const DWORD*)0x00820820;
        if (!gameData) return;
        if (!*(const DWORD*)(gameData + 0xB0C)) return;
        if (!SubtitleGate::ResolveActorHandle(a5)) return;
        if (!SubtitleGate::ResolveActorHandle(a2)) return;

        DWORD a3Actor = SubtitleGate::ResolveActorHandle(a3);
        DWORD player  = PlayerActor();
        bool  playerIsDirect = (a3Actor && player && a3Actor == player);

        char plusRKey[256];
        const char* key = locKey;
        if (!playerIsDirect)
        {
            size_t n = strlen(locKey);
            if (n && n + 2 <= sizeof(plusRKey))
            {
                memcpy(plusRKey, locKey, n);
                plusRKey[n]     = 'R';
                plusRKey[n + 1] = '\0';
                key = plusRKey;
            }
        }

        if (SubtitleGate::DiagEnabled())
            Log::Write("[SubtitleWalkie] key=%s a3=0x%08X a3Actor=0x%08X playerActor=0x%08X "
                       "playerIsDirect=%d => 補 %s 版",
                       locKey, (DWORD)a3, a3Actor, player, playerIsDirect ? 1 : 0,
                       playerIsDirect ? "原聲(不加R)" : "失真(+R)");

        // key 已是完整 LOC key path，同時當 category 與 locKey（對講機是一次性單句
        // 傳輸，不需「同輪對話延續同欄位」的分類語意，比照 Oneliners）。
        SubtitleGate::DispatchSubtitleLine("Walkie", key, key);
    }

    static __declspec(naked) void Detour_WalkieEntry()
    {
        __asm
        {
            pushfd
            pushad
            // pushfd(4)+pushad(20h)=24h；原 [esp+X] 現在在 [esp+24h+X]。
            // a2=[esp+4]->[esp+28h]，a3=[esp+8]->[esp+2Ch]，
            // String2=[esp+0Ch]->[esp+30h]，a5=[esp+10h]->[esp+34h]。
            mov  eax, [esp+34h]          ; a5
            mov  ebx, [esp+2Ch]          ; a3
            mov  ecx, [esp+28h]          ; a2
            mov  edx, [esp+30h]          ; String2
            push eax
            push ebx
            push ecx
            push edx
            call OnWalkieCall            ; __cdecl(String2, a2, a3, a5)
            add  esp, 16
            popad
            popfd

            mov  eax, fs:[0]            ; 被覆蓋的原指令（mov eax, large fs:0）
            jmp  kVA_WalkieContinue
        }
    }

    void Install()
    {
        BYTE* p = (BYTE*)kVA_WalkieSite;
        for (int i = 0; i < 6; i++)
        {
            if (p[i] != kExpectedWalkie[i])
            {
                Log::Write("[SubtitleWalkie] 0x%08X第%d byte是0x%02X，預期0x%02X"
                           "——版本不符或已被其他patch動過，放棄安裝sub_6C0220(對講機)字幕補完hook",
                           kVA_WalkieSite, i, p[i], kExpectedWalkie[i]);
                return;
            }
        }

        DWORD oldProt = 0;
        VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &oldProt);
        p[0] = 0xE9;
        *(INT32*)(p + 1) = (INT32)((DWORD)&Detour_WalkieEntry - (DWORD)(p + 5));
        p[5] = 0x90;
        VirtualProtect(p, 6, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), p, 6);

        Log::Write("[SubtitleWalkie] sub_6C0220(對講機傳輸)字幕補完hook安裝完成：site=0x%08X detour=%p"
                   "（不判斷距離；玩家＝直接收訊者補原聲版、否則補+R失真版，不影響原本語音播放邏輯）",
                   kVA_WalkieSite, &Detour_WalkieEntry);
    }
}
