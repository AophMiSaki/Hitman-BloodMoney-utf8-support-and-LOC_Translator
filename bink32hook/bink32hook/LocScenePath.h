#pragma once
#include <Windows.h>
#include <string>

// 場景zip路徑 → 中文LOC／原文檔案系統路徑的推導。ZipPathTrace側錄到的最近
// 一次場景zip相對路徑（"SCENES\M00\M00_MAIN.zip"）是唯一輸入來源。純函式、
// 不持有狀態。
namespace LocScenePath
{
    // zip相對路徑 → "<gamedir>\bink32hook\Scenes\<basename>"（不含副檔名）。
    // 只取basename、譯文檔平放不分子資料夾；呼叫端自行補`.LOC`/`.txt`兩種候選
    // 副檔名。側錄不到路徑或非.zip結尾回false。
    bool BuildSceneStemPath(char* outBuf, size_t bufSize);

    // 同BuildSceneStemPath，唯一差異是輸出目錄用"Orig_Scenes\"、只找
    // "_unpacked.txt"（HitmanLOCTranslator輸出的括號TXT格式）。
    bool BuildOrigSceneStemPath(char* outBuf, size_t bufSize);

    // 用FindFirstFileA列舉目標目錄、逐一_stricmp比對，找到就用該檔的真實
    // 大小寫覆寫path；找不到維持原path不動。zip內部檔名跟解出來後的實際檔名
    // 大小寫規則不保證一致，無法用單一轉換規則猜。
    void ResolveCaseInsensitivePath(char* path, size_t pathSize);

    // 從場景zip路徑取「任務目錄段」（SCENES\<這段>\...），作為
    // returned-pointer arena的歸屬邊界。取不到回空字串。
    std::string CurrentMissionTag();
}
