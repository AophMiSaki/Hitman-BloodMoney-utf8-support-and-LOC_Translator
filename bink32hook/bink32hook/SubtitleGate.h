#pragma once
#include <Windows.h>

// 首選/次選/第三三欄位字幕位置狀態機（8 態）＋ sub_661FD0 native 字幕入口
// 側錄（watchdog 用）＋ NPC/玩家距離過濾。
//
// native 沒有 Dialogue/Oneliners 字幕，這整條顯示都是我們自己加的。多個
// 「人」同時要顯示字幕時只有一個 native m_pSubtitles 物件，後呼叫的會蓋掉
// 前一個，所以改成三欄位設計：
//   - 首選：self-render D3D9 疊加，畫在 [Subtitle] SubtitlePlaceFirst（預設
//     50%,80%），完全不碰 native m_pSubtitles。
//   - 次選：沿用原本呼叫 native sub_661FD0 的做法，畫在 native 自己原本
//     顯示字幕的位置（無對應 ini key）。
//   - 第三：self-render 疊加，畫在 [Subtitle] SubtitlePlaceThird（預設
//     60%,90%）。
//
// 觸發點 detour（Dialogue＝sub_6AACA0 在 SubtitleDialogue、Oneliners＝
// sub_6A4240／sub_6BACC0 在 SubtitleOneliners），判斷完距離／原函式回傳值
// 後呼叫 DispatchSubtitleLine。繪製走 SubtitleRender 的 producer（首選／
// 第三 self-render）。
namespace SubtitleGate
{
    // 由 SubtitleHook::Install() 呼叫一次：讀 [Subtitle]/[Debug] 相關
    // key、確保 FontCategory::Subtitle atlas 就緒、把首選／第三字幕繪製
    // 註冊成 SubtitleRender 的 producer。
    void Init(HMODULE hModule);

    // sub_661FD0（native 顯示字幕函式）入口側錄安裝：累加呼叫世代供
    // watchdog 判斷次選字幕這段期間有沒有被別人蓋掉。Dialogue/Oneliners
    // 任一啟用即需安裝。
    void InstallNativeSubtitleWatchdog();

    // 8 態狀態機分派：state = (首選被佔?1) | (次選被佔?2) | (第三被佔?4)，
    // 同分類延續沿用原欄位，否則依序塞首選→次選→第三，三者皆被佔
    // （state 7）則本句捨棄。sourceTag 為 "Dialogue" 或 "Oneliners"，只用於
    // diag log。
    void DispatchSubtitleLine(const char* sourceTag, const char* category, const char* locKey);

    // 首選 self-render 欄位目前是否被 Dialogue/Oneliners 佔用中。SubtitleBrief
    // 讓位判斷用：首選被佔時開場簡報字幕整輪不顯示（不降級到次選/第三）。
    // Dialogue/Oneliners 皆停用（Init 未跑）時 g_primaryDeadlineMs 維持 0，
    // 恆回 false。
    bool IsPrimarySlotBusy();

    // 過場動畫／腳本序列字幕正在驅動中（m_OSD+0x69）＝這一刻不該疊自繪 HUD。
    // 內含遲滯窗（kScriptedHoldMs）與卡死自癒（+0x69 latch 在 1 逾 15s 且 native
    // 次選槽都沒東西 → 判 latch、回 false 解除封鎖）。Dialogue/Oneliners 皆停用
    // （SubtitleGate::Init/Tick 未跑）時退化成純讀 m_OSD+0x69 raw flag。
    // 過場時整個停顯示的自繪疊加：MinimapHud（含外框）、TrespassHud（擅闖）、
    // WarningHud（懷疑/戰鬥）、SubtitleBrief（簡報旁白）都在各自 Tick() 呼叫本函式。
    bool IsScriptedSubtitleBlocking();

    // 玩家操作鎖定中（過場/劇情鎖死輸入）＝actor+0x746==0。補
    // IsScriptedSubtitleBlocking() 的漏洞：純運鏡、沒有對白的過場動畫不會設
    // m_OSD+0x69，這個 byte 才抓得到。MinimapHud/TrespassHud/WarningHud 的
    // Tick() 用 `|| IsPlayerControlsLocked()` 並列在 IsScriptedSubtitleBlocking()
    // 後面（並列不是取代，字幕 gate 仍保留）。目前只有靜態反組譯驗證，尚未
    // 實機交叉確認，[Debug] DebugSubtitleDiagEnable 開啟時會印轉換供比對。
    bool IsPlayerControlsLocked();

    // ---- NPC/玩家距離過濾 helper（sub_6AACA0 / sub_6A4240 detour 共用）----
    //
    // ResolveActorHandle 包 native sub_4E5BE0（唯讀 handle table 解析，無
    // 副作用），IsNpcWithinHearRange 比對 NPC 與玩家世界座標平方距離、
    // 超過 [Subtitle] NpcHearRadius 回 false；解析失敗／功能停用一律回 true
    // （寧可多顯示也不要整批漏字幕）。
    DWORD ResolveActorHandle(int handle);
    bool  IsNpcWithinHearRange(void* actorPtr, float* outDistSq = nullptr);

    // 廣播 emitter 世界座標距離過濾（SubtitleTvRadio 用）。worldPos＝sub_4C5E10
    // 第 5 參數帶的 3-float 座標指標，radiusSq＝[Subtitle] TvRadioHearRadius 的
    // 平方；worldPos 為 nullptr／玩家取不到／radiusSq<=0 回 true（照顯示）。
    bool  IsWorldPosWithinRange(const float* worldPos, float radiusSq, float* outDistSq = nullptr);

    // detour 端印距離 diag log 用（受 [Debug] DebugSubtitleDiagEnable 控制）。
    bool  DiagEnabled();
    bool  NpcRadiusFilterEnabled();
    float NpcHearRadiusSq();
}
