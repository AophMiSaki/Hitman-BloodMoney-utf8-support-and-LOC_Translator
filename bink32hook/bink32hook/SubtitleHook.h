#pragma once
#include <Windows.h>

// 字幕功能總調度層：讀 [Subtitle]/[Debug] 開關，依開關安裝各字幕子模組。
// 無任何 per-frame 邏輯／asm／D3D／狀態機——那些分別在 SubtitleRender
// （唯一 EndScene compositor）、SubtitleGate（8 態狀態機＋距離過濾＋
// sub_661FD0 側錄）、SubtitleDialogue／SubtitleOneliners（觸發點 detour）、
// SubtitleBrief／SubtitleBriefSpeech（開場簡報字幕）。
namespace SubtitleHook
{
    // Hook::Init() 呼叫一次，無條件（三個開關全關時內部自行跳過所有安裝）：
    //   BriefingSubtitleEnabled   -> SubtitleBriefSpeech + SubtitleBrief
    //   Dialogue/OnelinersEnabled -> SubtitleGate（Init＋native 字幕 watchdog
    //                                側錄）＋對應觸發點 detour
    void Install(HMODULE hModule);

    // GlyphHook.cpp GlyphCheck() 每幀呼叫：任一字幕開關啟用時驅動
    // SubtitleRender::EnsureHookInstalled() 的 lazy retry（D3D9 裝置就緒前
    // 為 no-op）。開關全關時直接 return。
    void EnsureRenderReady();
}
