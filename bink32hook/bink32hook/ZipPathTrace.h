#pragma once
#include <Windows.h>

// sub_441920(engine\system\sysfile.cpp)是路徑字串正規化函式
// （__thiscall int Open(this, const char* name, char forceCreate)，內部把
// name的`\`轉成`/`後，把副檔名改成.zip，再透過vtable虛擬派發呼叫sub_42CE20
// 開檔）。這裡在sub_441920入口做function-entry hook，記錄name結尾是
// .loc/.tex/.zip的每一次呼叫連同呼叫端return address。純觀察，call原函式
// （原地重跑被覆蓋的前兩句指令）取得結果後才記log，不改變任何行為。
//
// GetLastSceneZipPath()的側錄結果是LocHook（原文/譯文路徑推導）、
// SubtitleBrief（場景切換偵測）、報紙分類判斷（NewsPaper*系列）共用的必要
// 資料來源，不是單純除錯用途。安裝開關在[General] ZipPathTrace；
// [Debug] DebugZipPathTrace只控制側錄逐次trace log要不要印，不影響安裝與
// GetLastSceneZipPath()本身是否運作，安裝失敗log一律顯示。

namespace ZipPathTrace
{
    // hModule：讀取[Debug] DebugZipPathTrace決定逐次trace log要不要印，
    // 不影響hook本體是否安裝成功（安裝失敗一律印log，不受這個旗標限制）。
    void Install(HMODULE hModule);

    // 供LocHook.cpp推導中文LOC路徑用。回傳最近一次側錄到、副檔名為
    // .zip的name字串（例："SCENES\M00\M00_MAIN.zip"，見
    // md/bloodmoney_utf8_support.md第7節第2點「相對路徑推導可行」段落，
    // 側錄到的是整個場景zip路徑，不是個別.loc/.tex entry路徑）。尚未側錄到
    // 任何.zip請求時回傳空字串。
    const char* GetLastSceneZipPath();
}
