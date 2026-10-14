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

    static const DWORD kOffset_TM_DecodeBitmapPtr = 0x14;
    static const DWORD kOffset_TM_AllocateSlot    = 0x18;  // sub_48F560，見頂部註解

    // 錯誤修正：候選A，不再呼叫native GetTexture(vtable+0x04/sub_48F140)——
    // 該函式內建一個int 3除錯中斷點(位址0x48F177)，textureDef+0x30殘留
    // 非NULL但指向的count==0時，我方固定傳的a3=0就會踩中；沒掛偵錯器時
    // 這個中斷例外復原不乾淨，暫存器/堆疊被汙染，之後才真正撞進已釋放的
    // atlas記憶體崩潰。改用反組譯出的定址公式直接算textureDef位址，完全
    // 不執行到sub_48F140那段程式碼；AllocateSlot保證index落在1..0x7FF，
    // 用不到sub_48F140對index==0/0x800的sentinel分支。見
    // md\CJK圖集ZBitmap延遲存取崩潰.md 第五/十一輪鑑識。
    static const DWORD kTextureDefStride = 0x44;  // 68 bytes/筆
    static const DWORD kTextureDefBase   = 0x04;  // textureDef = tmInstance + index*68 + 4

    // 錯誤修正：候選B防呆。CleanupBeforeCloseDown(sub_487990)會把
    // m_textureManager物件delete掉但不清NULL(hWnd+0x14)，留下懸空指標；
    // Update()每幀都重新讀這個指標、沒有任何有效性驗證，讀到懸空/損毀
    // 物件的vtable後直接呼叫，EIP就會跳進已釋放記憶體。ZTextureManagerD3D
    // 全binary只有一份靜態vtable，比對這個已知位址即可辨識懸空/損毀物件。
    // 見md\CJK圖集ZBitmap延遲存取崩潰.md 第十輪鑑識。
    static const DWORD kVA_TM_Vtable = 0x00763E3C;

    // textureDef(GetTexture回傳的68 bytes/筆記錄)+0x2C = DecodeBitmapPtr內部
    // 呼叫native CreateTexture後存放的實際材質指標；CreateTexture失敗時
    // native端毫無防呆、直接把NULL存進這裡，nativeIndex仍會被判定登記成功。
    // 之後native某個維護函式(sub_48F590)會無條件對這欄位讀vtable，讀到NULL
    // 就0xC0000005 crash（見md\NULL材質指標崩潰.md）。DecodeBitmapPtr呼叫完
    // 後必須檢查此欄位，NULL就當本次登記失敗、下一幀重試。
    static const DWORD kOffset_TexDef_NativeTexturePtr = 0x2C;

    static const DWORD kVA_ZBitmapCtor = 0x0043F9F0;
    static const DWORD kVA_ZBitmapDtor = 0x0043DAC0;
    // DecodeBitmapPtr內部的sub_48F330會無條件對bitmapObj vtable+0x4(GetName)
    // 的回傳值做strlen、不檢查NULL。ZBitmap ctor只把名稱欄位(this+0x18)歸零、
    // 不設名稱，故建構後必須呼叫此函式(sub_43DB30)給非NULL名稱，避免
    // strlen(NULL) crash。
    static const DWORD kVA_ZBitmapSetName = 0x0043DB30;
    // native ReserveTexture(sub_487CE0)在ZBitmapCtor之後、DecodeBitmapPtr之前
    // 多做一步：ZBitmap.this+0x1C = 剛分配到的slot index（ctor會把這欄位清成0，
    // 由呼叫端負責填；反組譯sub_487CE0第「v10[7] = v4」行證實）。我們繞過
    // ReserveTexture、原本完全沒設這個欄位，ZBitmap+0x1C永遠是0（見
    // md\CJK圖集ZBitmap延遲存取崩潰.md 2026-09-14第二輪鑑識）。
    static const DWORD kOffset_ZBitmap_OwnerSlotIndex = 0x1C;
    // 已知欄位用到+0x2C(DWORD)為止，總計0x30 bytes；多留一截padding避免
    // 版本差異/對齊誤差踩到未知的trailing欄位。
    static const size_t kZBitmapStorageSize = 0x40;

    // 用__fastcall+未使用的dummy edx參數模擬__thiscall——hWnd/m_textureManager/
    // ZBitmap都是遊戲自己編譯的C++類別（RTTI證實，非COM介面）。
    typedef DWORD (__fastcall* AllocateSlotFn)(void* thisPtr, void* /*unused_edx*/);
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

    // 錯誤修正：ReleaseTextures逾時警告＋BeginScene心跳，不受[Debug]開關限制
    // （比照失敗log）。用來抓「載入變慢、貼圖消失」這類沒有例外/崩潰、log
    // 平白卡住的現象：ReleaseTextures命中後若超過kStallWarnThresholdMs仍未
    // 重新登記成功，代表CJK_Atlas材質槽處於裸露狀態；心跳則確認BeginScene
    // 當下還有沒有在跑、卡了多久，取代事後只能靠log空白區間反推的推論方式。
    // 見md\CJK圖集ZBitmap延遲存取崩潰.md 2026-09-14 23:29強制關閉那次分析。
    static DWORD g_lastReleaseTick = 0;
    static bool  g_stallWarned = false;
    static const DWORD kStallWarnThresholdMs = 2000;
    static DWORD g_lastHeartbeatTick = 0;
    static const DWORD kHeartbeatIntervalMs = 5000;

    // 錯誤修正（2026-10-14 02:33）：ReleaseTextures迴圈與登記互斥，見.h的BeginReleaseGuard註解。
    // g_releaseActive：ReleaseTextures進行中；g_registering：正在AllocateSlot→DecodeBitmapPtr
    // 或UpdateTexture的Update()數量（計數器，BeginScene與DetermineCategory兩條路徑可能同時進入）。
    static volatile LONG g_releaseActive = 0;
    static volatile DWORD g_releaseStartTick = 0;
    static volatile LONG g_registering = 0;
    static const DWORD kRegWaitMs = 2000;
    static const DWORD kReleaseExpireMs = 5000;

    static bool IsReleaseActive()
    {
        if (!g_releaseActive) return false;
        return (GetTickCount() - g_releaseStartTick) < kReleaseExpireMs;
    }

    // 先遞增自己的計數器再檢查對方旗標（載入端是先設旗標再等計數器歸零），兩邊都有完整記憶體屏障，
    // 不會兩邊同時通過。拿不到就直接放棄，渲染執行緒不會被擋住。
    struct RegGuard
    {
        bool ok;
        RegGuard() : ok(false)
        {
            if (IsReleaseActive()) return;
            InterlockedIncrement(&g_registering);
            if (IsReleaseActive())
            {
                InterlockedDecrement(&g_registering);
                return;
            }
            ok = true;
        }
        ~RegGuard()
        {
            if (ok) InterlockedDecrement(&g_registering);
        }
    };

    bool BeginReleaseGuard()
    {
        LONG prev = InterlockedCompareExchange(&g_releaseActive, 1, 0);
        if (prev != 0)
        {
            // 已有一個ReleaseTextures在進行：未逾時就不重複取得（避免覆蓋返回位址）；逾時視為上一次
            // 的出口沒有執行，接手旗標。
            if ((GetTickCount() - g_releaseStartTick) < kReleaseExpireMs) return false;
            Log::Write("[NativeTextureRegistry] ReleaseTextures旗標已超過%lu ms未放開，視為失效並接手", kReleaseExpireMs);
        }
        g_releaseStartTick = GetTickCount();

        DWORD t0 = GetTickCount();
        while (g_registering > 0)
        {
            if (GetTickCount() - t0 >= kRegWaitMs)
            {
                Log::Write("[NativeTextureRegistry] ReleaseTextures等待進行中的登記逾時（%lu ms，登記計數=%ld），放行", kRegWaitMs, g_registering);
                break;
            }
            Sleep(1);
        }
        DWORD waited = GetTickCount() - t0;
        if (waited > 0)
            Log::Write("[NativeTextureRegistry] ReleaseTextures入口等待進行中的登記完成：%lu ms", waited);
        return true;
    }

    void EndReleaseGuard()
    {
        InterlockedExchange(&g_releaseActive, 0);
    }

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

    // 檢查已登記的 native 頁是否還活著。換場景清場不一定經過 TextureManagerReleaseHook
    // 掛的 sub_48F190，材質記錄可能已被清成 NULL 但 s.nativeIndex 仍是舊值，之後對它
    // 呼叫 UpdateTexture 會踩到 native NULL 材質指標 crash（見 md\NULL材質指標崩潰.md）。
    static bool IsRegisteredSlotAlive(void* tmInstance, int nativeIndex)
    {
        if (nativeIndex <= 0) return false;

        // 候選A：直接算位址取代呼叫GetTexture，見頂部kTextureDefStride註解。
        void* textureDef = (BYTE*)tmInstance + (size_t)nativeIndex * kTextureDefStride + kTextureDefBase;
        return *(DWORD*)((BYTE*)textureDef + kOffset_TexDef_NativeTexturePtr) != 0;
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
        // 錯誤修正：補回顯式NULL check——下面候選B的比對雖然邏輯上已能
        // 擋掉NULL(0 != 非0的kVA_TM_Vtable)，但編譯器/靜態分析器看不出這層
        // 推論，會對後面tmVtable[...]解引用報「正在取值NULL指標」警告。
        if (!tmVtable) return;
        // 候選B：vtable位址跟已知靜態位址不符，視為懸空/損毀物件，本幀
        // 跳過不使用（不對它呼叫任何native函式，也不嘗試釋放——native
        // 自己delete過，我方從未持有這塊記憶體的所有權；s.nativeIndex等
        // 既有登記狀態維持不動，下次讀到有效vtable時IsRegisteredSlotAlive
        // 會自然判斷是否要重登記，見頂部kVA_TM_Vtable註解）。
        // 錯誤修正：補上失敗log（不受[Debug]開關限制）——若這個判斷持續
        // 不通過，先前完全沒有任何log能看出CJK atlas為何卡在未登記狀態。
        if ((DWORD)tmVtable != kVA_TM_Vtable)
        {
            if (s.failLogCount < 5)
            {
                s.failLogCount++;
                Log::Write("[NativeTextureRegistry] tmVtable位址異常(#%lu)：讀到0x%08lX，預期0x%08lX（%s），視為懸空/損毀物件，本幀跳過",
                           s.failLogCount, (DWORD)tmVtable, kVA_TM_Vtable,
                           NativeBitmapName(category));
            }
            return;
        }

        int width  = GlyphAtlas::GetAtlasWidth();
        int height = GlyphAtlas::GetAtlasHeight();

        if (s.nativeIndex < 0)
        {
            // 錯誤修正（2026-10-14 02:33）：ReleaseTextures進行中不登記，避免AllocateSlot寫入的占位值1
            // 被迴圈當成COM指標解參考（見.h的BeginReleaseGuard註解）。
            if (IsReleaseActive()) return;

            // 準備ZBitmap（不碰材質槽表）移到AllocateSlot之前，縮短「AllocateSlot寫入占位值1」到
            // 「DecodeBitmapPtr寫入真正指標」之間的空窗，載入端等待登記的時間跟著縮短。
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

            // 跟native ReserveTexture一致：DecodeBitmapPtr用完這個暫時ZBitmap就dtor()，
            // 不再常駐（此前「常駐+不dtor」的防禦性修法無效，已移除，見
            // md\CJK圖集ZBitmap延遲存取崩潰.md 2026-09-14第二輪鑑識）。
            ZBitmapDtorFn dtor = (ZBitmapDtorFn)kVA_ZBitmapDtor;

            // 從這裡開始動材質槽表：RegGuard存活期間ReleaseTextures入口會等到本次登記完成。
            RegGuard guard;
            if (!guard.ok)
            {
                dtor(bitmapStorage, nullptr);
                return;
            }

            // 錯誤修正：C6011誤報（第189行已NULL check過tmVtable，分析器
            // flow追蹤跨不過中間的if區塊，屬已知false positive），直接
            // suppress這一行，不影響檔案其他地方的C6011檢查。
            #pragma warning(suppress : 6011)
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
                dtor(bitmapStorage, nullptr);
                return;
            }

            // 候選A：直接算位址取代呼叫GetTexture，見頂部kTextureDefStride
            // 註解。純指標運算不會是NULL（index已由AllocateSlot保證>=1），
            // 不需要再檢查失敗分支。
            void* textureDef = (BYTE*)tmInstance + (size_t)index * kTextureDefStride + kTextureDefBase;

            // 錯誤修正：補上native ReserveTexture會設、我們原本漏設的ZBitmap+0x1C
            // 所屬slot index（見頂部kOffset_ZBitmap_OwnerSlotIndex註解）。
            *(DWORD*)(bitmapStorage + kOffset_ZBitmap_OwnerSlotIndex) = (DWORD)index;

            DecodeBitmapPtrFn decodeBitmapPtr = (DecodeBitmapPtrFn)tmVtable[kOffset_TM_DecodeBitmapPtr / 4];
            decodeBitmapPtr(tmInstance, nullptr, bitmapStorage, textureDef);

            // 錯誤修正：DecodeBitmapPtr內部native CreateTexture失敗時不會有
            // 任何回報，textureDef+0x2C會殘留NULL；不擋住的話nativeIndex會
            // 被當成登記成功，之後native讀這個NULL材質指標會crash。
            DWORD nativeTexturePtr = *(DWORD*)((BYTE*)textureDef + kOffset_TexDef_NativeTexturePtr);

            dtor(bitmapStorage, nullptr);

            if (!nativeTexturePtr)
            {
                if (s.failLogCount < 5)
                {
                    s.failLogCount++;
                    Log::Write("[NativeTextureRegistry] DecodeBitmapPtr失敗(#%lu)：index=%d textureDef+0x2C仍為NULL（native CreateTexture失敗），本次不登記，下一幀重試",
                               s.failLogCount, index);
                }
                return;
            }

            s.nativeIndex = index;
            s.pushedGeneration = GlyphAtlas::GetGeneration(category);  // DecodeBitmapPtr剛把目前內容寫進去了
            Log::Write("[NativeTextureRegistry] 登記成功[%s]：%dx%d -> 頁碼index=%d（AllocateSlot+DecodeBitmapPtr，未經ReserveTexture wrapper）",
                       NativeBitmapName(category), width, height, index);
            // 錯誤修正：General重新登記成功，解除逾時警告狀態（見頂部
            // kStallWarnThresholdMs註解）。
            if (category == FontCategory::General)
            {
                g_lastReleaseTick = 0;
                g_stallWarned = false;
            }
            return;
        }

        // 錯誤修正（2026-10-14 02:33）：UpdateTexture同樣會動到材質槽，ReleaseTextures進行中不執行；
        // 登記計數遞增期間ReleaseTextures入口會等待。
        if (IsReleaseActive()) return;
        RegGuard updateGuard;
        if (!updateGuard.ok) return;

        // 失效就退回未登記狀態，下一幀由 if (s.nativeIndex < 0) 分支重新登記，
        // 不拿失效的 nativeIndex 去呼叫 UpdateTexture。
        if (!IsRegisteredSlotAlive(tmInstance, s.nativeIndex))
        {
            if (s.failLogCount < 5)
            {
                s.failLogCount++;
                Log::Write("[NativeTextureRegistry] 已登記頁失效(#%lu)：index=%d 的 native 材質+0x2C 為 NULL（場景切換清場未經 ReleaseTextures），退回未登記、下一幀重登記",
                           s.failLogCount, s.nativeIndex);
            }
            s.nativeIndex = -1;
            s.pushedGeneration = 0xFFFFFFFFu;
            GlyphHook::InvalidateSynthCache();
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

        // 錯誤修正：同上，補上ZBitmap+0x1C=所屬slot index。
        *(DWORD*)(bitmapStorage + kOffset_ZBitmap_OwnerSlotIndex) = (DWORD)s.nativeIndex;

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
        // 錯誤修正：心跳log，不受[Debug]開關限制——BeginScene若整段時間沒有
        // 任何log輸出，代表引擎卡在某處還沒跑到下一幀；固定間隔印心跳可以
        // 直接從log時間間隔看出「卡在哪個時間點、卡了多久」。
        DWORD now = GetTickCount();
        if (now - g_lastHeartbeatTick >= kHeartbeatIntervalMs)
        {
            g_lastHeartbeatTick = now;
            Log::Write("[NativeTextureRegistry] Heartbeat：BeginScene存活中 tick=%lu", now);
        }

        // 錯誤修正：ReleaseTextures命中後若經過kStallWarnThresholdMs仍未重新
        // 登記成功，代表CJK_Atlas材質槽處於裸露狀態（此刻畫面上CJK字是消失
        // 的）。只警告一次（g_stallWarned），避免同一次卡住洗版。
        if (g_lastReleaseTick != 0 && !g_stallWarned &&
            g_slots[(size_t)FontCategory::General].nativeIndex < 0 &&
            (now - g_lastReleaseTick) >= kStallWarnThresholdMs)
        {
            g_stallWarned = true;
            Log::Write("[NativeTextureRegistry] WARNING：ReleaseTextures命中後已過%lu ms，CJK_Atlas材質槽仍未重建（nativeIndex=-1），貼圖此刻對玩家而言是消失狀態",
                       now - g_lastReleaseTick);
        }

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

        // 錯誤修正：記錄清空時間，供Hook_BeginScene判斷是否逾時未重新登記
        // （見頂部kStallWarnThresholdMs註解）。
        g_lastReleaseTick = GetTickCount();
        g_stallWarned = false;

        // AllocateSlot重新登記時不保證拿回同一個頁碼（實測1->2過），
        // GlyphHook::g_synthCache裡已合成過的字若不一起清掉，會永遠帶著剛
        // 作廢的舊nativeIndex去取樣一張再也不會更新的材質頁（畫面呈現空白
        // 方框，只能靠整段文字重新排版才會消失）。
        GlyphHook::InvalidateSynthCache();
    }
}
