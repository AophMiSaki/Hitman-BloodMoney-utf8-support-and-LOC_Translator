#include "LocScenePath.h"
#include "Paths.h"
#include "Log.h"
#include "ZipPathTrace.h"
#include <cstring>
#include <cstdio>

namespace LocScenePath
{
    // 把zip路徑（例："SCENES\M00\M00_MAIN.zip"）換成
    // "<gamedir>\bink32hook\Scenes\<basename>"、不含副檔名。只取zip相對路徑
    // 的basename（去掉"M00\"這類子路徑前綴），譯文檔平放在Scenes\底下不分
    // 子資料夾。副檔名大小寫在NTFS下不影響查找。呼叫端自行補`.LOC`/`.txt`
    // 兩種候選副檔名。
    bool BuildSceneStemPath(char* outBuf, size_t bufSize)
    {
        const char* zipPath = ZipPathTrace::GetLastSceneZipPath();
        if (!zipPath || !zipPath[0])
        {
            Log::Write("[LocScenePath] 尚未側錄到任何場景zip路徑，無法推導LOC路徑");
            return false;
        }

        const char* rel = zipPath;
        if (_strnicmp(rel, "SCENES\\", 7) == 0) rel += 7;

        size_t relLen = strlen(rel);
        if (relLen < 4 || _stricmp(rel + relLen - 4, ".zip") != 0)
        {
            Log::Write("[LocScenePath] 場景路徑不是預期的.zip結尾，放棄推導：%s", zipPath);
            return false;
        }

        char stem[MAX_PATH];
        size_t stemLen = relLen - 4;
        if (stemLen >= sizeof(stem)) stemLen = sizeof(stem) - 1;
        memcpy(stem, rel, stemLen);
        stem[stemLen] = '\0';

        // 取basename（去掉子資料夾），Scenes\底下不分資料夾，全部平放。
        const char* baseName = stem;
        const char* lastSlash = strrchr(stem, '\\');
        if (lastSlash) baseName = lastSlash + 1;

        HMODULE hModule = GetModuleHandleA("binkw32.dll");
        char dataDir[MAX_PATH];
        Paths::GetDataDir(hModule, dataDir, sizeof(dataDir));

        _snprintf_s(outBuf, bufSize, _TRUNCATE, "%sScenes\\%s", dataDir, baseName);
        return true;
    }

    // zip內部檔名跟解出來後放在Scenes\/Orig_Scenes\底下的實際檔名不保證同一
    // 套大小寫規則（如zip內"Premission"、實際檔名"premission"；也有
    // "HitmanBloodMoney.LOC"這種混合大小寫），無法用單一轉換規則猜。改用
    // FindFirstFileA列舉目標目錄，逐一用_stricmp比對檔名，找到就採用該檔案
    // 的真實大小寫覆寫path；找不到維持原path不動（呼叫端CreateFileA一樣失敗，
    // 走既有「檔案不存在」分支，行為不變）。
    void ResolveCaseInsensitivePath(char* path, size_t pathSize)
    {
        char dir[MAX_PATH];
        strncpy_s(dir, sizeof(dir), path, _TRUNCATE);
        char* lastSlash = strrchr(dir, '\\');
        if (!lastSlash) return;

        const char* targetName = path + (lastSlash - dir) + 1;
        *(lastSlash + 1) = '\0';

        char searchPattern[MAX_PATH];
        _snprintf_s(searchPattern, _TRUNCATE, "%s*", dir);

        WIN32_FIND_DATAA findData;
        HANDLE hFind = FindFirstFileA(searchPattern, &findData);
        if (hFind == INVALID_HANDLE_VALUE) return;

        do
        {
            if (_stricmp(findData.cFileName, targetName) == 0)
            {
                char resolved[MAX_PATH];
                _snprintf_s(resolved, _TRUNCATE, "%s%s", dir, findData.cFileName);
                strncpy_s(path, pathSize, resolved, _TRUNCATE);
                break;
            }
        } while (FindNextFileA(hFind, &findData));

        FindClose(hFind);
    }

    // 從側錄到的場景zip路徑取「任務目錄段」（SCENES\<這段>\...），作為
    // g_returnedArena的歸屬邊界。例："SCENES\M01\M01_MAIN.zip" -> "M01"、
    // "SCENES\M01\M01_premission.zip" -> "M01"（同任務的premission→main子轉換
    // 取到同一段、不觸發arena清空）。取不到（無側錄／非SCENES\前綴／無子
    // 資料夾段）回空字串——呼叫端遇空字串不清arena（見LoadIfNeeded），避免
    // 場景切換瞬間zip尚未側錄到時誤清。
    std::string CurrentMissionTag()
    {
        const char* zipPath = ZipPathTrace::GetLastSceneZipPath();
        if (!zipPath || !zipPath[0]) return std::string();

        const char* rel = zipPath;
        if (_strnicmp(rel, "SCENES\\", 7) == 0) rel += 7;

        const char* slash = strchr(rel, '\\');
        if (!slash || slash == rel) return std::string();
        return std::string(rel, slash - rel);
    }

    // 跟BuildSceneStemPath幾乎一樣的zip路徑推導邏輯，唯一差異是輸出目錄用
    // "Orig_Scenes\"（不是"Scenes\"）。只找HitmanLOCTranslator工具輸出的
    // "_unpacked.txt"（括號TXT格式），不試原生二進位.LOC（雜湊分桶索引表，
    // 不是LocTxtParser能解析的格式）。不分子資料夾，"_unpacked.txt"整批平放
    // 在Orig_Scenes\底下，這裡只取zip相對路徑的basename組檔名。
    // 沒跟BuildSceneStemPath合併成帶參數的同一函式，是因為兩邊呼叫端各自
    // 獨立、改動風險最小。
    bool BuildOrigSceneStemPath(char* outBuf, size_t bufSize)
    {
        const char* zipPath = ZipPathTrace::GetLastSceneZipPath();
        if (!zipPath || !zipPath[0])
        {
            Log::Write("[LocScenePath] 尚未側錄到任何場景zip路徑，無法推導原文Orig_Scenes路徑");
            return false;
        }

        const char* rel = zipPath;
        if (_strnicmp(rel, "SCENES\\", 7) == 0) rel += 7;

        size_t relLen = strlen(rel);
        if (relLen < 4 || _stricmp(rel + relLen - 4, ".zip") != 0)
        {
            Log::Write("[LocScenePath] 場景路徑不是預期的.zip結尾，放棄推導原文路徑：%s", zipPath);
            return false;
        }

        char stem[MAX_PATH];
        size_t stemLen = relLen - 4;
        if (stemLen >= sizeof(stem)) stemLen = sizeof(stem) - 1;
        memcpy(stem, rel, stemLen);
        stem[stemLen] = '\0';

        // 取basename（去掉子資料夾），Orig_Scenes\底下不分資料夾，全部
        // "_unpacked.txt"平放。
        const char* baseName = stem;
        const char* lastSlash = strrchr(stem, '\\');
        if (lastSlash) baseName = lastSlash + 1;

        HMODULE hModule = GetModuleHandleA("binkw32.dll");
        char dataDir[MAX_PATH];
        Paths::GetDataDir(hModule, dataDir, sizeof(dataDir));

        _snprintf_s(outBuf, bufSize, _TRUNCATE, "%sOrig_Scenes\\%s", dataDir, baseName);
        return true;
    }
}
