#include "NativeTextureRegistry.h"
#include "GlyphAtlas.h"
#include "GlyphHook.h"
#include "Config.h"
#include "Log.h"
#include <d3d9.h>
#include <vector>
#include <cstring>

// hWnd = *(void**)0x8ACA30 = ZRenderWintelD3DDll 實例（RTTI 證實）。
//
// 不呼叫 hWnd vtable+0x98（sub_487CE0 / ReserveTexture）——其入口一次配置
// 約 256KB 堆疊(alloca) 生成一張用不到的 checkerboard 佔位圖，實機會 crash。
// 改直接呼叫 ZTextureManagerD3D（m_textureManager = hWnd+0x14）的：
//   vtable+0x18（sub_48F560，AllocateSlot）：找一個空閒 slot、標記為已用、
//     回傳 index（ReHitman SDK 誤稱 GetFreeTextureSlotsCount，實際是配置）。
//   vtable+0x14（sub_48D790，DecodeBitmapPtr）：轉呼叫 vtable+0x10（sub_48F330，
//     TEX entry 型別分派函式），把 ZBitmap 內容寫進材質。
// hWnd vtable+0x9C（sub_487C10，UpdateTexture(index,ZBitmap*)）輕量無 alloca，
// 用於「atlas 有新 glyph、要更新已登記頁面內容」。
//
// kVA_ZBitmapCtor(sub_43F9F0) 是 ZBitmap 建構子，把 buffer 依 mask 轉成 native
// canonical 格式；kVA_ZBitmapDtor(sub_43DAC0) 對應解構子（釋放內部 buffer）。
// 用法：stack 上構造 -> 傳指標給 texture manager -> 立刻解構。
//
// 重量級 native 呼叫只從 IDirect3DDevice9::BeginScene hook 觸發，不從
// GlyphCheck 深層呼叫鏈直接呼叫。

namespace NativeTextureRegistry
{
    static const DWORD kVA_hWnd = 0x008ACA30;
    static const DWORD kOffset_TexManager     = 0x14;  // hWnd+0x14 = m_textureManager
    static const DWORD kOffset_UpdateTexture  = 0x9C;  // hWnd自己的vtable slot

    static const DWORD kOffset_TM_GetTexture      = 0x04;
    static const DWORD kOffset_TM_DecodeBitmapPtr = 0x14;
    static const DWORD kOffset_TM_AllocateSlot    = 0x18;  // sub_48F560，見頂部註解

    static const DWORD kVA_ZBitmapCtor = 0x0043F9F0;
    static const DWORD kVA_ZBitmapDtor = 0x0043DAC0;
    // DecodeBitmapPtr內部的sub_48F330會無條件對bitmapObj vtable+0x4(GetName)
    // 的回傳值做strlen、不檢查NULL。ZBitmap ctor只把名稱欄位(this+0x18)歸零、
    // 不設名稱，故建構後必須呼叫此函式(sub_43DB30)給非NULL名稱，避免
    // strlen(NULL) crash。
    static const DWORD kVA_ZBitmapSetName = 0x0043DB30;
    // 已知欄位用到+0x2C(DWORD)為止，總計0x30 bytes；多留一截padding避免
    // 版本差異/對齊誤差踩到未知的trailing欄位。
    static const size_t kZBitmapStorageSize = 0x40;

    // 用__fastcall+未使用的dummy edx參數模擬__thiscall——hWnd/m_textureManager/
    // ZBitmap都是遊戲自己編譯的C++類別（RTTI證實，非COM介面）。
    typedef DWORD (__fastcall* AllocateSlotFn)(void* thisPtr, void* /*unused_edx*/);
    typedef void* (__fastcall* GetTextureFn)(void* thisPtr, void* /*unused_edx*/, unsigned int index, unsigned int a2);
    typedef void  (__fastcall* DecodeBitmapPtrFn)(void* thisPtr, void* /*unused_edx*/, void* bitmapObjPtr, void* textureDefPtr);
    typedef void  (__fastcall* UpdateTextureFn)(void* thisPtr, void* /*unused_edx*/, int index, void* bitmap);
    typedef void* (__fastcall* ZBitmapCtorFn)(void* thisPtr, void* /*unused_edx*/, const void* buffer,
                                               DWORD width, DWORD height,
                                               DWORD maskR, DWORD maskG, DWORD maskB, DWORD maskA, DWORD flag);
    typedef void  (__fastcall* ZBitmapDtorFn)(void* thisPtr, void* /*unused_edx*/);
    typedef void  (__fastcall* ZBitmapSetNameFn)(void* thisPtr, void* /*unused_edx*/, const char* name);

    // IDirect3DDevice9是COM介面，member function為__stdcall、this當explicit
    // 第一個參數傳，不套用上面的__fastcall trick。
    typedef HRESULT (__stdcall* BeginSceneFn)(IDirect3DDevice9* device);
    typedef HRESULT (__stdcall* ResetFn)(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* presentationParameters);

    struct SlotState
    {
        int   nativeIndex = -1;         // native頁碼，-1=尚未登記
        DWORD pushedGeneration = 0xFFFFFFFFu;  // 跟GlyphAtlas::GetGeneration()初始值(0)不同，保證第一次一定觸發
        DWORD failLogCount = 0;
        std::vector<DWORD> scratch32bpp;  // 8bpp atlas -> 32bpp ARGB轉換用緩衝區，重複利用避免每次都配置
    };

    // 通用/報紙各自登記獨立一份native頁碼（atlas內容不同，不能共用）。字幕
    // 不經過這條路徑，g_slots[Subtitle]配置了但永遠不會被用到。
    static SlotState g_slots[(size_t)FontCategory::Count];

    static const char* NativeBitmapName(FontCategory category)
    {
        switch (category)
        {
        // 報紙4個<font N>級別各自登記獨立的native材質頁，名稱各自不同以利辨識。
        case FontCategory::NewspaperFont1: return "CJK_Atlas_NewsFont1";
        case FontCategory::NewspaperFont2: return "CJK_Atlas_NewsFont2";
        case FontCategory::NewspaperFont3: return "CJK_Atlas_NewsFont3";
        case FontCategory::NewspaperFont4: return "CJK_Atlas_NewsFont4";
        case FontCategory::General:
        default:                             return "CJK_Atlas";
        }
    }

    static bool         g_hooked = false;
    static BeginSceneFn g_origBeginScene = nullptr;
    static DWORD         g_hookFailLogCount = 0;

    // [Debug] NativeTexRegDiagEnable：登記成功/失敗log不受此旗標限制一律顯示；
    // 旗標只控制其餘細節診斷log（Hook_BeginScene進入次數、InvalidateAllSlots
    // 重設通知、場景中途atlas內容更新完成）。預設關閉。
    static bool g_diagEnable = false;

    void Install(HMODULE hModule)
    {
        g_diagEnable = Config::LoadDebug(hModule).nativeTexRegDiagEnable;
        Log::Write("[NativeTextureRegistry] Install完成：diagEnable=%d（換場景時的登記成功/失敗log不受此旗標限制，一律顯示）", g_diagEnable);
    }

    static void* GetHWndObject()
    {
        DWORD obj = *(DWORD*)kVA_hWnd;
        if (!obj || obj == 0xFFFFFFFF) return nullptr;
        return (void*)obj;
    }

    // 8bpp灰階(A8語意) -> 32bpp ARGB：RGB固定填白(0xFFFFFF)，A填原始灰階值，
    // 跟native既有ASCII字型「白字+alpha覆蓋率、實際顏色由輸出記錄+0x24的ARGB
    // 欄位在draw時相乘」的慣例一致。
    static void ConvertAtlasTo32bpp(std::vector<DWORD>& out, FontCategory category, int width, int height)
    {
        const BYTE* src = GlyphAtlas::GetAtlasBuffer(category);
        out.resize((size_t)width * height);
        for (int i = 0; i < width * height; i++)
            out[i] = ((DWORD)src[i] << 24) | 0x00FFFFFFu;
    }

    // 真正呼叫native的重量級函式。
    // category必須是General或Newspaper（字幕不經過這條路徑）。呼叫端對
    // Newspaper要先檢查GlyphAtlas::IsCategoryReady()，還沒被實際觸發過時
    // 完全不呼叫，避免平白登記一份跟通用內容重複的材質槽。
    // 除Hook_BeginScene外，也公開給GlyphHook.cpp的DetermineCategory()在偵測
    // 到報紙分類的當下提前呼叫一次：報紙排版是native一次性同步跑完，等下一次
    // BeginScene才登記材質太慢，文字已排版完畢、CJK全部合成失敗。
    void Update(FontCategory category)
    {
        SlotState& s = g_slots[(size_t)category];

        void* hWndObj = GetHWndObject();
        if (!hWndObj) return;

        void* tmInstance = *(void**)((BYTE*)hWndObj + kOffset_TexManager);
        if (!tmInstance) return;
        DWORD* tmVtable = *(DWORD**)tmInstance;
        if (!tmVtable) return;

        int width  = GlyphAtlas::GetAtlasWidth();
        int height = GlyphAtlas::GetAtlasHeight();

        if (s.nativeIndex < 0)
        {
            AllocateSlotFn allocateSlot = (AllocateSlotFn)tmVtable[kOffset_TM_AllocateSlot / 4];
            int index = (int)allocateSlot(tmInstance, nullptr);

            if (index <= 0)
            {
                if (s.failLogCount < 5)
                {
                    s.failLogCount++;
                    Log::Write("[NativeTextureRegistry] AllocateSlot失敗(#%lu)：回傳index=%d",
                               s.failLogCount, index);
                }
                return;
            }

            GetTextureFn getTexture = (GetTextureFn)tmVtable[kOffset_TM_GetTexture / 4];
            void* textureDef = getTexture(tmInstance, nullptr, (unsigned int)index, 0);

            if (!textureDef)
            {
                if (s.failLogCount < 5)
                {
                    s.failLogCount++;
                    Log::Write("[NativeTextureRegistry] GetTexture失敗(#%lu)：index=%d 回傳NULL",
                               s.failLogCount, index);
                }
                return;
            }

            ConvertAtlasTo32bpp(s.scratch32bpp, category, width, height);

            BYTE bitmapStorage[kZBitmapStorageSize] = {};
            ZBitmapCtorFn ctor = (ZBitmapCtorFn)kVA_ZBitmapCtor;
            ctor(bitmapStorage, nullptr, s.scratch32bpp.data(), (DWORD)width, (DWORD)height,
                 0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0xFF000000u, 0u);

            // sub_48F330會對GetName()(vtable+0x4)回傳值做strlen，NULL會crash
            // （見頂部kVA_ZBitmapSetName註解）——ctor沒設名稱，這裡補上。
            {
                ZBitmapSetNameFn setName = (ZBitmapSetNameFn)kVA_ZBitmapSetName;
                setName(bitmapStorage, nullptr, NativeBitmapName(category));
            }

            DecodeBitmapPtrFn decodeBitmapPtr = (DecodeBitmapPtrFn)tmVtable[kOffset_TM_DecodeBitmapPtr / 4];
            decodeBitmapPtr(tmInstance, nullptr, bitmapStorage, textureDef);

            ZBitmapDtorFn dtor = (ZBitmapDtorFn)kVA_ZBitmapDtor;
            dtor(bitmapStorage, nullptr);

            s.nativeIndex = index;
            s.pushedGeneration = GlyphAtlas::GetGeneration(category);  // DecodeBitmapPtr剛把目前內容寫進去了
            Log::Write("[NativeTextureRegistry] 登記成功[%s]：%dx%d -> 頁碼index=%d（AllocateSlot+DecodeBitmapPtr，未經ReserveTexture wrapper）",
                       NativeBitmapName(category), width, height, index);
            return;
        }

        DWORD gen = GlyphAtlas::GetGeneration(category);
        if (gen == s.pushedGeneration) return;

        ConvertAtlasTo32bpp(s.scratch32bpp, category, width, height);

        BYTE bitmapStorage[kZBitmapStorageSize] = {};
        ZBitmapCtorFn ctor = (ZBitmapCtorFn)kVA_ZBitmapCtor;
        void* bitmap = ctor(bitmapStorage, nullptr, s.scratch32bpp.data(), (DWORD)width, (DWORD)height,
                             0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0xFF000000u, 0u);

        // 同樣補SetName，避免UpdateTexture內部鏈萬一也走到GetName()+strlen
        // 重演同一種NULL pointer dereference。
        {
            ZBitmapSetNameFn setName = (ZBitmapSetNameFn)kVA_ZBitmapSetName;
            setName(bitmapStorage, nullptr, NativeBitmapName(category));
        }

        DWORD* hWndVtable = *(DWORD**)hWndObj;
        UpdateTextureFn updateTexture = (UpdateTextureFn)hWndVtable[kOffset_UpdateTexture / 4];
        updateTexture(hWndObj, nullptr, s.nativeIndex, bitmap);

        ZBitmapDtorFn dtor = (ZBitmapDtorFn)kVA_ZBitmapDtor;
        dtor(bitmapStorage, nullptr);

        s.pushedGeneration = gen;
        if (g_diagEnable)
        {
            Log::Write("[NativeTextureRegistry] Update材質內容完成[%s]：index=%d generation=%lu %dx%d",
                       NativeBitmapName(category), s.nativeIndex, gen, width, height);
        }
    }

    static DWORD g_beginSceneHits = 0;

    static HRESULT __stdcall Hook_BeginScene(IDirect3DDevice9* device)
    {
        if (g_diagEnable && g_beginSceneHits < 3)
        {
            g_beginSceneHits++;
            Log::Write("[NativeTextureRegistry] Hook_BeginScene進入 #%lu", g_beginSceneHits);
        }
        // 通用一律更新；報紙4級各自只在IsCategoryReady時才Update——同一頁
        // 報紙不一定4級都會用到，沒被SynthesizeCJKGlyphRecord量到過的級別
        // 維持NotLoaded，這裡自然跳過、不登記材質。
        Update(FontCategory::General);
        static const FontCategory kNewspaperCategories[] = {
            FontCategory::NewspaperFont1, FontCategory::NewspaperFont2,
            FontCategory::NewspaperFont3, FontCategory::NewspaperFont4,
        };
        for (FontCategory cat : kNewspaperCategories)
        {
            if (GlyphAtlas::IsCategoryReady(cat))
                Update(cat);
        }
        return g_origBeginScene ? g_origBeginScene(device) : D3DERR_INVALIDCALL;
    }

    // 切換解析度/視窗/全螢幕時native只走IDirect3DDevice9::Reset()，不經
    // TextureManagerReleaseHook.cpp攔截的native ReleaseTextures，g_slots不會
    // 被InvalidateAllSlots()重設，Update()因「generation沒變」直接return、
    // 永遠不會重新登記（現象：切解析度後譯文字消失、不會自己恢復）。修法：
    // Reset成功後呼叫InvalidateAllSlots()，讓下次Hook_BeginScene重新登記。
    static ResetFn g_origReset = nullptr;
    static DWORD   g_resetHits = 0;

    static HRESULT __stdcall Hook_Reset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* presentationParameters)
    {
        HRESULT hr = g_origReset ? g_origReset(device, presentationParameters) : D3DERR_INVALIDCALL;

        g_resetHits++;
        if (g_resetHits <= 20)
        {
            Log::Write("[NativeTextureRegistry] Reset命中#%lu：hr=0x%08lX %ux%u windowed=%d -> %s",
                       g_resetHits, (DWORD)hr,
                       presentationParameters ? presentationParameters->BackBufferWidth : 0,
                       presentationParameters ? presentationParameters->BackBufferHeight : 0,
                       presentationParameters ? presentationParameters->Windowed : -1,
                       SUCCEEDED(hr) ? "InvalidateAllSlots" : "Reset失敗，不動材質槽狀態");
        }

        if (SUCCEEDED(hr))
        {
            InvalidateAllSlots();
        }
        return hr;
    }

    void EnsureBeginSceneHookInstalled()
    {
        if (g_hooked) return;

        IDirect3DDevice9* device = GlyphAtlas::GetD3D9Device();
        if (!device)
        {
            if (g_hookFailLogCount < 5)
            {
                g_hookFailLogCount++;
                Log::Write("[NativeTextureRegistry] EnsureBeginSceneHookInstalled失敗(#%lu)：D3D9裝置尚未就緒",
                           g_hookFailLogCount);
            }
            return;
        }

        // IDirect3DDevice9標準COM vtable：BeginScene=0xA4（index 41）。
        DWORD* vtable = *(DWORD**)device;
        DWORD oldProt = 0;

        VirtualProtect(&vtable[0xA4 / 4], 4, PAGE_EXECUTE_READWRITE, &oldProt);
        g_origBeginScene = (BeginSceneFn)vtable[0xA4 / 4];
        vtable[0xA4 / 4] = (DWORD)&Hook_BeginScene;
        VirtualProtect(&vtable[0xA4 / 4], 4, oldProt, &oldProt);

        // Reset=0x40（index 16），見上方Hook_Reset函式頭部註解。
        VirtualProtect(&vtable[0x40 / 4], 4, PAGE_EXECUTE_READWRITE, &oldProt);
        g_origReset = (ResetFn)vtable[0x40 / 4];
        vtable[0x40 / 4] = (DWORD)&Hook_Reset;
        VirtualProtect(&vtable[0x40 / 4], 4, oldProt, &oldProt);

        g_hooked = true;
        Log::Write("[NativeTextureRegistry] BeginScene/Reset hook安裝完成：device=%p vtable=%p origBeginScene=%p origReset=%p",
                   device, vtable, g_origBeginScene, g_origReset);
    }

    int GetRegisteredIndex(FontCategory category)
    {
        return g_slots[(size_t)category].nativeIndex;
    }

    // native ReleaseTextures（sub_48F190）會無差別walk全部材質槽並memset歸零，
    // 我們登記的slot跟native材質共用同一張表、毫無區別標記，被清掉後
    // s.nativeIndex仍指向一個已失效的頁碼。這裡只重設static狀態、不做任何
    // native呼叫，讓下次Update()因nativeIndex<0自動重新完整登記。
    void InvalidateAllSlots()
    {
        // 迴圈重設全部分類（含從不使用的g_slots[Subtitle]，重設它無害）。
        for (size_t i = 0; i < (size_t)FontCategory::Count; i++)
        {
            g_slots[i].nativeIndex = -1;
            g_slots[i].pushedGeneration = 0xFFFFFFFFu;
        }
        if (g_diagEnable)
            Log::Write("[NativeTextureRegistry] InvalidateAllSlots：全部分類nativeIndex重設為-1，下次BeginScene會重新登記");

        // AllocateSlot重新登記時不保證拿回同一個頁碼（實測1->2過），
        // GlyphHook::g_synthCache裡已合成過的字若不一起清掉，會永遠帶著剛
        // 作廢的舊nativeIndex去取樣一張再也不會更新的材質頁（畫面呈現空白
        // 方框，只能靠整段文字重新排版才會消失）。
        GlyphHook::InvalidateSynthCache();
    }
}
