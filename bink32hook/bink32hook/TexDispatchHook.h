// [DEBUG專用] TEX entry分派側錄工具，預設停用（Hook.cpp的Install()呼叫已註解）。
#pragma once
#include <Windows.h>

// hWnd+0x14物件的vtable slot +0x10：sub_48F330，TEX entry的Type分派器
// （反組譯byte-level確認：strncpy把entry名稱截斷複製到outputRecord+0x14，
// 依entry的Type FourCC分派到DXT1/DXT3/RGBA/PALN/I8/U8V8等格式載入函式，
// 見bloodmoney_project.md「TEX→D3D9材質分派函式」章節）。這是先前一路追的
// 資源載入鏈跟材質建立鏈唯一還沒實機側錄過的節點，目標是：
//   ①呼叫端returnAddr落在哪個函式（很可能就是ZTTFONT/ZTextureManagerD3D
//     一直找不到的「真正載入」方法）；
//   ②readerObj指標的內部結構長什麼樣子，供之後比對是否為字型頁載入路徑。
//
// 跟ResourceOpenHook/ZipPathTrace/VtableThunkHook同一款function-entry
// inline hook手法：sub_48F330本身位址固定（0x48F330，不是vtable指標本身，
// vtable指標要等物件建構才有效，但函式程式碼從binary載入當下就存在），
// 不需要TextureHook那種lazy retry。簽名確認為__thiscall
// void/int sub_48F330(this, void* readerObj, void* outputRecord)——this=
// hWnd(0x8ACA30)+0x14的TEX容器單例、readerObj=arg0(esp+4)、
// outputRecord=arg4(esp+8)。純觀察，call原函式（原地重跑被覆蓋的前兩句
// 指令）取得真正結果後才記log，不改變任何行為。

namespace TexDispatchHook
{
    void Install();
}
