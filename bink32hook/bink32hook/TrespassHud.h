#pragma once
#include <Windows.h>

// 元素 A：偽裝錯誤進入限制區時，在畫面上自繪一行警告字（Trespassing / 擅闖，
// 或 Hostile Area / 敵對區域）。判定訊號取自 native Hitman actor 的擅闖旗標
// bit（[actor+0x0B7C] bit 27），級別再依 GetCurrentZoneKind() 分兩段。繪製走
// SubtitleRender 的唯一 EndScene compositor（註冊成 producer），不另 hook。

namespace TrespassHud
{
    // 讀 [Hud] TrespassingEnabled / TrespassPos、載入 warningString.txt（缺檔
    // 則寫入預設），啟用時向 SubtitleRender 註冊 producer。
    void Install(HMODULE hModule);

    // 每幀由 GlyphHook.cpp GlyphCheck() 呼叫，驅動 SubtitleRender 唯一 EndScene
    // compositor 的 lazy 安裝。停用時直接 return。
    void EnsureRenderReady();
}
