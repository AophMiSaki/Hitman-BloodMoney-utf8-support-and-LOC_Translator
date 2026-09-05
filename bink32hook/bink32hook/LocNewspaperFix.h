#pragma once
#include <string>

// 報紙CJK自動換行bug workaround（見md「自動換行寬度判斷」節）。native
// sub_558CA0(<img> handler)算圖片旁縮窄寬度/下緣的公式會讀到未初始化堆疊值，
// CJK逐字換行判斷密度高因而肉眼可見擠字/超版；英文有空格斷點、判斷密度低
// 很多，同樣的垃圾值沒有造成可見症狀。這裡不修native，改在報紙譯文載入完成
// 後於CJK文字裡插入斷行候選空白，把判斷密度拉回接近英文版。只套用在報紙CJK
// 譯文，不動F11切原文用的英文原文（本來就有空格斷點、不需要）。
//
// 「套用到哪份map」的迴圈留在呼叫端（LocHook），這裡只出演算法＋分類
// ＋設定，不碰map型別。
namespace LocNewspaperFix
{
    // 由LocHook::Install從ini注入（[NewsPaper] NewsPaperSoftBreakFix
    // 開關 + 間隔）。
    void Configure(bool enable, int interval);
    bool Enabled();
    int  Interval();

    // zip路徑結尾是"_news.zip"或"_postmission.zip"才是報紙場景。
    bool IsNewspaperZipPath(const char* zipPath);

    // key前綴為"NewsPaper/Statistics/HeadLines"（大小寫不敏感）的entry是標題
    // 文字，走<justify center>單獨一欄置中排版、沒有這個換行bug，也不該插
    // 空白（會讓置中效果跑掉）。呼叫端據此跳過。
    bool IsHeadlineKey(const std::string& key);

    // 每隔Interval()個連續CJK字元（3-byte UTF-8序列）插入一個ASCII空白當
    // native換行判斷的斷點候選。跳過HTML-like tag(<...>)內部、遇既有空白/tab/
    // 換行時重置計數、行尾或下一字元已是空白時不插。就地改寫text。
    void InsertSoftBreaks(std::string& text);
}
