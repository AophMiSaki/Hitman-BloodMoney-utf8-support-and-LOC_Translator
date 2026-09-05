#include "SubtitleTvRadio.h"
#include "SubtitleGate.h"
#include "LocHook.h"
#include "ZipPathTrace.h"
#include "Config.h"
#include "Log.h"
#include <Windows.h>
#include <map>
#include <string>
#include <cstring>
#include <cmath>

namespace SubtitleTvRadio
{
    // ---- sub_4C5E10：音效管理員 vtable+0x118(280)，由 resId spawn（帶 emitter）----
    //   int __thiscall sub_4C5E10(mgr, emitter, resId, a4, worldPos*)
    // 經 JMP（非 CALL）進入，esp 與 native 在 0x4C5E10 當下完全相同：
    //   [esp+0]=retAddr [esp+4]=emitter(arg1) [esp+8]=resId(arg2)
    //   [esp+0Ch]=a4    [esp+10h]=worldPos*(arg5)；ecx=mgr
    //   4C5E10  mov edx,[esp+10h]   ; 8B 54 24 10
    //   4C5E14  mov eax,[ecx]       ; 8B 01            ← 偷 6（E9 rel32 + 1 NOP）
    //   4C5E16  push edx            ← 續行點
    //
    // arg5(worldPos*)＝emitter 的 3-float 世界座標（x/y/z at +0/+4/+8）：
    // sub_4C5E10→sub_4C5D00 把它複製進 sound 物件 +0x38/+0x3C/+0x40，呼叫者
    // sub_4EAA10 也對它做 sub_4E68E0 幾何轉換後餵給 3D 定位 slot 180——就是
    // native 拿來做距離衰減的座標。距離過濾用它（見 OnTvRadioPlay）。
    static const DWORD kVA_Site     = 0x004C5E10;
    static const DWORD kVA_Continue = 0x004C5E16;
    static const BYTE  kExpected[6] = { 0x8B, 0x54, 0x24, 0x10, 0x8B, 0x01 };

    static bool  g_diag = false;
    static float g_hearRadius   = 0.0f;   // [Subtitle] TvRadioHearRadius，0＝停用過濾
    static float g_hearRadiusSq = 0.0f;

    // {resId(＝.SND file offset) → 攤平 LOC key}，場景切換時由
    // LocHook::CollectTvRadioSndOffsets 重建。廣播循環播放：同 resId 冷卻窗
    // 內只 dispatch 一次，g_lastDispatchMs 記每個 resId 上次 dispatch 的 tick。
    // sub_4C5E10 可能從音效執行緒呼叫，g_cs 保護這兩張表的整體性。
    static const DWORD kCooldownMs = 9000;

    static CRITICAL_SECTION g_cs;
    static bool             g_csInit = false;
    static char             g_builtForZip[MAX_PATH] = {};
    static std::map<DWORD, std::string> g_offsetToKey;
    static std::map<DWORD, ULONGLONG>   g_lastDispatchMs;

    // ---- diag 用：發聲 entity 的 class name（＝發聲房間 zone 名），
    // *(char**)( *(DWORD*)(emitter+4) + 0x6C )。
    // 全程唯讀、任一步失敗回 nullptr。----
    static DWORD SafeDeref(DWORD p)
    {
        if (p < 0x10000 || IsBadReadPtr((const void*)p, 4)) return 0;
        return *(const DWORD*)p;
    }
    static const char* SafeCStr(DWORD p)
    {
        if (p < 0x10000 || IsBadReadPtr((const void*)p, 1)) return nullptr;
        const char* s = (const char*)p;
        for (int n = 0; n < 96; n++)
        {
            if (IsBadReadPtr((const void*)(p + n), 1)) return nullptr;
            unsigned char c = (unsigned char)s[n];
            if (c == 0) return (n >= 2) ? s : nullptr;
            if (c < 0x20 || c > 0x7E) return nullptr;
        }
        return nullptr;
    }
    static const char* EmitterClass(DWORD emitter)
    {
        if (!emitter) return nullptr;
        DWORD p1 = SafeDeref(emitter + 4);
        if (!p1) return nullptr;
        return SafeCStr(SafeDeref(p1 + 0x6C));
    }

    // 場景 zip 路徑變了就重建 resId→LOC key 對照表（＋清冷卻表）。呼叫端已
    // 持有 g_cs。ZipPathTrace::GetLastSceneZipPath() 跟 SubtitleBrief 場景切換
    // 偵測同一個訊號來源。
    static void RebuildIfSceneChanged()
    {
        const char* zip = ZipPathTrace::GetLastSceneZipPath();
        if (!zip || !zip[0]) return;
        if (_stricmp(zip, g_builtForZip) == 0) return;

        strncpy_s(g_builtForZip, zip, _TRUNCATE);
        g_offsetToKey.clear();
        g_lastDispatchMs.clear();
        LocHook::CollectTvRadioSndOffsets(g_offsetToKey);
        Log::Write("[SubtitleTvRadio] 場景切換，重建 resId→LOC key 對照表：%s，%zu 筆",
                   zip, g_offsetToKey.size());
    }

    // __cdecl，供 naked entry detour 呼叫；只讀不改原函式狀態。
    // worldPosRaw＝sub_4C5E10 arg5（emitter 座標指標），可能來自音效執行緒、
    // 指標在呼叫端 stack frame 上，複製 3 float 出來前先 IsBadReadPtr 檢查。
    static void __cdecl OnTvRadioPlay(DWORD resId, DWORD emitter, DWORD worldPosRaw)
    {
        if (!g_csInit) return;
        EnterCriticalSection(&g_cs);

        RebuildIfSceneChanged();

        auto it = g_offsetToKey.find(resId);
        if (it == g_offsetToKey.end())
        {
            // radio_tuning 轉台雜訊、或其他非 TVAndRadio 的 resId spawn，不顯示。
            LeaveCriticalSection(&g_cs);
            return;
        }

        std::string locKey = it->second;
        ULONGLONG now = GetTickCount64();
        auto cd = g_lastDispatchMs.find(resId);
        bool onCooldown = (cd != g_lastDispatchMs.end()) && (now - cd->second < kCooldownMs);
        if (!onCooldown) g_lastDispatchMs[resId] = now;

        LeaveCriticalSection(&g_cs);

        if (onCooldown) return;

        // 距離過濾：廣播關卡載入即全區循環播報，用 emitter 世界座標跟玩家
        // 距離比對，超過 [Subtitle] TvRadioHearRadius 就不顯示。worldPos 讀不到
        // 或門檻=0 時照顯示（SubtitleGate::IsWorldPosWithinRange 內處理）。
        const float* worldPos = nullptr;
        float posCopy[3] = {};
        if (worldPosRaw && !IsBadReadPtr((const void*)worldPosRaw, sizeof(float) * 3))
        {
            posCopy[0] = ((const float*)worldPosRaw)[0];
            posCopy[1] = ((const float*)worldPosRaw)[1];
            posCopy[2] = ((const float*)worldPosRaw)[2];
            worldPos = posCopy;
        }
        float distSq = 0.0f;
        bool within = SubtitleGate::IsWorldPosWithinRange(worldPos, g_hearRadiusSq, &distSq);

        if (g_diag)
        {
            const char* cls = EmitterClass(emitter);
            Log::Write("[SubtitleTvRadio] Distance key=%s dist=%.1f threshold=%.1f emitterClass=%s %s",
                       locKey.c_str(), worldPos ? sqrtf(distSq) : -1.0f, g_hearRadius,
                       cls ? cls : "-", within ? "顯示" : "擋下");
        }
        if (!within) return;

        // 與 Dialogue/Oneliners/Walkie 共用 SubtitleGate 8 態狀態機／欄位分配／
        // watchdog。sourceTag="TvRadio" 會解析 <i> 斜體（M09 _R_ Radio 句原文
        // 帶 [<i>南方廣播</i>] 之類電台名前綴）。
        SubtitleGate::DispatchSubtitleLine("TvRadio", locKey.c_str(), locKey.c_str());
    }

    // pushfd/pushad 保護 → 取 resId/emitter/worldPos 呼叫 OnTvRadioPlay →
    // popad/popfd → 補回被偷的 2 條原指令 → jmp 續行點。原函式行為完全不變。
    static __declspec(naked) void Detour_TvRadioEntry()
    {
        __asm
        {
            pushfd
            pushad
            // pushfd(4)+pushad(20h)=24h：原 [esp+4]=emitter → [esp+28h]，
            // 原 [esp+8]=resId → [esp+2Ch]，原 [esp+10h]=worldPos* → [esp+34h]。
            mov  eax, [esp+2Ch]          ; resId
            mov  edx, [esp+28h]          ; emitter
            mov  ecx, [esp+34h]          ; worldPos* (arg5)
            push ecx
            push edx
            push eax
            call OnTvRadioPlay           ; __cdecl(resId, emitter, worldPos)
            add  esp, 0Ch
            popad
            popfd

            mov  edx, [esp+10h]          ; 被覆蓋的原指令（6 bytes）
            mov  eax, [ecx]
            jmp  kVA_Continue
        }
    }

    void Install()
    {
        g_diag = Config::LoadDebug(nullptr).subtitleDiagEnable;
        g_hearRadius   = Config::LoadTvRadioHearRadius(nullptr);
        g_hearRadiusSq = g_hearRadius * g_hearRadius;

        if (!g_csInit)
        {
            InitializeCriticalSection(&g_cs);
            g_csInit = true;
        }

        BYTE* p = (BYTE*)kVA_Site;
        for (int i = 0; i < 6; i++)
        {
            if (p[i] != kExpected[i])
            {
                Log::Write("[SubtitleTvRadio] 0x%08X第%d byte是0x%02X，預期0x%02X"
                           "——版本不符或已被其他 hook 佔用同一入口，"
                           "放棄安裝 sub_4C5E10(電視/收音機廣播) 字幕補完hook",
                           kVA_Site, i, p[i], kExpected[i]);
                return;
            }
        }

        DWORD oldProt = 0;
        VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &oldProt);
        p[0] = 0xE9;
        *(INT32*)(p + 1) = (INT32)((DWORD)&Detour_TvRadioEntry - (DWORD)(p + 5));
        p[5] = 0x90;
        VirtualProtect(p, 6, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), p, 6);

        Log::Write("[SubtitleTvRadio] sub_4C5E10(電視/收音機廣播)字幕補完hook安裝完成：site=0x%08X detour=%p"
                   "（resId＝.SND offset，場景切換時 LocHook 掃 LOC TVAndRadio entry 建對照表，命中即交"
                   " SubtitleGate 狀態機；同 resId 冷卻 %lums，emitter 座標距離門檻 TvRadioHearRadius=%.1f）",
                   kVA_Site, &Detour_TvRadioEntry, kCooldownMs, g_hearRadius);
    }
}
