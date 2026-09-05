#include "SubtitleHook.h"
#include "Config.h"
#include "Log.h"
#include "SubtitleRender.h"
#include "SubtitleGate.h"
#include "SubtitleBrief.h"
#include "SubtitleBriefSpeech.h"
#include "SubtitleDialogue.h"
#include "SubtitleOneliners.h"
#include "SubtitleWalkie.h"
#include "SubtitleTvRadio.h"

namespace SubtitleHook
{
    // 任一字幕開關啟用（見 Install()）＝需要驅動 SubtitleRender 的唯一
    // EndScene compositor lazy retry。GlyphCheck() 每幀查，快取在這裡。
    static bool g_anySubtitleEnable = false;

    void Install(HMODULE hModule)
    {
        bool briefingEnable  = Config::LoadBriefingSubtitleEnabled(hModule);
        bool dialogueEnable  = Config::LoadDialogueSubtitleEnabled(hModule);
        bool onelinersEnable = Config::LoadOnelinersSubtitleEnabled(hModule);
        bool walkieEnable    = Config::LoadWalkieSubtitleEnabled(hModule);
        bool tvRadioEnable   = Config::LoadTvRadioSubtitleEnabled(hModule);
        g_anySubtitleEnable  = briefingEnable || dialogueEnable || onelinersEnable || walkieEnable || tvRadioEnable;

        Log::Write("[SubtitleHook] Install：BriefingSubtitleEnabled=%d DialogueSubtitleEnabled=%d OnelinersSubtitleEnabled=%d WalkieSubtitleEnabled=%d TvRadioSubtitleEnabled=%d",
                   briefingEnable, dialogueEnable, onelinersEnable, walkieEnable, tvRadioEnable);

        // ---- 開場簡報字幕 ----
        // SubtitleBriefSpeech::Install() 必須先於 SubtitleBrief::Install()，
        // 讓 GetSpeechStartEventId() 一開始就有效。
        if (briefingEnable)
        {
            SubtitleBriefSpeech::Install(hModule);
            SubtitleBrief::Install(hModule);
        }
        else
            Log::Write("[SubtitleHook] BriefingSubtitleEnabled=0，跳過 SubtitleBriefSpeech/SubtitleBrief 安裝");

        // ---- Dialogue／Oneliners／Walkie 對話喊話字幕 ----
        // SubtitleGate（8 態狀態機＋距離過濾＋首選/第三 self-render producer）
        // 與 native 字幕 watchdog 側錄是這三者共用基礎設施，任一啟用就要備妥。
        if (dialogueEnable || onelinersEnable || walkieEnable || tvRadioEnable)
        {
            SubtitleGate::Init(hModule);
            SubtitleGate::InstallNativeSubtitleWatchdog();
        }
        else
            Log::Write("[SubtitleHook] Dialogue/Oneliners/Walkie/TvRadio 皆停用，跳過 SubtitleGate 初始化與 native 字幕 watchdog 側錄");

        if (dialogueEnable)
            SubtitleDialogue::Install();
        else
            Log::Write("[SubtitleHook] DialogueSubtitleEnabled=0，跳過 sub_6AACA0(Dialogue) 觸發點 detour");

        if (onelinersEnable)
            SubtitleOneliners::Install();
        else
            Log::Write("[SubtitleHook] OnelinersSubtitleEnabled=0，跳過 sub_6A4240+sub_6BACC0(Oneliners) 觸發點 detour");

        if (walkieEnable)
            SubtitleWalkie::Install();
        else
            Log::Write("[SubtitleHook] WalkieSubtitleEnabled=0，跳過 sub_6C0220(對講機) 觸發點 detour");

        if (tvRadioEnable)
            SubtitleTvRadio::Install();
        else
            Log::Write("[SubtitleHook] TvRadioSubtitleEnabled=0，跳過 sub_4C5E10(電視/收音機廣播) 觸發點 detour");
    }

    void EnsureRenderReady()
    {
        if (!g_anySubtitleEnable) return;
        SubtitleRender::EnsureHookInstalled();
    }
}
