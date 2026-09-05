#include "SubtitleBriefSpeech.h"
#include "Config.h"
#include "Log.h"

namespace SubtitleBriefSpeech
{
    // [Debug] DebugSubtitleDiagEnable，Install() 時讀一次快取。目前只用來
    // 控制 sub_6849B0（訓練提示音效，非字幕訊號）那筆逐次觸發的診斷 log。
    static bool g_diagEnable = false;

    // 每次語音真正觸發都遞增，供SubtitleBrief做edge-detection。刻意
    // 從1開始（0保留給「本場景尚未觸發過」）。目前訊號源只有sub_59FA90
    // (M00文字驅動語音)跟sub_6CEA40(全任務通用旁白queue player)，見下方
    // OnCreateChannelEntry。
    static DWORD g_speechEventId = 0;

    // ---- sub_4EA670(建立聲音channel的共用底層函式)函式入口side-record ----
    //
    // 在共用底層函式入口埋點、用return address辨識呼叫端，免得逐一追上層
    // 10個呼叫端、或漏掉vtable間接呼叫進來的路徑。
    //
    // 插入點0x4EA670起5 bytes是單一完整指令(`mov eax, dword_820820`)，剛好
    // 5 bytes=JMP patch大小，不需NOP補齊。函式雖是thiscall，但進入後第一件
    // 事就是把傳入的ecx(this)整個蓋掉，原函式從來不用呼叫端傳進來的this——
    // 這裡記錄的ecx只是「呼叫端原本想傳的this」，供交叉核對呼叫端身分用，
    // 純觀察，patch完等價語意。
    static const DWORD kVA_CreateChannelSite = 0x004EA670;
    static const DWORD kVA_CreateChannelContinue = 0x004EA675;
    static const BYTE kExpectedCreateChannel[5] = { 0xA1,0x20,0x08,0x82,0x00 };

    // ---- 用returnAddr判斷M00簡報字幕的真正觸發點 ----
    //
    // sub_6849B0(訓練提示彈窗音效)沒有文字參數，語意上不是旁白，不當字幕
    // 訊號；sub_59FA90吃一個char* String2參數，經sub_4652C0/sub_464F10轉成
    // 音效資源才呼叫——只有這條路徑算「有文字內容的語音」，才是字幕觸發
    // 訊號。用returnAddr辨識：0x6849EA=sub_6849B0、0x59FAE0=sub_59FA90。
    static const DWORD kVA_TrainingReturnAddr = 0x006849EA;
    static const DWORD kVA_BriefingSpeakReturnAddr = 0x0059FAE0;

    // sub_6CEA40（全任務通用簡報旁白queue player，見
    // G:\bloodmoney\md\字幕功能.md）內部有兩處呼叫sub_4EA670建聲音channel：
    // 一處是本場景第一句旁白開始播放、一處是前一句播完index遞增後替新句
    // 建channel，對應return address分別是0x6CEA80(第一句)/0x6CEB27(後續句)。
    // 目前index存在sub_6CEA40的this+0x65，而esi從函式入口到這兩個call點
    // 都沒被動過，call當下esi仍等於sub_6CEA40的this——sub_4EA670的ecx參數
    // 在這兩處是另一個聲音物件、不是它的this，所以額外在
    // Detour_CreateChannelEntry多存一份esi傳進來取index，純供log交叉核對。
    //
    // 這兩個returnAddr命中時比照sub_59FA90分支更新t0錨點/eventId，把簡報
    // 字幕觸發訊號源從僅M00適用擴大成全任務通用，不動SubtitleBrief
    // 既有的LOC查表/渲染邏輯。
    //
    // sub_6CEA40跟sub_59FA90互斥（M00只走sub_59FA90、其他任務只走
    // sub_6CEA40），直接更新共用的g_speechEventId即可（曾拆獨立計數器，
    // 反而讓M00收不到任何line事件、字幕整個不顯示）。
    static const DWORD kVA_NarratorFirstLineReturnAddr = 0x006CEA80;
    static const DWORD kVA_NarratorNextLineReturnAddr = 0x006CEB27;

    static void __cdecl OnCreateChannelEntry(DWORD callerThis, DWORD returnAddr, DWORD arg0, DWORD callerEsi)
    {
        (void)callerThis; (void)arg0;
        // 舊版：每個sub_4EA670呼叫端都印（太吵，已停用）
        // Log::Write("[SubtitleBriefSpeech] sub_4EA670(建立聲音channel)被呼叫：returnAddr=0x%08X this=0x%08X arg0=0x%08X", returnAddr, callerThis, arg0);

        if (returnAddr == kVA_BriefingSpeakReturnAddr)
        {
            g_speechEventId++;
            // 舊版：Log::Write("[SubtitleBriefSpeech] 判定：sub_59FA90(文字驅動語音)觸發建channel——採用為字幕t0錨點，eventId=%lu", g_speechEventId);
            // 新版：本場景句0只印一次，確認t0偵測有生效（g_speechEventId在本檔不歸零，等於process內一次）
            if (g_speechEventId == 1)
                Log::Write("[SubtitleBriefSpeech] sub_59FA90 句0 開始播放");
        }
        else if (returnAddr == kVA_TrainingReturnAddr)
        {
            // sub_6849B0(訓練提示音效，無文字內容)不當字幕訊號、不遞增 eventId，
            // 這裡純診斷標記——訓練關每個提示都會觸發、量大，綁 DebugSubtitleDiagEnable。
            if (g_diagEnable)
                Log::Write("[SubtitleBriefSpeech] sub_6849B0被呼叫（訓練提示音效，不當字幕訊號）");
        }
        else if (returnAddr == kVA_NarratorFirstLineReturnAddr || returnAddr == kVA_NarratorNextLineReturnAddr)
        {
            int index = (int)*(const BYTE*)(callerEsi + 0x65);
            g_speechEventId++;
            // 舊版：Log::Write("[SubtitleBriefSpeech] 判定：sub_6CEA40(全任務通用簡報旁白)%s觸發建channel（narratorThis=0x%08X index=%d）——採用為字幕t0錨點，eventId=%lu", returnAddr == kVA_NarratorFirstLineReturnAddr ? "第一句" : "後續句", callerEsi, index, g_speechEventId);
            // 新版：只在句0(第一句)印一次，後續句不印
            if (returnAddr == kVA_NarratorFirstLineReturnAddr)
                Log::Write("[SubtitleBriefSpeech] sub_6CEA40 句%d 開始播放", index);
        }
    }

    static __declspec(naked) void Detour_CreateChannelEntry()
    {
        __asm
        {
            // 尚未執行任何原指令，此刻[esp]=return address、[esp+4]=arg0
            // （thiscall單一stack參數），ecx=呼叫端傳入但會被原函式忽略的this。
            // esi在這個時間點若呼叫端是sub_6CEA40，仍等於sub_6CEA40自己的
            // this（見上方OnCreateChannelEntry註解），其餘呼叫端esi值無意義
            // 但一併傳入不影響其他分支判斷。
            mov edx, [esp]
            mov eax, [esp+4]

            pushfd
            pushad
            push esi
            push eax
            push edx
            push ecx
            call OnCreateChannelEntry
            add esp, 16
            popad
            popfd

            mov eax, dword ptr ds:[00820820h]
            jmp kVA_CreateChannelContinue
        }
    }

    static void InstallCreateChannelPatch()
    {
        BYTE* p = (BYTE*)kVA_CreateChannelSite;
        for (int i = 0; i < 5; i++)
        {
            if (p[i] != kExpectedCreateChannel[i])
            {
                Log::Write("[SubtitleBriefSpeech] sub_4EA670 側錄hook安裝失敗："
                           "0x%08X 第%d byte=0x%02X 預期0x%02X", kVA_CreateChannelSite, i, p[i], kExpectedCreateChannel[i]);
                return;
            }
        }

        DWORD oldProt = 0;
        VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &oldProt);
        p[0] = 0xE9;
        *(INT32*)(p + 1) = (INT32)((DWORD)&Detour_CreateChannelEntry - (DWORD)(p + 5));
        VirtualProtect(p, 5, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), p, 5);

        Log::Write("[SubtitleBriefSpeech] sub_4EA670 側錄hook安裝成功");
    }

    // ---- sub_59FA90(文字驅動語音)函式入口side-record，記錄String2實際內容 ----
    //
    // sub_4EA670入口的OnCreateChannelEntry只能靠returnAddr判斷「是不是
    // sub_59FA90觸發的」，但拿不到原始文字——sub_59FA90內部呼叫
    // sub_4652C0/sub_464F10把String2轉成音效資源後，傳給sub_4EA670的引數
    // 已經不是文字了。要知道這句語音講的是哪一行字幕，必須在sub_59FA90
    // 自己的入口攔截，這裡的String2還是原始char*。
    //
    // 插入點0x59FA90起6 bytes是單一完整指令(`sub esp, 204h`)，5-byte JMP
    // patch後補1個NOP。此時函式尚未執行任何指令，esp還沒被調整，
    // [esp]=return address、[esp+4]=String2(char*，非thiscall)。
    static const DWORD kVA_BriefingSpeakEntrySite = 0x0059FA90;
    static const DWORD kVA_BriefingSpeakEntryContinue = 0x0059FA96;
    static const BYTE kExpectedBriefingSpeakEntry[6] = { 0x81,0xEC,0x04,0x02,0x00,0x00 };

    static void __cdecl OnBriefingSpeakEntry(DWORD returnAddr, const char* string2)
    {
        (void)returnAddr; (void)string2;
        // 舊版：印returnAddr + String2原始內容
        // Log::Write("[SubtitleBriefSpeech] sub_59FA90(文字驅動語音)被呼叫：returnAddr=0x%08X String2=\"%s\"", returnAddr, string2 ? string2 : "(null)");
        // 新版：只標記被呼叫（目前停用，需要時取消註解）
        Log::Write("[SubtitleBriefSpeech] sub_59FA90被呼叫！");
    }

    static __declspec(naked) void Detour_BriefingSpeakEntry()
    {
        __asm
        {
            // 尚未執行任何原指令：[esp]=return address，[esp+4]=String2(char*)。
            mov edx, [esp]
            mov eax, [esp+4]

            pushfd
            pushad
            push eax
            push edx
            call OnBriefingSpeakEntry
            add esp, 8
            popad
            popfd

            sub esp, 204h
            jmp kVA_BriefingSpeakEntryContinue
        }
    }

    static void InstallBriefingSpeakEntryPatch()
    {
        BYTE* p = (BYTE*)kVA_BriefingSpeakEntrySite;
        for (int i = 0; i < 6; i++)
        {
            if (p[i] != kExpectedBriefingSpeakEntry[i])
            {
                Log::Write("[SubtitleBriefSpeech] sub_59FA90 側錄hook安裝失敗："
                           "0x%08X 第%d byte=0x%02X 預期0x%02X", kVA_BriefingSpeakEntrySite, i, p[i], kExpectedBriefingSpeakEntry[i]);
                return;
            }
        }

        DWORD oldProt = 0;
        VirtualProtect(p, 6, PAGE_EXECUTE_READWRITE, &oldProt);
        p[0] = 0xE9;
        *(INT32*)(p + 1) = (INT32)((DWORD)&Detour_BriefingSpeakEntry - (DWORD)(p + 5));
        p[5] = 0x90;
        VirtualProtect(p, 6, oldProt, &oldProt);
        FlushInstructionCache(GetCurrentProcess(), p, 6);

        Log::Write("[SubtitleBriefSpeech] sub_59FA90 側錄hook安裝成功");
    }

    void Install(HMODULE hModule)
    {
        g_diagEnable = Config::LoadDebug(hModule).subtitleDiagEnable;
        InstallCreateChannelPatch();
        InstallBriefingSpeakEntryPatch();
    }

    DWORD GetSpeechStartEventId()
    {
        return g_speechEventId;
    }
}
