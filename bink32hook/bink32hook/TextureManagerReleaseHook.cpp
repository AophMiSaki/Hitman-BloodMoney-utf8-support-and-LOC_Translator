#include "TextureManagerReleaseHook.h"
#include "NativeTextureRegistry.h"
#include "LocHook.h"
#include "Log.h"
// [ExcTrace 取消註解 1/5] 以下兩行
// #include <stdio.h>
#include <string.h>
// #include <tlhelp32.h>

namespace TextureManagerReleaseHook
{
    // sub_48F190 = ZTextureManagerD3D::ReleaseTextures 的入口 5 bytes（kExpected，安裝前比對）。
    static const DWORD kVA_Entry = 0x0048F190;
    static const DWORD kVA_ReturnTo = 0x0048F195;
    static const BYTE kExpected[5] = { 0x53, 0x56, 0x8B, 0xD9, 0x57 };

    static DWORD g_thisStash = 0;

    // 重設NativeTextureRegistry兩個字型槽的nativeIndex/pushedGeneration，不做native/D3D呼叫
    // （重新登記留給下次Hook_BeginScene的Update()）。
    static void __cdecl OnReleaseTextures(DWORD thisPtr)
    {
        NativeTextureRegistry::InvalidateAllSlots();

        // 原文/譯文熱鍵切換功能的LOC快取失效。
        LocHook::InvalidateCache();

        Log::Write("[TextureManagerReleaseHook] ReleaseTextures命中：this=0x%08X，已清除NativeTextureRegistry字型槽狀態＋LOC快取",
                   thisPtr);
    }

    // 錯誤修正：ReleaseTextures的材質槽迴圈與渲染執行緒的CJK_Atlas登記同時進行時，迴圈讀到登記中的
    // 占位值1當成COM指標而閃退。入口先取得NativeTextureRegistry的旗標（最多等進行中的登記完成2秒），
    // 返回位址換成出口stub，返回時放開旗標；旗標未取得時不換返回位址。
    static DWORD g_relRet = 0;
    static DWORD g_relOwner = 0;
    static DWORD g_relEnterTick = 0;

    static DWORD __cdecl OnReleaseGuardBegin()
    {
        bool owner = NativeTextureRegistry::BeginReleaseGuard();
        if (owner) g_relEnterTick = GetTickCount();
        return owner ? 1 : 0;
    }

    static void __cdecl OnReleaseGuardEnd()
    {
        NativeTextureRegistry::EndReleaseGuard();
        Log::Write("[TextureManagerReleaseHook] ReleaseTextures離開：入口到出口 %lu ms，已放開登記旗標", GetTickCount() - g_relEnterTick);
    }

    static __declspec(naked) void ReleaseExitStub()
    {
        __asm
        {
            pushfd
            pushad
            call OnReleaseGuardEnd
            popad
            popfd
            jmp  dword ptr [g_relRet]
        }
    }

    // 進入時ecx=this，pushad/popad保持ecx不變；旗標先於OnReleaseTextures取得，再重跑被覆蓋的4句指令。
    static __declspec(naked) void ReleaseTexturesDetour()
    {
        __asm
        {
            mov  [g_thisStash], ecx

            pushad
            call OnReleaseGuardBegin
            mov  [g_relOwner], eax
            push dword ptr [g_thisStash]
            call OnReleaseTextures
            add  esp, 4
            popad

            cmp  dword ptr [g_relOwner], 0
            je   no_swap
            push eax
            mov  eax, [esp+4]
            mov  [g_relRet], eax
            mov  dword ptr [esp+4], offset ReleaseExitStub
            pop  eax
        no_swap:

            ; 原樣重跑被覆蓋的4句指令（此時ecx跟進入時相同）。
            push ebx
            push esi
            mov  ebx, ecx
            push edi

            jmp  kVA_ReturnTo
        }
    }

    // sub_45FCF0 = 場景資料載入的入口 8 bytes。
    static const DWORD kVA_Scene = 0x0045FCF0;
    static const DWORD kVA_SceneReturnTo = 0x0045FCF8;
    static const BYTE kSceneExpected[8] = { 0x6A, 0xFF, 0x64, 0xA1, 0x00, 0x00, 0x00, 0x00 };

    // sub_496F00 = 著色器效果載入的入口 7 bytes。
    static const DWORD kVA_Fx = 0x00496F00;
    static const DWORD kVA_FxReturnTo = 0x00496F07;
    static const BYTE kFxExpected[7] = { 0x6A, 0xFF, 0x68, 0x9F, 0xD5, 0x72, 0x00 };

    // 引擎 256 MB 保留區管理器（0x8B4FE8）：區塊數（+0xB88）、初始化旗標。
    static const DWORD kVA_MgrBlockCount = 0x008B5B70;
    static const DWORD kVA_MgrInitFlag = 0x008B5B7C;
    // 保留區基底（+0xB8C）、管理器物件、管理器全域臨界區。
    // 第 i 個 1 MB 區塊的表格在 物件 + 0x388 + i×8：+0 free list 頭(dword)、+4 已用頁數(word)、+6 下一個新頁序號(word)。
    static const DWORD kVA_MgrBase = 0x008B5B74;
    static const DWORD kVA_MgrObj = 0x008B4FE8;
    static const DWORD kVA_MgrCS = 0x008B4FCC;

    static volatile LONG g_sceneActive = 0;
    static DWORD g_sceneRet = 0;
    static DWORD g_sceneThread = 0;
    static unsigned g_sceneSeq = 0;

    static volatile LONG g_fxActive = 0;
    static DWORD g_fxRet = 0;

    // [ExcTrace 取消註解 2/5] 整段註解（結案後暫停使用；發生錯誤時取消註解，
    // 同時取消 1/5、3/5、4/5、5/5 共五處）。範圍：到 VehHandler 結尾的 "*/" 為止。
    /*
    // 場景載入期間用向量式例外處理器記每個首次例外（代碼、位址、所屬模組、暫存器、疑似返回位址），
    // 補CrashHandler抓不到的快速失敗。永遠回傳CONTINUE_SEARCH；模組表在場景開始時建好。
    static const DWORD kCodeLo = 0x00401000;
    static const DWORD kCodeHi = 0x00730000;

    struct ModEntry
    {
        DWORD base;
        DWORD size;
        char name[32];
    };
    static ModEntry g_mods[200] = {};
    static int g_modCount = 0;
    static volatile LONG g_inVeh = 0;
    static unsigned g_sceneExcCount = 0;
    static unsigned g_vehLogged = 0;
    static const unsigned kVehLogCap = 60;
    static bool g_vehInstalled = false;

    // 候選返回位址：落在exe程式碼範圍，且前面緊鄰的是call指令（E8 rel32 或 FF /2 各種長度）。
    static bool IsLikelyReturnAddr(DWORD a)
    {
        if (a < kCodeLo + 8 || a >= kCodeHi) return false;
        const BYTE* p = (const BYTE*)a;
        return p[-5] == 0xE8 || p[-2] == 0xFF || p[-3] == 0xFF || p[-6] == 0xFF;
    }

    static void RefreshModuleTable()
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
        if (snap == INVALID_HANDLE_VALUE) return;
        MODULEENTRY32 me;
        me.dwSize = sizeof(me);
        int n = 0;
        if (Module32First(snap, &me))
        {
            do
            {
                if (n < 200)
                {
                    g_mods[n].base = (DWORD)me.modBaseAddr;
                    g_mods[n].size = me.modBaseSize;
                    strncpy_s(g_mods[n].name, sizeof(g_mods[n].name), me.szModule, _TRUNCATE);
                    n++;
                }
            } while (Module32Next(snap, &me));
        }
        CloseHandle(snap);
        g_modCount = n;
    }

    static void FindModule(DWORD addr, char* out, size_t outLen, DWORD* offOut)
    {
        *offOut = 0;
        strncpy_s(out, outLen, "?", _TRUNCATE);
        for (int i = 0; i < g_modCount; i++)
        {
            if (addr >= g_mods[i].base && addr < g_mods[i].base + g_mods[i].size)
            {
                strncpy_s(out, outLen, g_mods[i].name, _TRUNCATE);
                *offOut = addr - g_mods[i].base;
                return;
            }
        }
    }

    static LONG CALLBACK VehHandler(PEXCEPTION_POINTERS ep)
    {
        if (!g_sceneActive) return EXCEPTION_CONTINUE_SEARCH;

        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        DWORD code = er->ExceptionCode;
        if (code == 0x40010006 || code == 0x406D1388 || code == 0x40010005 || code == 0x4001000A)
            return EXCEPTION_CONTINUE_SEARCH;

        g_sceneExcCount++;
        if (g_vehLogged >= kVehLogCap) return EXCEPTION_CONTINUE_SEARCH;
        if (InterlockedCompareExchange(&g_inVeh, 1, 0) != 0) return EXCEPTION_CONTINUE_SEARCH;
        g_vehLogged++;

        char modName[32];
        DWORD off = 0;
        FindModule((DWORD)er->ExceptionAddress, modName, sizeof(modName), &off);

        char info[96] = {};
        if (er->NumberParameters >= 2)
            _snprintf_s(info, sizeof(info), _TRUNCATE, "參數[0]=0x%X 參數[1]=0x%X", (unsigned)er->ExceptionInformation[0], (unsigned)er->ExceptionInformation[1]);
        else if (er->NumberParameters == 1)
            _snprintf_s(info, sizeof(info), _TRUNCATE, "參數[0]=0x%X", (unsigned)er->ExceptionInformation[0]);

        const CONTEXT* c = ep->ContextRecord;
        char chain[160] = {};
        BYTE stackBuf[512];
        SIZE_T got = 0;
        if (c && ReadProcessMemory(GetCurrentProcess(), (LPCVOID)c->Esp, stackBuf, sizeof(stackBuf), &got) && got >= 4)
        {
            const DWORD* d = (const DWORD*)stackBuf;
            int coff = 0;
            int found = 0;
            for (SIZE_T i = 0; i < got / 4 && found < 8 && coff < (int)sizeof(chain) - 12; i++)
            {
                if (IsLikelyReturnAddr(d[i]))
                {
                    coff += _snprintf_s(chain + coff, sizeof(chain) - coff, _TRUNCATE, " 0x%08X", d[i]);
                    found++;
                }
            }
        }

        Log::Write("[ExcTrace] 場景載入#%u 首次例外#%u：code=0x%08X flags=0x%X 位址=0x%08X(%s+0x%X) %s｜thread=%lu 是載入執行緒=%d｜EIP=0x%08X ESP=0x%08X EBP=0x%08X EAX=0x%08X EBX=0x%08X ECX=0x%08X EDX=0x%08X ESI=0x%08X EDI=0x%08X｜exe內疑似返回位址:%s｜sub_496F00進行中=%d",
                   g_sceneSeq, g_sceneExcCount, code, er->ExceptionFlags, (DWORD)er->ExceptionAddress, modName, off, info,
                   GetCurrentThreadId(), GetCurrentThreadId() == g_sceneThread ? 1 : 0,
                   c ? c->Eip : 0, c ? c->Esp : 0, c ? c->Ebp : 0, c ? c->Eax : 0, c ? c->Ebx : 0, c ? c->Ecx : 0, c ? c->Edx : 0, c ? c->Esi : 0, c ? c->Edi : 0,
                   chain, g_fxActive ? 1 : 0);
        g_inVeh = 0;
        return EXCEPTION_CONTINUE_SEARCH;
    }
    */ // [ExcTrace 取消註解 2/5] 結束

    // ---- 低位址區段修法（md\M05載入變慢調查.md §2）----
    // NtAllocateVirtualMemory hook內，把sub_496F00期間落在>=2GB的4～16 MB區段，換成引擎256MB保留區
    // 新借的連續k個1 MB區塊（持管理器臨界區、自己提交、表格設為已用16頁、區塊數加k），留到行程結束。
    // hook內不呼叫Log，事件寫進環狀陣列，場景開始／結束時輸出。
    static const SIZE_T kFixMinBytes = 4u * 1024u * 1024u;
    static const SIZE_T kFixMaxBytes = 16u * 1024u * 1024u;
    static const DWORD kFixBudgetMargin = 32;

    // 0=待檢查（管理器尚未初始化）、1=啟用、2=停用
    static volatile LONG g_fixState = 0;
    static DWORD g_fixMaxBlocks = 0;
    // 每場景：d3d向高位址配置的次數、已替換的次數；整個行程：累計借用的保留區塊數。
    static unsigned g_fixHighSeen = 0, g_fixHit = 0;
    static unsigned g_fixBlocksTotal = 0;

    struct FixEvt
    {
        volatile LONG ready;
        DWORD kind, tid, a, b, c, d, e;
    };
    static const int kFixRing = 128;
    static FixEvt g_fixRing[kFixRing] = {};
    static volatile LONG g_fixWrite = 0;
    static LONG g_fixRead = 0;

    static void FixEvent(DWORD kind, DWORD a, DWORD b, DWORD c, DWORD d, DWORD e)
    {
        LONG idx = InterlockedIncrement(&g_fixWrite) - 1;
        FixEvt* x = &g_fixRing[idx % kFixRing];
        x->ready = 0;
        x->kind = kind;
        x->tid = GetCurrentThreadId();
        x->a = a;
        x->b = b;
        x->c = c;
        x->d = d;
        x->e = e;
        x->ready = idx + 1;
    }

    static void FlushFixEvents()
    {
        LONG w = g_fixWrite;
        if (w - g_fixRead > kFixRing)
        {
            Log::Write("[LowFix] 事件環狀陣列溢出，略過 %ld 筆事件", w - g_fixRead - kFixRing);
            g_fixRead = w - kFixRing;
        }
        while (g_fixRead < w)
        {
            const FixEvt* e = &g_fixRing[g_fixRead % kFixRing];
            if (e->ready != g_fixRead + 1) break;
            g_fixRead++;
            switch (e->kind)
            {
            case 1:
                Log::Write("[LowFix] 已替換：d3d向高位址配置 0x%08X(>=2GB) → 保留區 0x%08X｜保留區本次配置 %u 塊(%u 頁，每頁 64 KB)｜管理器區塊數 %u→%u",
                           e->a, e->b, e->c, e->c * 16, e->d, e->e);
                break;
            case 2:
                Log::Write("[LowFix] 未替換：取鎖失敗(0x8B4FCC)，d3d向高位址配置 0x%08X 大小=%u KB 維持原狀", e->a, e->b >> 10);
                break;
            case 3:
                Log::Write("[LowFix] 未替換：保留區預算不足，d3d向高位址配置維持原狀｜管理器區塊數=%u 需要 k=%u 預算上限=%u", e->a, e->b, e->c);
                break;
            case 4:
                Log::Write("[LowFix] 未替換：保留區提交失敗，狀態=0x%08X 目標位址=0x%08X k=%u｜已向原函式重新要區段：狀態=0x%08X 位址=0x%08X", e->a, e->b, e->c, e->d, e->e);
                break;
            case 5:
                Log::Write("[LowFix] 未替換：還掉原區段 0x%08X 失敗，狀態=0x%08X，d3d向高位址配置維持原狀", e->b, e->a);
                break;
            default:
                Log::Write("[LowFix] 未知事件 kind=%lu", e->kind);
                break;
            }
        }
    }

    // 管理器初始化後首次場景開始時檢查：保留區基底、實際大小、是否在低 2GB、臨界區是否可用。
    static void ValidateFix()
    {
        if (g_fixState != 0) return;
        const char* why = NULL;
        DWORD base = 0, total = 0, count = 0;
        __try
        {
            if (!(*(const DWORD*)kVA_MgrInitFlag & 1)) return;
            count = *(const DWORD*)kVA_MgrBlockCount;
            base = *(const DWORD*)kVA_MgrBase;
            const CRITICAL_SECTION* cs = (const CRITICAL_SECTION*)kVA_MgrCS;
            MEMORY_BASIC_INFORMATION mbi = {};
            if (!cs->DebugInfo) why = "管理器臨界區 0x8B4FCC 未初始化";
            else if (count > 0x100) why = "管理器區塊數大於 256";
            else if (!base || (base & 0xFFFF)) why = "保留區基底為 0 或未 64 KB 對齊";
            else if (!VirtualQuery((const void*)base, &mbi, sizeof(mbi)) || mbi.Type != MEM_PRIVATE || (DWORD)mbi.AllocationBase != base)
                why = "保留區基底不是 MEM_PRIVATE 配置的起點";
            else
            {
                BYTE* p = (BYTE*)base;
                while (total < 0x20000000u && VirtualQuery(p, &mbi, sizeof(mbi)) && (DWORD)mbi.AllocationBase == base && mbi.RegionSize)
                {
                    total += (DWORD)mbi.RegionSize;
                    p += mbi.RegionSize;
                }
                if (!total) why = "VirtualQuery 查不到保留區大小";
                else if ((unsigned __int64)base + total > 0x80000000ull) why = "保留區超出低 2GB";
                else if ((total >> 20) < count + 8 + kFixBudgetMargin) why = "保留區太小，扣掉預算邊界後放不下一個區段";
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            why = "讀取管理器欄位時發生存取違規";
        }

        if (why)
        {
            g_fixState = 2;
            Log::Write("[LowFix] 功能停用：%s（保留區基底=0x%08X 實際保留大小=%u MB 管理器區塊數=%u）", why, base, total >> 20, count);
            return;
        }
        g_fixMaxBlocks = (total >> 20) < 256 ? (total >> 20) : 256;
        g_fixState = 1;
    }

    static void __cdecl OnSceneEnter()
    {
        g_sceneSeq++;
        g_fixHighSeen = g_fixHit = 0;
        // [ExcTrace 取消註解 3/5] 以下三行
        // g_sceneExcCount = 0;
        // g_vehLogged = 0;
        // RefreshModuleTable();
        g_sceneThread = GetCurrentThreadId();
        g_sceneActive = 1;

        FlushFixEvents();
        ValidateFix();
    }

    // 只在這個場景載入期間發生過d3d向高位址配置時輸出一行。借用的保留區塊由msvcrt堆積持有至行程結束，不會歸還。
    static void __cdecl OnSceneExit()
    {
        g_sceneActive = 0;

        FlushFixEvents();
        if (g_fixHighSeen)
            Log::Write("[LowFix] 場景載入#%u 結束：d3d向高位址配置 %u 次｜已替換 %u 次、未替換 %u 次｜保留區累計已配置 %u 塊(%u 頁，每頁 64 KB)｜歸還：否(區塊由msvcrt堆積持有至行程結束)",
                       g_sceneSeq, g_fixHighSeen, g_fixHit, g_fixHighSeen - g_fixHit, g_fixBlocksTotal, g_fixBlocksTotal * 16);
        // [ExcTrace 取消註解 4/5]
        // Log::Write("[ExcTrace] 場景載入#%u 載入期間首次例外共 %u 筆（已記錄 %u 筆，上限 %u）", g_sceneSeq, g_sceneExcCount, g_vehLogged, kVehLogCap);
    }

    static __declspec(naked) void SceneExitStub()
    {
        __asm
        {
            pushfd
            pushad
            call OnSceneExit
            popad
            popfd
            jmp  dword ptr [g_sceneRet]
        }
    }

    static __declspec(naked) void SceneDetour()
    {
        __asm
        {
            cmp  dword ptr [g_sceneActive], 0
            jne  passthru

            pushad
            call OnSceneEnter
            popad

            mov  eax, [esp]
            mov  [g_sceneRet], eax
            mov  dword ptr [esp], offset SceneExitStub

        passthru:
            push 0FFFFFFFFh
            mov  eax, dword ptr fs:[0]
            jmp  dword ptr [kVA_SceneReturnTo]
        }
    }

    static void __cdecl OnFxEnter()
    {
        g_fxActive = 1;
    }

    static void __cdecl OnFxExit()
    {
        g_fxActive = 0;
    }

    static __declspec(naked) void FxExitStub()
    {
        __asm
        {
            pushfd
            pushad
            call OnFxExit
            popad
            popfd
            jmp  dword ptr [g_fxRet]
        }
    }

    static __declspec(naked) void FxDetour()
    {
        __asm
        {
            mov  eax, fs:[24h]
            cmp  eax, [g_sceneThread]
            jne  passthru
            cmp  dword ptr [g_fxActive], 0
            jne  passthru

            pushad
            call OnFxEnter
            popad

            mov  eax, [esp]
            mov  [g_fxRet], eax
            mov  dword ptr [esp], offset FxExitStub

        passthru:
            push 0FFFFFFFFh
            push 0072D59Fh
            jmp  dword ptr [kVA_FxReturnTo]
        }
    }

    static bool PatchJmp(DWORD va, const BYTE* expected, int len, void* detour, const char* tag)
    {
        BYTE* p = (BYTE*)va;
        for (int i = 0; i < len; i++)
        {
            if (p[i] != expected[i])
            {
                Log::Write("[LowFix] %s 0x%08X第%d byte是0x%02X，預期0x%02X——版本不符或已被其他patch動過，放棄安裝",
                           tag, va, i, p[i], expected[i]);
                return false;
            }
        }

        DWORD oldProt = 0;
        VirtualProtect(p, len, PAGE_EXECUTE_READWRITE, &oldProt);
        p[0] = 0xE9;
        *(INT32*)(p + 1) = (INT32)((DWORD)detour - (DWORD)(p + 5));
        for (int i = 5; i < len; i++) p[i] = 0x90;
        VirtualProtect(p, len, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), p, len);
        return true;
    }

    // NtAllocateVirtualMemory（含 Zw 別名）hook；NtFreeVirtualMemory 只取原函式位址。
    typedef LONG (NTAPI* PFN_NtAlloc)(HANDLE, PVOID*, ULONG_PTR, PSIZE_T, ULONG, ULONG);
    typedef LONG (NTAPI* PFN_NtFree)(HANDLE, PVOID*, PSIZE_T, ULONG);
    static PFN_NtAlloc g_oNtAlloc = NULL;
    static PFN_NtFree g_oNtFree = NULL;

    // 持有管理器臨界區時執行：還掉 >=2GB 的原區段，借 k 塊連續低位址區塊並寫入表格。
    // 不呼叫引擎的sub_44CF00（提交失敗會int3崩潰）；提交失敗時向原函式重新要區段並回傳其結果。
    static LONG LowFixLocked(HANDLE h, PVOID* pBase, ULONG_PTR zero, PSIZE_T pSize, ULONG type, ULONG prot,
                             SIZE_T reqSize, DWORD addr, SIZE_T size, LONG st)
    {
        DWORD k = (DWORD)((size + 0xFFFFF) >> 20);
        DWORD count = *(const DWORD*)kVA_MgrBlockCount;
        DWORD base = *(const DWORD*)kVA_MgrBase;

        if (count + k + kFixBudgetMargin > g_fixMaxBlocks)
        {
            FixEvent(3, count, k, g_fixMaxBlocks, 0, 0);
            return st;
        }

        PVOID rb = (PVOID)addr;
        SIZE_T rs = 0;
        LONG fs = g_oNtFree(h, &rb, &rs, MEM_RELEASE);
        if (fs < 0)
        {
            FixEvent(5, (DWORD)fs, addr, 0, 0, 0);
            return st;
        }

        DWORD target = base + count * 0x100000u;
        PVOID cb = (PVOID)target;
        SIZE_T cs = (SIZE_T)k << 20;
        LONG cst = g_oNtAlloc(h, &cb, 0, &cs, MEM_COMMIT, PAGE_READWRITE);
        if (cst < 0)
        {
            PVOID nb = NULL;
            SIZE_T ns = reqSize;
            LONG rst = g_oNtAlloc(h, &nb, zero, &ns, type, prot);
            *pBase = nb;
            *pSize = ns;
            FixEvent(4, (DWORD)cst, target, k, (DWORD)rst, (DWORD)nb);
            return rst;
        }

        for (DWORD i = 0; i < k; i++)
        {
            BYTE* t = (BYTE*)(kVA_MgrObj + 0x388 + (count + i) * 8);
            *(DWORD*)t = 0;
            *(WORD*)(t + 4) = 16;
            *(WORD*)(t + 6) = 16;
        }
        *(DWORD*)kVA_MgrBlockCount = count + k;

        *pBase = (PVOID)target;
        *pSize = size;
        g_fixHit++;
        g_fixBlocksTotal += k;
        FixEvent(1, addr, target, k, count, count + k);
        return 0;
    }

    // 合格配置（原函式已成功）位址越線且功能啟用才取鎖處理，其餘放行。
    static LONG LowFixCandidate(HANDLE h, PVOID* pBase, ULONG_PTR zero, PSIZE_T pSize, ULONG type, ULONG prot, SIZE_T reqSize, LONG st)
    {
        DWORD addr = 0;
        SIZE_T size = 0;
        __try
        {
            addr = (DWORD)*pBase;
            size = *pSize;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return st;
        }

        if (addr < 0x80000000u) return st;
        g_fixHighSeen++;
        if (g_fixState != 1) return st;
        if (!TryEnterCriticalSection((LPCRITICAL_SECTION)kVA_MgrCS))
        {
            FixEvent(2, addr, (DWORD)size, 0, 0, 0);
            return st;
        }

        LONG r = st;
        __try
        {
            __try
            {
                r = LowFixLocked(h, pBase, zero, pSize, type, prot, reqSize, addr, size, st);
            }
            __finally
            {
                LeaveCriticalSection((LPCRITICAL_SECTION)kVA_MgrCS);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return r;
    }

    static LONG NTAPI H_NtAlloc(HANDLE h, PVOID* pBase, ULONG_PTR zero, PSIZE_T pSize, ULONG type, ULONG prot)
    {
        bool cand = false;
        SIZE_T reqSize = 0;
        if ((DWORD)h == 0xFFFFFFFFu && g_fxActive && (type & MEM_RESERVE) && prot == PAGE_READWRITE && GetCurrentThreadId() == g_sceneThread)
        {
            __try
            {
                if (pBase && pSize && *pBase == NULL)
                {
                    reqSize = *pSize;
                    cand = reqSize >= kFixMinBytes && reqSize <= kFixMaxBytes;
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }

        LONG st = g_oNtAlloc(h, pBase, zero, pSize, type, prot);
        if (!cand || st < 0) return st;
        return LowFixCandidate(h, pBase, zero, pSize, type, prot, reqSize, st);
    }

    // 把 ntdll 入口（必須是 B8 imm32）改成 jmp，回傳跳板；跳板位址先存進 *origOut 再改寫入口。
    static BYTE* NtHookStub(const char* name, void* hook, BYTE* sharedTramp, void** origOut)
    {
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        BYTE* p = nt ? (BYTE*)GetProcAddress(nt, name) : NULL;
        if (!p)
        {
            Log::Write("[NtHook] %s：找不到 ntdll 匯出，略過", name);
            return sharedTramp;
        }
        if (p[0] != 0xB8)
        {
            Log::Write("[NtHook] %s：入口第0 byte 不是 0xB8（實際 0x%02X），略過", name, p[0]);
            return sharedTramp;
        }
        BYTE* tr = sharedTramp;
        if (!tr)
        {
            tr = (BYTE*)VirtualAlloc(NULL, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (!tr)
            {
                Log::Write("[NtHook] %s：跳板配置失敗 err=%lu", name, GetLastError());
                return NULL;
            }
            memcpy(tr, p, 5);
            tr[5] = 0xE9;
            *(INT32*)(tr + 6) = (INT32)((DWORD)(p + 5) - ((DWORD)tr + 10));
            FlushInstructionCache(GetCurrentProcess(), tr, 16);
        }
        *origOut = tr;
        DWORD oldProt = 0;
        VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &oldProt);
        *(INT32*)(p + 1) = (INT32)((DWORD)hook - ((DWORD)p + 5));
        p[0] = 0xE9;
        VirtualProtect(p, 5, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), p, 5);
        return tr;
    }

    static void InstallNtHook()
    {
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        if (!nt)
        {
            Log::Write("[NtHook] 找不到 ntdll.dll，未安裝");
            return;
        }
        BYTE* aNt = (BYTE*)GetProcAddress(nt, "NtAllocateVirtualMemory");
        BYTE* aZw = (BYTE*)GetProcAddress(nt, "ZwAllocateVirtualMemory");

        // NtFreeVirtualMemory不hook，只取原函式位址，供LowFixLocked還掉越線的原區段。
        g_oNtFree = (PFN_NtFree)GetProcAddress(nt, "NtFreeVirtualMemory");

        BYTE* trA = NtHookStub("NtAllocateVirtualMemory", (void*)&H_NtAlloc, NULL, (void**)&g_oNtAlloc);
        if (trA && aZw && aZw != aNt) NtHookStub("ZwAllocateVirtualMemory", (void*)&H_NtAlloc, trA, (void**)&g_oNtAlloc);
        if (!trA || !g_oNtFree)
        {
            g_fixState = 2;
            Log::Write("[NtHook] 安裝失敗，修法停用：NtAllocateVirtualMemory hook=%s NtFreeVirtualMemory位址=%s", trA ? "成功" : "失敗", g_oNtFree ? "取得" : "取不到");
        }
    }

    static void InstallSceneHooks()
    {
        // [ExcTrace 取消註解 5/5] 以下整個 if 區塊
        // if (!g_vehInstalled)
        // {
        //     g_vehInstalled = AddVectoredExceptionHandler(1, VehHandler) != NULL;
        //     Log::Write("[ExcTrace] 向量式例外處理器%s", g_vehInstalled ? "安裝完成" : "安裝失敗");
        // }
        PatchJmp(kVA_Fx, kFxExpected, 7, (void*)&FxDetour, "sub_496F00(著色器效果載入)");
        PatchJmp(kVA_Scene, kSceneExpected, 8, (void*)&SceneDetour, "sub_45FCF0(場景資料載入)");
        InstallNtHook();
    }

    void Install()
    {
        InstallSceneHooks();

        BYTE* p = (BYTE*)kVA_Entry;
        for (int i = 0; i < 5; i++)
        {
            if (p[i] != kExpected[i])
            {
                Log::Write("[TextureManagerReleaseHook] 0x%08X第%d byte是0x%02X，預期0x%02X——版本不符或已被其他patch動過，放棄安裝",
                           kVA_Entry, i, p[i], kExpected[i]);
                return;
            }
        }

        DWORD oldProt = 0;
        VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &oldProt);
        *p = 0xE9;
        *(INT32*)(p + 1) = (INT32)((DWORD)&ReleaseTexturesDetour - (DWORD)(p + 5));
        VirtualProtect(p, 5, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), p, 5);

        Log::Write("[TextureManagerReleaseHook] sub_48F190 entry hook安裝完成：site=0x%08X detour=%p", kVA_Entry, &ReleaseTexturesDetour);
    }
}
