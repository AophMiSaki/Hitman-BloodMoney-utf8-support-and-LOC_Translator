// [DEBUG專用] TEX entry分派側錄工具，預設停用（Hook.cpp的Install()呼叫已註解）。
#include "TexDispatchHook.h"
#include "Log.h"
#include <cstring>

namespace TexDispatchHook
{
    // entry名稱getter簽名：反組譯sub_48F330確認的呼叫方式——
    //   v3 = *a2（=innerObj，跟hook既有的innerObj變數同一份）
    //   v5 = (*(int(__thiscall**)(int))(*(DWORD*)v3+4))(v3)
    // 即「呼叫innerVtable+4這個虛函式，this=innerObj，回傳const char*entry
    // 名稱」，sub_48F330本體再用strlen/strncpy把這個字串複製進
    // outputRecord+0x14（最長0x18 bytes，超長只留尾巴23字元）。我們在hook
    // 進入點（body尚未執行、outputRecord+0x14還沒被寫入）提前呼叫同一個
    // getter取得名稱，屬於單純唯讀查詢（原生code也是呼叫它取值後才做別的
    // 事），不會改變原本的執行結果。
    typedef const char* (__thiscall *GetEntryNameFn)(void* thisPtr);

    static bool IsFontsEntry(const char* name)
    {
        return name && strncmp(name, "Fonts/", 6) == 0;
    }

    // sub_48F330：__thiscall void sub_48F330(this, void* readerObj, void*
    // outputRecord)。反組譯確認的前兩句指令：
    //   mov eax, [esp+4]   ; 8B 44 24 04 (4 bytes) -- eax=readerObj(arg0)
    //   push ebx           ; 53          (1 byte)
    // 剛好5 bytes，足夠塞5-byte jmp，不用補NOP，回跳位址對齊第三句指令
    // (push ebp)起點。
    static const DWORD kVA_Entry = 0x0048F330;
    static const DWORD kVA_ReturnTo = 0x0048F335;

    static DWORD g_thisStash = 0;
    static DWORD g_arg0Stash = 0;
    static DWORD g_arg4Stash = 0;
    static DWORD g_retAddrStash = 0;
    static DWORD g_hitCount = 0;
    static DWORD g_fontsHitCount = 0;

    // __cdecl，供naked detour呼叫。readerObj的第一層/第二層解引用
    // （innerObj=*readerObj、innerVtable=*innerObj）不是猜測性讀取——是
    // sub_48F330自己緊接著會做的事（esi=[eax]; edx=[esi]; call [edx+50h]，
    // 見反組譯筆記），這裡side-record同樣的兩層讀取只是提前記log，風險跟
    // 原生程式碼本身相同，不是額外冒險。
    //
    // entry名稱：sub_48F330本體會呼叫innerVtable+4這個虛函式（this=
    // innerObj）取得const char* entry名稱，才strncpy進outputRecord+0x14。
    // 這裡提前呼叫同一個getter取得名稱，用來判斷方向(a)這條dispatch路徑
    // 是否真的處理過Fonts/開頭的字型entry（見bloodmoney_utf8_support.md
    // 第3節「下一步」）。
    static void __cdecl CheckDispatchCall(DWORD arg0ReaderObj, DWORD arg4OutputRecord, void* returnAddr, void* thisPtr)
    {
        g_hitCount++;

        DWORD innerObj = 0;
        DWORD innerVtable = 0;
        if (arg0ReaderObj)
        {
            innerObj = *(DWORD*)arg0ReaderObj;
            if (innerObj)
                innerVtable = *(DWORD*)innerObj;
        }

        const char* entryName = nullptr;
        if (innerVtable)
        {
            GetEntryNameFn getName = *(GetEntryNameFn*)(innerVtable + 4);
            if (getName)
                entryName = getName((void*)innerObj);
        }

        bool isFonts = IsFontsEntry(entryName);
        if (isFonts)
            g_fontsHitCount++;
    }

    // 進入時：ecx=this(hWnd+0x14的TEX容器單例)，[esp]=呼叫端returnAddr，
    // [esp+4]=readerObj(arg0)，[esp+8]=outputRecord(arg4)。跟
    // ResourceOpenHook/ZipPathTrace同款stash手法：pushad前先收集，
    // 呼叫log完popad後原樣重跑被覆蓋的兩句指令再跳回entry+5。
    static __declspec(naked) void DispatchDetour()
    {
        __asm
        {
            mov  eax, [esp]
            mov  [g_retAddrStash], eax
            mov  eax, [esp+4]
            mov  [g_arg0Stash], eax
            mov  eax, [esp+8]
            mov  [g_arg4Stash], eax
            mov  [g_thisStash], ecx

            pushad
            push dword ptr [g_thisStash]
            push dword ptr [g_retAddrStash]
            push dword ptr [g_arg4Stash]
            push dword ptr [g_arg0Stash]
            call CheckDispatchCall
            add  esp, 16
            popad

            ; 原樣重跑被覆蓋的前兩句指令（此時esp/ecx跟進入時相同，
            ; mov eax,[esp+4]不依賴任何被pushad/popad影響的狀態）。
            mov  eax, [esp+4]
            push ebx

            mov  edx, kVA_ReturnTo
            jmp  edx
        }
    }

    void Install()
    {
        BYTE* p = (BYTE*)kVA_Entry;
        const BYTE kExpected[5] = { 0x8B, 0x44, 0x24, 0x04, 0x53 };
        for (int i = 0; i < 5; i++)
        {
            if (p[i] != kExpected[i])
            {
                Log::Write("[TexDispatchHook] 0x%08X第%d byte是0x%02X，預期0x%02X——版本不符或已被其他patch動過，放棄安裝",
                           kVA_Entry, i, p[i], kExpected[i]);
                return;
            }
        }

        DWORD oldProt = 0;
        VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &oldProt);
        *p = 0xE9;
        *(INT32*)(p + 1) = (INT32)((DWORD)&DispatchDetour - (DWORD)(p + 5));
        VirtualProtect(p, 5, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), p, 5);

        Log::Write("[TexDispatchHook] sub_48F330 entry hook安裝完成：site=0x%08X detour=%p", kVA_Entry, &DispatchDetour);
    }
}
