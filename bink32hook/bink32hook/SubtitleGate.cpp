#include "SubtitleGate.h"
#include "Log.h"
#include "Config.h"
#include "FontCategory.h"
#include "GlyphAtlas.h"
#include "LocHook.h"
#include "SubtitleRender.h"
#include <string>
#include <vector>
#include <cstring>
#include <cstdint>

namespace SubtitleGate
{
    // [Debug] DebugSubtitleDiagEnable（涵蓋 Briefing/Dialogue/Oneliners 全部
    // 字幕功能的診斷開關，見 Config.h DebugConfig 註解）：控制距離過濾 log
    // 跟狀態機捨棄 log 等細節輸出。安裝成功/失敗 log 不受此旗標限制。
    static bool g_diagEnable = false;

    // ---- sub_661FD0 函式入口：native 顯示字幕函式，記錄呼叫次數供 watchdog 判斷 ----
    //
    // 0x661FD0 起 7 bytes（`push ebx` 1 byte ＋ `mov bl,byte_8ACA9C` 6 bytes
    // 共兩條完整指令），5-byte JMP patch 後剩 2 bytes 補 NOP。
    static const DWORD kVA_ShowSubtitleSite = 0x00661FD0;
    static const DWORD kVA_ShowSubtitleContinue = 0x00661FD7;
    static const BYTE kExpectedShowSubtitle[7] = { 0x53, 0x8A, 0x1D, 0x9C, 0xCA, 0x8A, 0x00 };

    // ★watchdog 用：每次任何人（原生對話、我們補顯、watchdog 清除）呼叫
    // sub_661FD0 都會累加，供 watchdog 判斷「這段期間畫面上顯示的還是不是
    // 我方排程要清除的那句」，見下方 CheckPendingClear()。
    static DWORD g_showSubtitleGeneration = 0;

    // ★過場 gate 豁免自己：ShowExtraSubtitle 呼叫 sub_661FD0
    // 補次選字幕後，記下當下的 g_showSubtitleGeneration。「次選槽現在顯示的是
    // 我方補顯內容」⟺ g_showSubtitleGeneration == g_selfIssuedSecondaryGeneration
    // （我方呼叫後 native／腳本序列都沒再呼叫過 sub_661FD0）。
    // 用途見 UpdateScriptedSubtitleBlock：+0x69 latch 自癒的「次選槽閒置」判斷
    // 只認 native／腳本(sub_530810)寫進次選槽的過場對白；我方補顯字幕不算，
    // 否則 latch 期間我方每補一句就重置 15s 閒置計時、自癒永不觸發、小地圖／
    // 擅闖／懷疑 HUD 被壓到關卡結束。初值 0xFFFFFFFF 避免開場誤配 generation 0。
    static DWORD g_selfIssuedSecondaryGeneration = 0xFFFFFFFF;

    static void __cdecl OnShowSubtitleEntryHit(DWORD thisPtr, DWORD returnAddr, const char* string2)
    {
        g_showSubtitleGeneration++;
    }

    // 進入時尚未執行任何原指令：[esp]=returnAddr，[esp+4]=String2(char*)，
    // ecx=this。detour 先原樣補回被覆蓋的 `push ebx; mov bl,byte_8ACA9C` 兩條
    // 指令（此時 esp 已因 push ebx 位移 +4，returnAddr/String2 要從 [esp+4]/
    // [esp+8] 讀），side-record 完再 tail-jmp 回續行點。
    static __declspec(naked) void Detour_ShowSubtitleEntry()
    {
        __asm
        {
            push ebx
            mov bl, byte ptr ds:[0x008ACA9C]

            mov eax, [esp+4]
            mov edx, [esp+8]

            pushfd
            pushad
            push edx
            push eax
            push ecx
            call OnShowSubtitleEntryHit
            add esp, 12
            popad
            popfd

            jmp kVA_ShowSubtitleContinue
        }
    }

    void InstallNativeSubtitleWatchdog()
    {
        BYTE* p = (BYTE*)kVA_ShowSubtitleSite;
        for (int i = 0; i < 7; i++)
        {
            if (p[i] != kExpectedShowSubtitle[i])
            {
                Log::Write("[SubtitleGate] 0x%08X第%d byte是0x%02X，預期0x%02X"
                           "——版本不符或已被其他patch動過，放棄安裝sub_661FD0側錄", kVA_ShowSubtitleSite, i, p[i], kExpectedShowSubtitle[i]);
                return;
            }
        }

        DWORD oldProt = 0;
        VirtualProtect(p, 7, PAGE_EXECUTE_READWRITE, &oldProt);
        p[0] = 0xE9;
        *(INT32*)(p + 1) = (INT32)((DWORD)&Detour_ShowSubtitleEntry - (DWORD)(p + 5));
        p[5] = 0x90;
        p[6] = 0x90;
        VirtualProtect(p, 7, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), p, 7);

        Log::Write("[SubtitleGate] sub_661FD0(顯示字幕)入口側錄安裝完成：site=0x%08X detour=%p"
                   "（供watchdog追蹤sub_661FD0呼叫次數）",
                   kVA_ShowSubtitleSite, &Detour_ShowSubtitleEntry);
    }

    // ---- 共用 native VA ----

    static float GetNowSec()
    {
        DWORD base = *(const DWORD*)0x00820820;
        if (!base) return 0.0f;
        return (float)(*(const int*)(base + 56)) * 0.0009765625f;
    }

    static DWORD GetOsdPtr()
    {
        DWORD gameData = *(const DWORD*)0x0082083C;
        if (!gameData) return 0;
        return *(const DWORD*)(gameData + 0xA4C);
    }

    // 次選是否目前正在顯示中（不分是 native 自己的對話還是我們補顯的）。
    // 不自己維護 flag，每次重讀 native m_pSubtitles 的 duration/startTime
    // ——這樣就算次選這段期間被真正的 native NPC 對話搶走畫面，也能從
    // native 自己的計時器讀到「現在還在顯示中」，不會誤判成空欄位而蓋掉
    // 別人。
    static bool IsSecondarySlotBusyNow(DWORD osd)
    {
        if (!osd) return false;
        float duration  = *(const float*)(osd + 0x60);
        float startTime = *(const float*)(osd + 0x64);
        return GetNowSec() < (startTime + duration);
    }

    // ---- 過場動畫／腳本序列字幕封鎖 ----
    //
    // sub_530810（腳本序列字幕驅動器，vtable 方法，與過場鏡頭播放器 sub_544030
    // 同段 0x53xxxx code 區）每 tick 寫 m_OSD+0x69 = (序列剩餘時間 > 0)。過場
    // 對白走這條 native 路徑顯示，跟我們 hook 的 sub_6AACA0/sub_6A4240 是兩條
    // 獨立管線；過場時 native 自己顯示過場對白，環境音補字幕不該再疊上去。
    //
    // 封鎖時：DispatchSubtitleLine 丟棄本句；Tick() 回 false（首選/第三不畫，
    // Draw 連帶跳過），次選若仍被我方補顯字幕佔用則 sub_661FD0(osd,"") 清掉讓位。
    //
    // 實機觀察：+0x69 多半整個過場維持 1、結束才歸 0（非逐句跳動），kScriptedHoldMs
    // 只接場景切換的一兩幀空窗。但也遇過 sub_530810 沒把剩餘時間歸零就停 tick、
    // +0x69 latch 在 1 直到關卡結束 → 卡死自癒：raw=1 但 native 次選槽
    // （IsSecondarySlotBusyNow）持續 kScriptedStuckMs 都沒東西顯示就判定 latch、
    // 停止封鎖，直到 raw 歸 0 或次選槽再度活躍才解除。DebugSubtitleDiagEnable
    // 開啟時 log +0x69 轉換、latch 判定、每句被丟棄的分類。
    static const DWORD kScriptedHoldMs  = 1000;
    // kScriptedStuckMs：raw=1 但次選槽閒置逾此值就判 latch、放行。15000＝
    // 正常過場（約 21 秒）多在尾段才被放行；再縮短會讓句間空檔就提早恢復顯示。
    static const DWORD kScriptedStuckMs = 15000;

    static ULONGLONG g_scriptedHoldUntilMs  = 0; // raw 為真時每幀刷新成 now+hold
    static ULONGLONG g_scriptedStuckSinceMs = 0; // raw=1 但次選槽閒置的起算點；0=未計時
    static bool      g_scriptedFlagStuck    = false;
    static bool      g_diagLastRaw          = false;
    static bool      g_diagLastStuck        = false;

    static bool ReadScriptedSubtitleFlag()
    {
        DWORD osd = GetOsdPtr();
        return osd && *(const BYTE*)(osd + 0x69) != 0;
    }

    // 每幀由 Tick() 開頭呼叫：維護遲滯窗與卡死自癒，回傳這幀是否封鎖。
    static bool UpdateScriptedSubtitleBlock()
    {
        ULONGLONG now = GetTickCount64();
        DWORD osd = GetOsdPtr();
        bool raw = osd && *(const BYTE*)(osd + 0x69) != 0;

        // 次選槽有東西顯示才算「過場對白正常在推進」→ 重置 latch 自癒計時。
        // 但我方補顯字幕（g_selfIssuedSecondaryGeneration 對得上＝我方呼叫
        // sub_661FD0 後 native／腳本都沒再呼叫）不算，否則 +0x69 latch 期間
        // 永遠自癒不了、HUD 被壓到關卡結束。
        bool secondaryNativeContent = osd && IsSecondarySlotBusyNow(osd) &&
            g_showSubtitleGeneration != g_selfIssuedSecondaryGeneration;
        if (!raw || secondaryNativeContent)
            g_scriptedStuckSinceMs = 0;
        else if (g_scriptedStuckSinceMs == 0)
            g_scriptedStuckSinceMs = now;
        g_scriptedFlagStuck = g_scriptedStuckSinceMs != 0 && now - g_scriptedStuckSinceMs > kScriptedStuckMs;

        if (raw && !g_scriptedFlagStuck)
            g_scriptedHoldUntilMs = now + kScriptedHoldMs;

        if (g_diagEnable && raw != g_diagLastRaw)
        {
            Log::Write("[SubtitleGate] m_OSD+0x69 %s：過場/腳本字幕%s",
                       raw ? "0→1" : "1→0", raw ? "開始，封鎖補字幕" : "結束");
            g_diagLastRaw = raw;
        }
        if (g_diagEnable && g_scriptedFlagStuck != g_diagLastStuck)
        {
            Log::Write("[SubtitleGate] +0x69 卡死自癒：%s（次選槽閒置逾 %lums）",
                       g_scriptedFlagStuck ? "判定 latch、暫停封鎖" : "解除", kScriptedStuckMs);
            g_diagLastStuck = g_scriptedFlagStuck;
        }

        return now < g_scriptedHoldUntilMs;
    }

    // DispatchSubtitleLine（detour 呼叫，可能落在兩幀之間）用：以 Tick() 維護的
    // 遲滯窗為主，另補讀一次 raw 接住「這幀剛開始、Tick 還沒跑」的空窗；latch
    // 期間靠 g_scriptedFlagStuck 一起放行。MinimapHud 也用（見 SubtitleGate.h）。
    bool IsScriptedSubtitleBlocking()
    {
        if (g_scriptedFlagStuck) return false;
        return GetTickCount64() < g_scriptedHoldUntilMs || ReadScriptedSubtitleFlag();
    }

    // ---- 首選／第三 self-render 欄位狀態 ----
    //
    // deadline=0 代表尚未使用過，GetTickCount64() 必定 >0，所以不用另外的
    // occupied bool，直接比較「現在 < deadline」即可。

    static float g_placeFirstX = 0.5f; // 0~1，ini 百分比 /100 後存這裡
    static float g_placeFirstY = 0.8f;
    static float g_placeThirdX = 0.6f;
    static float g_placeThirdY = 0.9f;

    static char        g_primaryCategory[64] = {};
    static ULONGLONG   g_primaryDeadlineMs = 0;
    static std::string g_primaryText;
    // 只有 sourceTag=="Oneliners" 的句子才讓 SubtitleRender
    // 解析 <i>/</i> 顯示斜體；其餘來源沿用舊行為（標籤原樣輸出）。
    static bool        g_primaryIsOneliner = false;

    static char        g_thirdCategory[64] = {};
    static ULONGLONG   g_thirdDeadlineMs = 0;
    static std::string g_thirdText;
    static bool        g_thirdIsOneliner = false;

    // 次選欄位：只記 category 供「同分類延續同一欄位」判斷，真正的佔用狀態
    // 一律用 IsSecondarySlotBusyNow() 動態讀 native 計時器（見上方說明）。
    static char g_secondaryCategory[64] = {};

    // 補顯字幕（Dialogue/Oneliners）多為單句短話，用「字數(UTF-8 byte 數)
    // *40ms + 1800ms」當保底時長估算——沒有語音時長可對齊，純粹保證讀完
    // 的時間，數值是經驗值。
    static DWORD EstimateDurationMs(const std::string& text)
    {
        return (DWORD)(text.size() * 40 + 1800);
    }

    // thiscall sub_661FD0(ZOSD* this, char* String2, float duration)。
    static const DWORD kVA_ShowSubtitleFn = 0x00661FD0;

    static void ShowExtraSubtitle(DWORD osd, const char* locKey)
    {
        __asm
        {
            push 0                    ; duration = 0.0f
            mov eax, locKey
            push eax                  ; String2
            mov ecx, osd              ; this = m_OSD
            call kVA_ShowSubtitleFn
        }

        // sub_661FD0 入口側錄 detour 剛在上面的 call 內把 g_showSubtitleGeneration
        // +1；記下來，讓過場 latch 自癒能分辨「次選槽是我方補顯內容，還是
        // native 過場對白」。見 g_selfIssuedSecondaryGeneration 宣告處。
        g_selfIssuedSecondaryGeneration = g_showSubtitleGeneration;
    }

    // ★watchdog 用（見下方 CheckPendingClear()）：排程「該在什麼時候清除
    // 我們補顯示的次選字幕」。
    static bool  g_pendingClearArmed = false;
    static DWORD g_pendingClearOsd = 0;
    static float g_pendingClearDeadlineSec = 0.0f;
    static DWORD g_pendingClearGeneration = 0;

    static void ShowPrimarySubtitle(const char* category, const std::string& text, bool isOneliner)
    {
        strncpy_s(g_primaryCategory, category, _TRUNCATE);
        g_primaryText = text;
        g_primaryIsOneliner = isOneliner;
        g_primaryDeadlineMs = GetTickCount64() + EstimateDurationMs(text);
    }

    static void ShowThirdSubtitle(const char* category, const std::string& text, bool isOneliner)
    {
        strncpy_s(g_thirdCategory, category, _TRUNCATE);
        g_thirdText = text;
        g_thirdIsOneliner = isOneliner;
        g_thirdDeadlineMs = GetTickCount64() + EstimateDurationMs(text);
    }

    static void ShowSecondarySubtitle(const char* category, DWORD osd, const char* locKey)
    {
        strncpy_s(g_secondaryCategory, category, _TRUNCATE);
        ShowExtraSubtitle(osd, locKey);

        // sub_661FD0 剛才已經把 duration/startTime 寫進 m_OSD+0x60/+0x64
        // （native 自己算出來的值，直接讀回來用，不用自己重算一次）。
        float duration = *(const float*)(osd + 0x60);
        float startTime = *(const float*)(osd + 0x64);
        g_pendingClearOsd = osd;
        g_pendingClearDeadlineSec = startTime + duration;
        g_pendingClearGeneration = g_showSubtitleGeneration;
        g_pendingClearArmed = true;
    }

    // ---- 8 態狀態機分派 ----
    //
    // state = (首選被佔?1) | (次選被佔?2) | (第三被佔?4)。
    //   首選空                    -> 進首選（self-render）
    //   首選佔、次選空            -> 進次選（native sub_661FD0 原位）
    //   首選佔、次選佔、第三空    -> 進第三（self-render）
    //   三者皆被佔（state 7）     -> 本句捨棄，只記 diag log
    //
    // 首選/第三佔用判斷：自己是唯一 writer，用自己的 GetTickCount64() ms
    // deadline。次選佔用判斷：動態讀 native 計時器（IsSecondarySlotBusyNow）。
    //
    // 同分類延續：同一輪對話還沒結束（deadline 未到、category 相同）時沿用
    // 原本已佔用的欄位、刷新顯示時間，不重新分配。
    //
    // F11 翻譯開關（LocHook::IsTranslationActive()）只決定 self-render
    // 欄位要查譯文(TryLookup)還是原文(TryLookupOriginal)，狀態機邏輯本身
    // 兩種情況完全共用。
    void DispatchSubtitleLine(const char* sourceTag, const char* category, const char* locKey)
    {
        // 過場動畫／腳本序列正在驅動 native 字幕（m_OSD+0x69）時，過場對白由
        // native 自己顯示，環境音 Dialogue/Oneliners 補字幕整句丟棄。
        if (IsScriptedSubtitleBlocking())
        {
            if (g_diagEnable)
                Log::Write("[SubtitleGate][%s] 分類=\"%s\"：過場/腳本字幕進行中(m_OSD+0x69)，本句丟棄",
                           sourceTag, category);
            return;
        }

        // Oneliners、TvRadio 廣播才啟用 <i>/</i> 斜體解析——M09 _R_ Radio 句原文帶
        // [<i>南方廣播</i>] 之類的電台名前綴，不解析會把標籤原樣畫出來。
        // 變數名沿用 isOneliner，語意實為「這個來源要解析 <i> 斜體標籤」。
        const bool isOneliner = sourceTag &&
            (_stricmp(sourceTag, "Oneliners") == 0 || _stricmp(sourceTag, "TvRadio") == 0);

        bool translationActive = LocHook::IsTranslationActive();
        auto lookupDisplayText = [&](std::string& outText) -> bool
        {
            return translationActive
                       ? LocHook::TryLookup(locKey, outText)
                       : LocHook::TryLookupOriginal(locKey, outText);
        };

        ULONGLONG nowMs = GetTickCount64();
        DWORD osd = GetOsdPtr();
        bool primaryBusy   = nowMs < g_primaryDeadlineMs;
        bool secondaryBusy = IsSecondarySlotBusyNow(osd);
        bool thirdBusy     = nowMs < g_thirdDeadlineMs;

        // 1. 同分類延續：沿用原本已佔用的欄位（刷新顯示時間），不重新分配。
        if (primaryBusy && _stricmp(category, g_primaryCategory) == 0)
        {
            std::string text;
            if (lookupDisplayText(text) && !text.empty())
            {
                ShowPrimarySubtitle(category, text, isOneliner);
                return;
            }
            // 查無文字，往下走一般分配流程。
        }
        if (secondaryBusy && _stricmp(category, g_secondaryCategory) == 0)
        {
            if (osd) { ShowSecondarySubtitle(category, osd, locKey); return; }
        }
        if (thirdBusy && _stricmp(category, g_thirdCategory) == 0)
        {
            std::string text;
            if (lookupDisplayText(text) && !text.empty())
            {
                ShowThirdSubtitle(category, text, isOneliner);
                return;
            }
        }

        // 2. 新一輪對話：依 8 態狀態機分配欄位（首選→次選→第三）。
        if (!primaryBusy)
        {
            std::string text;
            if (lookupDisplayText(text) && !text.empty())
            {
                ShowPrimarySubtitle(category, text, isOneliner);
                return;
            }
        }

        if (!secondaryBusy && osd)
        {
            ShowSecondarySubtitle(category, osd, locKey);
            return;
        }

        if (!thirdBusy)
        {
            std::string text;
            if (lookupDisplayText(text) && !text.empty())
            {
                ShowThirdSubtitle(category, text, isOneliner);
                return;
            }
        }

        // 狀態 7（首選/次選/第三皆被佔用）：本句字幕捨棄不顯示。
        if (g_diagEnable)
            Log::Write("[SubtitleGate][%s] 分類=\"%s\"：狀態7(首選/次選/第三皆被佔)，本句字幕捨棄不顯示",
                       sourceTag, category);
    }

    bool IsPrimarySlotBusy()
    {
        return GetTickCount64() < g_primaryDeadlineMs;
    }

    // ---- NPC/玩家距離過濾（Dialogue 跟 Oneliners 共用）----
    //
    // Dialogue（sub_6AACA0）跟 Oneliners（sub_6A4240）都是 NPC 依關卡腳本全程
    // 執行，不管玩家在不在附近都會觸發（native 語音靠 3D 音效引擎距離衰減讓
    // 玩家聽不到），但字幕 hook 直接接在觸發函式入口沒有這層判斷，會把全地圖
    // NPC 的字幕都顯示出來。
    //
    // sub_6AACA0/sub_6A4240 內部 `sub_4E5BE0(a1)` 解析出 ZHM3Actor*，這裡在
    // C++ 端重新呼叫一次 sub_4E5BE0（純讀取的 handle table 解析，無副作用）
    // 取得同一個 ZHM3Actor*，用來查它的世界座標。玩家角色指標：
    // `*(dword_82083C)+0xA40`。世界座標：ZGEOM::GetRootPoint(this, ZVector3*
    // pos)（thiscall，callee 清棧 retn 4；ZHM3Actor 繼承鏈 root 即 ZGEOM，
    // this 不需額外偏移）。
    //
    // 距離門檻：ini `[Subtitle] NpcHearRadius`，單位（公分/公尺）未確認、需
    // 實機測試調整；0＝停用過濾。用平方距離比較，避免每次算 sqrt。
    typedef DWORD(__cdecl* ResolveActorFn)(int handle);
    static const ResolveActorFn NativeResolveActor = (ResolveActorFn)0x004E5BE0;

    typedef void(__thiscall* GetRootPointFn)(void* thisPtr, float* outPos);
    static const GetRootPointFn NativeGetRootPoint = (GetRootPointFn)0x004E68E0;

    static float g_npcHearRadiusSq = 1000.0f * 1000.0f;
    static bool  g_npcHearRadiusEnabled = true;

    static void* GetPlayerActorPtr()
    {
        DWORD gameData = *(const DWORD*)0x0082083C;
        if (!gameData) return nullptr;
        return (void*)(*(const DWORD*)(gameData + 0xA40));
    }

    DWORD ResolveActorHandle(int handle)
    {
        return NativeResolveActor(handle);
    }

    // ---- 玩家操作鎖定（過場/劇情鎖死輸入）----
    //
    // msg_DisableControls/msg_EnableControls 驅動的 ZPlayer::DisableControls
    // (0x527B10)/EnableControls(0x527BB0)（參照計數在 actor+0x5A0）維護
    // actor+0x746：0=鎖定中（過場/劇情中，輸入不生效）、1=可操作，建構子
    // (sub_527560) 預設值就是 1。補 IsScriptedSubtitleBlocking() 抓不到的
    // 純運鏡過場（沒有對白，m_OSD+0x69 全程不會被設）。
    static const DWORD kActorControlsLockedOffset = 0x746;
    static bool g_diagLastControlsLocked = false;

    bool IsPlayerControlsLocked()
    {
        void* player = GetPlayerActorPtr();
        if (!player) return false;
        bool locked = *(const BYTE*)((const BYTE*)player + kActorControlsLockedOffset) == 0;

        if (g_diagEnable && locked != g_diagLastControlsLocked)
        {
            Log::Write("[SubtitleGate] actor+0x746 %s：玩家操作%s",
                       locked ? "1->0" : "0->1", locked ? "鎖定（過場/劇情中）" : "解鎖");
            g_diagLastControlsLocked = locked;
        }

        return locked;
    }

    bool DiagEnabled()          { return g_diagEnable; }
    bool NpcRadiusFilterEnabled() { return g_npcHearRadiusEnabled; }
    float NpcHearRadiusSq()      { return g_npcHearRadiusSq; }

    // actorPtr 為 nullptr（解析失敗）或功能停用時預設回傳 true（照樣顯示），
    // 查不到位置寧可多顯示也不要整批漏字幕。outDistSq 非 nullptr 時回填算出
    // 的平方距離（查不到位置/功能停用時不寫入，呼叫端要檢查回傳值決定
    // outDistSq 有沒有意義），供 diag log 用。
    bool IsNpcWithinHearRange(void* actorPtr, float* outDistSq)
    {
        if (!g_npcHearRadiusEnabled) return true;
        if (!actorPtr) return true;

        void* player = GetPlayerActorPtr();
        if (!player) return true;

        float npcPos[3] = {};
        float playerPos[3] = {};
        NativeGetRootPoint(actorPtr, npcPos);
        NativeGetRootPoint(player, playerPos);

        float dx = npcPos[0] - playerPos[0];
        float dy = npcPos[1] - playerPos[1];
        float dz = npcPos[2] - playerPos[2];
        float distSq = dx * dx + dy * dy + dz * dz;
        if (outDistSq) *outDistSq = distSq;
        return distSq <= g_npcHearRadiusSq;
    }

    // 廣播（TV/Radio）emitter 世界座標距離過濾：跟
    // IsNpcWithinHearRange 同一套平方距離比較，但來源是 sub_4C5E10 第 5 參數
    // 帶的 emitter 座標（不是 actor，沒有 GetRootPoint 可呼叫），門檻用呼叫端
    // 自己傳進來的 radiusSq（[Subtitle] TvRadioHearRadius，跟 NpcHearRadius
    // 分開）。worldPos 為 nullptr／玩家指標取不到／radiusSq<=0 一律回 true
    // （照顯示，寧可多顯示也不要整批漏字幕）。
    bool IsWorldPosWithinRange(const float* worldPos, float radiusSq, float* outDistSq)
    {
        if (radiusSq <= 0.0f) return true;
        if (!worldPos) return true;

        void* player = GetPlayerActorPtr();
        if (!player) return true;

        float playerPos[3] = {};
        NativeGetRootPoint(player, playerPos);

        float dx = worldPos[0] - playerPos[0];
        float dy = worldPos[1] - playerPos[1];
        float dz = worldPos[2] - playerPos[2];
        float distSq = dx * dx + dy * dy + dz * dz;
        if (outDistSq) *outDistSq = distSq;
        return distSq <= radiusSq;
    }

    // ---- watchdog：清除次選字幕 ----
    //
    // sub_661FD0 本身沒有自動計時清除機制：顯示成功時只把 duration/startTime
    // 寫進 m_OSD+0x60/+0x64 供渲染端算淡出用，「該收尾了」的判斷跟主動再
    // 呼叫一次 sub_661FD0(this,"",0) 清除狀態，永遠是呼叫端自己的責任（原生
    // NPC 對話靠 ZHM3DialogControl 逐幀 tick 做這件事）。我們的補顯只觸發
    // 一次、沒有後續 player，次選字幕顯示完不會自動消失，需要 watchdog 主動
    // 清。首選／第三是我們自己畫的，過了 deadline 直接不畫即可。
    static void CheckPendingClear()
    {
        if (!g_pendingClearArmed) return;

        if (g_showSubtitleGeneration != g_pendingClearGeneration)
        {
            // 期間有其他人（原生對話或我們自己下一句）呼叫過 sub_661FD0，
            // 畫面上顯示的已經不是我們排程要清除的那句，不用管它。
            g_pendingClearArmed = false;
            return;
        }

        if (GetNowSec() < g_pendingClearDeadlineSec) return;

        ShowExtraSubtitle(g_pendingClearOsd, "");
        g_pendingClearArmed = false;
    }

    // ---- SubtitleRender producer：watchdog + 首選／第三字幕繪製 ----

    // 每幀呼叫：跑次選字幕 watchdog 清除，回傳首選或第三字幕是否還在期限內
    // （＝這幀是否要繪製）。
    static bool Tick()
    {
        bool scriptedBlocked = UpdateScriptedSubtitleBlock();

        CheckPendingClear();

        if (scriptedBlocked)
        {
            // 次選若仍被我方補顯字幕佔用就清掉讓位給過場對白（CheckPendingClear
            // 多半已因 sub_530810 每幀呼叫 sub_661FD0 使世代對不上而 disarm）。
            if (g_pendingClearArmed && g_pendingClearOsd)
            {
                ShowExtraSubtitle(g_pendingClearOsd, "");
                g_pendingClearArmed = false;
            }
            return false;
        }

        ULONGLONG now = GetTickCount64();
        bool primaryVisible = now < g_primaryDeadlineMs && !g_primaryText.empty();
        bool thirdVisible   = now < g_thirdDeadlineMs   && !g_thirdText.empty();
        return primaryVisible || thirdVisible;
    }

    // 已斷好行的一組視覺行繪製：整段以 centerYPx 為文字塊垂直中心展開，
    // 避免 SubtitlePlace 的 Y 只對第一行有意義。
    static void DrawRows(const SubtitleRender::Frame& frame, const std::vector<std::string>& rows,
                         float placeX, float centerYPx, float lineHeight, bool parseTags)
    {
        float centerX = (float)frame.width * placeX;
        float blockTop = centerYPx - lineHeight * ((float)rows.size() - 1) * 0.5f;
        for (size_t r = 0; r < rows.size(); r++)
            SubtitleRender::DrawLine(frame.device, rows[r], centerX,
                                     blockTop + lineHeight * (float)r, frame.glyphTex, 0xFFFFFFFFu, parseTags);
    }

    // 只在 Tick() 回 true 的幀、且 render-state 已由 SubtitleRender 設好時
    // 呼叫。首選（預設 50%,80%）與第三（預設 60%,90%）可能同時存在，多行時
    // 兩個文字塊會疊到——首選在上、第三在下時把第三塊下推到與首選塊之間
    // 至少留一行間距；下推會超出畫面下緣（98%）時改為把首選塊上提補足
    // 差額。次選走 native 自身位置、不在這裡管。
    static void Draw(const SubtitleRender::Frame& frame)
    {
        ULONGLONG now = GetTickCount64();
        bool primaryVisible = now < g_primaryDeadlineMs && !g_primaryText.empty();
        bool thirdVisible   = now < g_thirdDeadlineMs   && !g_thirdText.empty();
        if (!primaryVisible && !thirdVisible) return;

        float lineHeight = SubtitleRender::LineHeight();
        float maxLineWidthPx = (float)frame.width * 0.8f;

        std::vector<std::string> primaryRows, thirdRows;
        if (primaryVisible) primaryRows = SubtitleRender::WrapSubtitleLine(g_primaryText, maxLineWidthPx, g_primaryIsOneliner);
        if (thirdVisible)   thirdRows   = SubtitleRender::WrapSubtitleLine(g_thirdText, maxLineWidthPx, g_thirdIsOneliner);

        float primaryCenterY = (float)frame.height * g_placeFirstY;
        float thirdCenterY   = (float)frame.height * g_placeThirdY;

        if (primaryVisible && thirdVisible && g_placeThirdY >= g_placeFirstY)
        {
            float primaryBottom = primaryCenterY + lineHeight * ((float)primaryRows.size() - 1) * 0.5f;
            float thirdHalf     = lineHeight * ((float)thirdRows.size() - 1) * 0.5f;
            float thirdTop      = thirdCenterY - thirdHalf;
            float minThirdTop   = primaryBottom + lineHeight;
            if (thirdTop < minThirdTop)
            {
                float shift = minThirdTop - thirdTop;
                float screenLimit = (float)frame.height * 0.98f;
                float overflow = (thirdCenterY + shift + thirdHalf) - screenLimit;
                if (overflow > 0.0f) { shift -= overflow; primaryCenterY -= overflow; }
                thirdCenterY += shift;
            }
        }

        if (primaryVisible) DrawRows(frame, primaryRows, g_placeFirstX, primaryCenterY, lineHeight, g_primaryIsOneliner);
        if (thirdVisible)   DrawRows(frame, thirdRows,   g_placeThirdX, thirdCenterY,   lineHeight, g_thirdIsOneliner);
    }

    void Init(HMODULE hModule)
    {
        Config::SubtitlePlaceConfig first = Config::LoadSubtitlePlaceFirst(hModule);
        g_placeFirstX = first.xPercent / 100.0f;
        g_placeFirstY = first.yPercent / 100.0f;

        Config::SubtitlePlaceConfig third = Config::LoadSubtitlePlaceThird(hModule);
        g_placeThirdX = third.xPercent / 100.0f;
        g_placeThirdY = third.yPercent / 100.0f;

        float npcHearRadius = Config::LoadNpcHearRadius(hModule);
        g_npcHearRadiusEnabled = npcHearRadius > 0.0f;
        g_npcHearRadiusSq = npcHearRadius * npcHearRadius;

        g_diagEnable = Config::LoadDebug(hModule).subtitleDiagEnable;

        // 首選/第三要畫字就需要 FontCategory::Subtitle 的 GDI 字型/atlas 就緒
        // ——冪等，跟 SubtitleBrief 同一觸發點重複呼叫沒有副作用（見
        // GlyphAtlas.h EnsureCategoryReady() 註解），這裡另外呼叫一次是為了
        // 不依賴「使用者一定會先看過簡報畫面」這個假設。
        GlyphAtlas::EnsureCategoryReady(hModule, FontCategory::Subtitle);

        // 首選/第三字幕繪製註冊成 SubtitleRender 的 producer；EndScene hook 由
        // SubtitleRender::EnsureHookInstalled() 的 lazy retry 安裝。
        SubtitleRender::Register({ &Tick, &Draw });

        Log::Write("[SubtitleGate] Init：SubtitlePlaceFirst=%.1f%%,%.1f%% SubtitlePlaceThird=%.1f%%,%.1f%%"
                   "（螢幕百分比，0,0=左上角）NpcHearRadius=%.1f",
                   first.xPercent, first.yPercent, third.xPercent, third.yPercent, npcHearRadius);
    }
}
