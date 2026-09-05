#pragma once
#include <Windows.h>

// sub_5A4850（native共用小工具函式：逐byte把來源字串當成
// single-byte/ANSI字串「升級」成UTF-8，內部呼叫sub_438FE0把每個byte當
// 一個codepoint重新編碼）的function-entry inline hook。
//
// 這個函式被多個呼叫端共用，混用兩種輸入型態：
//   - PickupSPC badge、Fire等鍵位設定畫面鍵名：餵進去的是查表拿到、本
//     來就已經是UTF-8編碼的CJK翻譯字串——被逐byte各自誤當codepoint重
//     編碼，每個高位byte都變成一個獨立的2-byte UTF-8序列，造成雙重編碼
//     亂碼。
//   - 玩家鍵盤/IME文字輸入欄：餵進去的是玩家打字的真正單一byte ANSI字
//     元（如æ/ø/å），這種情況「逐byte當codepoint升級成UTF-8」正是原本
//     設計要做的正確行為，不能跳過。
//
// 判斷依據＝「目前位置開始的byte序列本身是不是已經構成一段合法完整的
// UTF-8多byte序列」：是的話代表輸入本來就是UTF-8，原樣複製通過、不重新
// 編碼；不是的話（純ASCII、或不構成合法序列的孤立高位byte）才照原邏輯
// 用sub_438FE0逐codepoint編碼。判斷邏輯在共用函式本身做，各呼叫端不需
// 要個別patch。
namespace Utf8ReencodeFixHook
{
    void Install();
}
