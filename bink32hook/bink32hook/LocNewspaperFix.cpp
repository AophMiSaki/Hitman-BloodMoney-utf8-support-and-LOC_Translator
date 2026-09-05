#include "LocNewspaperFix.h"
#include <cstring>

namespace LocNewspaperFix
{
    static bool g_enable = true;
    static int  g_interval = 6;

    void Configure(bool enable, int interval)
    {
        g_enable = enable;
        g_interval = interval;
    }

    bool Enabled() { return g_enable; }
    int  Interval() { return g_interval; }

    bool IsNewspaperZipPath(const char* zipPath)
    {
        if (!zipPath || !zipPath[0]) return false;
        size_t len = strlen(zipPath);
        auto endsWith = [&](const char* suf) {
            size_t sufLen = strlen(suf);
            return len >= sufLen && _stricmp(zipPath + len - sufLen, suf) == 0;
        };
        return endsWith("_news.zip") || endsWith("_postmission.zip");
    }

    // 每隔g_interval個連續CJK字元（3-byte UTF-8序列，絕大多數CJK字都落在這個
    // 範圍，足夠用不需要完整codepoint判斷）插入一個ASCII空白，當native換行
    // 判斷的斷點候選。跳過HTML-like tag(<...>)內部（tagDepth>0時不計數/不
    // 插入，避免插壞tag語法，例如誤插成"<t ab>"）；遇到既有空白/tab/換行時
    // 重置計數，避免緊接著又插一個造成雙空白；行尾或下一個字元本身已經是
    // 空白時不插入。
    void InsertSoftBreaks(std::string& text)
    {
        if (g_interval <= 0) return;

        std::string result;
        result.reserve(text.size() + text.size() / 8);

        int tagDepth = 0;
        int cjkRun = 0;
        size_t i = 0;
        size_t n = text.size();

        while (i < n)
        {
            unsigned char c = (unsigned char)text[i];

            int seqLen = 1;
            if ((c & 0x80) == 0x00) seqLen = 1;
            else if ((c & 0xE0) == 0xC0) seqLen = 2;
            else if ((c & 0xF0) == 0xE0) seqLen = 3;
            else if ((c & 0xF8) == 0xF0) seqLen = 4;
            if (i + seqLen > n) seqLen = 1;

            if (c == '<') tagDepth++;

            result.append(text, i, seqLen);
            i += seqLen;

            if (c == '>' && tagDepth > 0) tagDepth--;

            if (tagDepth > 0) continue;

            if (seqLen == 3)
            {
                cjkRun++;
                if (cjkRun >= g_interval)
                {
                    cjkRun = 0;
                    bool nextIsSpace = (i < n) && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n');
                    bool atEnd = (i >= n);
                    if (!nextIsSpace && !atEnd)
                        result.push_back(' ');
                }
            }
            else if (seqLen == 1 && (c == ' ' || c == '\t' || c == '\r' || c == '\n'))
            {
                cjkRun = 0;
            }
        }

        text.swap(result);
    }

    // headline（<width580><linespacing -20>那組標題文字，見md「LOC結構參考」
    // 節）沒有這個換行bug，也不該被插空白——headline走<justify center>單獨
    // 一欄置中排版，跟內文<width198><autocolumn>是不同路徑，插入的空白會讓
    // 置中效果跑掉。這裡排除key前綴為"NewsPaper/Statistics/HeadLines"的entry
    // （大小寫不敏感，比照g_translations的CaseInsensitiveLess）。
    bool IsHeadlineKey(const std::string& key)
    {
        static const char kPrefix[] = "NewsPaper/Statistics/HeadLines";
        size_t prefixLen = sizeof(kPrefix) - 1;
        if (key.size() < prefixLen) return false;
        return _strnicmp(key.c_str(), kPrefix, prefixLen) == 0;
    }
}
