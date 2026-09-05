#pragma once
#include <Windows.h>

// 開場簡報native缺字幕。本模組完全不經native排版：偵測到場景切換到任務
// M**時，查"{missionId}/MissionBriefing/{missionId}_MB_{idx}D"這批key組出
// 字幕內容——F11 on查LocHook既有LOC譯文map，off直接呼叫native拿遊戲
// 自己已載入的原文。語音由native自行觸發，本模組被動觀察其開始tick，逐句
// 依真實語音開始tick算出可見時間窗。實際繪製註冊成 SubtitleRender 的
// producer，顯示在 ini [Subtitle] SubtitlePlaceFirst 設定的螢幕百分比位置。
//
// 字圖光柵化(GlyphAtlas)/材質上傳(TextureUpload)/EndScene compositor
// (SubtitleRender) 都是既有模組，這裡只是新增的consumer。

namespace SubtitleBrief
{
    // 讀ini、依開關向 SubtitleRender 註冊 producer。不做任何D3D9呼叫——
    // EndScene hook 由 SubtitleRender::EnsureHookInstalled() 的 lazy retry 安裝。
    void Install(HMODULE hModule);
}
