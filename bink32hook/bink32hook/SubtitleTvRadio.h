#pragma once

// 電視新聞／收音機廣播（TVAndRadio 分類）字幕補完 detour：音效管理員
// (*(dword_820820+0xB0C)) primary vtable 0x0076830C 的 slot 280（+0x118）
// ＝ sub_4C5E10（由 resId spawn、帶 emitter 的播放函式）。native 全程不呼叫
// sub_661FD0，所以電視新聞／收音機播報沒有字幕。
//
// 觸發路徑與資料格式在 字幕功能.md §3.4 實機定案：
//   - resId ＝ 該事件在 <關>_main.SND 的 file offset ＝ <關>_main.LOC 內該
//     TVAndRadio key 的「.SND offset」欄值。
//   - 載入場景時掃 LOC 所有 key 含 "TVAndRadio/" 的 entry，建
//     {sndOffset → 攤平 LOC key} 對照表（LocHook::CollectTvRadioSndOffsets）。
//   - detour 收到的 resId ∈ 表 → SubtitleGate::DispatchSubtitleLine
//     ("TvRadio", locKey, locKey)；查無（radio_tuning 轉台雜訊等）不顯示。
//     emitter class（＝發聲房間 zone 名，逐關不同）只留 diag，不當 gate。
//   - 廣播關卡載入即循環播、非玩家進房觸發 → 同 resId 於冷卻窗內只 dispatch
//     一次。距離過濾：用 sub_4C5E10 arg5 帶的 emitter 世界座標跟玩家比對，
//     超過 [Subtitle] TvRadioHearRadius 不顯示（0＝停用）。跟 NpcHearRadius
//     分開一個 key——廣播音量通常穿多個房間。見 字幕功能.md §3.4.3。
//   - M09 的 _R_ 巢狀 Radio 子群組跟 TVAnchor 走同一條路，只是不同 key。
//
// sub_4C5E10 入口 detour：偷 6 bytes（8B 54 24 10 8B 01，續行 0x004C5E16），
// 原函式照跑、行為不變。
namespace SubtitleTvRadio
{
    // 對應 [Subtitle] TvRadioSubtitleEnabled；呼叫端（SubtitleHook）已在旗標
    // 為 false 時跳過本函式。SubtitleGate 共用層由 SubtitleHook 另行確保安裝。
    void Install();
}
