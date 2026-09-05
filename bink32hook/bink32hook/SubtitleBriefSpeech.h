#pragma once
#include <Windows.h>

// 開場簡報語音真正開始播放的訊號來源。
//
// 在sub_4EA670(建聲音channel共用函式)入口用returnAddr判斷呼叫端——命中
// sub_59FA90(M00文字驅動語音)或sub_6CEA40(全任務通用旁白queue player)才
// 遞增GetSpeechStartEventId()，命中sub_6849B0(訓練提示音效，無文字參數)
// 只留診斷log、不觸發。同時sub_59FA90自己入口另有side-record記錄String2
// 原始文字內容——是MissionBriefing分類的key(例如
// "/M00/MissionBriefing/M00_MB_01D")，跟SubtitleBrief現有g_lines
// 來源一致。
//
// sub_59FA90/sub_6CEA40兩路徑互斥（M00走前者、其他任務走後者），逐句
// 排程共用同一個GetSpeechStartEventId()計數器即可。
namespace SubtitleBriefSpeech
{
    // 固定VA、DLL載入當下就能裝，不依賴任何runtime才建立的物件（跟
    // TextureManagerReleaseHook/Utf8ReencodeFixHook同款不需要lazy retry）。
    // hModule 只用來讀 [Debug] DebugSubtitleDiagEnable。
    void Install(HMODULE hModule);

    // 語音真正觸發事件的遞增序號，0代表本場景尚未有任何觸發。每次觸發都
    // 會遞增（不會重複），呼叫端只要記住上次看到的值、發現數值變了就代表
    // 有新的一句語音觸發，藉此支援同一場景內多次觸發各自重新起算顯示時間
    // 窗。涵蓋sub_59FA90(M00)跟sub_6CEA40(其他任務)兩條互斥路徑。
    DWORD GetSpeechStartEventId();
}
