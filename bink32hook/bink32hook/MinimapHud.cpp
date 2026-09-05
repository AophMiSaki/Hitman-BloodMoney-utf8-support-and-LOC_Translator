#include "MinimapHud.h"
#include "Config.h"
#include "Log.h"
#include "Paths.h"
#include "LocScenePath.h"
#include "SubtitleRender.h"
#include "SubtitleGate.h"
#include <d3d9.h>
#include <string>
#include <vector>
#include <map>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cmath>

namespace MinimapHud
{
    // native 全域關卡控制器單例 → +0xA40 = Hitman actor（可為 0）。只有進入
    // 關卡（actor 存在）時才畫，主選單/載入畫面不出現。
    static const DWORD kLevelCtrlSingletonPtr = 0x0082083C;
    static const DWORD kActorOffset           = 0xA40;

    // [ctrl+0xA50] = menu/loading 控制器；其 +0x50 是一個 byte「暫停原因
    // bitfield」：bit 0x04 = ESC 暫停選單(IngameMenu)、
    // bit 0x08 = 原生地圖畫面。任一開啟 → 整個小地圖（含外框）不畫，避免疊在
    // 選單/地圖上。逐 bit 由 native sub_671110(arg0,caseId) 設/清。
    static const DWORD kMenuCtrlOffset  = 0xA50;
    static const DWORD kPauseByteOffset = 0x50;
    static const BYTE  kPauseMaskHide   = 0x0C;   // 0x04 暫停選單 | 0x08 原生地圖

    // sub_4E68E0：__thiscall(entity, float out[3])，沿 scene-graph 父鏈累加
    // local translation → 世界座標，out[0]=X out[1]=Y out[2]=Z。與
    // MinimapEngine.cpp 同一支，對 Hitman actor 可用（north-up 俯視丟 yaw，
    // 只用 X/Z，Y 拿來選樓層）。
    typedef void(__thiscall* EntityWorldPos_t)(void* entity, float* outVec3);
    static const EntityWorldPos_t EntityWorldPos = (EntityWorldPos_t)0x004E68E0;

    // ---- 顏色（ARGB）----
    static const DWORD kFillColor   = 0x66101418u;  // 半透明深底
    static const DWORD kBorderColor = 0xFFFFCC33u;  // 琥珀色邊框
    static const DWORD kFloorColor  = 0x3AB8C4CFu;  // 樓層填色（低 alpha 灰，重疊邊界會自然加深＝房間分界感）
    static const DWORD kWallColor   = 0xFFEAF4FBu;  // 牆線（接近白）
    static const DWORD kPlayerColor = 0xFFFF3030u;  // 47 位置標記（紅）
    static const float kBorderPx    = 2.0f;
    static const float kInsetPx     = 3.0f;         // 牆線繪製區再往內縮，不貼邊框
    static const float kPlayerHalf  = 2.0f;         // 47 標記半邊長

    // ---- 設定（[Hud]）----
    static bool    g_enabled = false;
    static float   g_posX    = 0.05f;   // 螢幕比例 0~1（左上角）
    static float   g_posY    = 0.60f;
    static int     g_sizeMul = 15;      // 邊長 = g_sizeMul * 10 px
    static float   g_zoom    = 4000.0f; // 方框內橫向可見的 world 單位數；0=整層 fit
    static bool    g_floorProbe = false; // [Debug] MinimapFloorProbe：每秒 dump 選樓層線索
    static HMODULE g_hModule = nullptr;

    // 樓層選擇定案狀態（UpdateFloorSelection 更新、Draw 讀）。
    static int       g_committedFloor = -1;
    static int       g_pendingFloor   = -1;
    static ULONGLONG g_pendingSinceMs = 0;
    static const DWORD kFloorHoldMs   = 700;   // 候選要穩定這麼久（ms）才切換

    struct ScreenVertex { float x, y, z, rhw; DWORD color; float u, v; };

    // ---- bin 資料（minimap_<關名>.bin，格式見 md\小地圖.md）----
    // v2＝只有 segs；v3＝segs 後多一段 fill（樓層填色三角形）。兩版都吃。
    // v3 尾端可再接「房名→樓層表」（magic "ROOM"）：舊 DLL 讀完 floors
    // 就停、忽略尾段，故不動 version 數字仍相容。
    struct Floor
    {
        std::string        name;
        float              yMin = 0.0f, yMax = 0.0f;
        float              xMin = 0.0f, xMax = 0.0f;
        float              zMin = 0.0f, zMax = 0.0f;
        std::vector<float> segs;   // 每 4 個 float = x1,z1,x2,z2（world，已丟 Y）
        std::vector<float> fill;   // 每 6 個 float = ax,az,bx,bz,cx,cz（world 三角形；v2 時為空）
    };
    static std::vector<Floor>         g_floors;
    static std::map<std::string, int> g_roomTable;   // live ZROOM 房名 → g_floors index
    static std::string                g_loadedTag;   // 已嘗試載入的關卡 tag（""＝還沒試過）

    // 甲板 Y-band（bin 選用尾段 "DYBD"，接在 "ROOM" 之後）：房名表
    // 查不到時的第三層選層線索，只給「實體垂直堆疊、Y 可靠」的關（目前僅 M08）。
    struct DeckYBand { int floorIdx; float yLo, yHi; };
    static std::vector<DeckYBand> g_deckYBands;

    // 純色矩形。compositor 已設好 FVF / alpha blend；stage0 未綁材質時
    // D3DTA_TEXTURE 取樣為不透明白，MODULATE 後 alpha 即 diffuse alpha。
    // 座標減 0.5 對齊 pixel 中心。
    static void FillRect(IDirect3DDevice9* device, float x, float y, float w, float h, DWORD color)
    {
        if (w <= 0.0f || h <= 0.0f) return;
        const float ox = x - 0.5f;
        const float oy = y - 0.5f;
        ScreenVertex v[4] =
        {
            { ox,     oy,     0.0f, 1.0f, color, 0.0f, 0.0f },
            { ox + w, oy,     0.0f, 1.0f, color, 0.0f, 0.0f },
            { ox,     oy + h, 0.0f, 1.0f, color, 0.0f, 0.0f },
            { ox + w, oy + h, 0.0f, 1.0f, color, 0.0f, 0.0f },
        };
        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(ScreenVertex));
    }

    static bool InLevel()
    {
        DWORD ctrl = *(DWORD*)kLevelCtrlSingletonPtr;
        if (!ctrl) return false;
        return *(DWORD*)(ctrl + kActorOffset) != 0;
    }

    static bool PlayerWorldPos(float out[3])
    {
        DWORD ctrl = *(DWORD*)kLevelCtrlSingletonPtr;
        if (!ctrl) return false;
        DWORD actor = *(DWORD*)(ctrl + kActorOffset);
        if (!actor) return false;
        out[0] = out[1] = out[2] = 0.0f;
        EntityWorldPos((void*)actor, out);
        return true;
    }

    // ---- bin 載入 ----

    // <gamedir>\bink32hook\minimap\minimap_<TAG>.bin。成功時填 g_floors 回 true；
    // 任何失敗（缺檔/magic 不符/版本不符/截斷）都清空 g_floors 回 false，
    // 呼叫端據此不繪製（md：缺檔一律靜默略過）。
    static bool LoadBin(const std::string& tag)
    {
        g_floors.clear();
        g_roomTable.clear();
        g_deckYBands.clear();

        char dir[MAX_PATH];
        Paths::GetDataDir(g_hModule, dir, sizeof(dir));
        char path[MAX_PATH];
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%sminimap\\minimap_%s.bin", dir, tag.c_str());

        HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            Log::Write("[MinimapHud] 關卡 %s 無 %s，不繪製小地圖", tag.c_str(), path);
            return false;
        }

        DWORD size = GetFileSize(h, nullptr);
        std::vector<unsigned char> buf;
        bool ok = (size != INVALID_FILE_SIZE && size >= 12 && size <= 64u * 1024u * 1024u);
        if (ok)
        {
            buf.resize(size);
            DWORD got = 0;
            ok = (ReadFile(h, &buf[0], size, &got, nullptr) != 0) && got == size;
        }
        CloseHandle(h);
        if (!ok)
        {
            Log::Write("[MinimapHud] %s 讀取失敗（size=%lu）", path, size);
            return false;
        }

        const unsigned char* p   = &buf[0];
        const unsigned char* end = p + buf.size();

        if (memcmp(p, "MMAP", 4) != 0)
        {
            Log::Write("[MinimapHud] %s magic 不符（非 MMAP）", path);
            return false;
        }
        p += 4;

        auto rdU32 = [&](unsigned int& v) -> bool
        {
            if (p + 4 > end) return false;
            memcpy(&v, p, 4); p += 4; return true;   // x86 LE，直接讀
        };
        auto rdF32 = [&](float& v) -> bool
        {
            if (p + 4 > end) return false;
            memcpy(&v, p, 4); p += 4; return true;
        };

        unsigned int version = 0, floorCount = 0;
        if (!rdU32(version) || !rdU32(floorCount))
        {
            Log::Write("[MinimapHud] %s 標頭截斷", path);
            return false;
        }
        if (version != 2 && version != 3)
        {
            Log::Write("[MinimapHud] %s version=%u（預期 2 或 3），放棄", path, version);
            return false;
        }
        if (floorCount == 0 || floorCount > 64)
        {
            Log::Write("[MinimapHud] %s floorCount=%u 不合理，放棄", path, floorCount);
            return false;
        }

        std::vector<Floor> floors;
        floors.reserve(floorCount);
        for (unsigned int i = 0; i < floorCount; i++)
        {
            if (p + 1 > end) { Log::Write("[MinimapHud] %s 樓層 %u 名稱截斷", path, i); return false; }
            unsigned int nameLen = *p++;
            if (p + nameLen > end) { Log::Write("[MinimapHud] %s 樓層 %u 名稱截斷", path, i); return false; }

            Floor f;
            f.name.assign((const char*)p, nameLen);
            p += nameLen;

            if (!rdF32(f.yMin) || !rdF32(f.yMax) ||
                !rdF32(f.xMin) || !rdF32(f.xMax) ||
                !rdF32(f.zMin) || !rdF32(f.zMax))
            {
                Log::Write("[MinimapHud] %s 樓層 %u bbox 截斷", path, i);
                return false;
            }

            unsigned int segCount = 0;
            if (!rdU32(segCount)) { Log::Write("[MinimapHud] %s 樓層 %u segCount 截斷", path, i); return false; }
            if (segCount > 4u * 1024u * 1024u ||
                (size_t)(end - p) < (size_t)segCount * 16u)
            {
                Log::Write("[MinimapHud] %s 樓層 %u 線段資料截斷（segCount=%u）", path, i, segCount);
                return false;
            }

            f.segs.resize((size_t)segCount * 4u);
            if (segCount > 0) memcpy(&f.segs[0], p, (size_t)segCount * 16u);
            p += (size_t)segCount * 16u;

            if (version >= 3)
            {
                unsigned int triCount = 0;
                if (!rdU32(triCount)) { Log::Write("[MinimapHud] %s 樓層 %u triCount 截斷", path, i); return false; }
                if (triCount > 8u * 1024u * 1024u ||
                    (size_t)(end - p) < (size_t)triCount * 24u)
                {
                    Log::Write("[MinimapHud] %s 樓層 %u 填色資料截斷（triCount=%u）", path, i, triCount);
                    return false;
                }
                f.fill.resize((size_t)triCount * 6u);
                if (triCount > 0) memcpy(&f.fill[0], p, (size_t)triCount * 24u);
                p += (size_t)triCount * 24u;
            }

            floors.push_back(std::move(f));
        }

        g_floors.swap(floors);

        // ---- 尾段：房名→樓層表（magic "ROOM"；選用，缺就退回純幾何選層）----
        //   char[4] "ROOM"; u32 roomCount;
        //   per room: u8 nameLen; char name[nameLen]; u8 floorIdx
        if (p + 8 <= end && memcmp(p, "ROOM", 4) == 0)
        {
            p += 4;
            unsigned int roomCount = 0;
            if (rdU32(roomCount) && roomCount <= 100000u)
            {
                for (unsigned int i = 0; i < roomCount; i++)
                {
                    if (p + 1 > end) break;
                    unsigned int nl = *p++;
                    if (p + nl + 1 > end) break;
                    std::string rn((const char*)p, nl);
                    p += nl;
                    unsigned int fidx = *p++;
                    if (fidx < g_floors.size() && !rn.empty())
                        g_roomTable[rn] = (int)fidx;
                }
            }
        }

        // ---- 再一個選用尾段：甲板 Y-band（magic "DYBD"；選用，缺就
        // 不影響行為——只有 M08 目前有此段）----
        //   char[4] "DYBD"; u32 bandCount; per: u8 floorIdx; f32 yLo; f32 yHi
        if (p + 8 <= end && memcmp(p, "DYBD", 4) == 0)
        {
            p += 4;
            unsigned int bandCount = 0;
            if (rdU32(bandCount) && bandCount <= 64u)
            {
                for (unsigned int i = 0; i < bandCount; i++)
                {
                    if (p + 9 > end) break;
                    unsigned int fidx = *p++;
                    float yLo, yHi;
                    memcpy(&yLo, p, 4); p += 4;
                    memcpy(&yHi, p, 4); p += 4;
                    if (fidx < g_floors.size())
                    {
                        DeckYBand b = { (int)fidx, yLo, yHi };
                        g_deckYBands.push_back(b);
                    }
                }
            }
        }

        Log::Write("[MinimapHud] 載入 %s：v%u %u 樓層，房名表 %u 條，甲板 Y-band %u 條",
                   path, version, floorCount, (unsigned int)g_roomTable.size(),
                   (unsigned int)g_deckYBands.size());
        return true;
    }

    // 每幀（Tick）呼叫：關卡 tag 變了就重載 bin。tag 取自 ZipPathTrace 側錄的
    // 場景 zip 路徑段（LocScenePath::CurrentMissionTag()，例 "M01"），場景切換
    // 瞬間會短暫回空字串 → 保留現況、不清資料。
    static void RefreshLevel()
    {
        std::string tag = LocScenePath::CurrentMissionTag();
        if (tag.empty()) return;

        for (size_t i = 0; i < tag.size(); i++)
            tag[i] = (char)toupper((unsigned char)tag[i]);

        if (tag == g_loadedTag) return;   // 同一關，什麼都不做

        g_loadedTag = tag;                // 先記下（成功或失敗都不再每幀重試）
        LoadBin(tag);                     // 失敗時 g_floors 已被清空
        g_committedFloor = g_pendingFloor = -1;   // 換關重選樓層
    }

    // XZ 命中的樓層裡挑 Y 最接近的；Y 距離相近時（多層都把 47 包住、或薄樓層
    // 差幾單位，例 M01 站 Hangar 時 Outside/-4010..19 的 yd=0 而 Hangar/
    // -4010..-4000 只要腳略高於 -4000 yd 就 >0）改挑 XZ bbox 面積較小＝較
    // specific 的那層，避免整關大底圖 Outside 恆勝。kYdTie 取 100：M01 同 footprint
    // 的相鄰樓層 Y 間距都 ≥150（1F 60..250 / 2F 460），不會誤併。都沒 XZ 命中就
    // 挑「Y 距離＋XZ 中心距離」最小的（M12 單層 / M06 建物散落 / 過場站樓層外的
    // 退路）。g_floors 非空時必回有效 index。
    static const float kYdTie = 100.0f;   // Y 距離差在此以內視為相近，改比 bbox 面積

    static int SelectFloor(float wx, float wy, float wz)
    {
        int   best     = -1;
        float bestYd   = 0.0f;
        float bestArea = 0.0f;
        for (int i = 0; i < (int)g_floors.size(); i++)
        {
            const Floor& f = g_floors[i];
            if (wx < f.xMin || wx > f.xMax || wz < f.zMin || wz > f.zMax) continue;
            float yd = (wy < f.yMin) ? (f.yMin - wy)
                     : (wy > f.yMax) ? (wy - f.yMax) : 0.0f;
            float area = (f.xMax - f.xMin) * (f.zMax - f.zMin);
            bool better = (best < 0)
                       || (yd < bestYd - kYdTie)
                       || (yd <= bestYd + kYdTie && area < bestArea);
            if (better) { best = i; bestYd = yd; bestArea = area; }
        }
        if (best >= 0) return best;

        float bestScore = 0.0f;
        for (int i = 0; i < (int)g_floors.size(); i++)
        {
            const Floor& f = g_floors[i];
            float yd = (wy < f.yMin) ? (f.yMin - wy)
                     : (wy > f.yMax) ? (wy - f.yMax) : 0.0f;
            float cx = (f.xMin + f.xMax) * 0.5f;
            float cz = (f.zMin + f.zMax) * 0.5f;
            float dx = wx - cx, dz = wz - cz;
            float score = yd + sqrtf(dx * dx + dz * dz);
            if (best < 0 || score < bestScore) { best = i; bestScore = score; }
        }
        return best;
    }

    // ---- 選樓層定案側錄（[Debug] MinimapFloorProbe）----
    //
    // actor（[[0x0082083C]+0xA40]）上的 zone 欄位：
    //   +0x12D0 = 當前 zone 體積節點指標（玩家位移 ≥50 才更新，逐房間換值）
    //   +0x12D4 = m_eRoomZone  （ESecurityZone u32，治安等級，非樓層）
    //   +0x12D8 = m_eCustomZone （ESecurityZone u32；非 0 時蓋過 m_eRoomZone，同非樓層）
    // zoneNode 的 name 不在自己的 +0x6C/+0x50，DumpZoneNode() 逐 offset 挖原始
    // 佈局才找到（見下）。actorGraphChain 是角色 setup 樹，對不到 MAP 樓層。
    //
    // 每秒印一次：玩家世界 XYZ、SelectFloor() 選層、每層 bbox 命中、zone enum、
    // zoneNode 指標＋（換值時）原始 dump、m_currentMapGroup 名。純側錄不影響繪製。
    static const DWORD kActorZoneNode   = 0x12D0;
    static const DWORD kActorRoomZone   = 0x12D4;
    static const DWORD kActorCustomZone = 0x12D8;
    static const DWORD kLocParentOffset = 0x50;
    static const DWORD kLocNameOffset   = 0x6C;

    // ---- SEH 安全記憶體探測（取代 IsBadReadPtr / IsBadStringPtrA）----
    //
    // IsBadReadPtr 系列是微軟明文「不要用」的 API：它用內部 SEH 吃掉探測時的
    // 例外，若探到「堆疊 guard page」會把 guard page 一起吃掉 → OS 之後不再
    // 自動長堆疊 → 下一個吃堆疊的引擎呼叫（例如 M03 的 enginedatabase 載入）
    // 就踩爛 SEH 鏈 / 呼叫者的 ebp，整支 process 直接 fast-fail（無 dump）。
    //
    // 這裡的 filter 只吞「這塊記憶體讀不到」這一類（存取違規 / 分頁錯誤），
    // guard page（0x80000001）、堆疊溢位（0xC00000FD）等一律 CONTINUE_SEARCH
    // 往上丟——尤其 guard page 一定要讓 OS 收走、重新武裝，不能在這裡吃掉。
    static int SafeProbeFilter(unsigned long code)
    {
        if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
            return EXCEPTION_EXECUTE_HANDLER;
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // [p, p+n) 全可讀回 true（摸頭、尾、以及每個跨頁點）。POD-only、無 C++ 解構。
    static bool CanRead(const void* p, size_t n)
    {
        if (!p || n == 0) return false;
        __try
        {
            const volatile BYTE* b = (const volatile BYTE*)p;
            volatile BYTE sink = b[0];
            sink = (BYTE)(sink ^ b[n - 1]);
            for (size_t off = 0x1000; off < n; off += 0x1000)
                sink = (BYTE)(sink ^ b[off]);
            (void)sink;
            return true;
        }
        __except (SafeProbeFilter(GetExceptionCode()))
        {
            return false;
        }
    }

    // 把 [src, src+n) 複製到 dst；讀不到回 false（dst 內容未定義，呼叫端別用）。
    static bool SafeRead(void* dst, const void* src, size_t n)
    {
        __try
        {
            memcpy(dst, src, n);
            return true;
        }
        __except (SafeProbeFilter(GetExceptionCode()))
        {
            return false;
        }
    }

    // 取代 IsBadStringPtrA + 複製：逐字元把 s 安全抄進 buf（最多 bufSize-1 字元
    // + NUL），遇 NUL 或不可讀就停。回 true ＝ 至少抄到 1 個字元。
    static bool SafeReadStr(char* buf, size_t bufSize, const char* s)
    {
        if (!buf || bufSize == 0) return false;
        buf[0] = '\0';
        if (!s) return false;
        __try
        {
            const volatile char* p = (const volatile char*)s;
            size_t i = 0;
            for (; i + 1 < bufSize; i++)
            {
                char c = p[i];
                if (c == '\0') break;
                buf[i] = c;
            }
            buf[i] = '\0';
        }
        __except (SafeProbeFilter(GetExceptionCode()))
        {
            buf[0] = '\0';
            return false;
        }
        return buf[0] != '\0';
    }

    // node 顯示名：只讀 +0x6C entityName（char*，guarded）。actor scene-graph
    // 父鏈這條給得出乾淨名字（"Hero"／zip 名）；zone 節點這條讀不到（印
    // "<?>"）。已移除舊版 vtable[0x4C] GetName fallback：對非 locator 節點做
    // 不安全 indirect call、又只回 "<?>"，沒有意義。bufSize 至少 16。
    static void ReadNodeName(void* node, char* buf, size_t bufSize)
    {
        (void)lstrcpynA(buf, "<?>", (int)bufSize);
        if (!node || bufSize < 2) return;

        const char* nm = nullptr;
        if (!SafeRead(&nm, (const BYTE*)node + kLocNameOffset, sizeof(nm)) || !nm)
            return;
        char tmp[64];
        if (SafeReadStr(tmp, sizeof(tmp), nm) && tmp[0])
            (void)lstrcpynA(buf, tmp, (int)bufSize);
    }

    // 玩家當前 ZROOM 房名（選層查表用）。actor+0x12D0 ＝ zoneNode（ZROOM
    // 實例）；其名字被 ZTreeGroup 層 ctor 從 +0x6C 搬走當子鏈，真正的名字在
    // 伴生 info block ＝ [zoneNode+0x04] 的 +0x6C（char*），每次換房間都給
    // 乾淨房名（如 "M03_Lobby_101"）。單層 indirection、不呼 vtable；載入中
    // scene graph 會重建 → 每一跳都經
    // SafeRead / SafeReadStr（各自 SEH 包住、guard page 往上丟）。
    // 讀到才回 true 並填 buf（bufSize 建議 ≥64）。
    static bool ReadZoneRoomName(DWORD actor, char* buf, size_t bufSize)
    {
        if (bufSize) buf[0] = '\0';
        if (!actor || bufSize < 2) return false;

        void* zoneNode = nullptr;
        void* info     = nullptr;
        const char* nm = nullptr;
        if (!SafeRead(&zoneNode, (const void*)(actor + kActorZoneNode), sizeof(zoneNode)) || !zoneNode)
            return false;
        if (!SafeRead(&info, (const BYTE*)zoneNode + 0x04, sizeof(info)) || !info)
            return false;
        if (!SafeRead(&nm, (const BYTE*)info + kLocNameOffset, sizeof(nm)) || !nm)
            return false;
        return SafeReadStr(buf, bufSize, nm) && buf[0];
    }

    // 從 startNode 沿 +0x50 parent 往上，最多 kMaxDepth 層名字接成一行印出。
    static void LogNodeChain(const char* tag, void* startNode)
    {
        const int kMaxDepth = 12;
        char line[512];
        (void)lstrcpynA(line, tag, sizeof(line));
        int used = lstrlenA(line);

        void* node = startNode;
        for (int depth = 0; depth < kMaxDepth && node; depth++)
        {
            char  nm[64];
            void* parent = nullptr;
            // 載入中 scene graph 會重建，node/parent 可能指向剛釋放的記憶體 →
            // 每一跳都經 CanRead / SafeRead（各自 SEH 包住、guard page 往上丟）。
            int   ok = CanRead(node, 0x70) ? 1 : 0;
            if (ok)
            {
                ReadNodeName(node, nm, sizeof(nm));
                SafeRead(&parent, (const BYTE*)node + kLocParentOffset, sizeof(parent));
            }

            if (!ok) { lstrcatA(line, " <讀取例外>"); break; }

            char frag[96];
            _snprintf_s(frag, sizeof(frag), _TRUNCATE, "%s%p:\"%s\"",
                        depth ? " > " : " ", node, nm);
            if (used + (int)lstrlenA(frag) >= (int)sizeof(line) - 1) break;
            lstrcatA(line, frag);
            used += lstrlenA(frag);

            if (parent == node) break;
            node = parent;
        }
        Log::Write("%s", line);
    }

    // m_currentMapGroup 名字：[[0x0082083C]+0xA54]（CMapIconDraw）+0x2AC
    // （SMapGroup*）+0x8（char* m_name）＝ "M03__MAP_1stFloor" 這種字串。只在
    // 玩家開原生地圖畫面時更新，遊玩中是死值，不用於即時選層，僅供
    // FloorProbeTick 側錄比對。全程走 SafeRead / CanRead / SafeReadStr（各自
    // SEH 包住、guard page 往上丟）。
    static void ReadMapGroupName(DWORD ctrl, char* buf, size_t bufSize)
    {
        (void)lstrcpynA(buf, "<?>", (int)bufSize);
        if (bufSize < 2) return;

        DWORD mapIcon = 0, grp = 0;
        const char* nm = nullptr;
        if (!SafeRead(&mapIcon, (const void*)(ctrl + 0xA54), sizeof(mapIcon)) ||
            !mapIcon || !CanRead((void*)mapIcon, 0x2B4))
        {
            (void)lstrcpynA(buf, "<noicon>", (int)bufSize); return;
        }
        if (!SafeRead(&grp, (const void*)(mapIcon + 0x2AC), sizeof(grp)) ||
            !grp || !CanRead((void*)grp, 0x10))
        {
            (void)lstrcpynA(buf, "<nogrp>", (int)bufSize); return;
        }
        if (!SafeRead(&nm, (const void*)(grp + 0x08), sizeof(nm)) || !nm ||
            !SafeReadStr(buf, bufSize, nm) || !buf[0])
        {
            (void)lstrcpynA(buf, "<noname>", (int)bufSize);
        }
    }

    // zoneNode（[actor+0x12D0]）原始佈局 dump——name/parent 不在預期的 +0x6C/
    // +0x50，逐 offset 挖：前 0x90 bytes hex ＋ 每個指向可讀 ASCII 的 slot ＋
    // vtable 前 4 格（識別 class）。同一個 zoneNode 只 dump 一次（換房間才再 dump），
    // 避免洗版。POD-only + SEH。
    static void DumpZoneNode(void* node)
    {
        static void* s_lastDumped = (void*)1;   // sentinel：跟 null/任何真指標都不等
        if (node == s_lastDumped) return;
        s_lastDumped = node;
        if (!node) return;

        __try
        {
            if (!CanRead(node, 0x90))
            {
                Log::Write("[MinimapFloorProbe]   zn %p 不可讀", node);
                return;
            }
            const DWORD* d = (const DWORD*)node;   // CanRead 已保證 [node, node+0x90) 可讀
            for (int r = 0; r < 9; r++)
                Log::Write("[MinimapFloorProbe]   zn %p +0x%02X: %08X %08X %08X %08X",
                           node, r * 16, d[r * 4], d[r * 4 + 1], d[r * 4 + 2], d[r * 4 + 3]);

            for (int off = 0; off < 0x90; off += 4)
            {
                const char* s = *(const char* const*)((const BYTE*)node + off);
                char tmp[64];
                if (!s || !SafeReadStr(tmp, sizeof(tmp), s)) continue;
                int i = 0, ok = 1;
                for (; i < 40 && tmp[i]; i++)
                {
                    unsigned char c = (unsigned char)tmp[i];
                    if (c < 0x20 || c >= 0x7F) { ok = 0; break; }
                }
                if (ok && i >= 3)
                    Log::Write("[MinimapFloorProbe]   zn +0x%02X -> \"%s\"", off, tmp);
            }

            const DWORD* vt = *(const DWORD* const*)node;
            DWORD vtn[4];
            if (vt && SafeRead(vtn, vt, sizeof(vtn)))
                Log::Write("[MinimapFloorProbe]   zn vtable=%p [%08X %08X %08X %08X]",
                           vt, vtn[0], vtn[1], vtn[2], vtn[3]);

            // zoneNode 的 class ＝ ZROOM（vtable 0x0076E30C，父類 ZTreeGroup）。
            // 真正的名字被 ZTreeGroup 層 ctor（sub_4FD100）從 +0x6C 搬走改當
            // 子鏈用；名字實際在「伴生 info block」＝ [node+0x04]，其 +0x6C ＝
            // char* 名字（sub_4E7EF0：[node+4]=info、[info+0x6C]=strdup(name)；
            // sub_4E8550 印 geom 名亦讀 *(*(this+4)+0x6C)）。這裡把 info block
            // 也一併 dump 出來。
            void* info = *(void* const*)((const BYTE*)node + 0x04);
            if (info && CanRead(info, 0x80))
            {
                const DWORD* id = (const DWORD*)info;   // CanRead 已保證可讀
                for (int r = 0; r < 8; r++)
                    Log::Write("[MinimapFloorProbe]   zn.info %p +0x%02X: %08X %08X %08X %08X",
                               info, r * 16, id[r * 4], id[r * 4 + 1], id[r * 4 + 2], id[r * 4 + 3]);
                for (int off = 0; off < 0x80; off += 4)
                {
                    const char* s = *(const char* const*)((const BYTE*)info + off);
                    char tmp[64];
                    if (!s || !SafeReadStr(tmp, sizeof(tmp), s)) continue;
                    int i = 0, ok = 1;
                    for (; i < 48 && tmp[i]; i++)
                    {
                        unsigned char c = (unsigned char)tmp[i];
                        if (c < 0x20 || c >= 0x7F) { ok = 0; break; }
                    }
                    if (ok && i >= 3)
                        Log::Write("[MinimapFloorProbe]   zn.info +0x%02X -> \"%s\"", off, tmp);
                }
            }
            else
            {
                Log::Write("[MinimapFloorProbe]   zn.info [node+4]=%p 不可讀", info);
            }
        }
        __except (SafeProbeFilter(GetExceptionCode()))
        {
            Log::Write("[MinimapFloorProbe]   zn dump 例外 node=%p", node);
        }
    }

    // actor scene-graph 父鏈（＝角色 setup 樹 "Hero > herosetup.zip > ..."，
    // 對不到 MAP 樓層，僅供 probe log 完整性保留）。POD-only + SEH。
    static void DumpActorGraphChain(DWORD actor)
    {
        void* actorInner  = nullptr;
        void* actorParent = nullptr;
        if (!SafeRead(&actorInner, (const void*)(actor + 4), sizeof(actorInner)) || !actorInner)
            return;
        SafeRead(&actorParent, (const BYTE*)actorInner + kLocParentOffset, sizeof(actorParent));
        if (actorParent)
            LogNodeChain("[MinimapFloorProbe]   actorGraphChain:", actorParent);
    }

    // ---- 樓層選擇：房名表為主、幾何 XZ 退路、遲滯防抖 ----
    //
    // 地圖模型是「地圖畫面用的美術分解排版」，樓層 Y 不是物理堆疊，不能單純
    // 拿世界座標比 bbox Y；m_currentMapGroup 也只在開原生地圖時更新，遊玩中
    // 是死值。定案：
    //   1. 主判＝玩家所在 ZROOM 房名（ReadZoneRoomName）查 g_roomTable（離線
    //      由 scene.json 的樓層 ZROOM 分組建、隨 bin 尾段附）。
    //   2. 房名讀不到 / 表裡沒有（含抽取器故意不寫的跨層樓梯間）→ 退回幾何，
    //      且退路只比 XZ：剛好一層命中就採信；多層命中時維持現況樓層（現況也
    //      命中的話），否則取 Y 最近——不比 bbox 面積 tiebreak（樓層 Y 範圍互
    //      相涵蓋時會反向選錯）。
    //   3. 任何切換都要候選穩定 kFloorHoldMs 才 commit，濾單次跳動。
    //   4. 玩家世界座標 (0,0,0) ＝ actor 未就緒 → 保留現況。
    // Draw() 只讀 g_committedFloor。
    // MapGroupLeaf / FloorNameEndsWith / MatchFloorByMapGroup / FloorAmbiguousAtY
    // 已不參與選層，僅 FloorProbeTick 留著做對照側錄。

    // "M03/Map/1stFloor" → "1stFloor"（最後一個 '/' 或 '\\' 之後）；空/純分隔回 nullptr。
    static const char* MapGroupLeaf(const char* s)
    {
        if (!s || !s[0]) return nullptr;
        const char* leaf = s;
        for (const char* p = s; *p; p++)
            if (*p == '/' || *p == '\\') leaf = p + 1;
        return leaf[0] ? leaf : nullptr;
    }

    // Floor.name（"M03__MAP_1stFloor"）是否以 token（"1stFloor"）結尾，不分大小寫。
    static bool FloorNameEndsWith(const std::string& name, const char* token)
    {
        size_t tn = token ? strlen(token) : 0;
        if (tn == 0 || name.size() < tn) return false;
        return _strnicmp(name.c_str() + (name.size() - tn), token, tn) == 0;
    }

    // mapGroup 尾段對到哪個 g_floors index；"<nogrp>" 之類或對不到回 -1。
    static int MatchFloorByMapGroup(const char* mapGrp)
    {
        const char* leaf = MapGroupLeaf(mapGrp);
        if (!leaf || leaf[0] == '<') return -1;
        for (int i = 0; i < (int)g_floors.size(); i++)
            if (FloorNameEndsWith(g_floors[i].name, leaf)) return i;
        return -1;
    }

    // XZ 命中且 Y 範圍包住 wy 的樓層數 > 1 ＝幾何分不出。
    static bool FloorAmbiguousAtY(float wx, float wy, float wz)
    {
        int n = 0;
        for (size_t i = 0; i < g_floors.size(); i++)
        {
            const Floor& f = g_floors[i];
            if (wx < f.xMin || wx > f.xMax || wz < f.zMin || wz > f.zMax) continue;
            if (wy >= f.yMin && wy <= f.yMax) n++;
        }
        return n > 1;
    }

    // live ZROOM 房名 → g_roomTable index。先原名精確查；miss 再依序剝掉結尾的
    // "#<數字>"（runtime 去重後綴）與 "_<選用單一字母><數字>"（房號實例；地圖
    // 幾何節點的房號未必等於 live 房號）後重查。抽取器已把「去房號」
    // 形式在單一樓層無歧義時一併寫進表，故這裡剝完直接 find 即可。
    static int LookupRoomTable(const char* name)
    {
        if (!name || !name[0]) return -1;
        std::map<std::string, int>::const_iterator it = g_roomTable.find(name);
        if (it != g_roomTable.end()) return it->second;

        std::string s(name);

        // 結尾 "#<digits>"
        size_t h = s.find_last_of('#');
        if (h != std::string::npos && h + 1 < s.size())
        {
            bool alldig = true;
            for (size_t i = h + 1; i < s.size(); i++)
                if (!isdigit((unsigned char)s[i])) { alldig = false; break; }
            if (alldig)
            {
                s.erase(h);
                it = g_roomTable.find(s);
                if (it != g_roomTable.end()) return it->second;
            }
        }

        // 結尾 "_<選用單一字母><digits>"（B101 的 B、101、202…）
        size_t u = s.find_last_of('_');
        if (u != std::string::npos && u + 1 < s.size())
        {
            size_t i = u + 1;
            if (isalpha((unsigned char)s[i])) i++;
            bool ok = (i < s.size());
            for (; ok && i < s.size(); i++)
                if (!isdigit((unsigned char)s[i])) ok = false;
            if (ok)
            {
                std::string s2 = s.substr(0, u);
                it = g_roomTable.find(s2);
                if (it != g_roomTable.end()) return it->second;
            }
        }
        return -1;
    }

    // 玩家房名查 g_roomTable → 樓層 index。表為空 / 讀不到房名 / 表裡沒有此房
    // （含抽取器故意不寫的跨層樓梯間）皆回 -1，由呼叫端退回幾何。
    static int SelectFloorByRoom(DWORD actor)
    {
        if (g_roomTable.empty() || !actor) return -1;
        char room[80];
        if (!ReadZoneRoomName(actor, room, sizeof(room))) return -1;
        return LookupRoomTable(room);
    }

    // 房名表查不到時的退路：以 XZ 命中為準（不比 Y；地圖模型 Y 非物理堆疊）。
    //   剛好一層 XZ 命中 → 那層
    //   多層命中 → 挑 footprint（XZ bbox 面積）最小＝最 specific 的那層；現況樓層
    //             也在最小之列（±5%）就維持它，濾邊界抖動
    //   完全沒 XZ 命中 → 沿用 SelectFloor 無命中分支（Y＋中心距離最小）
    static int SelectFloorFallback(float wx, float wy, float wz)
    {
        int   hit = -1, hitCount = 0;
        int   tightest = -1;               // XZ footprint 最小的命中層（'small' 是 rpcndr.h 巨集，勿用）
        float tightestArea = 0.0f;
        for (int i = 0; i < (int)g_floors.size(); i++)
        {
            const Floor& f = g_floors[i];
            if (wx < f.xMin || wx > f.xMax || wz < f.zMin || wz > f.zMax) continue;
            hitCount++;
            hit = i;
            float area = (f.xMax - f.xMin) * (f.zMax - f.zMin);
            if (tightest < 0 || area < tightestArea) { tightest = i; tightestArea = area; }
        }
        if (hitCount == 1) return hit;
        if (hitCount > 1)
        {
            // 舊版「多層命中就維持 g_committedFloor」在「大 bbox 幾何上包住小
            // bbox」時是個永遠鬆不開的閂——例如 M04 的 MainBld_1stFloor bbox 完整
            // 涵蓋 MedWing_1stFloor，會害玩家整段醫療區 committed 卡在
            // MainBld_1stFloor、樓層 [4]/[5] 永遠選不到。改挑最小 footprint；同
            // footprint 堆疊樓層（M03 2F/3F、M10 7F/8F）面積相等 → 落在下面 5%
            // 容差內維持現況樓層，行為與舊版一致。
            if (g_committedFloor >= 0 && g_committedFloor < (int)g_floors.size())
            {
                const Floor& c = g_floors[g_committedFloor];
                if (wx >= c.xMin && wx <= c.xMax && wz >= c.zMin && wz <= c.zMax)
                {
                    float ca = (c.xMax - c.xMin) * (c.zMax - c.zMin);
                    if (ca <= tightestArea * 1.05f) return g_committedFloor;
                }
            }
            return tightest;
        }
        return SelectFloor(wx, wy, wz);
    }

    // 甲板 Y-band 查表：房名表查不到時的第二層線索，只給「實體
    // 垂直堆疊、Y 可靠」的關（g_deckYBands 為空的關這條完全不參與，回 -1）。
    // 命中區間就用；都沒命中挑中點最近者（同 SelectFloor 無 XZ 命中分支的精神，
    // 濾掉甲板間 ~300 單位空隙時的邊界誤差）。
    static int SelectFloorByDeckY(float wy)
    {
        if (g_deckYBands.empty()) return -1;
        for (size_t i = 0; i < g_deckYBands.size(); i++)
        {
            const DeckYBand& b = g_deckYBands[i];
            if (wy >= b.yLo && wy < b.yHi) return b.floorIdx;
        }
        int best = -1;
        float bestD = 0.0f;
        for (size_t i = 0; i < g_deckYBands.size(); i++)
        {
            const DeckYBand& b = g_deckYBands[i];
            float mid = (b.yLo <= -1e8f) ? b.yHi : (b.yHi >= 1e8f) ? b.yLo : (b.yLo + b.yHi) * 0.5f;
            float d = fabsf(wy - mid);
            if (best < 0 || d < bestD) { best = b.floorIdx; bestD = d; }
        }
        return best;
    }

    // 每幀由 Tick() 呼叫一次，更新 g_committedFloor。
    static void UpdateFloorSelection()
    {
        if (g_floors.empty()) { g_committedFloor = g_pendingFloor = -1; return; }

        float w[3];
        if (!PlayerWorldPos(w)) return;
        if (w[0] == 0.0f && w[1] == 0.0f && w[2] == 0.0f) return;   // actor 未就緒，保留現況

        DWORD ctrl  = *(DWORD*)kLevelCtrlSingletonPtr;
        DWORD actor = ctrl ? *(DWORD*)(ctrl + kActorOffset) : 0;

        int candidate = SelectFloorByRoom(actor);          // 主：房名查表
        if (candidate < 0)
            candidate = SelectFloorByDeckY(w[1]);           // 次：甲板 Y-band（僅 M08 有資料）
        if (candidate < 0)
            candidate = SelectFloorFallback(w[0], w[1], w[2]);   // 輔：幾何 XZ 退路
        if (candidate < 0) return;

        if (g_committedFloor < 0) { g_committedFloor = g_pendingFloor = candidate; return; }
        if (candidate == g_committedFloor) { g_pendingFloor = candidate; return; }

        ULONGLONG now = GetTickCount64();
        if (candidate != g_pendingFloor) { g_pendingFloor = candidate; g_pendingSinceMs = now; return; }
        if (now - g_pendingSinceMs >= kFloorHoldMs) g_committedFloor = candidate;
    }

    static void FloorProbeTick()
    {
        static ULONGLONG s_lastMs = 0;
        ULONGLONG now = GetTickCount64();
        if (now - s_lastMs < 1000) return;
        s_lastMs = now;

        DWORD ctrl = *(DWORD*)kLevelCtrlSingletonPtr;
        if (!ctrl) return;
        DWORD actor = *(DWORD*)(ctrl + kActorOffset);
        if (!actor) return;

        float w[3] = { 0.0f, 0.0f, 0.0f };
        if (!PlayerWorldPos(w)) return;

        // actor 指標在「關卡載入途中」就會非 0，但此時 actor / scene graph /
        // zone node 都還是半成品——對它們做指標探測會在引擎最脆弱的時候踩到
        // guard page 之類，破壞引擎自身的堆疊/SEH 狀態，曾導致 M03 載入時閃退。
        // 等玩家世界座標有值、且 bin 已載入（＝關卡真的可玩）再開始側錄。
        if (w[0] == 0.0f && w[1] == 0.0f && w[2] == 0.0f) return;
        if (g_floors.empty()) return;

        int geomFi = g_floors.empty() ? -1 : SelectFloor(w[0], w[1], w[2]);
        const char* geomName = (geomFi >= 0 && geomFi < (int)g_floors.size())
                             ? g_floors[geomFi].name.c_str() : "<none>";
        const char* cmtName = (g_committedFloor >= 0 && g_committedFloor < (int)g_floors.size())
                            ? g_floors[g_committedFloor].name.c_str() : "<none>";

        DWORD roomZone = 0, customZone = 0;
        void* zoneNode = nullptr;
        SafeRead(&roomZone,   (const void*)(actor + kActorRoomZone),   sizeof(roomZone));
        SafeRead(&customZone, (const void*)(actor + kActorCustomZone), sizeof(customZone));
        SafeRead(&zoneNode,   (const void*)(actor + kActorZoneNode),   sizeof(zoneNode));

        char mapGrp[80];
        ReadMapGroupName(ctrl, mapGrp, sizeof(mapGrp));
        const char* mgLeaf = MapGroupLeaf(mapGrp);
        bool ambig = !g_floors.empty() && FloorAmbiguousAtY(w[0], w[1], w[2]);
        int  mgFi  = MatchFloorByMapGroup(mapGrp);

        // 主判＝房名查表。room="讀到的 ZROOM 名"，roomTbl->[樓層]。
        char room[80];
        if (!ReadZoneRoomName(actor, room, sizeof(room)))
            (void)lstrcpynA(room, "<none>", sizeof(room));
        int roomFi = SelectFloorByRoom(actor);
        const char* roomFloorName = (roomFi >= 0 && roomFi < (int)g_floors.size())
                                  ? g_floors[roomFi].name.c_str() : "<none>";

        // 甲板 Y-band 側錄（g_deckYBands 空的關恆 -1，不影響其他關）。
        int deckFi = SelectFloorByDeckY(w[1]);
        const char* deckFloorName = (deckFi >= 0 && deckFi < (int)g_floors.size())
                                  ? g_floors[deckFi].name.c_str() : "<none>";

        Log::Write("[MinimapFloorProbe] tag=%s pos=(%.1f,%.1f,%.1f) room=\"%s\" "
                   "roomTbl->[%d]\"%s\" deckY->[%d]\"%s\" geom->[%d]\"%s\" committed->[%d]\"%s\" "
                   "ambig=%d mgLeaf=\"%s\" mgFi=%d "
                   "m_eRoomZone=%u m_eCustomZone=%u zoneNode=%p mapGroup=\"%s\"",
                   g_loadedTag.c_str(), w[0], w[1], w[2], room,
                   roomFi, roomFloorName, deckFi, deckFloorName, geomFi, geomName,
                   g_committedFloor, cmtName, ambig ? 1 : 0, mgLeaf ? mgLeaf : "", mgFi,
                   roomZone, customZone, zoneNode, mapGrp);

        for (size_t i = 0; i < g_floors.size(); i++)
        {
            const Floor& f = g_floors[i];
            bool xzHit = (w[0] >= f.xMin && w[0] <= f.xMax && w[2] >= f.zMin && w[2] <= f.zMax);
            Log::Write("[MinimapFloorProbe]   floor[%zu] \"%s\" y=%.0f..%.0f x=%.0f..%.0f z=%.0f..%.0f xzHit=%d",
                       i, f.name.c_str(), f.yMin, f.yMax, f.xMin, f.xMax, f.zMin, f.zMax, xzHit ? 1 : 0);
        }

        DumpZoneNode(zoneNode);
        DumpActorGraphChain(actor);
    }

    static void Draw(const SubtitleRender::Frame& frame)
    {
        const float side = (float)(g_sizeMul * 10);
        const float left = (float)frame.width  * g_posX;
        const float top  = (float)frame.height * g_posY;

        frame.device->SetTexture(0, nullptr);

        // 底 + 邊框（沿用佔位方框的視覺，當作小地圖外框）
        FillRect(frame.device, left, top, side, side, kFillColor);
        FillRect(frame.device, left, top,                    side, kBorderPx, kBorderColor);
        FillRect(frame.device, left, top + side - kBorderPx, side, kBorderPx, kBorderColor);
        FillRect(frame.device, left,                    top, kBorderPx, side, kBorderColor);
        FillRect(frame.device, left + side - kBorderPx, top, kBorderPx, side, kBorderColor);

        if (g_floors.empty()) return;

        float w[3];
        if (!PlayerWorldPos(w)) return;

        // 樓層由 UpdateFloorSelection()（Tick 內每幀跑）定案，這裡只讀結果，
        // 避免逐幀 SelectFloor 在樓層邊界抖動。
        int fi = g_committedFloor;
        if (fi < 0 || fi >= (int)g_floors.size())
            fi = SelectFloor(w[0], w[1], w[2]);   // tracker 還沒定案時的保底
        if (fi < 0) return;
        const Floor& f = g_floors[fi];

        IDirect3DDevice9* device = frame.device;
        const float drawArea = side - 2.0f * (kBorderPx + kInsetPx);
        if (drawArea <= 0.0f) return;

        // world (X,Z) → 螢幕的仿射轉換：screenX = ax + bx*X、screenY = ay + by*Z
        // （丟 yaw，north-up 俯視：world X→螢幕右、world Z→螢幕上）。
        float ax, bx, ay, by;
        bool  clip;               // 局部視窗模式要 scissor 裁切
        float markX, markY;       // 47 標記螢幕位置

        if (g_zoom > 0.0f)
        {
            // 以 47 為中心的固定比例局部視窗：g_zoom 個 world 單位剛好鋪滿繪圖區寬。
            const float ppw = drawArea / g_zoom;   // pixels per world unit
            const float cx  = left + side * 0.5f;
            const float cy  = top  + side * 0.5f;
            bx = ppw;   ax = cx - w[0] * ppw;
            by = -ppw;  ay = cy + w[2] * ppw;
            clip  = true;
            markX = cx; markY = cy;
        }
        else
        {
            // 整層等比縮放塞滿方框、內容置中。
            const float worldW = f.xMax - f.xMin;
            const float worldH = f.zMax - f.zMin;
            if (worldW <= 0.0f || worldH <= 0.0f) return;
            const float scale   = drawArea / (worldW > worldH ? worldW : worldH);
            const float originX = left + kBorderPx + kInsetPx + (drawArea - worldW * scale) * 0.5f;
            const float originY = top  + kBorderPx + kInsetPx + (drawArea - worldH * scale) * 0.5f;
            bx = scale;   ax = originX - f.xMin * scale;
            by = -scale;  ay = originY + f.zMax * scale;
            clip  = false;
            markX = ax + bx * w[0];
            markY = ay + by * w[2];
        }

        // ---- scissor（僅局部視窗模式；compositor 不碰 scissor，用完自行還原）----
        DWORD oldScEnable = 0;
        RECT  oldScRect   = {};
        if (clip)
        {
            device->GetRenderState(D3DRS_SCISSORTESTENABLE, &oldScEnable);
            device->GetScissorRect(&oldScRect);

            LONG rl = (LONG)(left + kBorderPx + kInsetPx);
            LONG rt = (LONG)(top  + kBorderPx + kInsetPx);
            LONG rr = (LONG)(left + side - kBorderPx - kInsetPx);
            LONG rb = (LONG)(top  + side - kBorderPx - kInsetPx);
            if (rl < 0) rl = 0;
            if (rt < 0) rt = 0;
            if (rr > (LONG)frame.width)  rr = (LONG)frame.width;
            if (rb > (LONG)frame.height) rb = (LONG)frame.height;
            if (rr > rl && rb > rt)
            {
                RECT r = { rl, rt, rr, rb };
                device->SetScissorRect(&r);
                device->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
            }
            else
            {
                clip = false;   // 方框退化，別開 scissor
            }
        }

        // ---- 樓層填色（v3；畫在牆線下面）：低 alpha 灰三角形，重疊處自然加深＝房間分界感 ----
        const size_t nTri = f.fill.size() / 6;
        if (nTri > 0)
        {
            static std::vector<ScreenVertex> s_fillVerts;
            s_fillVerts.clear();
            s_fillVerts.reserve(nTri * 3);
            for (size_t i = 0; i < nTri; i++)
            {
                const float* t = &f.fill[i * 6];
                for (int k = 0; k < 3; k++)
                {
                    ScreenVertex v = { ax + bx * t[k * 2] - 0.5f, ay + by * t[k * 2 + 1] - 0.5f,
                                       0.0f, 1.0f, kFloorColor, 0.0f, 0.0f };
                    s_fillVerts.push_back(v);
                }
            }
            device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)nTri, &s_fillVerts[0], sizeof(ScreenVertex));
        }

        // ---- 牆線：一次 DrawPrimitiveUP(LINELIST)。verts 用 static 保留 capacity。----
        const size_t nSeg = f.segs.size() / 4;
        if (nSeg > 0)
        {
            static std::vector<ScreenVertex> s_verts;
            s_verts.clear();
            s_verts.reserve(nSeg * 2);
            for (size_t i = 0; i < nSeg; i++)
            {
                const float* s = &f.segs[i * 4];
                ScreenVertex va = { ax + bx * s[0] - 0.5f, ay + by * s[1] - 0.5f, 0.0f, 1.0f, kWallColor, 0.0f, 0.0f };
                ScreenVertex vb = { ax + bx * s[2] - 0.5f, ay + by * s[3] - 0.5f, 0.0f, 1.0f, kWallColor, 0.0f, 0.0f };
                s_verts.push_back(va);
                s_verts.push_back(vb);
            }
            device->DrawPrimitiveUP(D3DPT_LINELIST, (UINT)nSeg, &s_verts[0], sizeof(ScreenVertex));
        }

        // ---- 47 位置標記（小方塊）。fit 模式 clamp 進邊框；局部視窗模式恆在中心。----
        if (!clip)
        {
            const float loX = left + kBorderPx + kPlayerHalf, hiX = left + side - kBorderPx - kPlayerHalf;
            const float loY = top  + kBorderPx + kPlayerHalf, hiY = top  + side - kBorderPx - kPlayerHalf;
            if (markX < loX) markX = loX; else if (markX > hiX) markX = hiX;
            if (markY < loY) markY = loY; else if (markY > hiY) markY = hiY;
        }
        FillRect(device, markX - kPlayerHalf, markY - kPlayerHalf, kPlayerHalf * 2, kPlayerHalf * 2, kPlayerColor);

        if (clip)
        {
            device->SetRenderState(D3DRS_SCISSORTESTENABLE, oldScEnable);
            device->SetScissorRect(&oldScRect);
        }
    }

    static bool Tick()
    {
        if (!g_enabled) return false;
        if (!InLevel()) return false;
        RefreshLevel();
        UpdateFloorSelection();

        if (g_floorProbe) FloorProbeTick();

        if (g_sizeMul <= 0) return false;

        // 過場動畫中（含 Outro／M11_Escape 結尾過場）→ 整個小地圖不畫。
        if (SubtitleGate::IsScriptedSubtitleBlocking() || SubtitleGate::IsPlayerControlsLocked() ||
            SubtitleGate::IsKnownEndingCutsceneActive()) return false;

        // ESC 暫停選單 / 原生地圖畫面開啟中 → 不畫。
        DWORD ctrl       = *(DWORD*)kLevelCtrlSingletonPtr;
        DWORD menuCtrl   = ctrl     ? *(DWORD*)(ctrl + kMenuCtrlOffset)     : 0;
        BYTE  pauseFlags = menuCtrl ? *(BYTE*)(menuCtrl + kPauseByteOffset) : 0;
        if (pauseFlags & kPauseMaskHide) return false;

        return true;   // 外框一律畫；有 bin 時 Draw 內再疊牆線 + 47 標記
    }

    void EnsureRenderReady()
    {
        if (!g_enabled) return;
        SubtitleRender::EnsureHookInstalled();
    }

    void Install(HMODULE hModule)
    {
        g_hModule = hModule;

        Config::HudConfig cfg = Config::LoadHud(hModule);
        g_enabled = cfg.minimapEnabled;
        g_posX    = cfg.minimapPosX / 100.0f;
        g_posY    = cfg.minimapPosY / 100.0f;
        g_sizeMul = cfg.minimapSizeMul;
        g_zoom    = cfg.minimapZoom;

        g_floorProbe = Config::LoadDebug(hModule).minimapFloorProbe;

        g_floors.clear();
        g_roomTable.clear();
        g_loadedTag.clear();
        g_committedFloor = g_pendingFloor = -1;

        if (!g_enabled)
        {
            Log::Write("[MinimapHud] Install：[Hud] MinimapEnabled=0，跳過");
            return;
        }

        SubtitleRender::Register({ &Tick, &Draw });
        Log::Write("[MinimapHud] Install完成（自繪 route B：bink32hook\\minimap\\minimap_<關>.bin）："
                   "pos=%.1f%%,%.1f%% size=%d(=%dpx) zoom=%.0f(%s) floorProbe=%d",
                   cfg.minimapPosX, cfg.minimapPosY, g_sizeMul, g_sizeMul * 10,
                   g_zoom, g_zoom > 0.0f ? "局部視窗" : "整層fit", g_floorProbe ? 1 : 0);
    }
}
