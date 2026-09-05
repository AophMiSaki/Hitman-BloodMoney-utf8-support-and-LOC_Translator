#pragma once
#include <Windows.h>

// 元素 B：警衛／NPC 對玩家起疑時，在畫面上方置中自繪一行警示字
// （Suspicious / 懷疑，或 Alerted / 戰鬥）。訊號取自 native ZOSD 警覺條緩動
// 顯示值 [[[0x0082083C]+0xA4C]+0x18C]（0..100）：>=75 → ALERTED（紅）、
// 50~75 → SUSPICIOUS（黃）、<50 不顯示。繪製走 SubtitleRender 唯一 EndScene
// compositor（註冊成 producer），不另 hook；字串共用 warningString.txt
// （key SUSPICIOUS / ALERTED，見 WarningStrings.h）。詳見
// md\小地圖與非法入侵顯示.md §2-3。

namespace WarningHud
{
    // 讀 [Hud] WarningEnabled / WarningPos，啟用時載入字串表並向 SubtitleRender
    // 註冊 producer。
    void Install(HMODULE hModule);

    // 每幀由 GlyphHook.cpp GlyphCheck() 呼叫，驅動 SubtitleRender 唯一 EndScene
    // compositor 的 lazy 安裝。停用時直接 return。
    void EnsureRenderReady();
}
