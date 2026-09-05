#pragma once
#include <Windows.h>
#include "FontCategory.h"

// bink32hook.ini 讀寫。
// CJK字型分3個獨立section（[FontCjk]/[FontSubtitle]/[FontNewspaper]），
// Subtitle/Newspaper缺席時各自fallback成跟FontCjk一樣的內建預設值（不是
// 繼承FontCjk目前的設定值——3份完全獨立，使用者要分別在ini裡調整）。
// ASCII字元也直接查[FontCjk]這份設定（沿用相同atlas/材質）；section名稱
// 沿用"FontCjk"不改名，保留使用者既有ini裡的自訂設定。
// 沒有FontItalic：斜體由引擎自己的<i>...</i>標籤處理，字型層級不需要
// 重複設定。

namespace Config
{
    struct FontConfig
    {
        char face[LF_FACESIZE];
        int  size;
        int  weight;   // GDI字重，100~900，0=FW_DONTCARE（讓系統決定，等同不設定）
        int  width;    // GDI LOGFONT.lfWidth，0=依裝置比例自動（GDI原生預設行為）
        int  spacing;  // 字距微調（像素），0=不調整
        int  yOffset;  // Y方向位置微調（像素），0=不調整
    };

    // 首次啟動時如果ini不存在，寫入一份預設值（沒有就略過，不會覆蓋既有設定）。
    void EnsureDefaultIni(HMODULE hModule);

    // 把整份ini讀進全域快取，只能呼叫一次（由Hook::Init()在
    // EnsureDefaultIni()之後負責呼叫）。呼叫之後下面全部LoadXxx()都只回傳
    // 快取，不再重新讀ini/印log，見Config.cpp「全域設定快取」節說明。
    void Init(HMODULE hModule);

    // 讀取[FontCjk]字型設定（通用分類，ASCII/CJK共用同一份），缺欄位/整份
    // ini缺席都會fallback成內建預設值，並記log。
    FontConfig Load(HMODULE hModule);

    // 字幕分類獨立的[FontSubtitle] section，缺欄位/整份section缺席都
    // fallback成跟[FontCjk]同樣的內建預設值（不是繼承FontCjk目前的實際
    // 設定值）。
    FontConfig LoadSubtitle(HMODULE hModule);

    // 報紙依<font N>分級。單一[FontNewspaper] section設定font1(headline)的
    // face/weight/width/spacing/yOffset/FontSize（預設38），font2~4的
    // face/weight/width/spacing/yOffset完全沿用font1，FontSize則以font1為
    // 基準固定減去14/28/30px（font2=font1-14、font3=font1-28、
    // font4=font1-30，下限clamp見下方NewspaperMinRenderSizeFor()），不能
    // 個別調整font2~4的字型/字重/字級偏移量。
    FontConfig LoadNewspaperFont1(HMODULE hModule);
    FontConfig LoadNewspaperFont2(HMODULE hModule);
    FontConfig LoadNewspaperFont3(HMODULE hModule);
    FontConfig LoadNewspaperFont4(HMODULE hModule);

    // font1~4的最小值＝38/22/10/8px，這組數字同時是①EnsureDefaultIni()
    // 寫入ini的font1預設值(38)、②font2~4由font1推算時的下限、③font1本身
    // FontSize過小時的clamp下限——三個用途統一用這一份常數。單位跟ini
    // `FontSize=`一致（px）。**硬性規則：下限不能超過native `+0x11
    // lineHeight`量測值（標題54/中段22/內文13，font4沿用內文級）**——
    // 下限只是安全底線，不等於預設顯示值（font1=38時font2預設顯示
    // 38-14=24，24>22所以不會被下限clamp），兩者不同不是bug。
    inline int NewspaperMinRenderSizeFor(FontCategory category)
    {
        switch (category)
        {
        case FontCategory::NewspaperFont1: return 38;
        case FontCategory::NewspaperFont2: return 22;
        case FontCategory::NewspaperFont3: return 10;
        case FontCategory::NewspaperFont4: return 8;
        default:                            return 1;
        }
    }

    // 依FontCategory dispatch到上面三個函式之一，供GlyphAtlas泛用的
    // per-category初始化邏輯呼叫，不用自己寫switch。
    FontConfig LoadForCategory(HMODULE hModule, FontCategory category);

    // [General]區段——必要功能總開關，預設都是1(啟用)，跟[Debug]底下預設
    // 關閉的純診斷開關性質不同、不能混在一起。
    struct GeneralConfig
    {
        // 通用/報紙分類是否套用自訂CJK字型：0時native GetGlyph（主渲染/
        // 選單/量測/換行4個call site）遇到CJK碼位一律維持native原本的
        // 「缺字」sentinel，完全不呼叫SynthesizeCJKGlyphRecord合成——不論
        // 文字來源是原文LOC還是譯文txt都一樣（合成邏輯只看codepoint，
        // 不分辨文字來源），畫面上中文字全部空白。同時身兼
        // TextureManagerReleaseHook/NativeTextureRegistry的安裝總開關（見
        // Hook.cpp）——這兩個模組都是字體取代管線的一環（材質reset/atlas
        // 材質登記），沒有各自獨立ini開關，=0時連同它們一起完全不安裝，
        // 回到原版行為。
        bool normalFontReplace;
        // PickupSPC/Fire鍵位雙重編碼修復（必要功能，不是診斷），性質上跟
        // NormalFontReplace一樣屬於[General]總開關。
        bool utf8ReencodeFix;
        // CJK GetGlyph 6個call site安裝總開關。這個開關同時也是字幕統一
        // compositor（SubtitleRender）lazy retry的共用驅動點（見
        // GlyphHook.cpp::GlyphCheck()內的SubtitleHook::EnsureRenderReady()
        // 呼叫），=0時CJK合成、字幕self-render全部失效，是真正的總電源；
        // NormalFontReplace只決定patch裝好之後「要不要合成CJK字」
        // （g_synthEnable，見GlyphHook.cpp SynthesizeCJKGlyphRecord()）。
        // 兩者關係是「glyphHook是normalFontReplace能不能生效的前提」，不是
        // 對等的兩個維度，所以要分開。
        bool glyphHook;
        // CJK toupper()亂碼修復安裝開關。必要功能修復，不是二分排查用的
        // 診斷旁路開關，性質跟normalFontReplace/utf8ReencodeFix/glyphHook
        // 一致。
        bool toUpperHook;
        // 決定要不要安裝sub_441920 entry hook（側錄場景zip路徑，見
        // ZipPathTrace.h）。這個側錄結果是LocHook（原文/譯文LOC路徑推導）、
        // SubtitleBrief（場景切換偵測）、NewsPaper*系列（報紙分類判斷）
        // 3個模組共用的必要資料來源，不是單純除錯side-record，關掉會直接
        // 讓中文譯文/原文切換整個失效（症狀：log印「尚未側錄到任何場景
        // zip路徑」）。詳細的逐次trace log改由[Debug] DebugZipPathTrace
        // 單獨控制（見DebugConfig::debugZipPathTrace），只影響log要不要印，
        // 不影響這個安裝開關、也不影響GetLastSceneZipPath()本身是否運作。
        bool zipPathTrace;
    };
    GeneralConfig LoadGeneral(HMODULE hModule);

    // [Debug]區段：純診斷開關，預設關閉，關閉時維持100% passthrough給
    // native。glyphDiagEnable只控制「命中時要不要印log」，不影響行為；
    // cjkPlaceholderEnable才是這裡唯一會改變native GetGlyph回傳值的開關。
    struct DebugConfig
    {
        bool glyphDiagEnable;
        int  glyphDiagCap;
        // 遇到native查不到的CJK碼位時，把charcode偷換成ASCII佔位符號'@'再
        // 交給native GetGlyph，讓它用既有的ASCII字圖正常顯示出來（不用等
        // Stage B的FreeType/stb_truetype光柵化引擎），方便肉眼+log快速定位
        // 哪些字缺字。見GlyphHook.cpp。預設關閉。
        bool cjkPlaceholderEnable;
        // TextureUpload（Stage D：GlyphAtlas CPU點陣圖上傳成D3D9材質）成功
        // 建立/更新材質時要不要印log，不影響行為本身。預設關閉。見
        // TextureUpload.cpp。
        bool texUploadDiagEnable;
        // LocHook（原文/譯文熱鍵切換，見bloodmoney_utf8_support.md
        // 第7節第2點）查表命中/fallback時要不要印log，不影響行為。預設
        // 關閉——對話密集的場景查詢頻率不低，避免洗版。
        bool locSwitchDiagEnable;
        // ToUpperHook（MSVCR71.DLL!toupper IAT hook，修CJK codepoint轉大寫
        // 弄壞bytes的bug）逐次呼叫的codepoint進出診斷log，不影響行為。
        // 預設關閉——sub_463D10/497650/559910這3個函式逐字元呼叫，開啟後
        // log量會很大。
        bool toUpperDiagEnable;
        // NativeTextureRegistry（CJK atlas材質登記進native材質頁表）除了
        // 「換場景時登記成功/失敗」以外的細節診斷（
        // Hook_BeginScene進入次數、InvalidateAllSlots重設通知、場景中途atlas
        // 內容更新完成）要不要印log，不影響行為。預設關閉——換場景時的登記
        // 成功/失敗log不受這個旗標限制，一律顯示。見NativeTextureRegistry.cpp。
        bool nativeTexRegDiagEnable;
        // 統一控制字幕播放功能（Briefing/Dialogue/Oneliners/Walkie，見
        // [Subtitle] BriefingSubtitleEnabled/DialogueSubtitleEnabled/
        // OnelinersSubtitleEnabled/WalkieSubtitleEnabled）除了「安裝成功/失敗」以外的全部細節
        // 診斷log——SubtitleBrief.cpp的場景切換/F11熱鍵觸發點追蹤/
        // 重試逾時/首選讓位，SubtitleDialogue.cpp/SubtitleOneliners.cpp的
        // NPC/玩家距離過濾log、SubtitleGate.cpp的狀態7(首選/次選/第三皆被佔)
        // 字幕捨棄log等。log會標明是Briefing/Dialogue/Oneliners哪一個產生
        // 的（見各自檔案log tag）。預設關閉。
        bool subtitleDiagEnable;
        // LabelWidthFixInventory/LabelWidthFixShop（Inventory武器檢視面板＋
        // 商店Price欄位CJK疊字修正）逐次呼叫的widgetOffset/v5/ourV5/
        // correction診斷log要不要印，不影響修正行為本身。預設關閉——每次
        // 旋轉武器模型會觸發7~8次呼叫，開啟後log量不小。
        bool labelWidthFixDiagEnable;
        // ZipPathTrace（見ZipPathTrace.h）sub_441920逐次側錄命中時要不要印
        // 詳細log（name/mode/呼叫端returnAddr），不影響側錄行為本身——
        // GetLastSceneZipPath()一律照常更新，跟這個旗標無關。純log開關，
        // 預設關閉。安裝失敗（0x00441920 byte版本不符）的log不受這個旗標
        // 限制，一律顯示，見ZipPathTrace.cpp Install()。是否安裝這個hook
        // 本體改由[General] ZipPathTrace控制（見GeneralConfig::zipPathTrace），
        // 跟這個log開關是兩層不同的東西。
        bool debugZipPathTrace;
        // TrespassHud（元素A擅闖/敵對區域自繪字）每次判定命中時印
        // zoneKind→級別的診斷log，不影響行為本身。預設關閉。
        bool hudDiagEnable;
        // MinimapFloorProbe（見 MinimapHud.cpp FloorProbeTick）：小地圖選樓層
        // route B2 定案用的一次性側錄。=1 時 MinimapHud::Tick() 每秒 dump 一次
        // ——玩家世界 XYZ、目前 SelectFloor() 選到的樓層、actor+0x12D0 zone 節點
        // 名＋父鏈、m_eRoomZone/m_eCustomZone、actor scene-graph 父鏈名——供離線
        // 對照 minimap_<關>.txt 的 FLOOR 名，決定選樓層改讀哪個欄位。純側錄不改
        // 繪製。預設關閉；走完 M03/M05/M10 對照就該關掉。
        bool minimapFloorProbe;
        // GlyphOverrunProbe（見 GlyphAtlas.cpp GetGlyphInSlot/BlitGray8、
        // GlyphHook.cpp SynthesizeCJKGlyphRecord）：報紙 CJK 字圖合成路徑的
        // stack/atlas 溢位定位側錄。=1 且「目前在 _news.zip/_postmission.zip
        // 畫面」時，才印 GetGlyphOutlineW 量測 size、blackBox、cell 尺寸、
        // dst 落點 vs kAtlasSize、e 欄位等；進報紙前完全安靜。純側錄；
        // BlitGray8 另加落點越界防呆。預設關閉。
        bool glyphOverrunProbe;
    };
    DebugConfig LoadDebug(HMODULE hModule);

    // [Subtitle]區段：字幕self-render疊加位置設定共用這個struct，都是螢幕
    // 百分比座標。SubtitlePlaceFirst(首選欄位，SubtitleGate首選字幕+
    // SubtitleBrief.cpp開場簡報字幕共用，預設50,80)、SubtitlePlaceThird
    // (第三顯示點，8態狀態機用，預設60,90)；次選固定沿用native自己原本
    // 顯示字幕的位置、沒有對應key。
    struct SubtitlePlaceConfig
    {
        float xPercent; // 0~100，0=最左，100=最右
        float yPercent; // 0~100，0=最上，100=最下
    };
    // 讀取[Subtitle] SubtitlePlaceFirst="X,Y"（例"50,80"），缺席/格式錯誤
    // 都fallback成50,80並記log；數值會clamp在0~100範圍內。
    SubtitlePlaceConfig LoadSubtitlePlaceFirst(HMODULE hModule);
    // 讀取[Subtitle] SubtitlePlaceThird="X,Y"，同上規則，預設60,90。
    // consumer：SubtitleGate 8態狀態機的第三 self-render 欄位。
    SubtitlePlaceConfig LoadSubtitlePlaceThird(HMODULE hModule);

    // 單一bool同時決定要不要裝SubtitleBriefSpeech::Install()（語音訊號
    // 側錄）跟SubtitleBrief::Install()/EnsureBeginSceneHookInstalled()
    // （字幕文字內容+D3D9 self-render顯示），預設1(啟用)。SubtitleBrief
    // 逐句排程依賴SubtitleBriefSpeech提供的語音開始事件計數器，兩者一定
    // 配套使用所以合成一個開關。對應「簡報旁白」這一種播放函式
    // （sub_59FA90/sub_6CEA40），跟下面DialogueSubtitleEnabled/
    // OnelinersSubtitleEnabled各自對應不同播放函式，見SubtitleDialogue.h/
    // SubtitleOneliners.h說明。
    bool LoadBriefingSubtitleEnabled(HMODULE hModule);

    // 兩個獨立開關，各自對應SubtitleDialogue.cpp(sub_6AACA0)/
    // SubtitleOneliners.cpp(sub_6A4240＋sub_6BACC0)的function-entry hook。
    // native字幕watchdog側錄（SubtitleGate::InstallNativeSubtitleWatchdog，
    // 三欄位狀態機共用讀取native duration/startTime用）只要任一個啟用就會
    // 安裝，見SubtitleHook::Install()呼叫處。預設都是1(啟用)。
    //
    // sub_6BACC0（M04詐死彩蛋台詞）併入OnelinersSubtitleEnabled管轄——
    // 性質上都是單人事件反應喊話，只是sub_6BACC0是M04專屬寫死的特例，
    // 不獨立開一個第4個開關。
    bool LoadDialogueSubtitleEnabled(HMODULE hModule);
    bool LoadOnelinersSubtitleEnabled(HMODULE hModule);

    // 對應SubtitleWalkie.cpp(sub_6C0220)的function-entry hook——關卡內NPC
    // 對講機傳輸的字幕補完。共用SubtitleGate狀態機／native字幕watchdog側錄
    // （dialogue/oneliners/walkie任一啟用即安裝，見SubtitleHook::Install()）。
    // 預設1(啟用)。
    bool LoadWalkieSubtitleEnabled(HMODULE hModule);

    // 對應SubtitleTvRadio.cpp(sub_4C5E10，音效管理員vtable+0x118)的
    // function-entry hook——關卡內電視新聞／收音機廣播的字幕補完。共用
    // SubtitleGate狀態機／native字幕watchdog側錄（dialogue/oneliners/walkie/
    // tvradio任一啟用即安裝，見SubtitleHook::Install()）。預設1(啟用)。
    // 見字幕功能.md §3.4。
    bool LoadTvRadioSubtitleEnabled(HMODULE hModule);

    // [LocSwitch]區段：原文/譯文熱鍵切換功能設定，見
    // bloodmoney_utf8_support.md第7節第2點。
    struct LocSwitchConfig
    {
        // 決定原文/譯文切換是否生效的開關——關閉時Install()仍會裝hook但
        // switch永遠視為off（等同100% passthrough），不影響其他功能。
        bool enable;
        int  hotkeyVK;    // 切換熱鍵的Windows VK code，預設VK_F11(0x7A)
    };
    LocSwitchConfig LoadLocSwitch(HMODULE hModule);

    // 本struct只是[NewsPaper]區段5個報紙patch開關的讀取容器（沿用舊名，
    // 不代表它們在[HookToggle]底下），全部預設1(啟用)。用途：切F11看
    // 「原文」畫面時，逐一關閉某個報紙patch重跑，藉此判斷報紙排版/圖片
    // 位置異常是不是某個特定hook造成——像NewsPaperImageWrapFix這種只依賴
    // 「是不是報紙zip」而非CJK codepoint的patch，即使切到原文一樣會生效，
    // 適合拿來這樣二分排查。
    struct HookToggleConfig
    {
        // 以下5個報紙開關實際ini key在[NewsPaper]區段（跟SoftBreakCjkInterval
        // 放一起），不是讀[HookToggle]；ini key為NewsPaperImageWrapFix，
        // C++ class/檔名NewsPaperImageWrapHook不變。
        bool newsPaperImageWrapHook;   // [NewsPaper] NewsPaperImageWrapFix（報紙sub_5585E0(0)→sub_5585E0(v122)修法）
        bool newsPaperSpaceAdvanceFix; // [NewsPaper] NewsPaperSpaceAdvanceFix（報紙疊字X軸修法：case32空白字元改成當場重查GetGlyph，不沿用函式進入時快取的過期指標）
        bool newsPaperLineHeightFix;   // [NewsPaper] NewsPaperLineHeightFix（報紙標題疊字Y軸修法：sub_559910兩個v102行高running-max更新點改查我方探測過的正確lineHeight，不沿用可能被污染的var_998+0x11）
        bool newsPaperJustifyFillFix;  // [NewsPaper] NewsPaperJustifyFillFix（報紙<justify fill>改跳過sub_5586B0撐開呼叫，等同<justify left>，避免CJK少量空白把撐開量集中放大）
        bool newsPaperSoftBreakFix;    // [NewsPaper] NewsPaperSoftBreakFix（報紙譯文載入後人工插入空白斷點，見LocNewspaperFix.cpp InsertSoftBreaks()——不修native的a3+824/a3+820垃圾值本身，改讓CJK換行判斷密度趨近英文）
    };
    HookToggleConfig LoadHookToggle(HMODULE hModule);

    // [Subtitle] NpcHearRadius——見SubtitleGate.cpp「NPC/玩家距離過濾」
    // 段落。Dialogue（sub_6AACA0）跟Oneliners（sub_6A4240）都是NPC依關卡
    // 腳本全程背景執行，不管玩家在不在附近都會觸發，native語音本身用3D
    // 音效引擎的距離衰減讓玩家聽不到遠處的聲音，但我們的字幕hook直接接在
    // 觸發函式入口，沒有這層距離判斷，會把全地圖NPC的對話/喊話都顯示
    // 出來。這個設定值是NPC跟玩家角色（ZGEOM::GetRootPoint取世界座標）
    // 距離超過多少就不顯示字幕，單位跟引擎座標系統一致（實際換算成公尺/
    // 公分未經確認），需要使用者實機測試調整。0＝停用距離過濾（僅供
    // 除錯用）。
    float LoadNpcHearRadius(HMODULE hModule);

    // [Subtitle] TvRadioHearRadius——見SubtitleTvRadio.cpp。電視新聞／收音機
    // 廣播關卡載入即全區循環播報，字幕hook接在播放函式(sub_4C5E10)入口，會把
    // 整張地圖的廣播字幕都顯示。用sub_4C5E10第5參數帶的emitter世界座標跟玩家
    // 距離比對，超過此值就不顯示。跟NpcHearRadius分開一個key：廣播音量通常
    // 穿多個房間，門檻比NPC講話大。單位同引擎座標系（未換算），需實機調整。
    // 0＝停用過濾（全地圖顯示）。
    float LoadTvRadioHearRadius(HMODULE hModule);

    // [NewsPaper] SoftBreakCjkInterval——見LocNewspaperFix.cpp
    // InsertSoftBreaks()。報紙譯文載入完成後，
    // 每隔N個連續CJK字元（不計HTML-like tag內部、不重複插在既有空白/標點
    // 後面）插入一個ASCII空白(0x20)當斷行候選點。0＝關閉這個功能（完全不
    // 修改譯文字串）。預設2，使用者可依畫面視覺效果調整這個間隔。
    int LoadNewsSoftBreakInterval(HMODULE hModule);

    // [Hud]區段：小地圖/擅闖字/AI警告的必要功能開關與位置。已實作元素A
    // （擅闖/敵對區域自繪字，見TrespassHud.h）、元素B（懷疑/戰鬥AI刺激字，
    // 見WarningHud.h）與小地圖佔位方框（MinimapHud.h，引擎地圖本體之後再接）。
    // 座標皆為螢幕百分比（0,0=左上）。
    struct HudConfig
    {
        bool  trespassingEnabled;
        float trespassPosX;   // 0~100
        float trespassPosY;   // 0~100
        bool  warningEnabled; // 元素B（SUSPICIOUS/ALERTED）總開關
        float warningPosX;    // 0~100，畫面上方置中，預設50
        float warningPosY;    // 0~100，預設5
        bool  minimapEnabled;
        float minimapPosX;    // 0~100，小地圖左上角
        float minimapPosY;    // 0~100
        int   minimapSizeMul; // 邊長倍數：實際像素邊長 = minimapSizeMul * 10；有效範圍0~100，無效退回15
        float minimapZoom;    // 小地圖顯示範圍＝方框內橫向可見的world單位數。>0＝以47為
                              // 中心的局部視窗（數值越小越放大），牆線裁在方框內、跟著47
                              // 捲動；0＝整層等比縮放塞滿方框（看全貌、不跟隨）。預設4000。
                              // 負值視為無效退回0。
    };
    // 讀取[Hud] TrespassingEnabled（預設1）、TrespassPos="X,Y"（預設16,85）、
    // WarningEnabled（預設1）、WarningPos="X,Y"（預設50,5）、
    // MinimapEnabled（預設1）、MinimapPos="X,Y"（預設5.0,60.0）：以上格式錯誤
    // fallback並記log、數值clamp 0~100。MinimapSize（預設15）：只接受純十進位
    // 整數字串、範圍0~100，非數字/含小數點正負號/位數>3/超過100一律無效並記
    // log、退回15（不clamp）。MinimapZoom（預設4000）：atof解析，負值退回0（＝整層）。
    HudConfig LoadHud(HMODULE hModule);
}
