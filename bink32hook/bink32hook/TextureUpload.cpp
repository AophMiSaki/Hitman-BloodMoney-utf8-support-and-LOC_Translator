#include "TextureUpload.h"
#include "GlyphAtlas.h"
#include "Log.h"
#include <cstring>

namespace TextureUpload
{
    struct SlotState
    {
        IDirect3DTexture9* texture = nullptr;
        DWORD uploadedGeneration = 0xFFFFFFFFu;  // sentinel：跟GlyphAtlas::GetGeneration()的初始值(0)不同，保證第一次一定觸發上傳
        DWORD deviceFailLogCount = 0;
        DWORD createFailLogCount = 0;
    };

    static SlotState g_slot;
    static bool g_diagEnable = false;

    // 把GlyphAtlas的A8點陣圖整塊複製進LockRect拿到的材質記憶體，逐列複製是
    // 因為LockedRect.Pitch（材質實際的列距，可能跟atlas寬度不同，取決於
    // 驅動的材質對齊要求）不保證等於atlas本身的列距。
    static bool UploadAtlas()
    {
        SlotState& s = g_slot;
        int width  = GlyphAtlas::GetAtlasWidth();
        int height = GlyphAtlas::GetAtlasHeight();

        D3DLOCKED_RECT locked = {};
        HRESULT hr = s.texture->LockRect(0, &locked, nullptr, 0);
        if (FAILED(hr))
        {
            Log::Write("[TextureUpload] LockRect失敗：hr=0x%08X", (DWORD)hr);
            return false;
        }

        // 固定讀取字幕分類的atlas（Subtitle未Ready/初始化失敗時GlyphAtlas
        // 內部會自動fallback回General，這裡跟著拿到同一份buffer）。
        const BYTE* src = GlyphAtlas::GetAtlasBuffer(FontCategory::Subtitle);
        BYTE* dst = (BYTE*)locked.pBits;
        // LockRect回S_OK不保證pBits非NULL。
        if (!dst)
        {
            Log::Write("[TextureUpload] LockRect成功但pBits=NULL，放棄上傳");
            s.texture->UnlockRect(0);
            return false;
        }
        for (int y = 0; y < height; y++)
            memcpy(dst + y * locked.Pitch, src + y * width, width);

        s.texture->UnlockRect(0);
        return true;
    }

    void EnsureReady()
    {
        SlotState& s = g_slot;

        IDirect3DDevice9* device = GlyphAtlas::GetD3D9Device();
        if (!device)
        {
            if (s.deviceFailLogCount < 5)
            {
                s.deviceFailLogCount++;
                Log::Write("[TextureUpload] EnsureReady失敗(#%lu)：D3D9裝置尚未就緒",
                           s.deviceFailLogCount);
            }
            return;
        }

        if (!s.texture)
        {
            HMODULE hModule = GetModuleHandleA("binkw32.dll");
            Config::DebugConfig dbg = Config::LoadDebug(hModule);
            g_diagEnable = dbg.texUploadDiagEnable;

            int width  = GlyphAtlas::GetAtlasWidth();
            int height = GlyphAtlas::GetAtlasHeight();
            HRESULT hr = device->CreateTexture((UINT)width, (UINT)height, 1, 0,
                                                D3DFMT_A8, D3DPOOL_MANAGED, &s.texture, nullptr);
            if (FAILED(hr) || !s.texture)
            {
                if (s.createFailLogCount < 5)
                {
                    s.createFailLogCount++;
                    Log::Write("[TextureUpload] CreateTexture失敗(#%lu)：%dx%d D3DFMT_A8 hr=0x%08X",
                               s.createFailLogCount, width, height, (DWORD)hr);
                }
                return;
            }
            Log::Write("[TextureUpload] CreateTexture成功：%dx%d D3DFMT_A8 texture=%p",
                       width, height, s.texture);
        }

        DWORD gen = GlyphAtlas::GetGeneration(FontCategory::Subtitle);
        if (gen == s.uploadedGeneration) return;  // atlas沒有新glyph，不用重傳

        if (UploadAtlas())
        {
            s.uploadedGeneration = gen;
            if (g_diagEnable)
                Log::Write("[TextureUpload] 上傳完成：generation=%lu", gen);
        }
    }

    IDirect3DTexture9* GetTexture() { return g_slot.texture; }
}
