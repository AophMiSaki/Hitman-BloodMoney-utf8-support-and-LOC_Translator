#include "LocTxtParser.h"
#include <vector>
#include <cstdio>

namespace LocTxtParser
{
    struct Token
    {
        enum Kind { LBRACKET, RBRACKET, EQUALS, STR, WORD, END } kind;
        std::string text;
    };

    static bool Tokenize(const char* s, size_t n, std::vector<Token>& out, std::string& err)
    {
        size_t i = 0;
        while (i < n)
        {
            char c = s[i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { i++; continue; }
            if (c == '[') { out.push_back({ Token::LBRACKET, "" }); i++; continue; }
            if (c == ']') { out.push_back({ Token::RBRACKET, "" }); i++; continue; }
            if (c == '=') { out.push_back({ Token::EQUALS, "" }); i++; continue; }
            if (c == '"')
            {
                size_t j = i + 1;
                std::string buf;
                bool closed = false;
                while (j < n)
                {
                    char cj = s[j];
                    if (cj == '\\' && j + 1 < n && s[j + 1] == '"') { buf.push_back('"'); j += 2; continue; }
                    if (cj == '"') { closed = true; break; }
                    buf.push_back(cj);
                    j++;
                }
                if (!closed)
                {
                    char buf2[128];
                    _snprintf_s(buf2, _TRUNCATE, "未封閉的字串，開始於位置 %zu", i);
                    err = buf2;
                    return false;
                }
                out.push_back({ Token::STR, buf });
                i = j + 1;
                continue;
            }
            // WORD：不帶引號的裸露字元序列，只用於entry尾隨的數字metadata。
            size_t j = i;
            while (j < n && s[j] != ' ' && s[j] != '\t' && s[j] != '\r' && s[j] != '\n'
                   && s[j] != '[' && s[j] != ']' && s[j] != '=' && s[j] != '"')
                j++;
            if (j == i)
            {
                char buf2[128];
                _snprintf_s(buf2, _TRUNCATE, "未預期字元 0x%02X，位置 %zu", (unsigned char)c, i);
                err = buf2;
                return false;
            }
            out.push_back({ Token::WORD, std::string(s + i, j - i) });
            i = j;
        }
        out.push_back({ Token::END, "" });
        return true;
    }

    static void SkipTrailingWords(const std::vector<Token>& toks, size_t& pos)
    {
        while (toks[pos].kind == Token::WORD) pos++;
    }

    // s是全數字（且非空）時回傳true並把值寫進out，否則回傳false不動out。
    static bool ParseUInt(const std::string& s, unsigned long long& out)
    {
        if (s.empty()) return false;
        unsigned long long v = 0;
        for (char ch : s)
        {
            if (ch < '0' || ch > '9') return false;
            v = v * 10 + (unsigned)(ch - '0');
        }
        out = v;
        return true;
    }

    // 遞迴解析目前容器層級底下的children，直到遇到非STR的token（呼叫端
    // 判斷那是不是預期的']'）。pathPrefix是目前巢狀路徑（已用"/"接好），
    // 根層級傳空字串。outMeta非nullptr時另收集entry尾隨的第一個數字metadata。
    static bool ParseChildren(const std::vector<Token>& toks, size_t& pos, const std::string& pathPrefix,
                               std::map<std::string, std::string>& outMap, std::string& err,
                               std::map<std::string, unsigned long long>* outMeta)
    {
        while (toks[pos].kind == Token::STR)
        {
            std::string name = toks[pos].text;
            pos++;

            if (toks[pos].kind == Token::LBRACKET)
            {
                pos++;
                std::string childPrefix = pathPrefix.empty() ? name : pathPrefix + "/" + name;
                if (!ParseChildren(toks, pos, childPrefix, outMap, err, outMeta)) return false;
                if (toks[pos].kind != Token::RBRACKET)
                {
                    err = "缺少對應的']'";
                    return false;
                }
                pos++;
            }
            else if (toks[pos].kind == Token::EQUALS)
            {
                pos++;
                if (toks[pos].kind != Token::STR)
                {
                    err = "'='後面預期字串";
                    return false;
                }
                std::string value = toks[pos].text;
                pos++;

                std::string fullKey = pathPrefix.empty() ? name : pathPrefix + "/" + name;

                // value後面第一個token若是純數字metadata就收進outMeta（見標頭
                // 說明；SkipTrailingWords之前先看一眼，不影響原本的丟棄行為）。
                if (outMeta && toks[pos].kind == Token::WORD)
                {
                    unsigned long long meta = 0;
                    if (ParseUInt(toks[pos].text, meta)) (*outMeta)[fullKey] = meta;
                }
                SkipTrailingWords(toks, pos);

                outMap[fullKey] = value;
            }
            else
            {
                // 只有key沒有value的特例（loc_txt_format.py的Entry.value=None）：
                // native端value只有單一控制字元（U+000B）的entry，工具輸出時
                // 會省略整段"= \"value\""、只留裸key。這裡記錄U+000B原始byte
                // （不是空字串或丟棄）：讓LookupText回傳的字串跟native原生查到
                // 的內容一致，走同一條「類似換行、不顯示」的native渲染路徑，
                // 也讓LocHook.cpp的IsBlankOriginalContent()空白佔位符判斷能查
                // 得到這個key，不落回category/key組合字串fallback。
                std::string fullKey = pathPrefix.empty() ? name : pathPrefix + "/" + name;
                outMap[fullKey] = std::string(1, '\x0B');
                SkipTrailingWords(toks, pos);
            }
        }
        return true;
    }

    bool Parse(const char* text, size_t length, std::map<std::string, std::string>& outMap, std::string& errorOut,
               std::map<std::string, unsigned long long>* outMeta)
    {
        std::vector<Token> toks;
        if (!Tokenize(text, length, toks, errorOut)) return false;

        size_t pos = 0;
        if (toks[pos].kind != Token::LBRACKET)
        {
            errorOut = "缺少開頭'['";
            return false;
        }
        pos++;

        if (!ParseChildren(toks, pos, "", outMap, errorOut, outMeta)) return false;

        if (toks[pos].kind != Token::RBRACKET)
        {
            errorOut = "缺少結尾']'";
            return false;
        }
        pos++;

        if (toks[pos].kind != Token::END)
        {
            errorOut = "結尾']'之後還有多餘內容";
            return false;
        }
        return true;
    }
}
