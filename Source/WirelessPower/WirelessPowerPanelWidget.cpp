#include "WirelessPowerPanelWidget.h"
#include "WirelessPowerLocalization.h"
#include "WirelessPowerBuildable.h"
#include "WirelessPowerId.h"
#include "WirelessPowerLoc.h"
#include "WirelessPowerRCO.h"
#include "FGPlayerController.h"
#include "FGBuildableSubsystem.h"
#include "Buildables/FGBuildable.h"
#include "Components/Border.h"
#include "Components/BorderSlot.h"
#include "Components/Button.h"
#include "Components/ComboBoxString.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Blueprint/WidgetTree.h"
#include "Styling/CoreStyle.h"
#include "Containers/Ticker.h"

// Theme (dark steel panel + orange accents, matching the original mod's look)
static const FLinearColor C_BG    = FLinearColor(0.05f, 0.06f, 0.07f, 0.96f);
static const FLinearColor C_ORANGE= FLinearColor(0.95f, 0.52f, 0.10f, 1.f);
static const FLinearColor C_LABEL = FLinearColor(0.75f, 0.78f, 0.82f, 1.f);
static const FLinearColor C_VALUE = FLinearColor(1.f, 1.f, 1.f, 1.f);
static const FLinearColor C_GREEN = FLinearColor(0.30f, 0.90f, 0.35f, 1.f);
static const FLinearColor C_RED   = FLinearColor(0.95f, 0.30f, 0.25f, 1.f);
static const FLinearColor C_AMBER = FLinearColor(1.f, 0.62f, 0.10f, 1.f);
static const FLinearColor C_DIV   = FLinearColor(0.95f, 0.52f, 0.10f, 0.55f);

UWirelessPowerPanelWidget::UWirelessPowerPanelWidget(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    // Vanilla interact panels block movement + look so the player can operate the UI.
    mUseKeyboard = true;
    mUseMouse = true;
    mMouseLockMode = EMouseLockMode::DoNotLock;
    mSupportsStacking = true;
    mDesiredHorizontalAlignment = HAlign_Center;
    mDesiredVerticalAlignment = VAlign_Center;
}

// ----------------------------------------------------------------------------
// UI construction (pure C++ UMG)
// ----------------------------------------------------------------------------

static UTextBlock* MakePanelText(UWidgetTree* Tree, const FText& InText, int32 FontSize, bool bBold, FLinearColor Color)
{
    UTextBlock* T = Tree->ConstructWidget<UTextBlock>();
    T->SetText(InText);
    FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(bBold ? TEXT("Bold") : TEXT("Regular"), FontSize);
    T->SetFont(Font);
    T->SetColorAndOpacity(FSlateColor(Color));
    T->SetJustification(ETextJustify::Center);
    return T;
}

static UButton* MakePanelButton(UWidgetTree* Tree, const FText& InText, FLinearColor BgColor, UTextBlock** OutLabel)
{
    UButton* B = Tree->ConstructWidget<UButton>();
    B->SetBackgroundColor(BgColor);
    UTextBlock* T = MakePanelText(Tree, InText, 16, true, FLinearColor::White);
    B->AddChild(T);
    if (OutLabel) { *OutLabel = T; }
    return B;
}

static UBorder* MakeDivider(UWidgetTree* Tree)
{
    UBorder* D = Tree->ConstructWidget<UBorder>();
    // Explicit engine white brush: a default-constructed FSlateBrush has NO resource, and a
    // zero-resource box painted through MakeBox -> BuildResourceHandle was the crash signature
    // (EXCEPTION_ACCESS_VIOLATION in FName::ToString during OnPaint). Always paint a brush
    // whose texture actually exists; SetBrushColor tints it.
    D->SetBrush(*FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")));
    D->SetBrushColor(C_DIV);
    D->SetPadding(FMargin(0.f, 4.f));
    return D;
}

namespace
{
    /** v1.0.52 (阶段 A) —— 编号回归 Feyko 原版的【字符串】语义。
     *
     * 历史包袱：v1.0.46 起我们把编号塞进 int32，还让两个族用不同进制显示
     * （继电器=十进制 / 转换器=4 位二进制 0001/0010），结果引出了
     * "转换器无法设置编号"（输入过滤器只留 2 位数字，把 4 位二进制的值截断，
     * 解析必然失败 → 静默回弹）。一手取证显示 Feyko 原版根本就是 String：
     * 继电器蓝图里有 EqualEqual_StriStri 节点，供电器面板明文字面量是 "Set ID here"。
     * 于是 v1.0.52 砍掉整套 int32/二进制，编号就是玩家输入的字符串，汉字天然合法。
     *
     * 显示：原样返回规范化结果，空编号显示为 "—"（避免出现"看不见的编号"）。
     */
    FString WirelessIdToDisplay(const FString& Channel)
    {
        return FWirelessId::ToDisplay(Channel);
    }
}

/** Per-building panel title: each of the four buildings gets its own console name
 *  (plus Mk tier, Laser Tower has no tiers). v1.0.46 naming chain (user):
 *  发电厂 -> 电源转换器 -> 电源继电器 -> 无线电杆(供电器形态). */
static FText MakePanelTitle(const AWirelessPowerBuildable* Node)
{
    const EWirelessBuildingType Type = Node ? Node->GetBuildingType() : EWirelessBuildingType::Relay;
    FText Name;
    switch (Type)
    {
        case EWirelessBuildingType::Converter:  Name = WP_Text(TEXT("Power Converter"), TEXT("电力转换器"));  break;
        case EWirelessBuildingType::AreaPole:   Name = WP_Text(TEXT("Power Feeder"), TEXT("供电杆"));     break;
        case EWirelessBuildingType::LaserTower: Name = WP_Text(TEXT("Power Transmission Tower"), TEXT("激光塔")); break;
        default:                                Name = WP_Text(TEXT("Power Relay"), TEXT("电力中继器"));      break;
    }
    // The converter is a single, un-tiered building now; the laser tower never had tiers.
    const bool bTiered = Node
        && Type != EWirelessBuildingType::LaserTower
        && Type != EWirelessBuildingType::Converter;
    if (bTiered)
    {
        Name = FText::Format(WP_Text(TEXT("{0} Mk{1}"), TEXT("{0} Mk{1}")),
                             Name, FText::AsNumber(Node->GetTier()));
    }
    return FText::Format(WP_Text(TEXT("{0} · Console"), TEXT("{0} · 控制台")), Name);
}

TSharedRef<SWidget> UWirelessPowerPanelWidget::RebuildWidget()
{
    RootBorder = WidgetTree->ConstructWidget<UBorder>();
    RootBorder->SetBrush(*FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")));
    RootBorder->SetBrushColor(C_BG);
    RootBorder->SetPadding(FMargin(26.f));
    RootBorder->SetHorizontalAlignment(HAlign_Fill);

    UVerticalBox* VB = WidgetTree->ConstructWidget<UVerticalBox>();
    RootBorder->AddChild(VB);

    auto AddRow = [&](UWidget* Child, float TopPad)
    {
        if (UVerticalBoxSlot* Slot = VB->AddChildToVerticalBox(Child))
        {
            Slot->SetPadding(FMargin(0.f, TopPad, 0.f, 0.f));
            Slot->SetHorizontalAlignment(HAlign_Fill);
            Slot->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
        }
    };

    // Title bar
    UBorder* TitleBar = WidgetTree->ConstructWidget<UBorder>();
    TitleBar->SetBrush(*FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")));
    TitleBar->SetBrushColor(FLinearColor(0.95f, 0.52f, 0.10f, 1.f));
    TitleBar->SetPadding(FMargin(10.f, 6.f));
    TitleText = MakePanelText(WidgetTree, WP_Text(TEXT("Wireless Power Console"), TEXT("无线电力控制台")), 22, true, FLinearColor(0.04f, 0.04f, 0.05f, 1.f));
    TitleBar->AddChild(TitleText);
    AddRow(TitleBar, 0.f);
    AddRow(MakeDivider(WidgetTree), 6.f);

    // ---- 开关连接 (Laser Tower only) ----------------------------------------
    // Feyko's laser tower panel puts SWITCH CONNECTION above the ID boxes: pick an ID in the
    // dropdown (bottom-left, under 断电) and use this button to connect/disconnect that pair.
    // NOTE: MakePanelButton takes a raw UTextBlock** out-param, so the label must go through a
    // plain pointer first (a TObjectPtr member cannot be passed as TObjectPtr*).
    UTextBlock* LaserLinkLabelRaw = nullptr;
    LaserLinkButton = MakePanelButton(WidgetTree, WP_Text(TEXT("Toggle link"), TEXT("切换连接")),
                                      FLinearColor(0.95f, 0.25f, 0.20f, 1.f), &LaserLinkLabelRaw);
    LaserLinkLabel = LaserLinkLabelRaw;
    AddRow(LaserLinkButton, 8.f);

    // ---- 编号 section (当前编号 / 新编号 + SET) ----
    {
        ChannelSectionText = MakePanelText(WidgetTree, WP_Text(TEXT("ID"), TEXT("ID")), 14, true, C_ORANGE);
        AddRow(ChannelSectionText, 8.f);

        UHorizontalBox* HB = WidgetTree->ConstructWidget<UHorizontalBox>();
        CurrentChannelLabel = MakePanelText(WidgetTree, WP_Text(TEXT("Current ID"), TEXT("当前 ID")), 15, false, C_LABEL);
        CurrentChannelText = MakePanelText(WidgetTree, FText::FromString(TEXT("—")), 20, true, C_VALUE);
        if (UHorizontalBoxSlot* S1 = HB->AddChildToHorizontalBox(CurrentChannelLabel)) { S1->SetPadding(FMargin(0.f, 0.f, 12.f, 0.f)); S1->SetVerticalAlignment(VAlign_Center); }
        if (UHorizontalBoxSlot* S2 = HB->AddChildToHorizontalBox(CurrentChannelText)) { S2->SetVerticalAlignment(VAlign_Center); }
        AddRow(HB, 6.f);
    }

    {
        UHorizontalBox* HB = WidgetTree->ConstructWidget<UHorizontalBox>();
        NewChannelLabel = MakePanelText(WidgetTree, WP_Text(TEXT("New ID"), TEXT("新 ID")), 15, false, C_LABEL);
        ChannelInput = WidgetTree->ConstructWidget<UEditableTextBox>();
        ChannelInput->SetJustification(ETextJustify::Center);
        // v1.0.45: 电线杆/中继器 switch this free-text box for a dropdown of nearby node IDs;
        // both widgets are created side by side and ApplyLayoutForBuildingType() shows exactly
        // one of them per family (laser tower / converter keep the text box).
        ChannelCombo = WidgetTree->ConstructWidget<UComboBoxString>();
        ChannelCombo->SetMaxListHeight(220.f);
        ChannelCombo->SetVisibility(ESlateVisibility::Collapsed);
        if (UHorizontalBoxSlot* S1 = HB->AddChildToHorizontalBox(NewChannelLabel)) { S1->SetPadding(FMargin(0.f, 0.f, 12.f, 0.f)); S1->SetVerticalAlignment(VAlign_Center); }
        if (UHorizontalBoxSlot* S2 = HB->AddChildToHorizontalBox(ChannelInput)) { S2->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); S2->SetVerticalAlignment(VAlign_Fill); }
        if (UHorizontalBoxSlot* S3 = HB->AddChildToHorizontalBox(ChannelCombo)) { S3->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); S3->SetVerticalAlignment(VAlign_Fill); }
        AddRow(HB, 6.f);

        SetChannelButton = MakePanelButton(WidgetTree, WP_Text(TEXT("Set"), TEXT("设定")), C_ORANGE, nullptr);
        AddRow(SetChannelButton, 8.f);
    }

    AddRow(MakeDivider(WidgetTree), 8.f);

    // ---- 覆盖半径 section: EXACT NUMBER entry (the old slider is gone) ----
    // Wrapped in its own box so the laser tower (which has no range row in Feyko's panel) can
    // collapse the whole section at once.
    RangeBox = WidgetTree->ConstructWidget<UVerticalBox>();
    {
        auto AddRangeRow = [&](UWidget* Child, float TopPad)
        {
            if (UVerticalBoxSlot* Slot = RangeBox->AddChildToVerticalBox(Child))
            {
                Slot->SetPadding(FMargin(0.f, TopPad, 0.f, 0.f));
                Slot->SetHorizontalAlignment(HAlign_Fill);
                Slot->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
            }
        };

        AddRangeRow(MakePanelText(WidgetTree, WP_Text(TEXT("Coverage Radius (m)"), TEXT("覆盖半径（米）")), 14, true, C_ORANGE), 8.f);
        RangeValueText = MakePanelText(WidgetTree, WP_Text(TEXT("Current range: 0 m"), TEXT("当前射程：0 米")), 15, false, C_LABEL);
        AddRangeRow(RangeValueText, 6.f);

        RangeInput = WidgetTree->ConstructWidget<UEditableTextBox>();
        RangeInput->SetJustification(ETextJustify::Center);
        RangeInput->SetHintText(WP_Text(TEXT("Enter an exact value (m)"), TEXT("输入精确数值（米）")));
        AddRangeRow(RangeInput, 6.f);

        SetRangeButton = MakePanelButton(WidgetTree, WP_Text(TEXT("Set range"), TEXT("设定射程")), C_ORANGE, nullptr);
        AddRangeRow(SetRangeButton, 8.f);
    }
    AddRow(RangeBox, 0.f);

    AddRow(MakeDivider(WidgetTree), 8.f);

    // ---- 电源 section: one row, two columns ----
    //   left  : 断电  + [可连激光塔编号]   (dropdown: laser tower only)
    //   right : 启动  + [激光的可见性]     (switch:   laser tower only)
    // v1.0.45: the whole row is LASER-TOWER-ONLY (user request: "马赛克部分，除输电塔外去掉") --
    // the other three families show neither 断电 nor 启动; ApplyLayoutForBuildingType() collapses
    // PowerRowBox for them.
    {
        UHorizontalBox* HB = WidgetTree->ConstructWidget<UHorizontalBox>();
        PowerRowBox = HB;

        UVerticalBox* ColLeft = WidgetTree->ConstructWidget<UVerticalBox>();
        UVerticalBox* ColRight = WidgetTree->ConstructWidget<UVerticalBox>();

        auto ColAdd = [&](UVerticalBox* Col, UWidget* Child, float TopPad)
        {
            if (UVerticalBoxSlot* Slot = Col->AddChildToVerticalBox(Child))
            {
                Slot->SetPadding(FMargin(0.f, TopPad, 0.f, 0.f));
                Slot->SetHorizontalAlignment(HAlign_Fill);
                Slot->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
            }
        };

        UTextBlock* UnusedLabel = nullptr;
        PowerDownButton = MakePanelButton(WidgetTree, WP_Text(TEXT("Power off"), TEXT("断电")), FLinearColor(0.80f, 0.22f, 0.18f, 1.f), &UnusedLabel);
        PowerUpButton = MakePanelButton(WidgetTree, WP_Text(TEXT("Power on"), TEXT("通电")), FLinearColor(0.20f, 0.65f, 0.30f, 1.f), &UnusedLabel);

        // 可连激光塔编号: lists the IDs of laser towers that already exist in the world.
        // NOTE: UComboBoxString's foreground colour can only be set at construction in this engine
        // build (InitForegroundColor is protected), so we keep the stock styling.
        LaserComboLabel = MakePanelText(WidgetTree, WP_Text(TEXT("Linkable tower IDs"), TEXT("可连接的传送塔 ID")), 13, true, C_LABEL);
        LaserCombo = WidgetTree->ConstructWidget<UComboBoxString>();
        LaserCombo->SetMaxListHeight(220.f);

        // 激光的可见性: show/hide the beams between this tower and its linked towers.
        UTextBlock* LaserVisLabelRaw = nullptr;
        LaserVisibleButton = MakePanelButton(WidgetTree, WP_Text(TEXT("Beams visible: on"), TEXT("光束可见：开")),
                                             FLinearColor(0.55f, 0.12f, 0.10f, 1.f), &LaserVisLabelRaw);
        LaserVisibleLabel = LaserVisLabelRaw;

        ColAdd(ColLeft, PowerDownButton, 0.f);
        ColAdd(ColLeft, LaserComboLabel, 8.f);
        ColAdd(ColLeft, LaserCombo, 4.f);
        ColAdd(ColRight, PowerUpButton, 0.f);
        ColAdd(ColRight, LaserVisibleButton, 8.f);

        if (UHorizontalBoxSlot* S1 = HB->AddChildToHorizontalBox(ColLeft)) { S1->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); S1->SetPadding(FMargin(0.f, 0.f, 6.f, 0.f)); S1->SetVerticalAlignment(VAlign_Top); }
        if (UHorizontalBoxSlot* S2 = HB->AddChildToHorizontalBox(ColRight)) { S2->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); S2->SetPadding(FMargin(6.f, 0.f, 0.f, 0.f)); S2->SetVerticalAlignment(VAlign_Top); }
        AddRow(HB, 6.f);
    }

    // Manual coverage-cube toggle switch. Player-controlled; persists after the panel closes so the
    // range cage can stay on while placing/building. (Extra row, not in Feyko's original panel.)
    {
        UTextBlock* RangeToggleLabelRaw = nullptr;
        RangeToggleButton = MakePanelButton(WidgetTree, WP_Text(TEXT("Range display: off"), TEXT("射程显示：关")),
                                            FLinearColor(0.22f, 0.30f, 0.38f, 1.f), &RangeToggleLabelRaw);
        RangeToggleLabel = RangeToggleLabelRaw;
        AddRow(RangeToggleButton, 8.f);
    }

    // Status line (title text is finalized per-building in RefreshValues once the
    // interact target is known)
    StatusText = MakePanelText(WidgetTree, FText::FromString(TEXT("-")), 15, true, C_GREEN);
    AddRow(StatusText, 10.f);

    // Usage hint: the node must be WIRED into a powered grid; wireless only bridges
    // same-channel peers that are inside range.
    UTextBlock* HintText = MakePanelText(WidgetTree,
        WP_Text(TEXT("Note: this building must be wired into a powered grid; nodes sharing an ID and standing within range link up automatically. Transmission towers can also be paired manually by ID with \"Toggle link\" (needs a clear line of sight)."), TEXT("注意：该建筑必须接入已通电的电网；共享同一 ID 且位于射程内的节点会自动连接。传送塔也可以通过「切换连接」按 ID 手动配对（需要通视）。")),
        12, false, C_LABEL);
    HintText->SetAutoWrapText(true);
    AddRow(HintText, 6.f);

    WidgetTree->RootWidget = RootBorder;
    return Super::RebuildWidget();
}

// ----------------------------------------------------------------------------
// Runtime
// ----------------------------------------------------------------------------

void UWirelessPowerPanelWidget::NativeConstruct()
{
    Super::NativeConstruct();

    TargetNode = Cast<AWirelessPowerBuildable>(mInteractObject);

    if (ChannelInput)
    {
        ChannelInput->OnTextChanged.AddDynamic(this, &UWirelessPowerPanelWidget::OnChannelInputChanged);
    }
    if (ChannelCombo)
    {
        ChannelCombo->OnSelectionChanged.AddDynamic(this, &UWirelessPowerPanelWidget::OnChannelComboChanged);
    }
    if (SetChannelButton)
    {
        SetChannelButton->OnClicked.AddDynamic(this, &UWirelessPowerPanelWidget::OnSetChannelClicked);
    }
    if (RangeInput)
    {
        RangeInput->OnTextChanged.AddDynamic(this, &UWirelessPowerPanelWidget::OnRangeInputChanged);
    }
    if (SetRangeButton)
    {
        SetRangeButton->OnClicked.AddDynamic(this, &UWirelessPowerPanelWidget::OnSetRangeClicked);
    }
    if (RangeToggleButton)
    {
        RangeToggleButton->OnClicked.AddDynamic(this, &UWirelessPowerPanelWidget::OnRangeToggleClicked);
    }
    if (LaserLinkButton)
    {
        LaserLinkButton->OnClicked.AddDynamic(this, &UWirelessPowerPanelWidget::OnLaserLinkToggleClicked);
    }
    if (LaserVisibleButton)
    {
        LaserVisibleButton->OnClicked.AddDynamic(this, &UWirelessPowerPanelWidget::OnLaserVisibleClicked);
    }
    if (PowerUpButton)
    {
        PowerUpButton->OnClicked.AddDynamic(this, &UWirelessPowerPanelWidget::OnPowerUpClicked);
    }
    if (PowerDownButton)
    {
        PowerDownButton->OnClicked.AddDynamic(this, &UWirelessPowerPanelWidget::OnPowerDownClicked);
    }
    if (LaserCombo)
    {
        LaserCombo->OnSelectionChanged.AddDynamic(this, &UWirelessPowerPanelWidget::OnLaserComboChanged);
    }

    // Show only the sections this building family uses (laser tower vs the other three).
    ApplyLayoutForBuildingType();

    // Force the dropdown to be rebuilt against the world's current laser towers.
    LastComboSignature.Empty();
    LastChannelComboSig.Empty();
    PendingLaserChannel.Empty();
    PendingChannel.Empty();
    LastChannelScanRadius = 0.f;

    RefreshValues(true);
}

void UWirelessPowerPanelWidget::NativeDestruct()
{
    // Do NOT hide the range cage here: it is controlled by the manual toggle and must persist
    // after the panel closes (so the player can keep building with the coverage cube visible).
    if (ChannelInput) { ChannelInput->OnTextChanged.RemoveAll(this); }
    if (ChannelCombo) { ChannelCombo->OnSelectionChanged.RemoveAll(this); }
    if (SetChannelButton) { SetChannelButton->OnClicked.RemoveAll(this); }
    if (RangeInput) { RangeInput->OnTextChanged.RemoveAll(this); }
    if (SetRangeButton) { SetRangeButton->OnClicked.RemoveAll(this); }
    if (RangeToggleButton) { RangeToggleButton->OnClicked.RemoveAll(this); }
    if (LaserLinkButton) { LaserLinkButton->OnClicked.RemoveAll(this); }
    if (LaserVisibleButton) { LaserVisibleButton->OnClicked.RemoveAll(this); }
    if (PowerUpButton) { PowerUpButton->OnClicked.RemoveAll(this); }
    if (PowerDownButton) { PowerDownButton->OnClicked.RemoveAll(this); }
    if (LaserCombo) { LaserCombo->OnSelectionChanged.RemoveAll(this); }
    Super::NativeDestruct();
}

void UWirelessPowerPanelWidget::ApplyLayoutForBuildingType()
{
    // Feyko's laser tower panel and the other three families' panels show different sections.
    const bool bLaser = IsValid(TargetNode)
        && TargetNode->GetBuildingType() == EWirelessBuildingType::LaserTower;

    // Laser-only: 开关连接, the 可连激光塔编号 dropdown, 激光的可见性.
    const ESlateVisibility LaserVis = bLaser ? ESlateVisibility::Visible : ESlateVisibility::Collapsed;
    if (LaserLinkButton)  { LaserLinkButton->SetVisibility(LaserVis); }
    if (LaserComboLabel)  { LaserComboLabel->SetVisibility(LaserVis); }
    if (LaserCombo)       { LaserCombo->SetVisibility(LaserVis); }
    if (LaserVisibleButton) { LaserVisibleButton->SetVisibility(LaserVis); }

    // v1.0.45: 断电/启动 (and the laser extras) live in ONE row that is laser-tower-only now --
    // the other three families show neither button (user request: "除输电塔外去掉").
    if (PowerRowBox)
    {
        PowerRowBox->SetVisibility(bLaser ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    }

    // v1.0.45: 电线杆/中继器 pick their new 编号 from the nearby-nodes dropdown; the laser tower
    // and the converter keep the free-text box.
    const bool bPoleOrRelay = IsValid(TargetNode)
        && (TargetNode->GetBuildingType() == EWirelessBuildingType::AreaPole
            || TargetNode->GetBuildingType() == EWirelessBuildingType::Relay);
    if (ChannelInput)
    {
        ChannelInput->SetVisibility(bPoleOrRelay ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
    }
    if (ChannelCombo)
    {
        ChannelCombo->SetVisibility(bPoleOrRelay ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    }

    // Range row: every family except the laser tower (whose reach is fixed at 4800 m).
    if (RangeBox)
    {
        RangeBox->SetVisibility(bLaser ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
    }
}

UWirelessPowerRCO* UWirelessPowerPanelWidget::GetRCO() const
{
    AFGPlayerController* PC = Cast<AFGPlayerController>(GetOwningPlayer());
    return PC ? PC->GetRemoteCallObjectOfClass<UWirelessPowerRCO>() : nullptr;
}

void UWirelessPowerPanelWidget::RefreshValues(bool bForceRangeInput)
{
    if (bTargetLost)
    {
        return;
    }
    if (!TargetNode || !IsValid(TargetNode))
    {
        TargetNode = Cast<AWirelessPowerBuildable>(mInteractObject);
    }
    if (!IsValid(TargetNode))
    {
        return;
    }

    const EWirelessBuildingType Type = TargetNode->GetBuildingType();
    const bool bLaser = (Type == EWirelessBuildingType::LaserTower);

    // Re-apply the section visibility if the interact target's family only became known now.
    const int32 TypeId = static_cast<int32>(Type);
    if (TypeId != LastLayoutTypeId)
    {
        LastLayoutTypeId = TypeId;
        ApplyLayoutForBuildingType();
    }

    // Each building family gets its own console title.
    if (TitleText)
    {
        TitleText->SetText(MakePanelTitle(TargetNode));
    }
    // Feyko's panels call the channel "编号"; the laser tower's is its tower ID.
    if (ChannelSectionText)
    {
        ChannelSectionText->SetText(bLaser ? WP_Text(TEXT("Tower ID"), TEXT("传送塔 ID"))
                                           : WP_Text(TEXT("ID"), TEXT("ID")));
    }
    if (CurrentChannelLabel)
    {
        CurrentChannelLabel->SetText(bLaser ? WP_Text(TEXT("Current tower ID"), TEXT("当前传送塔 ID"))
                                            : WP_Text(TEXT("Current ID"), TEXT("当前 ID")));
    }
    if (NewChannelLabel)
    {
        NewChannelLabel->SetText(bLaser ? WP_Text(TEXT("New tower ID"), TEXT("新传送塔 ID"))
                                        : WP_Text(TEXT("New ID"), TEXT("新 ID")));
    }
    if (CurrentChannelText)
    {
        // v1.0.52: 编号是字符串，四族统一显示口径（空编号显示 "—"）。
        CurrentChannelText->SetText(FText::FromString(
            WirelessIdToDisplay(TargetNode->Channel)));
    }

    // Range as an EXACT NUMBER: show the server value, but never overwrite the digits the player
    // is currently typing (bRangeDirty is set by OnRangeInputChanged and cleared by 设置范围).
    // v1.0.21: the PANEL speaks METRES (the player never sees centimetres); Unreal world units stay
    // centimetres, so this is display/entry-only -- 1 m == 100 uu.
    if (RangeValueText)
    {
        // v1.0.36: the per-tier ceiling is LOCKED (only lowering is allowed), so show it. Without
        // this the box would silently snap a bigger number back and look broken.
        const AWirelessPowerBuildable* Cdo =
            Cast<AWirelessPowerBuildable>(TargetNode->GetClass()->GetDefaultObject());
        const int32 CeilingM = WirelessRangeToMetres(
            Cdo ? Cdo->WirelessRange : TargetNode->WirelessRange);
        RangeValueText->SetText(FText::Format(
            WP_Text(TEXT("Current range: {0} m (cap {1} m, cannot exceed)"), TEXT("当前射程：{0} 米（上限 {1} 米，不可超过）")),
            FText::AsNumber(WirelessRangeToMetres(TargetNode->WirelessRange)),
            FText::AsNumber(CeilingM)));
    }
    if (RangeInput && (bForceRangeInput || !bRangeDirty))
    {
        RangeInput->SetText(FText::AsNumber(WirelessRangeToMetres(TargetNode->WirelessRange)));
        bRangeDirty = false;
    }

    // ---- Laser Tower block ----
    if (bLaser)
    {
        RefreshLaserDropdown();

        if (LaserVisibleLabel)
        {
            const bool bVis = TargetNode->IsLaserVisible();
            LaserVisibleLabel->SetText(bVis ? WP_Text(TEXT("Beams visible: on"), TEXT("光束可见：开"))
                                            : WP_Text(TEXT("Beams visible: off"), TEXT("光束可见：关")));
            if (LaserVisibleButton)
            {
                LaserVisibleButton->SetBackgroundColor(bVis
                    ? FLinearColor(0.78f, 0.16f, 0.12f, 1.f)
                    : FLinearColor(0.28f, 0.30f, 0.34f, 1.f));
            }
        }
        if (LaserLinkLabel)
        {
            FText Txt = WP_Text(TEXT("Toggle link"), TEXT("切换连接"));
            if (!PendingLaserChannel.IsEmpty())
            {
                if (FWirelessId::Equal(PendingLaserChannel, TargetNode->Channel))
                {
                    Txt = FText::Format(
                        WP_Text(TEXT("Toggle link: ID {0} (same ID · auto-linked)"), TEXT("切换连接：ID {0}（同 ID · 自动连接）")),
                        FText::FromString(PendingLaserChannel));
                }
                else
                {
                    Txt = FText::Format(
                        WP_Text(TEXT("Toggle link: ID {0} ({1})"), TEXT("切换连接：ID {0}（{1}）")),
                        FText::FromString(PendingLaserChannel),
                        TargetNode->HasManualLaserLink(PendingLaserChannel)
                            ? WP_Text(TEXT("linked"), TEXT("已连接"))
                            : WP_Text(TEXT("not linked"), TEXT("未连接")));
                }
            }
            LaserLinkLabel->SetText(Txt);
        }
    }

    // ---- Pole / Relay block: nearby-nodes channel dropdown (v1.0.45) ----
    if (Type == EWirelessBuildingType::AreaPole || Type == EWirelessBuildingType::Relay)
    {
        RefreshChannelDropdown();
    }

    // Reflect the manual range-cage toggle switch state, and tint the switch with THIS building's
    // own cage colour so the panel button and the cube you see in the world obviously match.
    if (RangeToggleLabel)
    {
        const bool bOn = TargetNode->IsRangeCageVisible();
        RangeToggleLabel->SetText(bOn ? WP_Text(TEXT("Range display: on"), TEXT("射程显示：开"))
                                      : WP_Text(TEXT("Range display: off"), TEXT("射程显示：关")));
        if (RangeToggleButton)
        {
            const FLinearColor CageCol = TargetNode->GetCageColor();
            RangeToggleButton->SetBackgroundColor(bOn
                ? FLinearColor(CageCol.R * 0.80f, CageCol.G * 0.80f, CageCol.B * 0.80f, 1.f)
                : FLinearColor(0.22f, 0.30f, 0.38f, 1.f));
        }
    }
    if (StatusText)
    {
        const int32 Linked = TargetNode->LinkedNodeCount;
        if (!TargetNode->bPowerEnabled)
        {
            StatusText->SetText(WP_Text(TEXT("Status: powered down"), TEXT("状态：已断电")));
            StatusText->SetColorAndOpacity(FSlateColor(C_RED));
        }
        else if (TargetNode->bActive)
        {
            // T08 fix (WP-T08): the counts are the *desired/candidate* sets computed by
            // ValidateWirelessMesh BEFORE the actual links are (re)applied, so they are NOT proof
            // that the machines are actually receiving power. We therefore label them "candidate /
            // coverage" instead of "linked / powered" -- a player must not read these as a guarantee
            // of live power (e.g. an upstream outage still shows these numbers).
            StatusText->SetText(FText::Format(
                WP_Text(TEXT("Status: running · {0} candidate peers · {1} machines in coverage"), TEXT("状态：运行中 · 候选连接 {0} 个节点 · 覆盖 {1} 台机器")),
                FText::AsNumber(Linked), FText::AsNumber(TargetNode->PoweredMachineCount)));
            StatusText->SetColorAndOpacity(FSlateColor(C_GREEN));
        }
        else if (Linked > 0)
        {
            // Peers are candidate-linked but none of them has power: the grid needs at least one
            // node wired into a powered source. The numbers are still the candidate sets (T08).
            StatusText->SetText(FText::Format(
                WP_Text(TEXT("Status: no power · {0} candidate peers · {1} machines in coverage (peers are unpowered; wire any one node into a grid)"), TEXT("状态：无电力 · 候选 {0} 个节点 · 覆盖 {1} 台机器（对端节点未通电；把任意一个节点接入电网即可）")),
                FText::AsNumber(Linked), FText::AsNumber(TargetNode->PoweredMachineCount)));
            StatusText->SetColorAndOpacity(FSlateColor(C_AMBER));
        }
        else
        {
            StatusText->SetText(
                WP_Text(TEXT("Status: no power · no nodes linked (wire this into a powered grid, or place a same-ID node within range)"), TEXT("状态：无电力 · 未连接任何节点（请将其接入已通电的电网，或在射程内放置一个同 ID 节点）")));
            StatusText->SetColorAndOpacity(FSlateColor(C_AMBER));
        }
    }
}

void UWirelessPowerPanelWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
    Super::NativeTick(MyGeometry, InDeltaTime);

    if (!TargetNode || !IsValid(TargetNode))
    {
        TargetNode = Cast<AWirelessPowerBuildable>(mInteractObject);
    }

    // Guard against painting a panel whose building is gone (demolished / save-unloaded while
    // open). Removing the widget from INSIDE Tick runs during Slate's paint walk and ends as a
    // dangling OnPaint (EXCEPTION_ACCESS_VIOLATION reading 0x1ffe in BuildResourceHandle ->
    // FName::ToString). So: hide immediately, then remove OUTSIDE the paint pass via the ticker.
    if (!IsValid(TargetNode) && !bTargetLost)
    {
        bTargetLost = true;
        SetVisibility(ESlateVisibility::Collapsed);
        if (!bRemoveScheduled)
        {
            bRemoveScheduled = true;
            TWeakObjectPtr<UWirelessPowerPanelWidget> WeakThis(this);
            // Remove OUTSIDE Slate's paint pass. Uses the engine's TUniqueFunction overload of
            // FTSTicker::AddTicker (the delegate type is the global FTickerDelegate, not a
            // FTSTicker::FTickerDelegate nested type).
            FTSTicker::GetCoreTicker().AddTicker(TEXT("WirelessPowerPanelClose"), 0.1f,
                [WeakThis](float) -> bool {
                    if (UWirelessPowerPanelWidget* P = WeakThis.Get())
                    {
                        P->RemoveFromParent();
                    }
                    return false; // one-shot
                });
        }
        return;
    }
    if (bTargetLost)
    {
        return; // waiting for the deferred removal; paint nothing, refresh nothing
    }

    TickRefreshAccum += InDeltaTime;
    if (TickRefreshAccum >= 0.4f)
    {
        TickRefreshAccum = 0.f;
        RefreshValues(false);
    }
}

// ---- handlers ----

void UWirelessPowerPanelWidget::OnChannelInputChanged(const FText& NewText)
{
    // v1.0.52（阶段 A）：编号改回 Feyko 原版的字符串语义，这个过滤器只做**长度**限制。
    //
    // 回顾 v1.0.50 的坑（用户报告「转换器无法设置编号」）：过滤器当时是「只留数字/二进制位」，
    // 而转换器显示格式是 4 位二进制，每敲一位都被截断，解析必然失败 → 静默回弹。
    // 教训：**不要在输入过滤器里做与显示/解析格式耦合的字符集白名单**，
    // 只要放宽到"任意可见字符 + 长度上限"，显示层和解析层就不可能再打架。
    // 因此现在：允许任何输入（含汉字、字母、空格），只按码点截到 FWirelessId::Validate 的上限。
    // 规范化（去首尾空格 / 全角折半角 / ASCII 大写）统一在提交时做，不在打字过程中做 ——
    // 否则玩家想输入 "A B" 时刚打完 "A " 空格就被吃掉，输入体验很糟。
    static constexpr int32 kMaxIdLen = 24;
    FString Out;
    Out.Reserve(FMath::Min(NewText.ToString().Len(), kMaxIdLen));
    for (TCHAR C : NewText.ToString())
    {
        // 换行/回车在单行输入框里没有意义，直接丢弃；其余一律保留。
        if (C == TEXT('\n') || C == TEXT('\r'))
        {
            continue;
        }
        if (Out.Len() >= kMaxIdLen)
        {
            break;
        }
        Out.AppendChar(C);
    }
    if (ChannelInput && Out != NewText.ToString())
    {
        ChannelInput->SetText(FText::FromString(Out));
    }
}

void UWirelessPowerPanelWidget::OnSetChannelClicked()
{
    if (!TargetNode || !ChannelInput)
    {
        return;
    }
    // v1.0.52: 编号就是玩家输入的字符串（汉字合法），只做规范化。
    // 以前那套"十进制/4 位二进制"解析已删除 —— 正是它把转换器编号吃掉的。
    // T05 fix (WP-T05): a blank field is the LEGAL default-network ID. ApplyChannel (server side)
    // already treats an empty string as "return to default", but the panel used to silently refuse
    // empty input (FWirelessId::Validate returns false for empty), so the player could never get
    // back to the default net through the UI. We now normalize and submit empty explicitly.
    FString NewChannel = FWirelessId::Normalize(ChannelInput->GetText().ToString());
    if (NewChannel.Len() > 24)
    {
        NewChannel = NewChannel.Left(24); // mirror FWirelessId::Validate's cap
    }
    if (UWirelessPowerRCO* RCO = GetRCO())
    {
        RCO->Server_SetChannel(TargetNode, NewChannel);
    }
    // v1.0.57 (bug #3): 记住这个编号 —— 但默认网络（空串）不记，避免“记忆”污染后续放置。
    if (!NewChannel.IsEmpty())
    {
        AWirelessPowerBuildable::RecordLastUsedChannel(NewChannel);
    }
    RefreshValues(false);
}

void UWirelessPowerPanelWidget::OnRangeInputChanged(const FText& NewText)
{
    // Digits only (max 6 => up to 999999 metres). The box is in METRES; the server stores cm.
    // While the player is typing we set bRangeDirty so the 0.4s refresh stops overwriting the box
    // with the server's current value.
    bRangeDirty = true;
    const FString S = NewText.ToString();
    FString Out;
    for (TCHAR C : S)
    {
        if (FChar::IsDigit(C) && Out.Len() < 6)
        {
            Out.AppendChar(C);
        }
    }
    if (Out != S && RangeInput)
    {
        RangeInput->SetText(FText::FromString(Out));
    }
}

void UWirelessPowerPanelWidget::OnSetRangeClicked()
{
    if (!IsValid(TargetNode) || !RangeInput)
    {
        return;
    }
    // Exact numeric input, in METRES (no 100-step quantisation any more). The RPC takes Unreal
    // units (cm), so convert here: the panel is the only place that knows about metres.
    int32 TypedM = FCString::Atoi(*RangeInput->GetText().ToString());
    // v1.0.36: the ceiling is locked per tier -- don't even send a value above it. The server clamps
    // again in ApplyRange (a modified client must not be able to push past it).
    if (const AWirelessPowerBuildable* Cdo =
            Cast<AWirelessPowerBuildable>(TargetNode->GetClass()->GetDefaultObject()))
    {
        TypedM = FMath::Clamp(TypedM, 0, WirelessRangeToMetres(Cdo->WirelessRange));
    }
    if (UWirelessPowerRCO* RCO = GetRCO())
    {
        RCO->Server_SetRange(TargetNode, static_cast<float>(TypedM) * WIRELESS_UU_PER_METRE);
    }
    bRangeDirty = false;
    RefreshValues(true);
}

void UWirelessPowerPanelWidget::OnLaserLinkToggleClicked()
{
    if (!IsValid(TargetNode) || PendingLaserChannel.IsEmpty())
    {
        return;
    }
    // Same-ID towers are already auto-linked by the channel match; nothing to toggle.
    if (FWirelessId::Equal(PendingLaserChannel, TargetNode->Channel))
    {
        RefreshValues(false);
        return;
    }
    if (UWirelessPowerRCO* RCO = GetRCO())
    {
        RCO->Server_ToggleLaserLink(TargetNode, PendingLaserChannel);
    }
    RefreshValues(false);
}

void UWirelessPowerPanelWidget::OnLaserVisibleClicked()
{
    if (!IsValid(TargetNode))
    {
        return;
    }
    if (UWirelessPowerRCO* RCO = GetRCO())
    {
        RCO->Server_SetLaserVisible(TargetNode, !TargetNode->IsLaserVisible());
    }
    RefreshValues(false);
}

void UWirelessPowerPanelWidget::OnLaserComboChanged(FString SelectedItem, ESelectInfo::Type SelectionType)
{
    const int32 Idx = LaserCombo ? LaserCombo->GetSelectedIndex() : INDEX_NONE;
    PendingLaserChannel = ComboChannels.IsValidIndex(Idx) ? ComboChannels[Idx] : FString();
    RefreshValues(false);
}

// v1.0.45: 电线杆/中继器 channel dropdown. Picking an option mirrors the channel into the hidden
// text box, so 设置 (OnSetChannelClicked) keeps its single existing code path.
void UWirelessPowerPanelWidget::OnChannelComboChanged(FString SelectedItem, ESelectInfo::Type SelectionType)
{
    const int32 Idx = ChannelCombo ? ChannelCombo->GetSelectedIndex() : INDEX_NONE;
    if (ChannelComboChannels.IsValidIndex(Idx) && ChannelInput && IsValid(TargetNode))
    {
        // v1.0.52: 编号不再是数字，"距离随编号放大"不再成立；改为【扫描半径只增不减】：
        // 玩家每点一个更远的编号，下一轮扫描半径就至少覆盖到它，于是越点能看见越远的节点。
        PendingChannel = ChannelComboChannels[Idx];
        // Mirror the picked ID into the (hidden) text box so 设置 keeps its single code path.
        ChannelInput->SetText(FText::FromString(WirelessIdToDisplay(PendingChannel)));
    }
}

void UWirelessPowerPanelWidget::RefreshChannelDropdown()
{
    if (!ChannelCombo || !IsValid(TargetNode))
    {
        return;
    }

    // List the channels of the wireless nodes AROUND this building — matching a neighbour's 编号
    // is the whole point, so the search radius is a generous multiple of this node's own reach.
    //
    // v1.0.52: 编号变成字符串后，"半径随编号数值放大"这一招失效（编号没有大小概念）。
    // 改为【扫描半径只增不减】：起点是 250 m 与本机 reach 的较大者；玩家每在下拉里点中一个
    // 更远的编号，下一轮半径就被抬到至少覆盖它（OnChannelComboChanged 设置 PendingChannel，
    // 这里用 BestDist 里它的实际距离做下限）。于是"越点看得越远"，且玩家不会丢掉自己选的条目。
    //
    // v1.0.57 (bug #3): 起点从 120 m 抬到 250 m。原来 120 m 的起点是个**死锁**：中继器的射程是
    // 60/120/160 m，坐落稍远的中继器压根进不了列表 -> 玩家选不中它 -> PendingChannel 永远为空
    // -> "只增不减"的半径永远不增长 -> 越放越选不到，只能靠手动跑近。250 m 一次覆盖全部中继器档位。
    float SearchRadius = FMath::Max(TargetNode->WirelessRange, 25000.f);
    SearchRadius = FMath::Max(SearchRadius, LastChannelScanRadius);
    const FVector Origin = TargetNode->GetActorLocation();
    TMap<FString, float> BestDist; // channel -> nearest node distance (cm)
    TArray<AWirelessPowerBuildable*> Nodes;
    AWirelessPowerBuildable::GetRegisteredNodes(Nodes);
    for (AWirelessPowerBuildable* N : Nodes)
    {
        if (!IsValid(N) || N == TargetNode)
        {
            continue;
        }
        const float Dist = FVector::Dist(Origin, N->GetActorLocation());
        if (Dist > SearchRadius)
        {
            continue;
        }
        // 同一编号的多个节点只留最近的一台（下拉里一个编号一行）。
        const FString Key = FWirelessId::Normalize(N->Channel);
        const float* Existing = BestDist.Find(Key);
        if (!Existing || Dist < *Existing)
        {
            BestDist.Add(Key, Dist);
        }
    }

    // v1.0.50/52: always keep the entry the player picked (and this node's own 编号) in the list,
    // even when it lives outside the current radius -- otherwise picking a far-away ID would make
    // that very entry disappear on the next rebuild, which reads as "my selection got eaten".
    // Distance 0 marks "kept alive, distance unknown"; also the hook that GROWS the next scan.
    auto KeepAlive = [&](const FString& Ch)
    {
        const FString Key = FWirelessId::Normalize(Ch);
        if (!Key.IsEmpty() && !BestDist.Contains(Key))
        {
            BestDist.Add(Key, 0.f);
        }
    };
    KeepAlive(PendingChannel);
    KeepAlive(TargetNode->Channel);

    // 扫描半径的记忆：若玩家选的编号在本次范围内有实测距离，就把半径记到"能覆盖它"为止（只增）。
    if (!PendingChannel.IsEmpty())
    {
        if (const float* Found = BestDist.Find(FWirelessId::Normalize(PendingChannel)))
        {
            if (*Found > 0.f)
            {
                LastChannelScanRadius = FMath::Max(LastChannelScanRadius, FMath::Min(*Found * 1.5f, 200000.f));
            }
        }
        else
        {
            // 选中的编号连 KeepAlive 之外都找不到 ⇒ 它在当前半径外，直接翻倍（带上限）。
            LastChannelScanRadius = FMath::Max(LastChannelScanRadius, FMath::Min(SearchRadius * 2.f, 200000.f));
        }
    }

    // 排序键：距离近的在前；距离都是 0 的（KeepAlive 条目）按编号字典序排到最后。
    TArray<FString> SortedChannels;
    BestDist.GetKeys(SortedChannels);
    SortedChannels.Sort([](const FString& A, const FString& B) { return A < B; });

    // Only rebuild the option list when it actually changed (keeps the open dropdown open and the
    // user's selection intact across the 0.4s refresh ticks).
    FString Sig;
    for (const FString& Ch : SortedChannels)
    {
        Sig += FString::Printf(TEXT("%s:%d;"), *Ch, FMath::RoundToInt(BestDist[Ch] / 100.f));
    }
    if (Sig == LastChannelComboSig)
    {
        return;
    }
    LastChannelComboSig = Sig;

    ChannelCombo->ClearOptions();
    ChannelComboChannels.Reset();
    for (const FString& Ch : SortedChannels)
    {
        // Distance 0 means "kept alive from a previous pick, no live node measured" (see KeepAlive) --
        // printing "距离 0 米" would read like the peer is standing on top of us, so say so instead.
        const int32 DistM = FMath::RoundToInt(BestDist[Ch] / 100.f);
        ChannelCombo->AddOption(DistM > 0
            ? FText::Format(WP_Text(TEXT("ID {0} · {1} m away"), TEXT("ID {0} · 距离 {1} 米")),
                            FText::FromString(WirelessIdToDisplay(Ch)), FText::AsNumber(DistM)).ToString()
            : FText::Format(WP_Text(TEXT("ID {0} · out of range"), TEXT("ID {0} · 超出射程")),
                            FText::FromString(WirelessIdToDisplay(Ch))).ToString());
        ChannelComboChannels.Add(Ch);
    }

    // Preselect this node's own channel when present (visual anchor; selecting it again is a no-op).
    const int32 Self = ChannelComboChannels.IndexOfByKey(FWirelessId::Normalize(TargetNode->Channel));
    if (Self != INDEX_NONE)
    {
        ChannelCombo->SetSelectedIndex(Self);
    }
}

void UWirelessPowerPanelWidget::RefreshLaserDropdown()
{
    if (!LaserCombo || !IsValid(TargetNode))
    {
        return;
    }

    // Collect the IDs of every OTHER laser tower in the world (Feyko's panel lists the towers that
    // already exist; the player picks one and uses 开关连接 to bridge them).
    // v1.0.44: walk the shared wireless-node registry instead of every buildable in the world --
    // this dropdown refreshes while the panel is open, so the old world walk was a per-frame cost
    // in a large base (same root cause as the mesh instability fix).
    TSet<FString> IDs;
    TArray<AWirelessPowerBuildable*> Nodes;
    AWirelessPowerBuildable::GetRegisteredNodes(Nodes);
    for (AWirelessPowerBuildable* N : Nodes)
    {
        if (IsValid(N) && N != TargetNode && N->GetBuildingType() == EWirelessBuildingType::LaserTower)
        {
            const FString Key = FWirelessId::Normalize(N->Channel);
            if (!Key.IsEmpty())
            {
                IDs.Add(Key);
            }
        }
    }
    TArray<FString> Sorted = IDs.Array();
    Sorted.Sort([](const FString& A, const FString& B) { return A < B; });

    // Only rebuild the option list when it actually changed: rebuilding every refresh would keep
    // resetting the player's selection (and close the dropdown while it is open).
    FString Sig;
    for (const FString& Ch : Sorted)
    {
        const bool bLinked = TargetNode->HasManualLaserLink(Ch) || FWirelessId::Equal(TargetNode->Channel, Ch);
        Sig += FString::Printf(TEXT("%s%s;"), *Ch, bLinked ? TEXT("+") : TEXT("-"));
    }
    if (Sig == LastComboSignature)
    {
        return;
    }
    LastComboSignature = Sig;

    LaserCombo->ClearOptions();
    ComboChannels.Reset();
    for (const FString& Ch : Sorted)
    {
        const bool bLinked = TargetNode->HasManualLaserLink(Ch) || FWirelessId::Equal(TargetNode->Channel, Ch);
        LaserCombo->AddOption(bLinked
            ? FText::Format(WP_Text(TEXT("ID {0} · linked"), TEXT("ID {0} · 已连接")),
                            FText::FromString(Ch)).ToString()
            : FText::Format(WP_Text(TEXT("ID {0}"), TEXT("ID {0}")), FText::FromString(Ch)).ToString());
        ComboChannels.Add(Ch);
    }

    // Restore the previous pick when it still exists; otherwise fall back to the first entry.
    int32 Sel = ComboChannels.IndexOfByKey(FWirelessId::Normalize(PendingLaserChannel));
    if (Sel == INDEX_NONE)
    {
        Sel = (ComboChannels.Num() > 0) ? 0 : INDEX_NONE;
    }
    if (Sel != INDEX_NONE)
    {
        LaserCombo->SetSelectedIndex(Sel);
        PendingLaserChannel = ComboChannels[Sel];
    }
    else
    {
        PendingLaserChannel.Empty();
    }
}

void UWirelessPowerPanelWidget::OnRangeToggleClicked()
{
    if (!IsValid(TargetNode))
    {
        return;
    }
    // Flip the coverage cube on this building. Client-side visual only; persists after the panel
    // closes so the player can keep the range cage on while building. EndPlay still cleans it up.
    TargetNode->SetRangeCageVisible(!TargetNode->IsRangeCageVisible());
    RefreshValues(false);
}

void UWirelessPowerPanelWidget::OnPowerUpClicked()
{
    if (!TargetNode)
    {
        return;
    }
    if (UWirelessPowerRCO* RCO = GetRCO())
    {
        RCO->Server_SetPowerEnabled(TargetNode, true);
    }
    RefreshValues(false);
}

void UWirelessPowerPanelWidget::OnPowerDownClicked()
{
    if (!TargetNode)
    {
        return;
    }
    if (UWirelessPowerRCO* RCO = GetRCO())
    {
        RCO->Server_SetPowerEnabled(TargetNode, false);
    }
    RefreshValues(false);
}
