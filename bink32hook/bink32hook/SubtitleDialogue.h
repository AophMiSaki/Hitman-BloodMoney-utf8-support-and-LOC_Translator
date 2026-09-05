#pragma once

// Dialogue 字幕觸發點 detour：sub_6AACA0（雙人對話，topic/index/role 組 key）。
// native 本身完全不顯示這類字幕，整條顯示是我方補上的。
//
// 函式入口用 trampoline 跑完原函式本體取得真正回傳值，成功（LOC key 存在、
// 原生已照常播放語音）才做 NPC/玩家距離過濾，通過後把 key 交給
// SubtitleGate::DispatchSubtitleLine 決定顯示欄位。距離過濾 helper、8 態
// 狀態機、self-render 繪製、sub_661FD0 側錄 watchdog 全在 SubtitleGate。
namespace SubtitleDialogue
{
    // 對應 [Subtitle] DialogueSubtitleEnabled。呼叫端（SubtitleHook）已在
    // 旗標為 false 時跳過本函式。sub_661FD0 native 字幕 watchdog 側錄由
    // SubtitleHook 統一安裝（Dialogue/Oneliners 共用）。
    void Install();
}
