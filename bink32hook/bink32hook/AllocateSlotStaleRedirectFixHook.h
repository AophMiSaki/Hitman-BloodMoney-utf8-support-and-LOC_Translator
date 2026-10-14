#pragma once
#include <Windows.h>

// AllocateSlot(ZTextureManagerD3D vtable+0x18 / sub_48F560)挑到一個「+0x2C
// 材質指標==0」的空閒slot時，只會把+0x2C標記成占用，完全不檢查/不清除這格
// 的+0x30（多子頁貼圖的count+redirect表指標）。如果這個slot「上一個占用者」
// 是一張多子頁貼圖，它的+0x30會原封不動殘留給下一個接手的貼圖（不管是native
// 自己的普通貼圖還是我方CJK atlas）——這正是GetTexture(sub_48F140)讀到殘留
// +0x30、a3越界崩潰的源頭（見md\CJK圖集ZBitmap延遲存取崩潰.md 候選A）。
//
// 這裡直接補在AllocateSlot找到slot、標記占用之前：若這格+0x30非0，比照
// ReleaseTextures(sub_48F190)自己對每格的處理方式，呼叫native的釋放函式
// (sub_4466D0)正確free掉舊的redirect表、再把欄位清成0，讓接手的新貼圖從
// 一開始就拿到乾淨的slot。跟sub_48F140的讀取端防呆(GetTextureBoundsFixHook)
// 不衝突，兩邊都補：這裡是正本清源（source），讀取端是保底（defense in depth）。
namespace AllocateSlotStaleRedirectFixHook
{
    void Install();
}
