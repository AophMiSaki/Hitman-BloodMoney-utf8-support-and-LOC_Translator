#pragma once

// 對講機（Walkie Talkie）字幕補完：hook sub_6C0220（由 sub_6C0C80 在訊息碼
// 0x8D4 呼叫）＝關卡內 NPC 對講機傳輸播放函式。native 全程不呼叫 sub_661FD0，
// 所以對講機通話沒有字幕。
//
// 不判斷距離（比照原版音訊 participant/channel 制照播，字幕功能.md §3.5）：
// 只要 sub_6C0220 會真的播（重放開頭三道唯讀 guard 確認）就補字幕。玩家＝直接
// 收訊者 a3 → 補 String2（原聲版）；否則 → 補 String2+"R"（無線電失真版）。
// sourceTag 一律 "Walkie"（只影響 diag 字串，狀態機／欄位分配共用 SubtitleGate）。
namespace SubtitleWalkie
{
    // 對應 [Subtitle] WalkieSubtitleEnabled；呼叫端（SubtitleHook）已在旗標為
    // false 時跳過。SubtitleGate 共用層由 SubtitleHook 另行確保安裝。
    void Install();
}
