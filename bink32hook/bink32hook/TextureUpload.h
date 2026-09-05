#pragma once
#include <Windows.h>
#include <d3d9.h>

// 把GlyphAtlas的A8點陣圖atlas上傳成D3D9材質(IDirect3DTexture9)，供
// SubtitleBrief.cpp self-render字幕取樣。固定上傳FontCategory::Subtitle分類
// 的atlas（Subtitle尚未Ready或初始化失敗時GlyphAtlas內部會fallback成General，
// 這裡上傳的內容/generation都會自動跟著fallback結果走、維持UV座標一致）。
// 只有這一個consumer，不做成通用多分類版本。
//
// D3DPOOL_MANAGED：裝置Reset/Lost時D3D9 runtime自己保留系統記憶體備份、自動
// 處理，不需要額外掛OnLostDevice/OnResetDevice回呼。已知限制：如果D3D9裝置
// 整個被銷毀重建（不是單純Reset），這裡快取的材質會變成屬於舊裝置的孤兒
// 指標——目前沒有偵測這種情況。

namespace TextureUpload
{
    // 冪等、可每次GetGlyph命中都呼叫：D3D9裝置未就緒時什麼都不做；材質尚未
    // 建立就建立；材質已建立但GlyphAtlas::GetGeneration()比對出有新glyph就
    // 重新上傳；都沒變化時只做一次裝置指標檢查+generation比對，成本很低。
    void EnsureReady();

    // 目前已建立好的材質，尚未建立回傳nullptr。
    IDirect3DTexture9* GetTexture();
}
