#pragma once
#include <Windows.h>

// 修正native「按鈕/HUD亂碼」根因：native有5個呼叫端（sub_463D10/sub_497650/
// sub_559910）對字元逐一呼叫CRT toupper()，其中sub_463D10會拿CJK codepoint
// （如"空"=U+7A7A）當參數呼叫，超出toupper()的定義域(0~255)，undefined
// behavior把值弄壞，壞值被寫回caption buffer就顯示成亂碼。對MSVCR71.DLL的
// toupper匯入做IAT hook，一次修掉全部5個呼叫端，不用逐一hook call site。
namespace ToUpperHook
{
    void Install(HMODULE hModule);
}
