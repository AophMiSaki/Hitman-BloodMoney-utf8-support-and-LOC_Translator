#pragma once
// 判斷一個（UTF-8解碼後的）Unicode codepoint要不要交給CJK合成字型路徑處理。
// 輸入是已解碼的codepoint，不是DBCS lead/trail byte pair。
// 名稱沿用歷史，實際語意已放寬：ASCII、Latin-1補充(0x00–0xFF)、以及原生字型
// 字元表額外列出的彎雙引號 “ ” 維持native（Blood Money的LOC "Letters" key已
// 宣告這些glyph都烘進原生atlas）；其餘所有碼位（CJK、假名、諺文、西里爾、
// 泰文、CJK擴充B+、其他標點…）一律回true導向合成字型，缺glyph由使用者自行
// 換字型解決。ASCII在F11原文/譯文模式下的分流另由SynthesizeCJKGlyphRecord
// 的forceAll決定，不受這裡影響。

static inline bool BMIsCJKCodepoint(unsigned int cp)
{
    if (cp < 0x0100) return false;
    if (cp == 0x201C || cp == 0x201D) return false;
    return true;
}
