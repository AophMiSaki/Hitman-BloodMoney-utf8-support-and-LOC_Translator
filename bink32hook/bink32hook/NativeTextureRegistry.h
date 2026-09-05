#pragma once
#include <Windows.h>
#include "FontCategory.h"

// 把GlyphAtlas的CJK atlas內容登記進native材質頁表，供GlyphHook合成CJK
// glyph記錄時取樣。
//
// 避開native的ReserveTexture wrapper（hWnd vtable+0x98 / sub_487CE0——入口
// 一次配置約256KB堆疊(alloca)，實機會crash），改用ZTextureManagerD3D的
// AllocateSlot(vtable+0x18)＋DecodeBitmapPtr(vtable+0x14)直接建材質；已登記
// 頁面的內容更新走hWnd vtable+0x9C（UpdateTexture，輕量無alloca）。
//
// 重量級native呼叫只從IDirect3DDevice9::BeginScene hook觸發，不從GlyphCheck
// 深層呼叫鏈直接呼叫；GlyphCheck/SynthesizeCJKGlyphRecord只呼叫不做任何
// native呼叫的GetRegisteredIndex()。

namespace NativeTextureRegistry
{
    // 只讀取[Debug] NativeTexRegDiagEnable旗標，不做任何D3D9/native呼叫；
    // hook安裝由EnsureBeginSceneHookInstalled()的lazy retry負責。登記成功／
    // 失敗log不受此旗標限制，旗標只控制其餘細節診斷log。
    void Install(HMODULE hModule);

    // 冪等，可每次GetGlyph命中都呼叫：安裝（若還沒裝）IDirect3DDevice9::
    // BeginScene的vtable hook。重量級native呼叫延到每幀BeginScene觸發時才做
    // （見.cpp的Update()），這裡成本只有一次vtable slot比對。
    void EnsureBeginSceneHookInstalled();

    // 純讀取、不做任何native呼叫，可安全從GetGlyph深層呼叫鏈裡呼叫：尚未
    // 在某次BeginScene完成登記時回傳-1（呼叫端應fallback成native passthrough，
    // 不要合成假glyph記錄），已登記則回傳快取的native頁碼。
    // 通用/報紙各自登記獨立的native材質槽（字幕不經過這條路徑）；category
    // 要傳GlyphAtlas::ResolveCategory()解析後的有效分類，否則Newspaper
    // fallback回General時會去查一個從未登記過的頁碼、永遠拿到-1。
    int GetRegisteredIndex(FontCategory category);

    // 三分支（見.cpp實作頭部）：未登記時做完整AllocateSlot+DecodeBitmapPtr
    // 登記／已登記且atlas內容沒變時只比一個DWORD就return／已登記但內容有變
    // 時只做UpdateTexture——冪等，「已登記且沒變」時幾乎零成本。
    // 除Hook_BeginScene外，也公開給GlyphHook.cpp的DetermineCategory()在偵測
    // 到報紙分類的當下提前呼叫一次：報紙排版是native一次性同步跑完，等下一次
    // BeginScene才登記材質太慢，文字已排版完畢、CJK全部合成失敗。
    // 風險：此呼叫點重新進入當初刻意迴避的GetGlyph深層呼叫深度，尚未實機驗證。
    void Update(FontCategory category);

    // 供TextureManagerReleaseHook.cpp／Hook_Reset在native釋放材質後呼叫。
    // 只寫static全域變數（nativeIndex重設-1、pushedGeneration重設回無效
    // sentinel），不做任何native/D3D呼叫，故裝置正在銷毀中呼叫也安全。
    // 下次Hook_BeginScene的Update()看到nativeIndex<0會自動重新走完整的
    // AllocateSlot+DecodeBitmapPtr登記流程。
    void InvalidateAllSlots();
}
