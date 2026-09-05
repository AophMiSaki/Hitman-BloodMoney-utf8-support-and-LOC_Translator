#include "SubtitleBrief.h"
#include "Log.h"
#include "Config.h"
#include "ZipPathTrace.h"
#include "SubtitleBriefSpeech.h"
#include "LocHook.h"
#include "GlyphAtlas.h"
#include "FontCategory.h"
#include "SubtitleRender.h"
#include "SubtitleGate.h"
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdint>

// 字幕文字查LOC（key格式"{missionId}/MissionBriefing/{missionId}_MB_{idx}D"），
// 跟隨F11熱鍵：on查LocHook::TryLookup既有譯文map；off直接呼叫native
// 拿遊戲自己已載入的原文。語音由native自行觸發，本模組被動觀察、不主動
// 呼叫PlaySpeech。顯示排程用逐句真實開始tick：下一句真開始=上一句結束，
// 只有最後一句用原文長度×0.05+2.0秒估算保底。

namespace SubtitleBrief
{
    static HMODULE g_hModule = nullptr;
    static char g_lastZipPath[MAX_PATH] = {};
    static std::vector<std::string> g_lines;
    static bool g_hasContent = false;

    // 是否顯示native缺失的簡報字幕，整個功能的總開關。=0時Install()不向
    // SubtitleRender註冊producer，也就不會有任何查詢/lazy字型init/繪製發生。
    static bool g_subtitleExpandEnable = true;

    // 細節診斷log開關（場景切換/F11熱鍵觸發點追蹤、重試逾時）。「安裝成功/
    // 失敗」跟「查到內容/沒查到內容」不受這個旗標限制，一律顯示。log都帶
    // `[SubtitleBrief]`前綴。預設關閉。
    static bool g_diagEnable = false;

    // 簡報字幕self-render疊加位置（螢幕百分比，0~1）：讀首選欄位
    // [Subtitle] SubtitlePlaceFirst，預設水平置中、垂直80%。
    static float g_firstPlaceX = 0.5f;
    static float g_firstPlaceY = 0.8f;

    // zip路徑一偵測到變化就立刻查LOC簡報文字，但zip剛切換的當下距離場景
    // 真正載入完成還有20幾秒落差，native的LOC查詢服務這時還沒備妥、第一次
    // 查詢必定落空。故查無資料時在時限內定期重試，而不是查一次就永久放棄。
    static char g_pendingMissionId[16] = {};
    static bool g_pendingRetry = false;
    static ULONGLONG g_zipSwitchTick = 0;
    static ULONGLONG g_lastRetryTick = 0;
    static const DWORD kRetryIntervalMs = 500;
    static const DWORD kRetryTimeoutMs = 60000;

    // 記錄本輪retry迴圈嘗試次數，完成log只在retry迴圈真正結束（查到內容/
    // 逾時放棄）才印一次，避免最長60秒重試期間每500ms洗一行版。
    static DWORD g_pollAttemptCount = 0;

    // 每句各自的真實開始tick（見PollLineStart）：句N結束時間＝句N+1真實
    // 開始時間（native queue player逐句遞增、不會跳號，下一句一開始播就
    // 代表上一句播完）。只有「目前已知的最後一句」還沒等到下一句訊號時，
    // 才用原文長度估算當保底duration（EstimateLineDurationMs），下一句訊號
    // 一到就立刻被真實時間取代。
    static const int kMaxLines = 32;
    static ULONGLONG g_lineStartTick[kMaxLines] = {};
    static int g_lineStartCount = 0;
    static DWORD g_lastSpeechEventId = 0;

    // 修正字幕過早結束：只延長每句的可見duration 0.5秒（結束時間後移，
    // 開始時間不動），不整體平移時間窗——跟下一句的重疊區間內兩句都會
    // 同時顯示。只影響顯示排程，不影響語音本身。
    static const DWORD kSubtitleDelayMs = 500;

    // BuildMissionBriefingLines只被PollMissionChange()在場景切換/查詢重試
    // 時呼叫，不會偵測LocHook::IsTranslationActive()狀態變化，切成
    // 譯文後字幕還是沿用切換前抓到的那份原文、永遠不會重查。這裡新增一個
    // 獨立輪詢點，偵測到狀態翻轉就強制重新查一次。
    static bool g_lastTranslationActive = false;

    // 首選欄位（SubtitlePlaceFirst）被 SubtitleGate
    // 的 Dialogue/Oneliners 字幕佔用時，開場簡報字幕整輪讓位、完全不顯示
    // （不降級到次選/第三）。這一輪一旦偵測到被佔就 latch 住，直到下一輪
    // 簡報開始（場景切換，或 PollLineStart 偵測到 g_lineStartCount 歸零後
    // 的第一句）才清除——玩家可再透過 PDA 重看。
    static bool g_suppressedByPrimary = false;

    // 從場景zip路徑（例："SCENES\M00\M00_MAIN.zip"）取出任務資料夾名稱片段
    // （例："M00"）：跳過字首"SCENES\\"，取到下一個'\\'為止。
    static bool ExtractMissionId(const char* zipPath, char* outBuf, size_t bufSize)
    {
        const char* rel = zipPath;
        if (_strnicmp(rel, "SCENES\\", 7) == 0) rel += 7;

        const char* backslash = strchr(rel, '\\');
        size_t len = backslash ? (size_t)(backslash - rel) : strlen(rel);
        if (len == 0 || len >= bufSize) return false;

        memcpy(outBuf, rel, len);
        outBuf[len] = '\0';
        return true;
    }

    // ---- 字幕文字：從LOC查表載入 ----

    // key格式跟native對齊：index<10補零成"0%d"，否則"%d"。
    static void BuildMissionBriefingKey(const char* missionId, int index, char* outBuf, size_t bufSize)
    {
        char idxStr[8];
        if (index < 10) _snprintf_s(idxStr, _TRUNCATE, "0%d", index);
        else _snprintf_s(idxStr, _TRUNCATE, "%d", index);
        _snprintf_s(outBuf, bufSize, _TRUNCATE, "%s/MissionBriefing/%s_MB_%sD", missionId, missionId, idxStr);
    }

    // 原文直接呼叫native sub_6CF850(index)拿遊戲自己已載入的原文，不維護
    // 額外檔案。這個函式不依賴任何特定物件實例（純靠目前任務ID+LOC查詢
    // 服務singleton+傳入的index組key查詢），呼叫時this傳nullptr即可。
    // 查無資料回傳0，查到回傳文字指標。
    typedef const char* (__thiscall* GetBriefingLineFn)(void* thisPtr, int index);
    static const GetBriefingLineFn NativeGetBriefingLine = (GetBriefingLineFn)0x006CF850;

    static bool TryGetNativeOriginalLine(int index, std::string& outText)
    {
        const char* result = NativeGetBriefingLine(nullptr, index);
        if (!result) return false;
        outText = result;
        return true;
    }

    // 保底duration估算，只在「目前已知的最後一句」還沒等到下一句真實開始
    // 訊號時使用。公式沿用native對這類逐句字幕的估算（原文長度×0.05+2.0
    // 秒）——固定用native原文長度，跟F11顯示原文/譯文無關：語音是英文原文
    // 錄的，字數才跟語音實際時長有關聯，畫面顯示的CJK譯文長度跟這無關。
    // lineIndex0Based是g_lines的0-based索引，換算成native LOC key的1-based
    // index。
    static DWORD EstimateLineDurationMs(int lineIndex0Based)
    {
        std::string original;
        if (!TryGetNativeOriginalLine(lineIndex0Based + 1, original))
            return 4000; // 查無原文時的防呆固定值
        return (DWORD)(original.size() * 50 + 2000);
    }

    // 從index=1開始依序查詢，查到就存一行、index+1繼續查，miss就停止
    // （空MissionBriefing區塊直接0行，不當錯誤）。上限32純防呆，避免資料
    // 異常時無限迴圈。跟隨F11熱鍵：on查LocHook既有譯文map，off直接
    // 呼叫native拿遊戲自己載入的原文。
    static void BuildMissionBriefingLines(const char* missionId)
    {
        bool translationActive = LocHook::IsTranslationActive();

        g_lines.clear();
        for (int index = 1; index <= 32; index++)
        {
            std::string text;
            bool found;
            if (translationActive)
            {
                char key[128];
                BuildMissionBriefingKey(missionId, index, key, sizeof(key));
                found = LocHook::TryLookup(key, text);
            }
            else
            {
                found = TryGetNativeOriginalLine(index, text);
            }

            if (!found) break;
            g_lines.push_back(std::move(text));
        }
        g_hasContent = !g_lines.empty();
    }

    // ---- 語音觸發：被動等待native自己呼叫PlaySpeech ----
    //
    // 不主動呼叫PlaySpeech：native會在真正的hint切換時機自己force-replay，
    // 搶在前面播會造成雙重播放。每句真實開始tick改由PollLineStart()觀察
    // SubtitleBriefSpeech的事件計數器。
    static void TryBuildAndTrigger(const char* missionId)
    {
        g_pollAttemptCount++;
        BuildMissionBriefingLines(missionId);
        if (g_hasContent)
        {
            g_pendingRetry = false;
            Log::Write("[SubtitleBrief] 簡報文字查表完成（%s來源）：%s，%zu行 #%lu",
                       LocHook::IsTranslationActive() ? "LOC譯文" : "native原文",
                       missionId, g_lines.size(), g_pollAttemptCount);
        }
    }

    // 用SubtitleBriefSpeech::GetSpeechStartEventId()做edge-detection：每次
    // 語音真正觸發序號都會遞增，這裡偵測到數值變化就照順序記錄該句的真實
    // 開始tick，g_lineStartCount即目前已知有幾句真的開始播放過。跟g_lines
    // 的對應關係倚賴native queue player逐句遞增、不跳號的假設：第k次訊號
    // ＝g_lines[k]。GetSpeechStartEventId()涵蓋兩條語音dispatch路徑（不同
    // 任務走不同路徑），不能只用單一路徑的計數器，否則另一條路徑的任務
    // 永遠等不到line事件、字幕完全不顯示。
    static void PollLineStart()
    {
        if (!g_hasContent) return;

        DWORD eventId = SubtitleBriefSpeech::GetSpeechStartEventId();
        if (eventId == 0 || eventId == g_lastSpeechEventId) return;

        g_lastSpeechEventId = eventId;
        if (g_lineStartCount == 0)
            g_suppressedByPrimary = false; // 新一輪簡報的第一句，清除上輪讓位latch（Tick隨後會依當下首選狀態重新判斷）
        if (g_lineStartCount >= kMaxLines || g_lineStartCount >= (int)g_lines.size())
        {
            if (g_diagEnable)
                Log::Write("[SubtitleBrief] 語音line事件(#%lu)超出目前g_lines行數(%zu)，忽略"
                           "（可能是LOC查表跟native queue不同步）", eventId, g_lines.size());
            return;
        }

        g_lineStartTick[g_lineStartCount] = GetTickCount64();
        if (g_diagEnable)
            Log::Write("[SubtitleBrief] 偵測到句%d真實開始播放(speechEventId=#%lu)，tick=%llu",
                       g_lineStartCount, eventId, g_lineStartTick[g_lineStartCount]);
        else
            Log::Write("[SubtitleBrief] 偵測到句%d真實開始播放", g_lineStartCount);
        g_lineStartCount++;
    }

    // ---- 場景切換偵測：跟LocHook同一個訊號來源（場景zip路徑，見
    // ZipPathTrace::GetLastSceneZipPath()），不另外掛檔案I/O hook ----
    //
    // 去重用完整zip路徑、不是ExtractMissionId取出的頂層資料夾名：同一個
    // 任務資料夾底下會依序切換好幾個不同zip（過場→loader→主場景），
    // missionId全部相同，只用missionId去重會讓第一個之後的所有場景切換
    // （含真正該查詢的主場景）全被誤判成「已處理過」而跳過。查無資料時
    // 進入g_pendingRetry定期重試（見上方註解）。
    static void PollMissionChange()
    {
        const char* zipPath = ZipPathTrace::GetLastSceneZipPath();
        if (!zipPath || !zipPath[0]) return;

        if (_stricmp(zipPath, g_lastZipPath) != 0)
        {
            strncpy_s(g_lastZipPath, zipPath, _TRUNCATE);

            char missionId[16];
            if (!ExtractMissionId(zipPath, missionId, sizeof(missionId)))
            {
                g_pendingRetry = false;
                return;
            }

            if (g_diagEnable)
                Log::Write("[SubtitleBrief] 偵測到場景切換：%s（任務%s）", zipPath, missionId);

            // 字幕字型分類lazy init——偵測到場景切換代表接下來很可能要畫
            // 字幕，觸發一次（冪等）。失敗時DrawLine等處的
            // GlyphAtlas::GetGlyph()會自動fallback回通用字型，不會開天窗。
            GlyphAtlas::EnsureCategoryReady(g_hModule, FontCategory::Subtitle);

            strncpy_s(g_pendingMissionId, missionId, _TRUNCATE);
            g_lineStartCount = 0;
            g_suppressedByPrimary = false; // 新一輪簡報，清除上輪的首選讓位latch
            g_zipSwitchTick = GetTickCount64();
            g_lastRetryTick = g_zipSwitchTick;
            g_pendingRetry = true;
            g_pollAttemptCount = 0;

            TryBuildAndTrigger(missionId);
            return;
        }

        if (!g_pendingRetry) return;

        ULONGLONG now = GetTickCount64();
        if (now - g_zipSwitchTick > kRetryTimeoutMs)
        {
            if (g_diagEnable)
                Log::Write("[SubtitleBrief] 簡報文字查詢重試逾時(%lums)，放棄：%s", kRetryTimeoutMs, g_pendingMissionId);
            Log::Write("[SubtitleBrief] 簡報文字查表完成（%s來源）：%s，%zu行 #%lu",
                       LocHook::IsTranslationActive() ? "LOC譯文" : "native原文",
                       g_pendingMissionId, g_lines.size(), g_pollAttemptCount);
            g_pendingRetry = false;
            return;
        }

        if (now - g_lastRetryTick >= kRetryIntervalMs)
        {
            g_lastRetryTick = now;
            TryBuildAndTrigger(g_pendingMissionId);
        }
    }

    // 偵測LocHook::IsTranslationActive()狀態翻轉（F11熱鍵），命中就
    // 用目前已知的missionId重新查一次g_lines——跟場景切換共用同一個
    // BuildMissionBriefingLines，這裡只是多一個觸發點，不影響原有場景切換
    // /重試邏輯。g_pendingMissionId在場景切換後會一直保留最後解析出的
    // missionId（不會被清空），可以放心在這裡重用。
    static void PollTranslationToggle()
    {
        bool translationActive = LocHook::IsTranslationActive();
        if (translationActive == g_lastTranslationActive) return;

        g_lastTranslationActive = translationActive;
        if (!g_hasContent || !g_pendingMissionId[0]) return;

        if (g_diagEnable)
            Log::Write("[SubtitleBrief] 偵測到F11熱鍵切換（%s），重新查詢字幕文字",
                       translationActive ? "顯示譯文" : "顯示原文");
        BuildMissionBriefingLines(g_pendingMissionId);
        if (g_diagEnable)
            Log::Write("[SubtitleBrief] 簡報文字查表完成（%s來源）：%s，%zu行",
                       translationActive ? "LOC譯文" : "native原文", g_pendingMissionId, g_lines.size());
    }

    // ---- SubtitleRender producer：輪詢 + 繪製 ----
    // 繪製與字串排版由 SubtitleRender 提供，本模組為其 consumer。

    // 每幀呼叫：偵測場景切換 / F11 切換 / 逐句語音開始，回傳這幀是否有句子
    // 落在可見時間窗內。可見區間 = [g_lineStartTick[li], 下一句真實開始 或
    // EstimateLineDurationMs 保底) + kSubtitleDelayMs。
    static bool Tick()
    {
        PollMissionChange();
        PollTranslationToggle();
        PollLineStart();

        // 過場動畫／腳本序列字幕正在驅動 native 字幕（m_OSD+0x69）→ 這幀不畫，
        // 讓位給過場自己的字幕。latch 值不寫 g_suppressedByPrimary，過場結束
        // 自然恢復。
        if (SubtitleGate::IsScriptedSubtitleBlocking())
            return false;

        // 首選欄位被 SubtitleGate 的對話字幕佔用 → 本輪簡報字幕整輪讓位。
        if (!g_suppressedByPrimary && SubtitleGate::IsPrimarySlotBusy())
        {
            g_suppressedByPrimary = true;
            if (g_diagEnable)
                Log::Write("[SubtitleBrief] 首選欄位被對話字幕佔用，本輪簡報字幕讓位不顯示");
        }
        if (g_suppressedByPrimary) return false;

        if (!g_hasContent || g_lines.empty() || g_lineStartCount <= 0) return false;

        ULONGLONG now = GetTickCount64();
        for (int li = 0; li < g_lineStartCount; li++)
        {
            ULONGLONG lineStart = g_lineStartTick[li];
            ULONGLONG lineDuration = (li + 1 < g_lineStartCount)
                                      ? (g_lineStartTick[li + 1] - lineStart)
                                      : EstimateLineDurationMs(li);
            lineDuration += kSubtitleDelayMs;
            if (now - lineStart < lineDuration) return true;
        }
        return false;
    }

    // 只在 Tick() 回 true 的幀、且 render-state 已由 SubtitleRender 設好時
    // 呼叫。逐句重算可見區間只畫落在窗內的行；每句可能被 WrapSubtitleLine
    // 拆成多視覺行，用 rowIndex 累計實際行數排 Y，避免長句換行後行距不一致。
    static void Draw(const SubtitleRender::Frame& frame)
    {
        if (g_suppressedByPrimary) return; // Tick() 已 gate，此處防禦性再擋一次

        ULONGLONG now = GetTickCount64();
        float lineHeight = SubtitleRender::LineHeight();
        float centerX = (float)frame.width * g_firstPlaceX;
        float startY  = (float)frame.height * g_firstPlaceY;
        float maxLineWidthPx = (float)frame.width * 0.8f;

        int rowIndex = 0;
        for (int li = 0; li < g_lineStartCount; li++)
        {
            ULONGLONG lineStart = g_lineStartTick[li];
            ULONGLONG lineDuration = (li + 1 < g_lineStartCount)
                                      ? (g_lineStartTick[li + 1] - lineStart)
                                      : EstimateLineDurationMs(li);
            lineDuration += kSubtitleDelayMs;
            if (now - lineStart >= lineDuration) continue;

            std::vector<std::string> rows = SubtitleRender::WrapSubtitleLine(g_lines[li], maxLineWidthPx);
            for (size_t r = 0; r < rows.size(); r++)
            {
                SubtitleRender::DrawLine(frame.device, rows[r], centerX,
                                         startY + lineHeight * (float)rowIndex, frame.glyphTex);
                rowIndex++;
            }
        }
    }

    void Install(HMODULE hModule)
    {
        g_hModule = hModule;
        g_subtitleExpandEnable = Config::LoadBriefingSubtitleEnabled(hModule);
        g_diagEnable = Config::LoadDebug(hModule).subtitleDiagEnable;

        Config::SubtitlePlaceConfig place = Config::LoadSubtitlePlaceFirst(hModule);
        g_firstPlaceX = place.xPercent / 100.0f;
        g_firstPlaceY = place.yPercent / 100.0f;

        // =0 時不註冊 producer，SubtitleRender 就不會呼叫本模組的繪製。
        if (g_subtitleExpandEnable)
            SubtitleRender::Register({ &Tick, &Draw });

        Log::Write("[SubtitleBrief] Install完成：subtitleExpandEnable=%d diagEnable=%d SubtitlePlaceFirst=%.1f%%,%.1f%%",
                   g_subtitleExpandEnable, g_diagEnable, place.xPercent, place.yPercent);
    }
}
