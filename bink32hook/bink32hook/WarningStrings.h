#pragma once
#include <Windows.h>
#include <string>

// HUD 警示字串共用表（warningString.txt）。TrespassHud（元素 A）與 WarningHud
// （元素 B，SUSPICIOUS/ALERTED AI 刺激字）共用同一份表：各自在 Install() 呼
// EnsureLoaded() 一次，繪製時呼 Pick()。
//
// 檔案路徑 <遊戲目錄>\bink32hook\warningString.txt（Paths::GetDataDir 推導，
// 不寫死絕對路徑）；缺檔自動寫入內建預設；編碼 UTF-8（讀取去 BOM）；格式沿用
// LocTxtParser 巢狀括號，攤平成 category/key -> value。開檔或解析失敗都退回
// 內建預設，功能不因缺字串失效。

namespace WarningStrings
{
    // 冪等：首次呼叫確保預設檔存在並讀檔解析，之後直接 return。多個 HUD 模組
    // 都可安全呼叫。
    void EnsureLoaded(HMODULE hModule);

    // category（如 "TRESPASSING" / "HOSTILE" / "SUSPICIOUS" / "ALERTED"）依
    // F11 翻譯開關選 zh / en；查無 zh 退回 en，再查無退回 category 字面值。
    std::string Pick(const char* category);

    // 已載入的 category/key 條目數（供 Install log 用）。
    size_t Count();
}
