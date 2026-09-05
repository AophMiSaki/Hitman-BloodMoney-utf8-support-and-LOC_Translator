#include "LocHook.h"
#include "Config.h"
#include "Log.h"
#include "ZipPathTrace.h"
#include "GlyphAtlas.h"
#include "LocTxtParser.h"
#include "LocScenePath.h"
#include "LocNewspaperFix.h"
#include <d3d9.h>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <cstring>

namespace LocHook
{
    static bool g_enable = true;
    static int  g_hotkeyVK = VK_F11;
    static bool g_diagEnable = false;

    static bool g_switchOn = false;
    static bool g_prevHotkeyDown = false;

    // native自己的key查找（sub_464FF0）用_strnicmp逐段比對，大小寫不敏感。
    // 某些呼叫端傳進來的key casing跟LOC原文/翻譯txt裡登錄的casing不一致
    // （如"UnLockDoor" vs "UnlockDoor"），但邏輯上是同一個key——native用
    // 大小寫不敏感比對吸收掉這種不一致，我們的map查表也要一樣大小寫不敏感，
    // 否則這些明明存在的翻譯會被casing差異誤判成miss。
    struct CaseInsensitiveLess
    {
        bool operator()(const std::string& a, const std::string& b) const
        {
            return _stricmp(a.c_str(), b.c_str()) < 0;
        }
    };

    static bool g_cacheLoaded = false;
    static std::map<std::string, std::string, CaseInsensitiveLess> g_translations;
    // "category/key"查無翻譯時的debug字串快取，跟g_translations同生命週期
    // （InvalidateCache一起清空）。每幀重查、用完即丟的UI文字用這個壽命即可；
    // 會被消費端留存的回傳值改走g_returnedArena（見下方）。
    static std::map<std::string, std::string, CaseInsensitiveLess> g_fallbackCache;

    // 會被消費端「留存」的回傳字串專用arena。g_translations／g_fallbackCache的
    // 內部指標壽命只到下次InvalidateCache，但動作提示widget（ZGEOM_Action
    // caption）留存GetText回傳指標並每幀重畫，活得比它久 → UAF。白名單呼叫端
    // （kReturnKeyOnMissReturnAddrs）的回傳值改走這個arena，指標壽命延長到
    // 「任務目錄段」邊界（SCENES\M01\ → SCENES\M02\ ／回frontend）才隨
    // g_returnedArena.clear()釋放——此時native已銷毀前一任務所有HUD widget。
    // InvalidateCache不動這個arena。std::set為node-based，元素位址插入後穩定，
    // 可安全把c_str()交給native留存。
    static std::set<std::string> g_returnedArena;
    static std::string g_arenaMissionTag;

    // 把s複製進g_returnedArena、回傳穩定的const char*。已存在就回傳既有
    // 那份——idempotent：連續組裝鏈把先前譯文回頭再查miss時，原樣拿回同
    // 一段譯文，組裝鏈天然不會斷。
    static const char* InternToArena(const std::string& s)
    {
        return g_returnedArena.insert(s).first->c_str();
    }

    // 獨立的第二份map，存原文（英文）內容，供TryLookupOriginal()查詢（見
    // LocHook.h頂部說明）。跟g_translations分開載入/分開快取失效，
    // 來源目錄也不同（Orig_Scenes\而不是Scenes\）。
    static bool g_originalsLoaded = false;
    static std::map<std::string, std::string, CaseInsensitiveLess> g_originals;

    // ---- BeginScene鏈式hook：只用來每幀輪詢熱鍵狀態，跟
    // NativeTextureRegistry.cpp各自獨立安裝（同一個vtable slot上鏈式
    // hook：先裝的變成後裝的的orig，呼叫鏈自然接起來，不需要跨檔案耦合）。
    typedef HRESULT(__stdcall* BeginSceneFn)(IDirect3DDevice9*);
    static BeginSceneFn g_origBeginScene = nullptr;
    static bool g_beginSceneHooked = false;
    static DWORD g_beginSceneFailLog = 0;

    static void PollHotkey()
    {
        if (!g_enable) return;
        bool down = (GetAsyncKeyState(g_hotkeyVK) & 0x8000) != 0;
        if (down && !g_prevHotkeyDown)
        {
            g_switchOn = !g_switchOn;
            Log::Write("[LocHook] 熱鍵切換：目前顯示%s", g_switchOn ? "譯文" : "原文");
        }
        g_prevHotkeyDown = down;
    }

    static HRESULT __stdcall Hook_BeginScene(IDirect3DDevice9* device)
    {
        PollHotkey();
        return g_origBeginScene ? g_origBeginScene(device) : D3DERR_INVALIDCALL;
    }

    // __cdecl、無參數、冪等：從GetTextDetour每次命中時呼叫，直到裝置就緒
    // 為止（跟TextureHook::EnsureInstalled()呼叫端慣例一致）。裝在GetText
    // 命中路徑而不是等某個特定畫面，是因為熱鍵要在「switch還是off」時就能
    // 被偵測到才能切換成on——off分支本身不會呼叫任何C++函式，所以改在
    // detour最前面、不分on/off分支都先呼叫一次。
    static void __cdecl EnsureHotkeyPollInstalled()
    {
        if (g_beginSceneHooked) return;

        IDirect3DDevice9* device = GlyphAtlas::GetD3D9Device();
        if (!device)
        {
            if (g_beginSceneFailLog < 5)
            {
                g_beginSceneFailLog++;
                Log::Write("[LocHook] EnsureHotkeyPollInstalled失敗(#%lu)：D3D9裝置尚未就緒", g_beginSceneFailLog);
            }
            return;
        }

        DWORD* vtable = *(DWORD**)device;
        DWORD oldProt = 0;

        // IDirect3DDevice9::BeginScene = vtable+0xA4，跟NativeTextureRegistry.
        // cpp已驗證過的同一個offset（見該檔案註解交叉核對）。
        VirtualProtect(&vtable[0xA4 / 4], 4, PAGE_EXECUTE_READWRITE, &oldProt);
        g_origBeginScene = (BeginSceneFn)vtable[0xA4 / 4];
        vtable[0xA4 / 4] = (DWORD)&Hook_BeginScene;
        VirtualProtect(&vtable[0xA4 / 4], 4, oldProt, &oldProt);

        g_beginSceneHooked = true;
        Log::Write("[LocHook] 熱鍵輪詢BeginScene鏈式hook安裝完成：device=%p orig=%p", device, g_origBeginScene);
    }

    // ---- 場景中文LOC lazy load ----

    // 嘗試載入單一候選檔案（開檔/讀取/parse全部成功才算成功）。
    // 失敗（含檔案不存在）只log、不動targetMap，讓呼叫端接著試下一個候選
    // 格式（loc優先、失敗換txt）。targetMap參數化：LoadIfNeeded()傳
    // g_translations（譯文）、LoadOriginalIfNeeded()傳g_originals（原文），
    // 共用同一套開檔/parse邏輯。檔案固定當UTF-8處理。
    static bool LoadCandidate(const char* pathIn, const char* formatLabel,
                               std::map<std::string, std::string, CaseInsensitiveLess>& targetMap,
                               std::map<std::string, unsigned long long>* outMeta = nullptr)
    {
        char path[MAX_PATH];
        strncpy_s(path, sizeof(path), pathIn, _TRUNCATE);
        LocScenePath::ResolveCaseInsensitivePath(path, sizeof(path));

        HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            Log::Write("[LocHook] %s格式場景中文檔不存在：%s", formatLabel, path);
            return false;
        }

        DWORD size = GetFileSize(h, nullptr);
        std::vector<char> raw(size);
        DWORD bytesRead = 0;
        BOOL ok = size ? ReadFile(h, raw.data(), size, &bytesRead, nullptr) : TRUE;
        CloseHandle(h);
        if (!ok || bytesRead != size)
        {
            Log::Write("[LocHook] 讀取%s格式場景中文檔失敗：%s (read=%lu/%lu)", formatLabel, path, bytesRead, size);
            return false;
        }

        std::string utf8Text(raw.data(), raw.size());

        // 先parse進暫存map，成功才swap進targetMap——避免parse到一半失敗時
        // targetMap殘留半成品內容。
        std::map<std::string, std::string> parsed;
        std::map<std::string, unsigned long long> parsedMeta;
        std::string err;
        if (!LocTxtParser::Parse(utf8Text.c_str(), utf8Text.size(), parsed, err, outMeta ? &parsedMeta : nullptr))
        {
            Log::Write("[LocHook] %s格式場景中文檔解析失敗：%s -- %s", formatLabel, path, err.c_str());
            return false;
        }
        if (outMeta) *outMeta = std::move(parsedMeta);

        // parsed是LocTxtParser::Parse固定簽章的大小寫敏感std::map，跟
        // targetMap（CaseInsensitiveLess）型別不同不能直接swap，逐筆
        // move進去（emplace在key只有大小寫差異時保留先插入那筆，這裡假設
        // 翻譯txt不會刻意用大小寫區分出兩個不同entry，跟native自己
        // _strnicmp查找的語意一致）。
        targetMap.clear();
        for (auto& kv : parsed) targetMap.emplace(std::move(kv.first), std::move(kv.second));
        Log::Write("[LocHook] %s格式場景文字檔載入完成：%s，%zu筆", formatLabel, path, targetMap.size());
        return true;
    }

    // 報紙譯文載入完成後套用LocNewspaperFix的CJK斷點空白。只在開關開啟、
    // 目前場景是報紙zip時才跑；逐筆跳過headline key（置中排版不需要、插了
    // 會跑版）。只動g_translations，不動g_originals。
    static void ApplyNewspaperSoftBreaks()
    {
        if (!LocNewspaperFix::Enabled() || LocNewspaperFix::Interval() <= 0) return;
        if (!LocNewspaperFix::IsNewspaperZipPath(ZipPathTrace::GetLastSceneZipPath())) return;

        size_t modified = 0;
        size_t skippedHeadline = 0;
        for (auto& kv : g_translations)
        {
            if (LocNewspaperFix::IsHeadlineKey(kv.first)) { skippedHeadline++; continue; }
            size_t before = kv.second.size();
            LocNewspaperFix::InsertSoftBreaks(kv.second);
            if (kv.second.size() != before) modified++;
        }
        Log::Write("[LocHook] NewsPaperSoftBreakFix：報紙譯文插入斷行空白完成，%zu/%zu筆文字被修改（間隔=%d，排除headline=%zu筆）",
                   modified, g_translations.size(), LocNewspaperFix::Interval(), skippedHeadline);
    }

    static void LoadIfNeeded()
    {
        if (g_cacheLoaded) return;

        // g_returnedArena（會被動作提示widget留存的回傳指標）綁「任務目錄段」
        // 邊界釋放，不跟InvalidateCache（那是任務內子場景轉換粒度、widget撐
        // 得過去，跟著清會UAF）。檢查點放這裡而不是InvalidateCache——
        // InvalidateCache當下新場景zip還沒側錄到，下一次GetText觸發
        // LoadIfNeeded時才就緒。只在取到非空且不同的tag時clear（換任務）；
        // 此時native已銷毀前一任務所有HUD widget，清arena安全。
        {
            std::string tag = LocScenePath::CurrentMissionTag();
            if (!tag.empty() && tag != g_arenaMissionTag)
            {
                if (!g_returnedArena.empty())
                    Log::Write("[LocHook] 任務目錄段變更（%s -> %s），釋放returned-pointer arena（%zu筆）",
                               g_arenaMissionTag.empty() ? "(空)" : g_arenaMissionTag.c_str(),
                               tag.c_str(), g_returnedArena.size());
                g_returnedArena.clear();
                g_arenaMissionTag = tag;
            }
        }

        // 不管接下來成不成功，都標記已嘗試過，避免每次GetText查詢都重新
        // 開檔（檔案不存在只log一次，不做特殊處理）。
        g_cacheLoaded = true;

        char stem[MAX_PATH];
        if (!LocScenePath::BuildSceneStemPath(stem, sizeof(stem))) return;

        char locPath[MAX_PATH];
        _snprintf_s(locPath, _TRUNCATE, "%s.LOC", stem);
        if (LoadCandidate(locPath, "loc", g_translations))
        {
            ApplyNewspaperSoftBreaks();
            return;
        }

        char txtPath[MAX_PATH];
        _snprintf_s(txtPath, _TRUNCATE, "%s.txt", stem);
        if (LoadCandidate(txtPath, "txt", g_translations))
        {
            ApplyNewspaperSoftBreaks();
            return;
        }

        // 兩種格式都失敗——沿用既有fallback行為，不做特殊處理：g_translations
        // 維持空map，之後LookupText全部走查無翻譯的組合字串fallback。
        Log::Write("[LocHook] loc/txt兩種格式皆載入失敗，此場景切換功能暫時無效：%s", stem);
    }

    static void LoadOriginalIfNeeded()
    {
        if (g_originalsLoaded) return;
        g_originalsLoaded = true;

        char stem[MAX_PATH];
        if (!LocScenePath::BuildOrigSceneStemPath(stem, sizeof(stem))) return;

        char txtPath[MAX_PATH];
        _snprintf_s(txtPath, _TRUNCATE, "%s_unpacked.txt", stem);
        if (LoadCandidate(txtPath, "orig-unpacked-txt", g_originals)) return;

        Log::Write("[LocHook] Orig_Scenes原文_unpacked.txt載入失敗，此場景原文查詢功能暫時無效：%s", stem);
    }

    void InvalidateCache()
    {
        // ═══ SIDE-RECORD-13（debug側錄，受 [Debug] g_diagEnable 控制）═══
        // clear()前把即將被釋放的字串緩衝位址dump出來。崩潰log裡下一幀動作
        // 提示重畫時的存取位址若命中這裡剛被釋放的g_translations／
        // g_fallbackCache某筆buf，即坐實widget留存了我們會釋放的指標。
        // g_returnedArena本函式不釋放，一併印出來當反證。g_translations只印
        // 總數不逐一列。
        if (g_diagEnable)
        {
            Log::Write("[LocHook] SIDE-RECORD-13 InvalidateCache前：g_translations=%zu筆(即將釋放,未逐一列) g_fallbackCache=%zu筆(即將釋放) g_originals=%zu筆(即將釋放) g_returnedArena=%zu筆(保留不釋放)",
                       g_translations.size(), g_fallbackCache.size(), g_originals.size(), g_returnedArena.size());
            for (auto& kv : g_fallbackCache)
                Log::Write("[LocHook] SIDE-RECORD-13   fallbackCache即將釋放：buf=%p key=\"%s\" 內容=\"%s\"",
                           (const void*)(kv.second.empty() ? nullptr : kv.second.data()), kv.first.c_str(), kv.second.c_str());
            for (const auto& s : g_returnedArena)
                Log::Write("[LocHook] SIDE-RECORD-13   arena保留(不釋放)：buf=%p 內容=\"%s\"", (const void*)s.c_str(), s.c_str());
        }

        g_cacheLoaded = false;
        g_translations.clear();
        g_fallbackCache.clear();
        g_originalsLoaded = false;
        g_originals.clear();
        // g_returnedArena／g_arenaMissionTag故意不在這裡清——InvalidateCache是
        // 「任務內子場景轉換」粒度（premission→main也觸發），動作提示widget撐
        // 得過這種轉換並每幀重畫，跟著清會UAF。arena改綁「任務目錄段」邊界，
        // 在LoadIfNeeded()開頭檢查、換任務時才clear。
        Log::Write("[LocHook] 場景切換，LOC快取已標記失效，下次查詢時重新載入");
    }

    // 供SubtitleBrief.cpp直接查詢完整LOC key（不經GetText detour/native呼叫
    // 路徑），只查g_translations（原文改由呼叫端在IsTranslationActive()為
    // false時直接呼叫native sub_6CF850）。
    bool TryLookup(const char* combinedKey, std::string& outText)
    {
        if (!combinedKey || !combinedKey[0]) return false;
        LoadIfNeeded();

        const char* normalized = combinedKey;
        while (*normalized == '/') normalized++;

        auto it = g_translations.find(normalized);
        if (it == g_translations.end()) return false;

        outText = it->second;
        return true;
    }

    // 見LocHook.h頂部說明，跟TryLookup同語意但查g_originals
    // （Orig_Scenes\底下的原文，lazy load）。
    bool TryLookupOriginal(const char* combinedKey, std::string& outText)
    {
        if (!combinedKey || !combinedKey[0]) return false;
        LoadOriginalIfNeeded();

        const char* normalized = combinedKey;
        while (*normalized == '/') normalized++;

        auto it = g_originals.find(normalized);
        if (it == g_originals.end()) return false;

        outText = it->second;
        return true;
    }

    bool IsTranslationActive()
    {
        return g_switchOn;
    }

    // 見 LocHook.h 宣告處說明。跟 LoadIfNeeded 走同一套 stem 推導與 loc→txt
    // 候選，但獨立一次性讀檔解析（帶 outMeta），不動 g_translations 快取。
    // 呼叫頻率＝每次場景切換一次（SubtitleTvRadio），不是每幀。
    void CollectTvRadioSndOffsets(std::map<DWORD, std::string>& out)
    {
        out.clear();

        char stem[MAX_PATH];
        if (!LocScenePath::BuildSceneStemPath(stem, sizeof(stem)))
        {
            Log::Write("[LocHook] TVAndRadio offset 掃描：取不到場景 zip 路徑，跳過");
            return;
        }

        std::map<std::string, std::string, CaseInsensitiveLess> throwaway;
        std::map<std::string, unsigned long long> meta;

        char locPath[MAX_PATH];
        _snprintf_s(locPath, _TRUNCATE, "%s.LOC", stem);
        bool ok = LoadCandidate(locPath, "loc(TVAndRadio掃描)", throwaway, &meta);
        if (!ok)
        {
            char txtPath[MAX_PATH];
            _snprintf_s(txtPath, _TRUNCATE, "%s.txt", stem);
            ok = LoadCandidate(txtPath, "txt(TVAndRadio掃描)", throwaway, &meta);
        }
        if (!ok)
        {
            Log::Write("[LocHook] TVAndRadio offset 掃描：loc/txt 皆載入失敗，SubtitleTvRadio 本場景無字幕：%s", stem);
            return;
        }

        size_t collisions = 0;
        for (const auto& kv : meta)
        {
            if (kv.first.find("TVAndRadio/") == std::string::npos) continue;
            if (kv.second > 0xFFFFFFFFull) continue;
            if (!out.emplace((DWORD)kv.second, kv.first).second) collisions++;
        }
        Log::Write("[LocHook] TVAndRadio offset 掃描完成：%s，%zu 筆%s",
                   stem, out.size(), collisions ? "（有 .SND offset 重複，保留先插入者）" : "");
    }

    // 判斷原文（g_originals查到的內容）是不是「本來就沒內容」的空白佔位符。
    // Info3/4/5這類編號欄位是「最多N條、實際用幾條看武器」的模板，沒用到的
    // 欄位native自己塞控制字元佔位（如value只有單一個U+000B），不是翻譯miss。
    // 只要value裡除了ASCII控制字元(0x00~0x1F/0x7F)、空白、tab、換行以外沒有
    // 其他byte，就視為空白佔位符——UTF-8多位元組CJK字元每個byte都>0x7F，
    // 不會被誤判。
    static bool IsBlankOriginalContent(const std::string& value)
    {
        for (unsigned char c : value)
        {
            if (c > 0x20 && c != 0x7F) return false;
        }
        return true;
    }

    // 「動作提示widget會留存GetText回傳指標」型呼叫端白名單。跟後面的
    // kPathCReturnAddrs（Path C：引擎內部路徑組件，絕不翻譯、強制passthrough）
    // 是獨立的兩套機制、語意相反——這裡是「照常翻譯，但 (a)回傳的指標必須
    // 永久（intern進g_returnedArena），(b)雙表miss時回傳傳進來的key而不是
    // 空字串」。
    //
    // 目前唯一一筆：0x0052AF61 = sub_52AF00內對sub_465370的call的return
    // address。sub_52AF00建ZGEOM_Action widget前，會把前面兩次GetText翻好、
    // 就地合成的字串（如「變裝 警衛」）回頭再丟給sub_465370查一次，拿回傳值
    // 餵caption setter（vtable[0xB8]）。合成字串在雙表裡查不到 → 舊版回空字串
    // → caption畫不出來；且回傳的map內部指標壽命只到下次InvalidateCache，
    // widget卻留存並每幀重畫 → InvalidateCache後UAF。見md「原文譯文切換」第13節。
    static const DWORD kReturnKeyOnMissReturnAddrs[] = { 0x0052AF61 };

    static bool IsReturnKeyOnMissCaller(DWORD retAddr)
    {
        for (DWORD addr : kReturnKeyOnMissReturnAddrs)
        {
            if (addr == retAddr) return true;
        }
        return false;
    }

    // __cdecl，供naked detour呼叫。回傳的char*一定非nullptr，且指向map裡存的
    // std::string（生命週期到下次InvalidateCache為止，安全交給native排版
    // pipeline使用）。retAddr參數（呼叫端return address）純供miss診斷log用，
    // 不影響查表邏輯——category單獨無法鎖定呼叫端（"AllLevels/Interface"就有
    // 63個呼叫端共用，見GetTextDetour前方Path A/B/C註解），需要return address
    // 才能從log反查是哪個函式傳了空key進來miss。
    // 記錄 category 是否為 Outro／M11_Escape，供 SubtitleGate 查詢是否在這兩段
    // 結尾過場中。
    static ULONGLONG g_lastOutroHitMs = 0;
    static ULONGLONG g_lastEscapeHitMs = 0;
    static const DWORD kEndingCutsceneHoldMs = 1000;

    static bool CategoryMatches(const char* category, const char* name)
    {
        if (!category) return false;
        while (*category == '/') category++;
        size_t nameLen = strlen(name);
        if (_strnicmp(category, name, nameLen) != 0) return false;
        return category[nameLen] == '\0' || category[nameLen] == '/';
    }

    bool IsEndingCutsceneCategoryActive()
    {
        ULONGLONG now = GetTickCount64();
        return (now - g_lastOutroHitMs < kEndingCutsceneHoldMs) || (now - g_lastEscapeHitMs < kEndingCutsceneHoldMs);
    }

    static const char* __cdecl LookupText(const char* category, const char* key, DWORD retAddr)
    {
        ULONGLONG nowMs = GetTickCount64();
        if (CategoryMatches(category, "Outro")) g_lastOutroHitMs = nowMs;
        if (CategoryMatches(category, "M11_Escape")) g_lastEscapeHitMs = nowMs;

        // ═══ SIDE-RECORD-13（debug側錄，受 [Debug] g_diagEnable 控制）═══
        // detour進入點：印 retAddr／category／key指標值／key前16 byte十六進位／
        // key strlen（re-query那筆的strlen是合成字串的byte長度，用來評估
        // caption setter若「複製進固定小buffer」的overflow風險）。
        if (g_diagEnable)
        {
            char keyHex[64] = { 0 };
            if (key)
                for (int i = 0; i < 16 && key[i]; i++)
                    _snprintf_s(keyHex + i * 3, sizeof(keyHex) - i * 3, _TRUNCATE, "%02X ", (unsigned char)key[i]);
            Log::Write("[LocHook] SIDE-RECORD-13 entry：retAddr=0x%08X category=%p(\"%s\") key=%p(\"%s\") keyBytes=[%s] keyLen=%d",
                       retAddr, (const void*)category, category ? category : "(null)",
                       (const void*)key, key ? key : "(null)", keyHex, key ? (int)strlen(key) : -1);
        }

        // 空key不是翻譯miss——是像sub_561030這類共用UI widget方法本來就沒有
        // 文字要顯示的正常情況（sub_561030把[esi+70h]這個可能為空的欄位直接
        // 當key傳進來，沒檢查是否為空）。不查表、不當miss記診斷log、直接回傳
        // 空字串，避免fallback的"category/"組合字串被當成正常文字顯示。
        //
        // 特例：key空但category非空時，native語意是「直接查category這個完整
        // 路徑本身的leaf文字」，不是「category+key兩段式」組合查詢（如
        // retAddr=0x00670698 sub_670630 H01_GetToPier任務提示鏈，
        // category="m00/Tutorials/H01_GetToPier"、key留空）。先把category本身
        // 當完整LOC key查g_translations，命中就回傳。跟sub_561030那種category
        // 是共用目錄前綴的case不衝突——目錄前綴在LocTxtParser攤平的map裡只是
        // 中繼節點、沒有自己的leaf entry，查表必定miss、自然落回下面的空字串
        // 分支。
        if (!key || !key[0])
        {
            if (category && category[0])
            {
                LoadIfNeeded();
                const char* normalizedCategory = category;
                while (*normalizedCategory == '/') normalizedCategory++;
                auto catIt = g_translations.find(normalizedCategory);
                if (catIt != g_translations.end())
                {
                    if (g_diagEnable) Log::Write("[LocHook] 查表命中(純category，key為空)：%s -> %s（呼叫端returnAddr=0x%08X）", normalizedCategory, catIt->second.c_str(), retAddr);
                    return catIt->second.c_str();
                }
            }

            if (g_diagEnable) Log::Write("[LocHook] 空key，直接回傳空字串（呼叫端returnAddr=0x%08X）", retAddr);
            static const char* kEmptyKey = "";
            return kEmptyKey;
        }

        LoadIfNeeded();

        // category/key分開正規化（category去尾端'/'、key去開頭'/'）再拼接。
        // 不能無條件塞"/"分隔：有一大批呼叫端傳入category=""、key=完整路徑
        // （"AllLevels/Actions/BreakUtilBox"）本身，LocTxtParser建map時根層級
        // 不加"/"前綴，category為空還硬塞"/"會組出map裡查無此key的
        // "/AllLevels/..."。key本身有時也帶開頭的'/'（如category="AllLevels"、
        // key="/ClothNames/M01_ChileanGuard"），只對combined整串開頭trim的話
        // category非空時中間的雙斜線trim不到，跟永遠單斜線的map key對不上，
        // 變成假性miss。兩邊各自可能有多個多餘'/'都一併處理掉。
        const char* keyNorm = key;
        while (*keyNorm == '/') keyNorm++;

        const char* categoryEnd = category ? category + strlen(category) : category;
        while (category && categoryEnd > category && *(categoryEnd - 1) == '/') categoryEnd--;
        size_t categoryLen = category ? (size_t)(categoryEnd - category) : 0;

        char combined[256];
        if (categoryLen > 0)
            _snprintf_s(combined, _TRUNCATE, "%.*s/%s", (int)categoryLen, category, keyNorm);
        else
            _snprintf_s(combined, _TRUNCATE, "%s", keyNorm);

        char* normalizedCombined = combined;
        while (*normalizedCombined == '/') normalizedCombined++;

        // M04 call site（sub_6BC560, ReviveCIAAgent分支）診斷log，方便日後
        // 交叉核對（見GetTextDetour前方Path B註解）。
        if (key && _stricmp(key, "ReviveCIAAgent") == 0)
        {
            Log::Write("[LocHook] 命中M04 call site候選key(ReviveCIAAgent)：category=%s",
                       category ? category : "(null)");
        }

        // 命中log帶retAddr，方便反查像"PickupSPC"這類命中但畫面顯示亂碼
        // （CJK自繪排版問題，不是查表問題）的呼叫端。
        auto it = g_translations.find(normalizedCombined);
        if (it != g_translations.end())
        {
            if (g_diagEnable) Log::Write("[LocHook] 查表命中：%s -> %s（呼叫端returnAddr=0x%08X）", normalizedCombined, it->second.c_str(), retAddr);
            // 白名單呼叫端（動作提示widget builder，會把GetText回傳指標留存、
            // 每幀重畫）命中時也要intern——g_translations內部指標壽命只到下次
            // InvalidateCache，widget撐得比那久 → UAF。arena綁任務目錄段釋放
            // （見LoadIfNeeded），非白名單路徑不受影響。
            if (IsReturnKeyOnMissCaller(retAddr))
            {
                const char* interned = InternToArena(it->second);
                if (g_diagEnable) Log::Write("[LocHook] SIDE-RECORD-13 hit-return(白名單intern)：retAddr=0x%08X ret=%p 內容=\"%s\" bytes=%d arena筆數=%zu",
                                             retAddr, (const void*)interned, interned, (int)strlen(interned), g_returnedArena.size());
                return interned;
            }
            // SIDE-RECORD-13：非白名單命中也印回傳指標（指向g_translations內部，
            // 壽命只到下次InvalidateCache），崩潰存取位址若命中即坐實有消費端
            // 留存了會被釋放的指標。
            if (g_diagEnable) Log::Write("[LocHook] SIDE-RECORD-13 hit-return(一般)：retAddr=0x%08X ret=%p", retAddr, (const void*)it->second.c_str());
            return it->second.c_str();
        }

        auto fit = g_fallbackCache.find(normalizedCombined);
        if (fit != g_fallbackCache.end())
        {
            // SIDE-RECORD-13：白名單呼叫端理論上不該在這裡命中（白名單分支
            // 不寫g_fallbackCache）；若真的命中代表有別的呼叫端先產生了相同
            // 的combined並快取——印出來確認是否誤把空字串回給re-query。
            if (g_diagEnable && IsReturnKeyOnMissCaller(retAddr))
                Log::Write("[LocHook] SIDE-RECORD-13 白名單呼叫端命中g_fallbackCache：retAddr=0x%08X combined=\"%s\" ret=%p 內容=\"%s\"",
                           retAddr, normalizedCombined, (const void*)fit->second.c_str(), fit->second.c_str());
            return fit->second.c_str();
        }

        // 翻譯miss前，先查原文（g_originals，Orig_Scenes dump；若此場景沒放
        // dump則g_originals是空map、這段查不到、直接落回下面的combo字串
        // fallback，行為不變）。如果原文本身是IsBlankOriginalContent()判定的
        // 空白佔位符（如Info3/4/5這類沒用到的模板欄位），代表不是翻譯miss、
        // 是native自己就沒內容要顯示，回傳空字串取代難看的"category/.../InfoN"
        // 組合字串，同樣快取進g_fallbackCache避免重複查找。
        LoadOriginalIfNeeded();
        auto origIt = g_originals.find(normalizedCombined);
        if (origIt != g_originals.end() && IsBlankOriginalContent(origIt->second))
        {
            if (g_diagEnable) Log::Write("[LocHook] 原文為空白佔位符，略過fallback顯示：%s（呼叫端returnAddr=0x%08X）", normalizedCombined, retAddr);
            auto insertedBlank = g_fallbackCache.emplace(normalizedCombined, std::string());
            const char* retBlank = insertedBlank.first->second.c_str();
            if (g_diagEnable) Log::Write("[LocHook] SIDE-RECORD-13 miss-return(空白佔位符)：retAddr=0x%08X ret=%p 內容=\"%s\"", retAddr, (const void*)retBlank, retBlank);
            return retBlank;
        }

        // 原文（g_originals）也完全沒有這個key，代表native在所有語言都沒給過
        // 這段文字（跟上面的空白佔位符不同——上面是「有entry但內容是控制字元
        // 佔位符」，這裡是「entry本身就不存在」，如sub_6348F0寫死呼叫的
        // "Drag"、sub_54F2F0底下的裸"Legend"，見[[原文譯文切換]]第3節）。這種
        // miss不是缺翻譯，只記log，不回傳combo debug字串——避免把這類native
        // 本身就沒內容的key洩漏到畫面上（AllLevels/Map/Legend已實機驗證combo
        // 字串會被native設進UI顯示）。
        //
        // ⚠️副作用：如果這個場景根本沒放Orig_Scenes原文dump檔（g_originals
        // 整個是空map，見LoadOriginalIfNeeded前方註解），這裡會導致「該場景
        // 所有真正缺翻譯的key」也一併被這個分支擋掉、只留log看不到畫面上的
        // combo提示——之後排查某場景「明明沒翻譯但畫面什麼都沒顯示」時，要
        // 先確認是不是漏放了Orig_Scenes dump。
        if (origIt == g_originals.end())
        {
            // 白名單呼叫端（sub_52AF00內0x0052AF61那個「把已翻好的合成字串
            // 回頭當key再查一次」的re-query）撞到這個double-miss分支時，不能回
            // 空字串——widget caption拿到空就畫不出來。改回傳「傳進來的key」
            // 本身（intern進arena、指標永久），對齊native sub_465370「miss回a3
            // （呼叫端的key字面量指標，永久）」的契約。這一步天然idempotent：
            // 先前翻好的字串原樣拿回去，組裝鏈不會斷。
            if (IsReturnKeyOnMissCaller(retAddr))
            {
                if (g_diagEnable) Log::Write("[LocHook] 白名單re-query呼叫端double-miss，回傳輸入key(intern)：%s（呼叫端returnAddr=0x%08X）", keyNorm, retAddr);
                const char* interned = InternToArena(std::string(keyNorm));
                // SIDE-RECORD-13：回傳interned key指標。印指標值＋內容＋byte長度
                // （setter若「複製進固定小buffer」，這個長度決定有無overflow——
                // 指標壽命修法擋不住那種，屬第13節列的殘餘風險）。
                if (g_diagEnable) Log::Write("[LocHook] SIDE-RECORD-13 miss-return(白名單intern)：retAddr=0x%08X ret=%p 內容=\"%s\" bytes=%d arena筆數=%zu",
                                             retAddr, (const void*)interned, interned, (int)strlen(interned), g_returnedArena.size());
                return interned;
            }

            if (g_diagEnable) Log::Write("[LocHook] 原文/譯文皆無此key，僅記錄不顯示：%s（呼叫端returnAddr=0x%08X）", normalizedCombined, retAddr);
            auto insertedNone = g_fallbackCache.emplace(normalizedCombined, std::string());
            const char* retNone = insertedNone.first->second.c_str();
            if (g_diagEnable) Log::Write("[LocHook] SIDE-RECORD-13 miss-return(原文譯文皆無)：retAddr=0x%08X ret=%p", retAddr, (const void*)retNone);
            return retNone;
        }

        // 同一個combined字串只有第一次miss會走到這裡印log（之後都被
        // g_fallbackCache擋掉）；如果之後發現同一個retAddr對不上懷疑的呼叫端，
        // 要留意可能有別的呼叫端也產生了完全相同的combined miss、被這次的
        // cache提早擋掉沒能各自留下log。retAddr本身不足以判斷miss是否安全
        // （miss的fallback字串會被native直接設進UI顯示，已用AllLevels/Map/
        // Legend實機驗證），每筆miss還是要個別確認呼叫端在native/原文裡是否
        // 真的有對應內容。
        //
        // 這行不受g_diagEnable控制、一律印出：這是「原文有實質內容但中文還沒
        // 翻」的真正缺翻譯清單，要拿來追蹤翻譯進度，不是單純除錯用診斷log，
        // 不應因為關掉診斷開關就漏掉。
        Log::Write("[LocHook] 查無翻譯，fallback回傳組合字串：%s（呼叫端returnAddr=0x%08X）", normalizedCombined, retAddr);
        auto inserted = g_fallbackCache.emplace(normalizedCombined, normalizedCombined);
        const char* retCombo = inserted.first->second.c_str();
        if (g_diagEnable) Log::Write("[LocHook] SIDE-RECORD-13 miss-return(組合字串fallback)：retAddr=0x%08X ret=%p 內容=\"%s\"", retAddr, (const void*)retCombo, retCombo);
        return retCombo;
    }

    // ---- sub_465370(DNameNode::GetText slot0) function-entry inline hook ----
    //
    // 前10 bytes（兩句完整指令，乾淨邊界）：
    //   465370  8B 54 24 04        mov edx, [esp+4]     ; edx=category(arg_0)
    //   465374  81 EC CC 00 00 00  sub esp, 0CCh
    // 進入時：ecx=this(DNameNode*)，[esp+4]=category，[esp+8]=key，函式尾端
    // retn 8，純thiscall+2個stack參數。10 bytes足夠塞5-byte jmp，剩5 bytes補NOP。
    static const DWORD kVA_Entry = 0x00465370;
    static const DWORD kVA_ReturnTo = 0x0046537A;
    static const BYTE kExpected[10] = { 0x8B, 0x54, 0x24, 0x04, 0x81, 0xEC, 0xCC, 0x00, 0x00, 0x00 };

    // ---- Path A/B/C上游呼叫端紀錄 ----
    //
    // sub_465370的2參數呼叫慣例（category在前、key在後、retn 8）在下列所有
    // 呼叫端都完全一致，無法從參數格式/數量分辨用途，只能靠呼叫端的return
    // address（進入GetTextDetour當下[esp]的值）識別是「誰」呼叫的：
    //
    //   Path A（一般UI顯示文字，查到就該翻譯）：
    //     sub_5B4730，return addr=0x5b47a9
    //     例：category="AllLevels/Actions" key="PickupClothes"
    //
    //   Path B（UI顯示文字+額外tag，本質上是Path A的變形）：
    //     sub_6BC560，M04任務腳本。呼叫慣例跟Path A完全相同，不需要特殊處理
    //     （跟著熱鍵開關正常翻譯）。return addr=0x6bc709（ReviveCIAAgent分支）、
    //     0x6bcbe2（SabotageGas/FixGas分支）。
    //
    //   Path C（引擎內部字面字串，回傳值被當成檔案系統路徑/檔名組件使用，
    //     絕對不能被翻譯取代——熱鍵ON時fallback或命中的譯文會混進路徑/檔名，
    //     破壞存檔導致「找不到存檔」／「儲存失敗」）：
    //     - sub_6753B0：存檔資料夾路徑組合（Documents\Hitman Blood Money\<這裡>\），
    //       category="AllLevels/Interface" key="Profiles"，return addr=0x675464。
    //     - sub_675520：組完整存檔檔名 <Profiles>\<profile名稱>\<Profile>.pro，
    //       key="Profile"（單數，跟"Profiles"是不同key），return addr=0x6755B4，
    //       回傳值直接接在.pro副檔名前。
    //     - sub_675F10：獨立函式，自己直接呼叫GetText再組檔名（key="Profile"
    //       回傳值接在.pro前），走「關卡結束後」的存檔流程，return addr=0x675F9D。
    //
    // 已知Path C呼叫端列在下面；如果之後又發現類似「回傳值被拿去做UI顯示以外
    // 用途」的call site，比照這裡追加return addr。
    //
    // 另有一套語意相反的白名單kReturnKeyOnMissReturnAddrs（定義在LookupText
    // 前）——「回傳值被動作提示widget留存」型呼叫端，那批是「照常翻譯，但
    // 指標要永久＋miss回key不回空字串」，跟Path C的「絕不翻譯、強制
    // passthrough」不同。見md「原文譯文切換」第13節。
    static const DWORD kPathCReturnAddrs[] = { 0x00675464, 0x006755B4, 0x00675F9D };

    // ---- 存在性探測型呼叫端（回傳值只做strcmp、不顯示，強制passthrough）----
    //
    // 跟Path C機制相同（一律回native原生結果、不翻譯），但理由不同：這批
    // 呼叫端把GetText回傳值拿去跟哨兵字串Caption（位址0x00752515，空字串）
    // 做strcmp，用「有沒有這個key」決定native內部的後續分支，回傳值本身
    // 從不進畫面。我方若照常回譯文，會把native誤導成「key存在」翻掉它的
    // fallback邏輯。
    //
    //   0x006708E3 = sub_670630 內 if(Src) 區塊的「m00/Tutorials/<page>
    //     存不存在」探測（category="m00/Tutorials/55_Splash_01"、key=""）。
    //     native原意：裸key查無 → strcmp(結果,"")==0 → 補平台後綴"_PC"改查
    //     m00/Tutorials/<page>_PC。我方表有裸key entry時會讓這個strcmp變
    //     false、native不再補"_PC"，擾動它組教學slot key/id的字串。
    //     splash實際顯示文字改由SplashCaptionDetour（site 0x0067069C）處理，
    //     這筆探測交還native即可。見md「原文譯文切換」splash節。
    static const DWORD kForcePassthroughReturnAddrs[] = { 0x006708E3 };

    // __cdecl，供naked detour呼叫。偵測到Path C／存在性探測白名單內的呼叫端
    // 時回傳non-zero，detour據此強制passthrough——不管熱鍵是否為ON，一律回傳
    // native原生查詢結果，避免引擎內部路徑組件字串被誤翻譯、或探測型strcmp
    // 被誤導。
    static int __cdecl IsPathCCaller(DWORD retAddr)
    {
        for (DWORD addr : kPathCReturnAddrs)
        {
            if (addr == retAddr)
            {
                if (g_diagEnable)
                    Log::Write("[LocHook] 偵測到Path C呼叫端(retAddr=0x%08X)，強制passthrough不翻譯", retAddr);
                return 1;
            }
        }
        for (DWORD addr : kForcePassthroughReturnAddrs)
        {
            if (addr == retAddr)
            {
                if (g_diagEnable)
                    Log::Write("[LocHook] 偵測到存在性探測呼叫端(retAddr=0x%08X)，強制passthrough交還native", retAddr);
                return 1;
            }
        }
        return 0;
    }

    // switch off時：完全passthrough，原樣重跑被偷走的2句指令再跳回原函式
    // 剩餘部分，ecx全程沒被我們動過，native後續邏輯（this+4查找/slot1呼叫）
    // 不受影響。switch on時：先檢查是不是已知的Path C呼叫端，是的話一樣
    // 強制passthrough（不翻譯）；否則用LookupText的回傳值+retn 8取代整個
    // 原函式，不再繼續執行native任何查找邏輯。
    static __declspec(naked) void GetTextDetour()
    {
        __asm
        {
            push ecx
            call EnsureHotkeyPollInstalled
            pop  ecx

            ; ecx全程要維持是native傳進來的this（DNameNode*，passthrough路徑
            ; 跳回native剩餘程式碼後還會用到），IsPathCCaller是一般C++函式、
            ; 沒有義務保留ecx，呼叫前後必須自行push/pop保護——這裡先把
            ; retAddr讀進eax（此時esp仍是進入本函式時的狀態，[esp]=retAddr），
            ; 再push ecx保護，push eax當參數呼叫，呼叫完pop ecx還原。
            mov  eax, [esp]
            push ecx
            push eax
            call IsPathCCaller
            add  esp, 4
            pop  ecx
            test eax, eax
            jnz  passthrough

            cmp  byte ptr [g_switchOn], 0
            jz   passthrough

            ; LookupText多吃一個retAddr參數（純供miss診斷log用，見LookupText
            ; 前方註解）。此時esp仍是進入本函式時的
            ; 狀態（前面IsPathCCaller那段push/pop已還原），[esp]=retAddr、
            ; [esp+4]=category、[esp+8]=key；ecx這裡開始不需要再保留this
            ; （接下來直接retn 8跳過整個native剩餘邏輯，不會用到），可以
            ; 放心當暫存器用。
            mov  eax, [esp]
            mov  ecx, [esp + 4]
            mov  edx, [esp + 8]
            push eax
            push edx
            push ecx
            call LookupText
            add  esp, 12
            retn 8

        passthrough:
            mov  edx, [esp + 4]
            sub  esp, 0CCh
            jmp  kVA_ReturnTo
        }
    }

    // ---- sub_670630 教學/splash caption 覆寫 (site 0x0067069C) ----
    //
    // 背景（見 md「原文譯文切換」）：教學 splash 頁（m00/Tutorials/55_Splash_01
    // ～61_Splash_07）的標題＋內文是由 mission script 以「英文字面字串」透過
    // sub_670630 的 String2 參數傳入，sub_4652A0 判定它不是 DName key → 不查
    // loc → 直接畫英文。上面的 GetText hook 在這條路徑上只會被 sub_670630 深處
    // 那筆「m00/Tutorials/<page> 存不存在」的探測（retAddr=0x006708E3）呼叫到，
    // 該筆結果只餵給 strcmp、不會顯示，所以翻了也沒用。
    //
    // 修法：在 sub_670630 內 v27（最終要畫的字串指標）定案的匯流點 0x0067069C
    // 攔一刀。此處 String2→sub_4652A0→resolve 的分支已跑完，native 用
    // 「mov [esp+0x14], eax」把 v27 存在該處堆疊槽；Src（第 8 參數，頁 ID 如
    // "55_Splash_01"）在 [ebp+0x20]；ebx=this。熱鍵為譯文模式、Src 非空、且
    // g_translations 有 "m00/Tutorials/<Src>" 時，把 v27 直接覆寫成譯文指標。
    // Src 為空（目標提示鏈、Caption reset 呼叫）時原樣放行，不影響既有行為。
    //
    // ⚠️ v27 一定要用 esp-relative 定址（跟 native 那句 store 一致）。
    // sub_670630 序言有 and esp,0FFFFFFF8h，esp↔ebp 差距隨 caller 對齊浮動，
    // IDA 標的 [ebp-0x45C] 只在 ebp 8-aligned 時才等於該槽，用 ebp 寫在對齊
    // 不成立時會打到隔壁 dword、覆寫等於沒生效。
    //
    // 下游安全：sub_670630 對 v27 一律是同步 byte-copy 進 v32[1..]（~524 byte
    // 固定 stack buffer）或 append 進區域 std::string，不留存我方指標；且
    // strlen(v27) >= 0x200 會觸發 __debugbreak()，ResolveSplashCaption 對逼近
    // 上限的譯文回 nullptr（退回英文、不覆寫）當保險。
    static const DWORD kVA_SplashSite     = 0x0067069C;
    static const DWORD kVA_SplashResumeTo = 0x006706A2;
    static const BYTE  kSplashExpected[6] = { 0x8A, 0x83, 0xF0, 0x3E, 0x00, 0x00 }; // mov al,[ebx+3EF0h]

    // __cdecl，供 SplashCaptionDetour 呼叫。src = sub_670630 第 8 參數（Src），
    // 教學/splash 頁 ID。命中且長度安全時回傳 arena 穩定指標，否則回 nullptr
    // （detour 據此決定要不要覆寫 v27）。
    static const char* __cdecl ResolveSplashCaption(const char* src)
    {
        if (!g_switchOn || !src || !src[0]) return nullptr;

        // src 指向 native 記憶體裡的短字串；長度異常直接放棄，避免組 key 失控。
        size_t srcLen = 0;
        while (srcLen < 128 && src[srcLen]) srcLen++;
        if (srcLen == 0 || srcLen >= 128) return nullptr;

        LoadIfNeeded();

        std::string combined = "m00/Tutorials/";
        combined.append(src, srcLen);

        // 譯文檔對這批教學頁的 key 命名不一致：55～60 用裸 key
        // （m00/Tutorials/60_Splash_06），01_Control／61_Splash_07 則沿用原生
        // loc 的平台後綴 key（m00/Tutorials/61_Splash_07_PC）。裸 key 查無時補
        // "_PC" 再查一次——跟 native sub_670630 對這批 key 的 _PC fallback
        // 慣例一致（off_7A19F4="_PC"）。
        auto it = g_translations.find(combined);
        if (it == g_translations.end())
        {
            it = g_translations.find(combined + "_PC");
            if (it == g_translations.end()) return nullptr;
        }

        // sub_670630：strlen(v27) >= 0x200 會 __debugbreak()，且會逐 byte 複製
        // 進固定 stack buffer。譯文逼近上限時寧可不翻、退回英文原文。
        if (it->second.size() >= 0x1F0) return nullptr;

        if (g_diagEnable)
            Log::Write("[LocHook] splash caption 覆寫：%s -> %s", it->first.c_str(), it->second.c_str());

        // v27 之後由 native 同步複製，理論上不需延長壽命；仍走 arena（跟動作
        // 提示 widget 白名單同款保險），指標壽命撐到任務目錄段邊界。
        return InternToArena(it->second);
    }

    // jmp（E9）進入，ESP 與 native 在 0x0067069C 當下完全相同（跟 native 那句
    // 「mov [esp+0x14], eax」的 esp 同值）。只保護會被 __cdecl 呼叫弄髒的
    // eax/ecx/edx（ebx/esi/edi/ebp 由被呼叫端保留）；用 push imm32 / ret 跳回，
    // 避免載入目標位址時破壞被偷指令寫好的 al。
    //
    // 堆疊：進入時 v27 槽 = [esp+0x14]。3 次 push（0xC）後 → [esp+0x20]。
    static __declspec(naked) void SplashCaptionDetour()
    {
        __asm
        {
            push eax
            push ecx
            push edx
            mov  eax, [ebp + 20h]        ; Src（sub_670630 第 8 參數，不受 and esp 影響）
            test eax, eax
            jz   splash_done
            push eax
            call ResolveSplashCaption    ; __cdecl，回 char* 或 0
            add  esp, 4
            test eax, eax
            jz   splash_done
            mov  [esp + 20h], eax        ; 覆寫 v27（= 進入時 [esp+0x14]，+0xC 補 3 次 push）
        splash_done:
            pop  edx
            pop  ecx
            pop  eax
            mov  al, [ebx + 3EF0h]       ; 被偷的原指令（ebx=this）
            push 006706A2h               ; = kVA_SplashResumeTo；push/ret 跳回，不碰暫存器
            ret
        }
    }

    void Install(HMODULE hModule)
    {
        Config::LocSwitchConfig cfg = Config::LoadLocSwitch(hModule);
        g_enable = cfg.enable;
        g_hotkeyVK = cfg.hotkeyVK;

        Config::DebugConfig dbg = Config::LoadDebug(hModule);
        g_diagEnable = dbg.locSwitchDiagEnable;

        Config::HookToggleConfig toggles = Config::LoadHookToggle(hModule);
        LocNewspaperFix::Configure(toggles.newsPaperSoftBreakFix, Config::LoadNewsSoftBreakInterval(hModule));

        BYTE* p = (BYTE*)kVA_Entry;
        for (int i = 0; i < 10; i++)
        {
            if (p[i] != kExpected[i])
            {
                Log::Write("[LocHook] 0x%08X第%d byte是0x%02X，預期0x%02X——版本不符或已被其他patch動過，放棄安裝",
                           kVA_Entry, i, p[i], kExpected[i]);
                return;
            }
        }

        DWORD oldProt = 0;
        VirtualProtect(p, 10, PAGE_EXECUTE_READWRITE, &oldProt);
        p[0] = 0xE9;
        *(INT32*)(p + 1) = (INT32)((DWORD)&GetTextDetour - (DWORD)(p + 5));
        p[5] = p[6] = p[7] = p[8] = p[9] = 0x90;
        VirtualProtect(p, 10, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), p, 10);

        Log::Write("[LocHook] sub_465370(GetText slot0) entry hook安裝完成：site=0x%08X detour=%p enable=%d hotkeyVK=0x%02X",
                   kVA_Entry, &GetTextDetour, g_enable, g_hotkeyVK);

        // sub_670630 splash caption 覆寫（獨立 site、獨立 byte 檢查，跟上面的
        // GetText entry hook 不互相依賴）。
        BYTE* q = (BYTE*)kVA_SplashSite;
        bool splashMatch = true;
        for (int i = 0; i < 6; i++)
        {
            if (q[i] != kSplashExpected[i])
            {
                Log::Write("[LocHook] 0x%08X第%d byte是0x%02X，預期0x%02X——版本不符或已被其他patch動過，放棄安裝splash覆寫",
                           kVA_SplashSite, i, q[i], kSplashExpected[i]);
                splashMatch = false;
                break;
            }
        }
        if (splashMatch)
        {
            DWORD oldProtSplash = 0;
            VirtualProtect(q, 6, PAGE_EXECUTE_READWRITE, &oldProtSplash);
            q[0] = 0xE9;
            *(INT32*)(q + 1) = (INT32)((DWORD)&SplashCaptionDetour - (DWORD)(q + 5));
            q[5] = 0x90;
            VirtualProtect(q, 6, oldProtSplash, &oldProtSplash);
            FlushInstructionCache(GetCurrentProcess(), q, 6);

            Log::Write("[LocHook] sub_670630 splash caption 覆寫hook安裝完成：site=0x%08X detour=%p resumeTo=0x%08X",
                       kVA_SplashSite, &SplashCaptionDetour, kVA_SplashResumeTo);
        }
    }
}
