#include "WarningHud.h"
#include "Config.h"
#include "Log.h"
#include "WarningStrings.h"
#include "SubtitleRender.h"
#include "SubtitleGate.h"
#include <string>

namespace WarningHud
{
    // native 全域關卡控制器單例 → +0xA4C = ZOSD（HUD 物件，可為 0）。
    static const DWORD kLevelCtrlSingletonPtr = 0x0082083C;
    static const DWORD kZosdOffset            = 0xA4C;

    // ZOSD +0x18C：警覺條緩動後顯示值（float 0..100），sub_6601D0 每幀寫。
    static const DWORD kZosdTensionEased = 0x18C;

    // 門檻＝原生條色切換用的 exe 硬編碼常數（flt_76944C / flt_76122C）。
    static const float kThresholdAlerted    = 75.0f;
    static const float kThresholdSuspicious = 50.0f;

    // 對齊原生警覺條色：紅 = 戰鬥、黃 = 懷疑（ARGB）。
    static const DWORD kColorAlerted    = 0xFFDA0000u;
    static const DWORD kColorSuspicious = 0xFFE9C616u;

    enum Level { LV_NONE = 0, LV_SUSPICIOUS, LV_ALERTED };

    // ---- 設定 ----
    static bool  g_enabled = false;
    static bool  g_diag    = false;
    static float g_posX    = 0.5f;    // 螢幕比例 0~1
    static float g_posY    = 0.05f;

    // ---- 目前狀態（Tick 算、Draw 用）----
    static Level g_level = LV_NONE;

    static Level Evaluate()
    {
        DWORD ctrl = *(DWORD*)kLevelCtrlSingletonPtr;
        if (!ctrl) return LV_NONE;
        DWORD zosd = *(DWORD*)(ctrl + kZosdOffset);
        if (!zosd) return LV_NONE;

        float lvl = *(float*)(zosd + kZosdTensionEased);

        Level lv = LV_NONE;
        if (lvl >= kThresholdAlerted)         lv = LV_ALERTED;
        else if (lvl >= kThresholdSuspicious) lv = LV_SUSPICIOUS;

        if (g_diag && lv != LV_NONE)
            Log::Write("[WarningHud][debug] tension=%.1f -> %s",
                       lvl, lv == LV_ALERTED ? "ALERTED" : "SUSPICIOUS");
        return lv;
    }

    // ---- SubtitleRender producer ----

    static bool Tick()
    {
        // 過場動畫／腳本序列字幕進行中（SubtitleGate m_OSD+0x69），或純運鏡
        // 無對白靠 actor+0x746 操作鎖補漏 → 整個警覺提示不畫，讓位給過場
        // 畫面。比照 MinimapHud::Tick()。
        if (SubtitleGate::IsScriptedSubtitleBlocking() || SubtitleGate::IsPlayerControlsLocked() ||
            SubtitleGate::IsKnownEndingCutsceneActive())
        {
            g_level = LV_NONE;
            return false;
        }

        g_level = Evaluate();
        return g_level != LV_NONE;
    }

    static void Draw(const SubtitleRender::Frame& frame)
    {
        if (g_level == LV_NONE) return;

        const char* category = (g_level == LV_ALERTED) ? "ALERTED" : "SUSPICIOUS";
        DWORD color = (g_level == LV_ALERTED) ? kColorAlerted : kColorSuspicious;
        std::string line = WarningStrings::Pick(category);

        float centerX   = (float)frame.width  * g_posX;
        float baselineY = (float)frame.height * g_posY;
        SubtitleRender::DrawLine(frame.device, line, centerX, baselineY, frame.glyphTex, color);
    }

    void EnsureRenderReady()
    {
        if (!g_enabled) return;
        SubtitleRender::EnsureHookInstalled();
    }

    void Install(HMODULE hModule)
    {
        Config::HudConfig cfg = Config::LoadHud(hModule);
        g_enabled = cfg.warningEnabled;
        g_diag    = Config::LoadDebug(hModule).hudDiagEnable;
        g_posX    = cfg.warningPosX / 100.0f;
        g_posY    = cfg.warningPosY / 100.0f;

        if (!g_enabled)
        {
            Log::Write("[WarningHud] Install：[Hud] WarningEnabled=0，跳過");
            return;
        }

        WarningStrings::EnsureLoaded(hModule);
        SubtitleRender::Register({ &Tick, &Draw });

        Log::Write("[WarningHud] Install完成：pos=%.1f%%,%.1f%% diag=%d 字串數=%zu",
                   cfg.warningPosX, cfg.warningPosY, g_diag, WarningStrings::Count());
    }
}
