#pragma once
#include <Windows.h>
#include <string>
#include <vector>

struct IDirect3DDevice9;
struct IDirect3DTexture9;

// 字幕統一 compositor：全專案唯一的 IDirect3DDevice9::EndScene hook。各字幕
// 來源（SubtitleBrief 的開場簡報字幕、SubtitleGate 的首選/第三對話
// 字幕）註冊成 producer，本模組每幀先讓每個 producer 輪詢一次，任一個回報
// 「這幀要顯示」時才做一次 render-state 存/設/還原，再依註冊順序呼叫其
// 繪製（＝疊放層序）。字圖光柵化(GlyphAtlas)/材質上傳(TextureUpload)是既有
// 模組，這裡只是新增的 consumer。
namespace SubtitleRender
{
    struct Frame
    {
        IDirect3DDevice9*  device;
        unsigned int       width;    // backbuffer 寬
        unsigned int       height;   // backbuffer 高
        IDirect3DTexture9* glyphTex; // 已 EnsureReady 的 FontCategory::Subtitle A8 atlas
    };

    // tick()：每幀呼叫，做輪詢/watchdog，回傳這幀是否要繪製。
    // draw()：只有任一 producer 的 tick() 回 true 時才呼叫，且呼叫時
    //         render-state 已設定完成、frame 內容已備妥。
    struct Producer
    {
        bool (*tick)();
        void (*draw)(const Frame&);
    };
    void Register(const Producer& producer);

    // 唯一 EndScene hook 的 lazy 安裝：D3D9 裝置未就緒時 no-op，由
    // GlyphHook.cpp 的 GlyphCheck() 每幀驅動重試。
    void EnsureHookInstalled();

    // ---- 排版 + 繪製 helper（供 producer 的 draw() 使用）----

    // 文字含 '\n' 時依換行符切、不套寬度限制（視為已手動排版）；否則依
    // 標點斷行、每視覺行寬度不超過 maxWidthPx。
    // parseTags=true（只有 Oneliners 字幕會傳）：把 <i>/</i>
    // 當零寬標籤，不計入斷行寬度、保留在輸出字串裡；斷點落在 <i>..</i> 中間時
    // 對續行開頭補一個 <i>，讓 DrawLine 逐行判斷斜體不需跨行狀態。
    std::vector<std::string> WrapSubtitleLine(const std::string& text, float maxWidthPx,
                                             bool parseTags = false);

    // centerX 置中、baselineY 為基線，用 FontCategory::Subtitle atlas 逐字
    // 畫 A8 quad。color 為 ARGB vertex diffuse（A8 材質只有覆蓋率，顏色由此
    // 決定），預設白。
    // parseTags=true：<i>/</i> 之間的字以 baseline 為樞軸做水平剪切（假斜體），
    // 標籤本身不佔寬不繪製。
    void DrawLine(IDirect3DDevice9* device, const std::string& line,
                  float centerX, float baselineY, IDirect3DTexture9* tex,
                  unsigned long color = 0xFFFFFFFFu, bool parseTags = false);

    // 統一行高＝[FontSubtitle] size + 8px，首次呼叫後快取（避免每幀讀 ini）。
    float LineHeight();
}
