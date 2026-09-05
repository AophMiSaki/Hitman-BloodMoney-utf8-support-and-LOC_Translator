#include "SubtitleRender.h"
#include "Log.h"
#include "Config.h"
#include "FontCategory.h"
#include "GlyphAtlas.h"
#include "TextureUpload.h"
#include <d3d9.h>
#include <string>
#include <vector>
#include <cstdint>

namespace SubtitleRender
{
    // ---- 排版 ----

    // 極簡 UTF-8 解碼，只吃 BMP 常見的 1~3 byte 序列：回傳這個字元佔的
    // byte 數，非法/截斷序列當 1 byte 處理，避免整行卡死。
    static int DecodeUtf8(const char* s, unsigned int& outCp)
    {
        unsigned char c0 = (unsigned char)s[0];
        if (c0 < 0x80) { outCp = c0; return 1; }
        if ((c0 & 0xE0) == 0xC0 && (unsigned char)s[1] >= 0x80)
        {
            outCp = ((c0 & 0x1Fu) << 6) | ((unsigned char)s[1] & 0x3Fu);
            return 2;
        }
        if ((c0 & 0xF0) == 0xE0 && (unsigned char)s[1] >= 0x80 && (unsigned char)s[2] >= 0x80)
        {
            outCp = ((c0 & 0x0Fu) << 12) | (((unsigned char)s[1] & 0x3Fu) << 6) | ((unsigned char)s[2] & 0x3Fu);
            return 3;
        }
        outCp = c0;
        return 1;
    }

    // 全形/半形標點都算斷點，斷在標點「之後」（標點留在前一行）。
    static bool IsBreakablePunctuation(unsigned int cp)
    {
        switch (cp)
        {
            case 0x3001: case 0x3002: case 0xFF0C: case 0xFF0E:
            case 0xFF1B: case 0xFF1A: case 0xFF01: case 0xFF1F:
            case 0x2026:
            case ',': case '.': case ';': case ':': case '!': case '?':
                return true;
            default:
                return false;
        }
    }

    // 只認 <i> <I> </i> </I>（允許標籤內前後空白），其餘一律
    // 不當標籤、原樣輸出（避免把 "a < b" 這種正文的 '<' 吃掉）。s 指向 '<'。
    // 回傳標籤總長（含 '<' 與 '>'），非標籤回 0；outDelta：<i> 為 +1、</i> 為 -1。
    static int ScanItalicTag(const char* s, size_t remain, int& outDelta)
    {
        outDelta = 0;
        if (remain < 3 || s[0] != '<') return 0;
        size_t j = 1;
        while (j < remain && j <= 5 && s[j] != '>' && s[j] != '<') j++;
        if (j >= remain || s[j] != '>') return 0;

        size_t a = 1, b = j;
        while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
        size_t len = b - a;
        if (len == 1 && (s[a] == 'i' || s[a] == 'I'))
            outDelta = 1;
        else if (len == 2 && s[a] == '/' && (s[a + 1] == 'i' || s[a + 1] == 'I'))
            outDelta = -1;
        else
            return 0;
        return (int)(j + 1);
    }

    // 已切好的每一視覺行：斷點若落在 <i>..</i> 中間，續行會缺開頭的 <i>。
    // 逐行掃 <i>/</i> 維護巢狀深度，對「開頭仍在斜體內」的行補一個 <i>，
    // 讓 DrawLine 只需逐行從頭判斷即可。
    static void CarryItalicAcrossLines(std::vector<std::string>& lines)
    {
        int depth = 0;
        for (std::string& ln : lines)
        {
            if (depth > 0)
                ln.insert(0, "<i>");
            for (size_t k = 0; k + 1 < ln.size(); k++)
            {
                if (ln[k] != '<') continue;
                int d = 0;
                int tl = ScanItalicTag(ln.c_str() + k, ln.size() - k, d);
                if (tl > 0)
                {
                    depth += d;
                    if (depth < 0) depth = 0;
                    k += (size_t)tl - 1;
                }
            }
        }
    }

    static float MeasureLineWidth(const std::string& line, bool parseTags)
    {
        float width = 0.0f;
        size_t i = 0;
        while (i < line.size())
        {
            if (parseTags && line[i] == '<')
            {
                int d = 0;
                int tl = ScanItalicTag(line.c_str() + i, line.size() - i, d);
                if (tl > 0) { i += (size_t)tl; continue; }
            }

            unsigned int cp = 0;
            int len = DecodeUtf8(line.c_str() + i, cp);
            i += (size_t)len;

            const GlyphAtlas::Entry* e = GlyphAtlas::GetGlyph(cp, FontCategory::Subtitle);
            if (e) width += (float)e->gm.gmCellIncX;
        }
        return width;
    }

    // 逐字元量寬度+記錄斷點候選，超出 maxWidthPx 時優先切在最後一個標點
    // 斷點；整段沒有標點（或首字元本身就超寬）時退回強制切在超出前的最後
    // 一個字元，確保任何情況都不會有單行超出寬度上限。
    static std::vector<std::string> WrapByWidth(const std::string& text, float maxWidthPx, bool parseTags)
    {
        struct CharInfo { size_t byteStart; size_t byteLen; float width; bool breakable; };
        std::vector<CharInfo> chars;
        size_t i = 0;
        while (i < text.size())
        {
            // <i>/</i>：零寬、不可斷，但保留在 chars 裡讓下面的 substr 帶著它。
            if (parseTags && text[i] == '<')
            {
                int d = 0;
                int tl = ScanItalicTag(text.c_str() + i, text.size() - i, d);
                if (tl > 0)
                {
                    chars.push_back({ i, (size_t)tl, 0.0f, false });
                    i += (size_t)tl;
                    continue;
                }
            }

            unsigned int cp = 0;
            int len = DecodeUtf8(text.c_str() + i, cp);
            const GlyphAtlas::Entry* e = GlyphAtlas::GetGlyph(cp, FontCategory::Subtitle);
            float w = e ? (float)e->gm.gmCellIncX : 0.0f;
            chars.push_back({ i, (size_t)len, w, IsBreakablePunctuation(cp) });
            i += (size_t)len;
        }

        std::vector<std::string> outLines;
        size_t n = chars.size();
        if (n == 0) { outLines.push_back(text); return outLines; }

        size_t lineStart = 0;
        while (lineStart < n)
        {
            size_t idx = lineStart;
            float width = 0.0f;
            size_t lastBreak = SIZE_MAX;
            while (idx < n && width + chars[idx].width <= maxWidthPx)
            {
                width += chars[idx].width;
                if (chars[idx].breakable) lastBreak = idx;
                idx++;
            }

            if (idx >= n)
            {
                outLines.push_back(text.substr(chars[lineStart].byteStart, text.size() - chars[lineStart].byteStart));
                break;
            }

            size_t breakAt = (lastBreak != SIZE_MAX) ? lastBreak : (idx > lineStart ? idx - 1 : lineStart);
            size_t byteEnd = chars[breakAt].byteStart + chars[breakAt].byteLen;
            outLines.push_back(text.substr(chars[lineStart].byteStart, byteEnd - chars[lineStart].byteStart));
            lineStart = breakAt + 1;
        }
        return outLines;
    }

    std::vector<std::string> WrapSubtitleLine(const std::string& text, float maxWidthPx, bool parseTags)
    {
        std::vector<std::string> outLines;
        if (text.find('\n') != std::string::npos)
        {
            size_t start = 0;
            while (true)
            {
                size_t pos = text.find('\n', start);
                std::string segment = (pos == std::string::npos)
                                           ? text.substr(start)
                                           : text.substr(start, pos - start);
                if (!segment.empty() && segment.back() == '\r') segment.pop_back(); // CRLF 殘留
                outLines.push_back(std::move(segment));
                if (pos == std::string::npos) break;
                start = pos + 1;
            }
        }
        else
        {
            outLines = WrapByWidth(text, maxWidthPx, parseTags);
        }

        if (parseTags)
            CarryItalicAcrossLines(outLines);
        return outLines;
    }

    float LineHeight()
    {
        static float s_lineHeight = -1.0f;
        if (s_lineHeight < 0.0f)
            s_lineHeight = (float)Config::LoadSubtitle(nullptr).size + 8.0f;
        return s_lineHeight;
    }

    // ---- 繪製 primitives ----

    struct ScreenVertex
    {
        float x, y, z, rhw;
        DWORD color;
        float u, v;
    };
    static const DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

    // 假斜體剪切量（tan θ）：0.0 為不剪切。
    static const float kItalicShear = 0.20f;

    // D3DFVF_XYZRHW（預先變換好的螢幕座標）搭配貼圖取樣時，座標要減 0.5 讓
    // texel 中心對齊 pixel 中心。否則取樣點跨在兩個 texel 中間，而 atlas 字
    // 與字之間只留 1px padding，半 texel 偏移混進來的是隔壁字的墨跡，畫面
    // 糊成一片。
    // shear!=0 時：以 baselineY 為樞軸的水平剪切，越靠上位移
    // 越大、baseline 上的點不動 → 字往右傾、行首行尾對齊不亂。
    static void DrawGlyphQuad(IDirect3DDevice9* device, float x, float y, float w, float h,
                              float u0, float v0, float u1, float v1, DWORD color,
                              float shear = 0.0f, float baselineY = 0.0f)
    {
        const float ox = x - 0.5f;
        const float oy = y - 0.5f;
        const float topDX = (shear != 0.0f) ? shear * (baselineY - oy)         : 0.0f;
        const float botDX = (shear != 0.0f) ? shear * (baselineY - (oy + h))   : 0.0f;
        ScreenVertex verts[4] =
        {
            { ox + topDX,     oy,     0.0f, 1.0f, color, u0, v0 },
            { ox + w + topDX, oy,     0.0f, 1.0f, color, u1, v0 },
            { ox + botDX,     oy + h, 0.0f, 1.0f, color, u0, v1 },
            { ox + w + botDX, oy + h, 0.0f, 1.0f, color, u1, v1 },
        };
        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, verts, sizeof(ScreenVertex));
    }

    void DrawLine(IDirect3DDevice9* device, const std::string& line, float centerX, float baselineY,
                  IDirect3DTexture9* tex, unsigned long color, bool parseTags)
    {
        float lineWidth = MeasureLineWidth(line, parseTags);
        float penX = centerX - lineWidth * 0.5f;

        int italicDepth = 0;
        size_t i = 0;
        while (i < line.size())
        {
            if (parseTags && line[i] == '<')
            {
                int d = 0;
                int tl = ScanItalicTag(line.c_str() + i, line.size() - i, d);
                if (tl > 0)
                {
                    italicDepth += d;
                    if (italicDepth < 0) italicDepth = 0;
                    i += (size_t)tl;
                    continue;
                }
            }

            unsigned int cp = 0;
            int len = DecodeUtf8(line.c_str() + i, cp);
            i += (size_t)len;

            const GlyphAtlas::Entry* e = GlyphAtlas::GetGlyph(cp, FontCategory::Subtitle);
            if (!e) continue;

            if (e->width > 0 && e->height > 0 && tex)
            {
                int atlasW = GlyphAtlas::GetAtlasWidth();
                int atlasH = GlyphAtlas::GetAtlasHeight();

                float u0 = (float)e->atlasX / (float)atlasW;
                float v0 = (float)e->atlasY / (float)atlasH;
                float u1 = (float)(e->atlasX + e->width) / (float)atlasW;
                float v1 = (float)(e->atlasY + e->height) / (float)atlasH;

                float gx = penX + (float)e->gm.gmptGlyphOrigin.x;
                float gy = baselineY - (float)e->gm.gmptGlyphOrigin.y;

                device->SetTexture(0, tex);
                DrawGlyphQuad(device, gx, gy, (float)e->width, (float)e->height, u0, v0, u1, v1, (DWORD)color,
                              italicDepth > 0 ? kItalicShear : 0.0f, baselineY);
            }

            penX += (float)e->gm.gmCellIncX;
        }
    }

    // ---- producer 註冊 ----

    static std::vector<Producer> g_producers;

    void Register(const Producer& producer)
    {
        g_producers.push_back(producer);
    }

    // ---- 唯一 EndScene hook ----

    typedef HRESULT(__stdcall* EndSceneFn)(IDirect3DDevice9*);
    static EndSceneFn g_origEndScene = nullptr;
    static bool  g_endSceneHooked = false;
    static DWORD g_endSceneFailLog = 0;

    static HRESULT __stdcall Hook_EndScene(IDirect3DDevice9* device)
    {
        // 每幀先讓每個 producer 輪詢（場景切換偵測/watchdog 清除/可見性
        // 判斷），成本只有輪詢本身、不碰 D3D。全部回 false 就直接放行，
        // 完全不動 render-state。
        bool wantDraw[16] = {};
        bool anyWantDraw = false;
        size_t n = g_producers.size();
        if (n > 16) n = 16;
        for (size_t i = 0; i < n; i++)
        {
            if (g_producers[i].tick && g_producers[i].tick())
            {
                wantDraw[i] = true;
                anyWantDraw = true;
            }
        }

        if (anyWantDraw)
        {
            IDirect3DSurface9* backBuffer = nullptr;
            if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) && backBuffer)
            {
                D3DSURFACE_DESC desc = {};
                backBuffer->GetDesc(&desc);
                backBuffer->Release();

                TextureUpload::EnsureReady();

                Frame frame = {};
                frame.device   = device;
                frame.width    = desc.Width;
                frame.height   = desc.Height;
                frame.glyphTex = TextureUpload::GetTexture();

                // 存受影響的 render-state，畫完還原——native 不保證每幀都會
                // 重設這些 state，不還原可能干擾 native 下一批 draw call。
                DWORD oldFvf = 0; device->GetFVF(&oldFvf);
                IDirect3DBaseTexture9* oldTex = nullptr; device->GetTexture(0, &oldTex);
                DWORD oldAlphaBlend = 0; device->GetRenderState(D3DRS_ALPHABLENDENABLE, &oldAlphaBlend);
                DWORD oldSrcBlend = 0;   device->GetRenderState(D3DRS_SRCBLEND, &oldSrcBlend);
                DWORD oldDestBlend = 0;  device->GetRenderState(D3DRS_DESTBLEND, &oldDestBlend);
                DWORD oldZEnable = 0;    device->GetRenderState(D3DRS_ZENABLE, &oldZEnable);
                DWORD oldCull = 0;       device->GetRenderState(D3DRS_CULLMODE, &oldCull);
                DWORD oldColorOp = 0, oldColorArg1 = 0, oldAlphaOp = 0, oldAlphaArg1 = 0, oldAlphaArg2 = 0;
                device->GetTextureStageState(0, D3DTSS_COLOROP, &oldColorOp);
                device->GetTextureStageState(0, D3DTSS_COLORARG1, &oldColorArg1);
                device->GetTextureStageState(0, D3DTSS_ALPHAOP, &oldAlphaOp);
                device->GetTextureStageState(0, D3DTSS_ALPHAARG1, &oldAlphaArg1);
                device->GetTextureStageState(0, D3DTSS_ALPHAARG2, &oldAlphaArg2);

                // native 畫完 data-driven UI 後常留著 vertex/pixel shader；只要
                // shader 還繫著，下面設的 fixed-function texture stage state 會
                // 被忽略、字幕顏色跟著 native 前一個元件跑。清成 nullptr 逼回
                // fixed-function pipeline，畫完還原。
                IDirect3DVertexShader9* oldVS = nullptr; device->GetVertexShader(&oldVS);
                IDirect3DPixelShader9* oldPS = nullptr;  device->GetPixelShader(&oldPS);
                device->SetVertexShader(nullptr);
                device->SetPixelShader(nullptr);

                device->SetFVF(kFvf);
                device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
                device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
                device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
                device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
                device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                // A8 材質只有 alpha channel：顏色用 vertex diffuse（固定白），
                // alpha 用 texture 覆蓋率乘 vertex alpha——標準 A8 字型繪製。
                device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
                device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
                device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
                device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
                device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

                // 依註冊順序繪製＝疊放層序（先註冊的在下層）。
                for (size_t i = 0; i < n; i++)
                    if (wantDraw[i] && g_producers[i].draw)
                        g_producers[i].draw(frame);

                device->SetFVF(oldFvf);
                device->SetTexture(0, oldTex);
                if (oldTex) oldTex->Release();
                device->SetVertexShader(oldVS);
                if (oldVS) oldVS->Release();
                device->SetPixelShader(oldPS);
                if (oldPS) oldPS->Release();
                device->SetRenderState(D3DRS_ALPHABLENDENABLE, oldAlphaBlend);
                device->SetRenderState(D3DRS_SRCBLEND, oldSrcBlend);
                device->SetRenderState(D3DRS_DESTBLEND, oldDestBlend);
                device->SetRenderState(D3DRS_ZENABLE, oldZEnable);
                device->SetRenderState(D3DRS_CULLMODE, oldCull);
                device->SetTextureStageState(0, D3DTSS_COLOROP, oldColorOp);
                device->SetTextureStageState(0, D3DTSS_COLORARG1, oldColorArg1);
                device->SetTextureStageState(0, D3DTSS_ALPHAOP, oldAlphaOp);
                device->SetTextureStageState(0, D3DTSS_ALPHAARG1, oldAlphaArg1);
                device->SetTextureStageState(0, D3DTSS_ALPHAARG2, oldAlphaArg2);
            }
        }

        return g_origEndScene ? g_origEndScene(device) : D3DERR_INVALIDCALL;
    }

    void EnsureHookInstalled()
    {
        if (g_endSceneHooked) return;

        IDirect3DDevice9* device = GlyphAtlas::GetD3D9Device();
        if (!device)
        {
            if (g_endSceneFailLog < 5)
            {
                g_endSceneFailLog++;
                Log::Write("[SubtitleRender] EnsureHookInstalled 失敗(#%lu)：D3D9 裝置尚未就緒", g_endSceneFailLog);
            }
            return;
        }

        DWORD* vtable = *(DWORD**)device;
        DWORD oldProt = 0;

        // IDirect3DDevice9::EndScene = vtable+0xA8（index 42）。
        VirtualProtect(&vtable[0xA8 / 4], 4, PAGE_EXECUTE_READWRITE, &oldProt);
        g_origEndScene = (EndSceneFn)vtable[0xA8 / 4];
        vtable[0xA8 / 4] = (DWORD)&Hook_EndScene;
        VirtualProtect(&vtable[0xA8 / 4], 4, oldProt, &oldProt);

        g_endSceneHooked = true;
        Log::Write("[SubtitleRender] 統一 EndScene compositor hook 安裝完成：device=%p orig=%p producer數=%zu",
                   device, g_origEndScene, g_producers.size());
    }
}
