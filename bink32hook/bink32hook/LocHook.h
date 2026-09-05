#pragma once
#include <Windows.h>
#include <string>
#include <map>

// 原文/譯文熱鍵切換功能。完整設計決策見 bloodmoney_utf8_support.md 第7節第2點：
// - hook DNameNode::GetText(category,key) slot0（0x465370），不hook slot1。
//   熱鍵off時100% passthrough native；on時DLL自己查std::map回傳翻譯，完全
//   不觸碰native的this+4 buffer/sub_464FF0查找函式。
// - 查有回傳翻譯；查無回傳"category/key"組合字串（方便肉眼定位缺翻譯的
//   資源，不是裸key）。
// - 場景切換cache失效跟TextureManagerReleaseHook（0x48F190）同一觸發點，
//   lazy load：下次查詢時才重新解析對應場景的中文LOC文字檔。
// - 中文對照檔用括號TXT格式（見LocTxtParser.h），不是原生二進位.LOC格式。
// - 檔案副檔名：依zip↔LOC路徑推導出檔名主幹後，依序找`.LOC`與`.txt`（內容
//   格式相同，都用LocTxtParser::Parse解析）。都存在時`.LOC`優先；開檔或解析
//   失敗則改試`.txt`；兩者都失敗沿用fallback（map維持空，GetText查詢全部走
//   "category/key"組合字串）。
// - 熱鍵預設F11（VK_F11=0x7A），INI可調整。

namespace LocHook
{
    // 對sub_465370做function-entry inline hook。固定VA、不依賴任何runtime
    // 才建立的物件，DLL載入當下就能裝（跟TextureManagerReleaseHook同款）。
    void Install(HMODULE hModule);

    // 供TextureManagerReleaseHook.cpp在偵測到native ReleaseTextures執行後
    // 呼叫（跟NativeTextureRegistry::InvalidateAllSlots同一觸發點，見設計
    // 決策③）。只清空map+標記cache失效，不做任何檔案I/O，下次GetText查詢
    // 時才lazy重新載入對應場景的中文LOC。
    void InvalidateCache();

    // 供SubtitleBrief.cpp查詢完整LOC key（例如"M00/MissionBriefing/M00_MB_01D"）
    // 用，不經過native GetText呼叫路徑。內部會lazy load（跟GetText查詢共用同一
    // 個LoadIfNeeded/場景快取），只查譯文map（g_translations）——原文改由呼叫端
    // 直接呼叫native sub_6CF850(index)取得（見SubtitleBrief.cpp）。查無資料回傳
    // false，outText不動。
    bool TryLookup(const char* combinedKey, std::string& outText);

    // 供SubtitleGate.cpp首選/第三欄位（self-render疊加）在F11顯示原文時也能查到
    // 文字內容用——跟TryLookup同一個combinedKey查詢語意，但查的是獨立的第二份
    // map(g_originals)，從 <遊戲目錄>\bink32hook\Orig_Scenes\{stem}_unpacked.txt
    // 載入（括號TXT格式，不試原生二進位.LOC；整批平放不分子資料夾，見
    // LocScenePath.cpp BuildOrigSceneStemPath註解）。跟譯文用的Scenes\分開存放。
    // lazy load、查無資料回傳false，outText不動。
    bool TryLookupOriginal(const char* combinedKey, std::string& outText);

    // 供SubtitleBrief.cpp判斷目前該查譯文還是原文：true時字幕應該
    // 用TryLookup查g_translations；false時應該改呼叫native sub_6CF850
    // （SubtitleBrief專用，Mission Briefing index-based）或改用
    // 上面的TryLookupOriginal（一般combinedKey查詢，SubtitleGate用）。
    bool IsTranslationActive();

    // 掃目前場景的 LOC/txt（跟 TryLookup 同一份檔案、同一套 loc→txt 候選與
    // 大小寫解析），取出所有 key 含 "TVAndRadio/" 的 entry 及其第一個數字
    // metadata 欄（＝該事件在 <關>_main.SND 的 file offset），回填 out：
    //   out[sndOffset] = 攤平後的 combinedKey（與 TryLookup 傳入格式相同）
    // SubtitleTvRadio 在場景切換時呼叫一次，把播放 detour(sub_4C5E10) 收到的
    // resId 對回 LOC key。見 字幕功能.md §3.4。out 進來會先被清空；載入失敗
    // 時 out 留空（呼叫端據此判斷本場景無 TV/Radio 字幕）。
    void CollectTvRadioSndOffsets(std::map<DWORD, std::string>& out);
}
