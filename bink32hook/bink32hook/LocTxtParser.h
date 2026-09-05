#pragma once
#include <map>
#include <string>

// HitmanKi.exe/HitmanBe.exe用的巢狀括號TXT格式parser，逐條規則對照
// HitmanLOCTranslator\loc_txt_format.py（已round-trip驗證過的權威實作）
// 手動port成C++，故意不用Container/Entry樹狀結構——我們只需要「攤平成
// category/key -> value」的查表結果，不需要保留完整樹狀結構，單一pass
// 邊解析邊塞map比較省事。
//
// 語法（跟loc_txt_format.py docstring一致）：
//   [ "容器名" [ "key" = "value"  "只有key" "子容器" [ ... ] ] ]
// 字串內的雙引號用\"跳脫，沒有其他跳脫序列；value後面可能接1~2個不帶引號
// 的數字metadata，原樣丟棄不解讀（語意未知，跟顯示文字無關）。
//
// 呼叫端負責保證傳進來的位元組是UTF-8——這裡逐byte比對結構字元
// （[ ] = " \），只在輸入是UTF-8時安全：UTF-8多位元組續位元組
// （0x80~0xBF）不會跟這些ASCII結構字元衝突。GBK來源已移除
// （場景中文檔固定當UTF-8處理），不再需要先整檔轉碼。
namespace LocTxtParser
{
    // 成功回傳true，結果累加進outMap（呼叫端如果要每次重新載入，記得自己
    // 先clear）。失敗回傳false，errorOut填入人類可讀的錯誤描述。
    //
    // outMeta非nullptr時，另把每個entry「value後面第一個純數字metadata」原樣
    // 存進outMeta（key跟outMap同，攤平後的category/.../name）——TVAndRadio
    // 這類entry第一欄＝該事件在.SND的file offset（見字幕功能.md §3.4），
    // SubtitleTvRadio拿它當resId對照。非數字或沒有metadata的entry不進outMeta。
    bool Parse(const char* text, size_t length, std::map<std::string, std::string>& outMap, std::string& errorOut,
               std::map<std::string, unsigned long long>* outMeta = nullptr);
}
