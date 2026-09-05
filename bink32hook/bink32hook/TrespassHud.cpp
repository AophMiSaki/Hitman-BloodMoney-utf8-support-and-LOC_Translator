#include "TrespassHud.h"
#include "Config.h"
#include "Log.h"
#include "WarningStrings.h"
#include "SubtitleRender.h"
#include "SubtitleGate.h"
#include <string>

namespace TrespassHud
{
    // native 全域關卡控制器單例 → +0xA40 = Hitman actor（可為 0）。
    static const DWORD kLevelCtrlSingletonPtr = 0x0082083C;
    static const DWORD kActorOffset           = 0xA40;

    // actor +0x0B7C：HUD 狀態旗標字，bit 27 = 擅闖（native 每幀重建）。
    static const DWORD kActorHudFlags   = 0x0B7C;
    static const DWORD kTrespassBit     = 0x08000000;

    // sub_5F29B0 = GetCurrentZoneKind()，__thiscall(actor) → ESecurityZone 整數值。
    typedef int(__thiscall* GetCurrentZoneKind_t)(void* actor);
    static const GetCurrentZoneKind_t GetCurrentZoneKind = (GetCurrentZoneKind_t)0x005F29B0;

    enum WarnLevel { WARN_NONE = 0, WARN_TRESPASS, WARN_HOSTILE };

    // ESecurityZone 是單一 bit 值：eZone1=1 eZone2=2 eZone2A=4 eZone2B=8
    // eZone3=16 eZone3A=32 eZone3B=64 eZoneMegaForbidden=128。目前只有
    // eZoneMegaForbidden 顯示 Hostile Area，其餘擅闖旗標命中一律顯示
    // Trespassing（eZone3A/3B 是否算敵對區，待與警衛警戒狀態一起判定，
    // 見元素 B）。
    static const int kZoneMegaForbidden = 0x80;

    static WarnLevel ClassifyZone(int zoneKind)
    {
        return (zoneKind & kZoneMegaForbidden) ? WARN_HOSTILE : WARN_TRESPASS;
    }

    // ---- 設定 ----
    static bool  g_enabled  = false;
    static bool  g_diag     = false;
    static float g_posX     = 0.5f;   // 螢幕比例 0~1
    static float g_posY     = 0.86f;

    // ---- 目前狀態（Tick 算、Draw 用）----
    static WarnLevel g_level = WARN_NONE;

    static WarnLevel Evaluate()
    {
        DWORD ctrl = *(DWORD*)kLevelCtrlSingletonPtr;
        if (!ctrl) return WARN_NONE;
        DWORD actor = *(DWORD*)(ctrl + kActorOffset);
        if (!actor) return WARN_NONE;

        if (!(*(DWORD*)(actor + kActorHudFlags) & kTrespassBit)) return WARN_NONE;

        int zoneKind = GetCurrentZoneKind((void*)actor);
        WarnLevel lv = ClassifyZone(zoneKind);

        if (g_diag)
            Log::Write("[TrespassHud][debug] trespass bit=1 zoneKind=%d -> %s",
                       zoneKind, lv == WARN_HOSTILE ? "HOSTILE" : "TRESPASS");
        return lv;
    }

    // ---- SubtitleRender producer ----

    static bool Tick()
    {
        // 過場動畫／腳本序列字幕進行中（SubtitleGate m_OSD+0x69），或純運鏡
        // 無對白靠 actor+0x746 操作鎖補漏 → 整個擅闖提示不畫，讓位給過場
        // 畫面。比照 MinimapHud::Tick()。
        if (SubtitleGate::IsScriptedSubtitleBlocking() || SubtitleGate::IsPlayerControlsLocked() ||
            SubtitleGate::IsKnownEndingCutsceneActive())
        {
            g_level = WARN_NONE;
            return false;
        }

        g_level = Evaluate();
        return g_level != WARN_NONE;
    }

    static void Draw(const SubtitleRender::Frame& frame)
    {
        if (g_level == WARN_NONE) return;

        const char* category = (g_level == WARN_HOSTILE) ? "HOSTILE" : "TRESPASSING";
        std::string line = WarningStrings::Pick(category);

        float centerX = (float)frame.width * g_posX;
        float baselineY = (float)frame.height * g_posY;
        SubtitleRender::DrawLine(frame.device, line, centerX, baselineY, frame.glyphTex, 0xFFFF3030u);
    }

    void EnsureRenderReady()
    {
        if (!g_enabled) return;
        SubtitleRender::EnsureHookInstalled();
    }

    void Install(HMODULE hModule)
    {
        Config::HudConfig cfg = Config::LoadHud(hModule);
        g_enabled = cfg.trespassingEnabled;
        g_diag    = Config::LoadDebug(hModule).hudDiagEnable;
        g_posX    = cfg.trespassPosX / 100.0f;
        g_posY    = cfg.trespassPosY / 100.0f;

        if (!g_enabled)
        {
            Log::Write("[TrespassHud] Install：[Hud] TrespassingEnabled=0，跳過");
            return;
        }

        WarningStrings::EnsureLoaded(hModule);
        SubtitleRender::Register({ &Tick, &Draw });

        Log::Write("[TrespassHud] Install完成：pos=%.1f%%,%.1f%% diag=%d 字串數=%zu",
                   cfg.trespassPosX, cfg.trespassPosY, g_diag, WarningStrings::Count());
    }
}
