#pragma once
#include <Windows.h>
#include "AddressTable.h"

// 遊戲版本偵測與位址查表。位址全部集中在 AddressTable.h（gog／steam 兩欄），程式碼經 Addr() 取目前載入的exe版本的實際位址。

namespace GameAddr
{
    enum Build
    {
        Unknown = 0,
        Gog = 1,
        Steam = 2
    };

    // 比對主執行檔PE檔頭的 TimeDateStamp 與 SizeOfImage 判斷版本並記錄；DllMain階段呼叫一次。
    void Init();

    Build GetBuild();

    // 版本是 Gog 或 Steam 才為 true。Unknown 時所有 Addr() 都回傳 0。
    bool IsSupported();

    // 目前版本的位址。版本不明回傳 0；表內該版本欄位為 0 也回傳 0（失敗log不受[Debug]開關限制，每個名稱只記一次）。
    DWORD Addr(AddressTable::Id id);

    // 讀取 id 對應位址上的 DWORD（全域指標／旗標）。位址為0（版本不明或查詢失敗）或讀不到記憶體回傳 0。
    DWORD ReadDword(AddressTable::Id id);

    // 把 addr 起 n 個位元組格式化成 "XX XX ..." 寫入 out（讀不到寫入 "(無法讀取)"），供安裝失敗 log 使用。
    void HexAt(DWORD addr, size_t n, char* out, size_t cap);

    // 比對時要略過的位元組範圍（相對於比對起點）。
    struct Wild
    {
        int off;
        int len;
    };

    // 讀取 addr 起 n 個位元組與 expected 比對，wild 內的範圍不比。讀不到記憶體視為不符。
    // 用於 kExpected 這類含絕對位址或 rel32 的預期位元組，在不同版本位址會不同。
    bool MatchMasked(DWORD addr, const BYTE* expected, size_t n, const Wild* wild, size_t nWild);
}
