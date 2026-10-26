// 全部位址集中在此，分 GOG／Steam 兩欄；程式碼一律經 GameAddr::Addr(AddressTable::名稱) 取用。
// Steam欄來源標記：P主特徵碼 B前置 S跨越 C呼叫端 V表槽位 D資料引用 F由MgrObj+欄位偏移推得 T toupper IAT槽位。
// 只放位址；vtable槽位與結構欄位偏移兩版相同，不放這裡。
#pragma once
#include <Windows.h>

namespace AddressTable
{
    // 主執行檔PE檔頭值，供 GameAddr::Init() 判斷版本。
    struct BuildInfo
    {
        DWORD timeDateStamp;
        DWORD sizeOfImage;
    };
    static const BuildInfo kGogBuild = { 0x4492B845UL, 0x005C0000UL };
    static const BuildInfo kSteamBuild = { 0x447EF98AUL, 0x005C8000UL };

    struct Entry
    {
        DWORD gog;
        DWORD steam;
    };

    // 位址識別名稱；順序必須與下面 kEntries 一致（兩者由同一份清單產生）。
    enum Id
    {
        NextChild,
        EncodeCodepointUtf8,
        ZBitmap_Dtor,
        ZBitmap_SetName,
        ZBitmap_Ctor,
        ZipPathTrace_Entry,
        ZipPathTrace_Resume,
        NativeFree,
        SceneDataLoad_Entry,
        SceneDataLoad_Resume,
        ToUpper_SwitchSite,
        ToUpper_SwitchResume,
        ToUpper_SwitchBypass,
        GetText_Entry,
        GetText_Resume,
        GetTextureBounds_BreakpointSite,
        ReleaseTextures_Entry,
        ReleaseTextures_Resume,
        TexTypeDispatch_Entry,
        TexTypeDispatch_Resume,
        AllocateSlot_StaleClearSite,
        AllocateSlot_StaleClearResume,
        ShaderEffectLoad_Entry,
        ShaderEffectLoad_Resume,
        TvRadioSubtitle_Entry,
        TvRadioSubtitle_Resume,
        ResolveActor,
        GetRootPoint,
        CreateChannel_Entry,
        CreateChannel_Resume,
        HasChild,
        CheckStillWaiting,
        ActionWidgetRequery_CallerReturn,
        Glyph_MenuSite,
        Glyph_MenuResume,
        Glyph_MeasureSite,
        Glyph_MeasureResume,
        Glyph_LineBreakSite,
        Glyph_LineBreakResume,
        HtmlLineAvailableWidth,
        HtmlJustifyFill,
        Glyph_SpaceASite,
        Glyph_SpaceAResume,
        Glyph_SpaceBSite,
        Glyph_SpaceBResume,
        NewsImageWrap_CaseBrSite,
        NewsImageWrap_CaseBrResume,
        NewsSpaceAdvance_Site,
        NewsSpaceAdvance_Resume,
        NewsLineHeight_SiteA,
        NewsLineHeight_ResumeA,
        NewsLineHeight_SiteB,
        NewsLineHeight_ResumeB,
        Glyph_MainSite,
        Glyph_MainResume,
        NewsImageWrap_MainCheckSite,
        NewsImageWrap_MainCheckResume,
        NewsJustifyFill_Site,
        NewsJustifyFill_Resume,
        NewsImageWrap_WrapBrSite,
        NewsImageWrap_WrapBrResume,
        MeasureLabel,
        BriefingSpeak_Entry,
        BriefingSpeak_Resume,
        BriefingSpeak_CallerReturn,
        LatinToUtf8Copy_Entry,
        GetCurrentZoneKind,
        ShowSubtitle_Entry,
        ShowSubtitle_Resume,
        SplashCaption_Site,
        SplashCaption_Resume,
        TutorialPageProbe_CallerReturn,
        SaveFolderPath_CallerReturn,
        SaveFileName_CallerReturn,
        SaveFileNameAfterMission_CallerReturn,
        ShopPriceLabel_Site,
        ShopPriceLabel_Resume,
        InventoryLabel_SiteA,
        InventoryLabel_ResumeA,
        InventoryLabel_SiteB,
        InventoryLabel_ResumeB,
        TrainingPopupSound_CallerReturn,
        Oneliners_Entry,
        Oneliners_Resume,
        NpcDialogue_Entry,
        NpcDialogue_Resume,
        FeignDeathEgg_Site,
        FeignDeathEgg_Resume,
        Walkie_Entry,
        Walkie_Resume,
        NarratorFirstLine_CallerReturn,
        NarratorNextLine_CallerReturn,
        GetBriefingLine,
        Ftol2,
        ZipPathTrace_SehHandler,
        ToUpperIatSlot,
        TextureManagerVtable,
        GameDataBase,
        LevelCtrlSingleton,
        HWndObjectPtr,
        SubtitleEnableFlag,
        TextureMgrCriticalSection,
        TextureMgrObject,
        TextureMgrBlockCount,
        TextureMgrBase,
        TextureMgrInitFlag,
        D3D9WrapperPtr,
        RttiTag,
        RttiMask,
        Count
    };

    static const Entry kEntries[Count] =
    {
        { 0x00431E20UL, 0x00431E90UL }, // NextChild [BCPS]
        { 0x00438FE0UL, 0x00439030UL }, // EncodeCodepointUtf8 [CPS]
        { 0x0043DAC0UL, 0x0043DA90UL }, // ZBitmap_Dtor [CP]
        { 0x0043DB30UL, 0x0043DB00UL }, // ZBitmap_SetName [CPS]
        { 0x0043F9F0UL, 0x0043F9A0UL }, // ZBitmap_Ctor [CPS]
        { 0x00441920UL, 0x004418E0UL }, // ZipPathTrace_Entry [PS]
        { 0x00441927UL, 0x004418E7UL }, // ZipPathTrace_Resume [BPS]
        { 0x004466D0UL, 0x004465E0UL }, // NativeFree [CPS]
        { 0x0045FCF0UL, 0x0045FBC0UL }, // SceneDataLoad_Entry [PS]
        { 0x0045FCF8UL, 0x0045FBC8UL }, // SceneDataLoad_Resume [BPS]
        { 0x00463D30UL, 0x00463BE0UL }, // ToUpper_SwitchSite [BPS]
        { 0x00463D39UL, 0x00463BE9UL }, // ToUpper_SwitchResume [BPS]
        { 0x00463D54UL, 0x00463C04UL }, // ToUpper_SwitchBypass [BPS]
        { 0x00465370UL, 0x00465210UL }, // GetText_Entry [PS]
        { 0x0046537AUL, 0x0046521AUL }, // GetText_Resume [BPS]
        { 0x0048F175UL, 0x0048EF75UL }, // GetTextureBounds_BreakpointSite [BPS]
        { 0x0048F190UL, 0x0048EF90UL }, // ReleaseTextures_Entry [PS]
        { 0x0048F195UL, 0x0048EF95UL }, // ReleaseTextures_Resume [BPS]
        { 0x0048F330UL, 0x0048F130UL }, // TexTypeDispatch_Entry [PS]
        { 0x0048F335UL, 0x0048F135UL }, // TexTypeDispatch_Resume [BPS]
        { 0x0048F57BUL, 0x0048F37BUL }, // AllocateSlot_StaleClearSite [BPS]
        { 0x0048F580UL, 0x0048F380UL }, // AllocateSlot_StaleClearResume [BPS]
        { 0x00496F00UL, 0x00496CC0UL }, // ShaderEffectLoad_Entry [CPS]
        { 0x00496F07UL, 0x00496CC7UL }, // ShaderEffectLoad_Resume [BPS]
        { 0x004C5E10UL, 0x004C52D0UL }, // TvRadioSubtitle_Entry [PS]
        { 0x004C5E16UL, 0x004C52D6UL }, // TvRadioSubtitle_Resume [BPS]
        { 0x004E5BE0UL, 0x004E5110UL }, // ResolveActor [CPS]
        { 0x004E68E0UL, 0x004E5DC0UL }, // GetRootPoint [CPS]
        { 0x004EA670UL, 0x004E9B90UL }, // CreateChannel_Entry [CPS]
        { 0x004EA675UL, 0x004E9B95UL }, // CreateChannel_Resume [BPS]
        { 0x004EB700UL, 0x004EAC20UL }, // HasChild [CPS]
        { 0x0051D680UL, 0x0051CBE0UL }, // CheckStillWaiting [CP]
        { 0x0052AF61UL, 0x0052A471UL }, // ActionWidgetRequery_CallerReturn [BPS]
        { 0x00550BE4UL, 0x00550074UL }, // Glyph_MenuSite [BPS]
        { 0x00550BEAUL, 0x0055007AUL }, // Glyph_MenuResume [BPS]
        { 0x005555C9UL, 0x00554AA9UL }, // Glyph_MeasureSite [PS]
        { 0x005555CFUL, 0x00554AAFUL }, // Glyph_MeasureResume [BPS]
        { 0x00555A68UL, 0x00554F48UL }, // Glyph_LineBreakSite [BPS]
        { 0x00555A6EUL, 0x00554F4EUL }, // Glyph_LineBreakResume [BPS]
        { 0x005585E0UL, 0x00557AE0UL }, // HtmlLineAvailableWidth [CPS]
        { 0x005586B0UL, 0x00557BB0UL }, // HtmlJustifyFill [CPS]
        { 0x00559A21UL, 0x00558F21UL }, // Glyph_SpaceASite [BPS]
        { 0x00559A27UL, 0x00558F27UL }, // Glyph_SpaceAResume [BPS]
        { 0x00559AFCUL, 0x00558FFCUL }, // Glyph_SpaceBSite [BPS]
        { 0x00559B02UL, 0x00559002UL }, // Glyph_SpaceBResume [BPS]
        { 0x00559C9EUL, 0x0055919EUL }, // NewsImageWrap_CaseBrSite [BPS]
        { 0x00559CA3UL, 0x005591A3UL }, // NewsImageWrap_CaseBrResume [BPS]
        { 0x00559DB8UL, 0x005592B8UL }, // NewsSpaceAdvance_Site [BPS]
        { 0x00559DC0UL, 0x005592C0UL }, // NewsSpaceAdvance_Resume [BPS]
        { 0x00559F10UL, 0x00559410UL }, // NewsLineHeight_SiteA [BPS]
        { 0x00559F18UL, 0x00559418UL }, // NewsLineHeight_ResumeA [BPS]
        { 0x00559FB2UL, 0x005594B2UL }, // NewsLineHeight_SiteB [PS]
        { 0x00559FBAUL, 0x005594BAUL }, // NewsLineHeight_ResumeB [BPS]
        { 0x0055A0A0UL, 0x005595A0UL }, // Glyph_MainSite [BPS]
        { 0x0055A0A6UL, 0x005595A6UL }, // Glyph_MainResume [BPS]
        { 0x0055A0E4UL, 0x005595E4UL }, // NewsImageWrap_MainCheckSite [BPS]
        { 0x0055A0E9UL, 0x005595E9UL }, // NewsImageWrap_MainCheckResume [BPS]
        { 0x0055A22CUL, 0x0055972CUL }, // NewsJustifyFill_Site [BPS]
        { 0x0055A231UL, 0x00559731UL }, // NewsJustifyFill_Resume [BPS]
        { 0x0055A334UL, 0x00559834UL }, // NewsImageWrap_WrapBrSite [BPS]
        { 0x0055A339UL, 0x00559839UL }, // NewsImageWrap_WrapBrResume [BPS]
        { 0x0055E550UL, 0x0055DB20UL }, // MeasureLabel [CPS]
        { 0x0059FA90UL, 0x0059F250UL }, // BriefingSpeak_Entry [PS]
        { 0x0059FA96UL, 0x0059F256UL }, // BriefingSpeak_Resume [BPS]
        { 0x0059FAE0UL, 0x0059F2A0UL }, // BriefingSpeak_CallerReturn [BPS]
        { 0x005A4850UL, 0x005A4030UL }, // LatinToUtf8Copy_Entry [CPS]
        { 0x005F29B0UL, 0x005F2220UL }, // GetCurrentZoneKind [CPS]
        { 0x00661FD0UL, 0x006616F0UL }, // ShowSubtitle_Entry [CPS]
        { 0x00661FD7UL, 0x006616F7UL }, // ShowSubtitle_Resume [BPS]
        { 0x0067069CUL, 0x0066FDBCUL }, // SplashCaption_Site [BPS]
        { 0x006706A2UL, 0x0066FDC2UL }, // SplashCaption_Resume [BPS]
        { 0x006708E3UL, 0x00670003UL }, // TutorialPageProbe_CallerReturn [PS]
        { 0x00675464UL, 0x00674B54UL }, // SaveFolderPath_CallerReturn [PS]
        { 0x006755B4UL, 0x00674CA4UL }, // SaveFileName_CallerReturn [PS]
        { 0x00675F9DUL, 0x0067568DUL }, // SaveFileNameAfterMission_CallerReturn [PS]
        { 0x0067E175UL, 0x0067D865UL }, // ShopPriceLabel_Site [BPS]
        { 0x0067E17AUL, 0x0067D86AUL }, // ShopPriceLabel_Resume [BPS]
        { 0x006820F1UL, 0x006817D1UL }, // InventoryLabel_SiteA [BPS]
        { 0x006820F6UL, 0x006817D6UL }, // InventoryLabel_ResumeA [BPS]
        { 0x00682725UL, 0x00681E05UL }, // InventoryLabel_SiteB [BPS]
        { 0x0068272AUL, 0x00681E0AUL }, // InventoryLabel_ResumeB [BPS]
        { 0x006849EAUL, 0x006840CAUL }, // TrainingPopupSound_CallerReturn [PS]
        { 0x006A4240UL, 0x006A3900UL }, // Oneliners_Entry [PS]
        { 0x006A424AUL, 0x006A390AUL }, // Oneliners_Resume [BPS]
        { 0x006AACA0UL, 0x006AA360UL }, // NpcDialogue_Entry [PS]
        { 0x006AACA6UL, 0x006AA366UL }, // NpcDialogue_Resume [BPS]
        { 0x006BACC0UL, 0x006BA4D0UL }, // FeignDeathEgg_Site [CPS]
        { 0x006BACC5UL, 0x006BA4D5UL }, // FeignDeathEgg_Resume [BPS]
        { 0x006C0220UL, 0x006BF7C0UL }, // Walkie_Entry [CPS]
        { 0x006C0226UL, 0x006BF7C6UL }, // Walkie_Resume [BPS]
        { 0x006CEA80UL, 0x006CE150UL }, // NarratorFirstLine_CallerReturn [BPS]
        { 0x006CEB27UL, 0x006CE1F7UL }, // NarratorNextLine_CallerReturn [BPS]
        { 0x006CF850UL, 0x006CEF20UL }, // GetBriefingLine [CPS]
        { 0x007286ACUL, 0x00727D4CUL }, // Ftol2 [CPS]
        { 0x0072AABAUL, 0x0072A15AUL }, // ZipPathTrace_SehHandler [BPS]
        { 0x00752230UL, 0x00751230UL }, // ToUpperIatSlot [T]
        { 0x00763E3CUL, 0x00762E3CUL }, // TextureManagerVtable [D]
        { 0x00820820UL, 0x0081F820UL }, // GameDataBase [D]
        { 0x0082083CUL, 0x0081F83CUL }, // LevelCtrlSingleton [D]
        { 0x008ACA30UL, 0x008ABA30UL }, // HWndObjectPtr [D]
        { 0x008ACA9CUL, 0x008ABA9CUL }, // SubtitleEnableFlag [D]
        { 0x008B4FCCUL, 0x008B3FCCUL }, // TextureMgrCriticalSection [D]
        { 0x008B4FE8UL, 0x008B3FE8UL }, // TextureMgrObject [D]
        { 0x008B5B70UL, 0x008B4B70UL }, // TextureMgrBlockCount [F]
        { 0x008B5B74UL, 0x008B4B74UL }, // TextureMgrBase [F]
        { 0x008B5B7CUL, 0x008B4B7CUL }, // TextureMgrInitFlag [D]
        { 0x0090AF0CUL, 0x00909F0CUL }, // D3D9WrapperPtr [D]
        { 0x009A2B98UL, 0x009A1598UL }, // RttiTag [D]
        { 0x009A2B9CUL, 0x009A159CUL }, // RttiMask [D]
    };

    static_assert(sizeof(kEntries) / sizeof(kEntries[0]) == Count, "kEntries與Id數量不符");
}
