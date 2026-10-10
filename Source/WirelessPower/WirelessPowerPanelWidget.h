#pragma once

#include "UI/FGInteractWidget.h"
#include "Types/SlateEnums.h"
#include "WirelessPowerPanelWidget.generated.h"

class UBorder;
class UButton;
class UComboBoxString;
class UEditableTextBox;
class UHorizontalBox;
class UTextBlock;
class UVerticalBox;
class AWirelessPowerBuildable;
class UWirelessPowerRCO;

/**
 * Control panel for every Wireless Power building (opened with E), laid out to mirror Feyko's
 * original panels:
 *
 *   无线中继器 / 电力转换器 / 区域电线杆 : 当前编号 / 新编号 + SET  ->  覆盖范围(精确数字)  ->
 *                                          断电 | 启动  ->  范围显示
 *   无线输电塔 (Laser Tower)            : 开关连接 -> 当前激光塔编号 / 新激光塔编号 + SET ->
 *                                          左列[断电 + 可连激光塔编号下拉] | 右列[启动 + 激光的可见性]
 *
 * Ranges are typed as EXACT NUMBERS (the old slider is gone). The laser tower connects BY ID: the
 * dropdown lists the IDs of laser towers that already exist, and 开关连接 toggles that pair.
 *
 * The whole UI is built in C++ (WidgetTree->ConstructWidget) because the mod ships without UMG
 * assets. Changes go to the server through UWirelessPowerRCO; replicated state (Channel,
 * WirelessRange, bPowerEnabled, bActive, LaserPeers, bLaserVisible) flows back and refreshes.
 */
UCLASS()
class WIRELESSPOWER_API UWirelessPowerPanelWidget : public UFGInteractWidget
{
    GENERATED_BODY()
public:
    UWirelessPowerPanelWidget(const FObjectInitializer& ObjectInitializer);

    // Begin UUserWidget interface
    virtual TSharedRef<SWidget> RebuildWidget() override;
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;
    virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
    // End UUserWidget interface

protected:
    // ---- button handlers ----
    UFUNCTION()
    void OnSetChannelClicked();

    UFUNCTION()
    void OnSetRangeClicked();

    UFUNCTION()
    void OnPowerUpClicked();

    UFUNCTION()
    void OnPowerDownClicked();

    UFUNCTION()
    void OnChannelInputChanged(const FText& NewText);

    /** Range box: digits only + marks the value dirty so the 0.4s refresh stops overwriting it. */
    UFUNCTION()
    void OnRangeInputChanged(const FText& NewText);

    /** Manual range-cage toggle switch (shows/hides the coverage cube). */
    UFUNCTION()
    void OnRangeToggleClicked();

    /** Laser Tower: 开关连接 -- connect/disconnect the ID picked in the dropdown. */
    UFUNCTION()
    void OnLaserLinkToggleClicked();

    /** Laser Tower: 激光的可见性 -- show/hide the beams. */
    UFUNCTION()
    void OnLaserVisibleClicked();

    /** Laser Tower: the "可连激光塔编号" dropdown changed. */
    UFUNCTION()
    void OnLaserComboChanged(FString SelectedItem, ESelectInfo::Type SelectionType);

    /** v1.0.45: Pole/Relay — the "新编号" nearby-nodes dropdown changed; mirrors the pick into
     *  the (hidden) text box so 设置 keeps its single code path. */
    UFUNCTION()
    void OnChannelComboChanged(FString SelectedItem, ESelectInfo::Type SelectionType);

private:
    /** Route a server change through the player controller's RCO. */
    UWirelessPowerRCO* GetRCO() const;

    /** Push the current buildable state into the widgets. */
    void RefreshValues(bool bForceRangeInput);

    /** Rebuild the "可连激光塔编号" dropdown from the laser towers that exist in the world. */
    void RefreshLaserDropdown();

    /** v1.0.45: rebuild the "新编号" dropdown from wireless nodes NEAR this building
     *  (Area Power Pole / Relay only) — the point of matching 编号 is to reach those neighbours. */
    void RefreshChannelDropdown();

    /** Show the sections that apply to this building family and hide the rest. */
    void ApplyLayoutForBuildingType();

    UPROPERTY()
    TObjectPtr<UBorder> RootBorder = nullptr;

    UPROPERTY()
    TObjectPtr<UTextBlock> TitleText = nullptr;

    // ---- 编号 section ----
    UPROPERTY()
    TObjectPtr<UTextBlock> ChannelSectionText = nullptr;

    UPROPERTY()
    TObjectPtr<UTextBlock> CurrentChannelLabel = nullptr;

    UPROPERTY()
    TObjectPtr<UTextBlock> NewChannelLabel = nullptr;

    UPROPERTY()
    TObjectPtr<UTextBlock> CurrentChannelText = nullptr;

    UPROPERTY()
    TObjectPtr<UEditableTextBox> ChannelInput = nullptr;

    /** v1.0.45: 电线杆/中继器 pick the new 编号 from a dropdown of nearby node IDs instead of
     *  free text (created next to ChannelInput; one of the two is hidden per family). */
    UPROPERTY()
    TObjectPtr<UComboBoxString> ChannelCombo = nullptr;

    /** Parallel to ChannelCombo options: option index -> nearby node channel (编号). v1.0.52: 字符串。 */
    TArray<FString> ChannelComboChannels;

    /** Option strings last pushed into the channel dropdown, so an unchanged list keeps the
     *  user's selection (same trick as the laser combo). */
    FString LastChannelComboSig;

    UPROPERTY()
    TObjectPtr<UButton> SetChannelButton = nullptr;

    /** v1.0.50: the 编号 the player last picked in the "新编号" dropdown (空 = nothing picked yet).
     *  The dropdown list is a window whose RADIUS grows with this value, so picking a far-away ID
     *  widens the next scan and can reveal further peers (see RefreshChannelDropdown).
     *  Purely a panel-side hint: the authoritative ID is still Channel on the buildable.
     *  v1.0.52: 编号改成字符串/汉字后，"距离随编号放大"不再成立（编号没有大小概念），
     *  改为记录上一次扫描半径并**只增不减**，同样能达到"越点看得越远"的效果。 */
    FString PendingChannel;

    /** v1.0.52: 上一次"新编号"下拉用的扫描半径（cm），只增不减 —— 详见 PendingChannel 注释。 */
    float LastChannelScanRadius = 0.f;

    // ---- 覆盖范围 section (not shown on the laser tower) ----
    UPROPERTY()
    TObjectPtr<UVerticalBox> RangeBox = nullptr;

    UPROPERTY()
    TObjectPtr<UTextBlock> RangeValueText = nullptr;

    /** Exact numeric range entry (replaces the old slider). */
    UPROPERTY()
    TObjectPtr<UEditableTextBox> RangeInput = nullptr;

    UPROPERTY()
    TObjectPtr<UButton> SetRangeButton = nullptr;

    // ---- 激光塔连接 section (laser tower only) ----
    UPROPERTY()
    TObjectPtr<UButton> LaserLinkButton = nullptr;

    UPROPERTY()
    TObjectPtr<UTextBlock> LaserLinkLabel = nullptr;

    UPROPERTY()
    TObjectPtr<UComboBoxString> LaserCombo = nullptr;

    /** Parallel to the combo options: option index -> laser tower ID (编号). v1.0.52: 字符串。 */
    TArray<FString> ComboChannels;

    /** Option strings last pushed into the combo, so an unchanged list keeps the user's selection. */
    FString LastComboSignature;

    /** The ID currently picked in the dropdown (空 = nothing selected). */
    FString PendingLaserChannel;

    // ---- 电源 section ----
    // One row, two columns (exactly Feyko's laser layout):
    //   left  = 断电  + [可连激光塔编号 dropdown]      (dropdown only on the laser tower)
    //   right = 启动  + [激光的可见性]                 (switch only on the laser tower)
    // v1.0.45: the WHOLE row is laser-tower-only now -- the other three families get neither
    // 断电 nor 启动 (a pole/relay/converter has no reason to power itself down).
    UPROPERTY()
    TObjectPtr<UHorizontalBox> PowerRowBox = nullptr;

    UPROPERTY()
    TObjectPtr<UButton> PowerUpButton = nullptr;

    UPROPERTY()
    TObjectPtr<UButton> PowerDownButton = nullptr;

    /** "可连激光塔编号" caption (laser tower only). */
    UPROPERTY()
    TObjectPtr<UTextBlock> LaserComboLabel = nullptr;

    UPROPERTY()
    TObjectPtr<UButton> LaserVisibleButton = nullptr;

    UPROPERTY()
    TObjectPtr<UTextBlock> LaserVisibleLabel = nullptr;

    // ---- shared ----
    UPROPERTY()
    TObjectPtr<UButton> RangeToggleButton = nullptr;

    UPROPERTY()
    TObjectPtr<UTextBlock> RangeToggleLabel = nullptr;

    UPROPERTY()
    TObjectPtr<UTextBlock> StatusText = nullptr;

    UPROPERTY()
    TObjectPtr<AWirelessPowerBuildable> TargetNode = nullptr;

    /** True after the user edited the range box but before pressing 设置范围: keeps the tick refresh
     *  from snapping the digits back to the server's value mid-typing. */
    bool bRangeDirty = false;
    float TickRefreshAccum = 0.f;

    /** Building type the current layout was built for (INDEX_NONE = not applied yet). Lets
     *  RefreshValues re-apply the layout if the interact target only becomes known later. */
    int32 LastLayoutTypeId = -1;

    /** The interact target died (building demolished while open): stop refreshing, hide, and
     *  close the panel from OUTSIDE Slate's paint pass (deferred removal). */
    bool bTargetLost = false;
    /** Guards scheduling the deferred RemoveFromParent exactly once. */
    bool bRemoveScheduled = false;
};
