#pragma once
#include <Windows.h>

// ReleaseTextures（sub_48F190）入口的inline patch：場景切換或引擎關閉時，native會把全部材質槽歸零，
// 連同我們登記的CJK atlas頁碼一起清掉，這裡在清掉前重設NativeTextureRegistry的字型槽與LOC快取。
//
// 另在Install()裡掛場景資料載入（sub_45FCF0）與著色器效果載入（sub_496F00）的進出追蹤，以及ntdll
// NtAllocateVirtualMemory的hook，做低位址區段修法：sub_496F00期間要的4～16 MB保留區段若落在>=2GB，
// 改用引擎256MB保留區管理器新借的連續低位址區塊（log前綴[LowFix]）。所有naked hook限定在場景載入執行緒。
// 調查過程與設計見 md\M05載入變慢調查.md。
namespace TextureManagerReleaseHook
{
    void Install();
}
