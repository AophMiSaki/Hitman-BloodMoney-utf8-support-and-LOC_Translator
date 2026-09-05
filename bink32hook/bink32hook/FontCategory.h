#pragma once

// CJK字型分類（通用/字幕/報紙三分法）。
// General：native GetGlyph呼叫點與SubtitleBrief以外情境的預設分類，
//   開機即建立、狀態必為Ready，永遠可當fallback目標。
// Subtitle：SubtitleBrief.cpp self-render字幕專用。
// Newspaper：報紙依native <font 1>~<font 4>分成4級，各對應不同目標渲染高度
//   桶（門檻邏輯見GlyphHook.cpp SynthesizeCJKGlyphRecord的
//   ProbeNewspaperFontSize；字級推算見Config.h LoadNewspaperFont1~4）。只需
//   判斷「是不是報紙分類」時用下方IsNewspaperCategory()，不要逐一比較。
enum class FontCategory
{
    General = 0,
    Subtitle = 1,
    NewspaperFont1 = 2,
    NewspaperFont2 = 3,
    NewspaperFont3 = 4,
    NewspaperFont4 = 5,
    Count = 6,
};

// 粗粒度「是不是報紙分類」判斷，供只需要true/false、不需知道細分級別的呼叫點
// 使用。依賴enum裡NewspaperFont1~4是連續值，調整enum數值順序時要一併檢查這裡。
inline bool IsNewspaperCategory(FontCategory category)
{
    return category >= FontCategory::NewspaperFont1 && category <= FontCategory::NewspaperFont4;
}
