#pragma once

// Oneliners 字幕觸發點 detour：
//   sub_6A4240 ＝ 真正的 Oneliners（單人事件反應喊話，呼叫端直接傳完整 key）
//   sub_6BACC0 ＝ M04 詐死彩蛋台詞（寫死 key，性質同屬 Oneliners 家族）
// native 本身完全不顯示這類字幕。機制與 SubtitleDialogue 相同：入口
// trampoline 跑完原函式取真正回傳值，成功才過 NPC/玩家距離過濾，通過後
// 交給 SubtitleGate::DispatchSubtitleLine。
namespace SubtitleOneliners
{
    // 對應 [Subtitle] OnelinersSubtitleEnabled，同時裝 sub_6A4240 跟
    // sub_6BACC0。呼叫端（SubtitleHook）已在旗標為 false 時跳過本函式。
    void Install();
}
