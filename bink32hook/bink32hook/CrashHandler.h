#pragma once
#include <Windows.h>

// 沒有現成的第三方crash logger能直接用在這款遊戲上——OBTC生態圈常見的
// CrashLogger.dll是依附OBSE plugin loader的Oblivion專用工具，Blood Money
// 沒有這層基礎設施可以掛。這裡改成用SetUnhandledExceptionFilter自建一個最
// 小可用版本：例外發生時記錄log(含記憶體用量)+寫一份minidump，不吃掉例外
// (return EXCEPTION_CONTINUE_SEARCH)，讓遊戲/系統原本的處理流程照常繼續。
//
// 刻意不用VEH(AddVectoredExceptionHandler)：VEH會攔截遊戲內部拿SEH例外當
// 控制流程用的正常例外，誤判成crash而洗版假警報；SetUnhandledExceptionFilter
// 只在真的沒人處理的例外才會觸發，不會有這個問題。

namespace CrashHandler
{
    void Init(HMODULE hModule);
}
