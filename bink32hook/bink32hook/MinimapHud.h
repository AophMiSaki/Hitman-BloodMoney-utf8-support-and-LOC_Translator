#pragma once
#include <Windows.h>

// 小地圖（左下角，正方形）。自繪 route B（見 md\小地圖.md §九）：
// 換關時載 bink32hook\minimap\minimap_<關名>.bin（每關一個，離線由
// tools\minimap\ 抽取器產出；缺檔的關卡＝不畫牆線只留外框）。選樓層＝先讀玩家
// 所在 ZROOM 房名查 bin 尾段的房名→樓層表（§9-24），查不到才退回幾何 XZ 命中；
// 再投影進 [Hud] MinimapPos / MinimapSize 方框：先 DrawPrimitiveUP
// (TRIANGLELIST) 畫樓層填色（bin v3 的 fill；低 alpha 灰、重疊處自然加深＝房間
// 分界感），再疊 (LINELIST) 牆線 + 47 位置標記。bin v2（無 fill）時只畫牆線。
// [Hud] MinimapZoom>0＝以 47 為中心的固定比例局部視窗（scissor 裁在框內、跟著
// 捲動）；=0＝整層等比縮放看全貌。關卡名取自
// LocScenePath::CurrentMissionTag()（ZipPathTrace 側錄的場景 zip 路徑段）。
// 繪製共用 SubtitleRender 的唯一 EndScene compositor（註冊成 producer），不另 hook。
//
// 過場動畫（SubtitleGate::IsScriptedSubtitleBlocking，m_OSD+0x69）與 ESC 暫停
// 選單／原生地圖畫面（[[0x0082083C]+0xA50]+0x50 bit 0x04/0x08，見 md §9-21）
// 期間 Tick() 回 false → 整個小地圖含外框都不畫。
// [Debug] MinimapFloorProbe=1 時每秒側錄選樓層線索（見 MinimapHud.cpp
// FloorProbeTick，md §9-24）：room=讀到的房名、roomTbl->[樓層]、geom->、
// committed->、mgFi 對照，供驗證房名表覆蓋率與退路是否選對。

namespace MinimapHud
{
    // 讀 [Hud] MinimapEnabled / MinimapPos / MinimapSize，啟用時向
    // SubtitleRender 註冊 producer。
    void Install(HMODULE hModule);

    // 每幀由 GlyphHook.cpp GlyphCheck() 呼叫，驅動 SubtitleRender 唯一
    // EndScene compositor 的 lazy 安裝。停用時直接 return。
    void EnsureRenderReady();
}
