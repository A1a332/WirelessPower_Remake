#pragma once

#include "Buildables/FGBuildable.h"
#include "FGPowerConnectionComponent.h"
#include "FGPowerInfoComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ObjectPtr.h"
#include "WirelessPowerConnection.h"
#include "WirelessPowerTypes.h"
#include "WirelessPowerId.h"
#include "WirelessPowerBuildable.generated.h"

class UWirelessPowerPanelWidget;
class UParticleSystemComponent;
class UParticleSystem;

/**
 * Base class for every Wireless Power building.
 *
 * Feyko topology (exact, as re-stated by the user):
 *   输电塔 (Laser Tower) : 4 visible wire ports -- different towers interconnect via different numbers.
 *   转换器 (Power Converter): 1 visible wire port -- the gateway between a WIRED grid and the wireless mesh.
 *   继电器 (Relay)        : 0 visible ports -- pure wireless mesh node.
 *   无线电杆 (Area Power Pole): 0 visible ports -- BUT it is the ONLY node that broadcasts power
 *                                       to vanilla machines within its (cubic) range.
 *
 * Circuit model:
 *   - One HIDDEN UWirelessPowerConnection (WirelessConnection) is the wireless ANTENNA. It owns its
 *     own UFGPowerInfoComponent (WirelessPowerInfo). Peer antennas merge into ONE circuit via
 *     AddHiddenConnection(), so a powered grid wired into one node feeds any machine the mesh reaches.
 *   - Buildings WITH visible ports (Laser Tower, Converter) create N visible UWirelessPowerConnection
 *     ports (each with its own UFGPowerInfoComponent). Each visible port is fused to the antenna by a
 *     PERMANENT internal hidden bridge (visible<->antenna), deferred to Tick until both circuits exist
 *     (the circuit subsystem creates each connection's private circuit only AFTER actor BeginPlay).
 *   - Buildings WITHOUT visible ports (Relay, Area Power Pole) have NO internal bridge -- the antenna's
 *     circuit IS the node's circuit.
 *
 * CRITICAL SDK CONTRACT (FGPowerConnectionComponent::mPowerInfo):
 *   "The power info for this connection should be unique for each connection or a race condition can
 *    occur for setting the circuit ID on the power info." So every port (visible AND antenna) owns its
 *    OWN power info. v1.0.6 shared one and the merge never formed.
 *
 * Machine broadcast (Area Power Pole only):
 *   The pole scans AFGBuildableSubsystem for vanilla machines inside its CUBIC range, grabs each
 *   machine's UFGPowerConnectionComponent (the power plug) and AddHiddenConnection's it to the antenna.
 *   This merges the machine into the (powered) wireless circuit, so the machine runs with NO wire.
 *   Self-healing + range/disable teardown mirror the peer mesh. Non-pole nodes never do this.
 *
 * Range shape: a CUBE (正方体网格形) per the user's correction of Feyko's original sphere. A point is
 *   in range when max(|dx|,|dy|,|dz|) <= WirelessRange. The debug draw is a wireframe box.
 */
UCLASS(Abstract)
class WIRELESSPOWER_API AWirelessPowerBuildable : public AFGBuildable
{
    GENERATED_BODY()
public:
    AWirelessPowerBuildable();

    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    // ---- Interaction outline (v1.0.28) ----
    // AFGBuildable highlights the CROSSHAIRED building with UFGOutlineComponent::ShowOutline(this, ...),
    // which covers EVERY mesh component on the actor -- and that included the coverage cage and the
    // laser beams. The user saw the thin bars repainted in the outline colour ("指向它时，显示出来的是
    // 描边颜色"). These overrides keep the vanilla highlight but scope it to the real building mesh, so
    // the visualisation geometry stays in its own colour. The Stop* side is overridden as well: the
    // component-scoped call stores its state separately (FActorOutlineState::mComponentSpecified*), so
    // it must be cleared the same way or the building mesh could stay highlighted forever.
    virtual void StartIsLookedAt_Implementation(class AFGCharacterPlayer* byCharacter, const FUseState& state) override;
    virtual void StopIsLookedAt_Implementation(class AFGCharacterPlayer* byCharacter, const FUseState& state) override;
    virtual void StartIsLookedAtForDismantle_Implementation(class AFGCharacterPlayer* byCharacter) override;
    virtual void StopIsLookedAtForDismantle_Implementation(class AFGCharacterPlayer* byCharacter) override;
    virtual void StartIsLookedAtForConnection(class AFGCharacterPlayer* byCharacter, class UFGCircuitConnectionComponent* overlappingConnection) override;
    virtual void StopIsLookedAtForConnection(class AFGCharacterPlayer* byCharacter) override;

    /** Buildings only share power with others on the same 编号 (channel). Now a free-form CHINESE
     *  word ("一号" / "甲" / "仓库" ...) instead of a number -- v1.0.52, user request ("不行就改为
     *  输入中文汉字吧"). Empty string = the default network.
     *
     *  Identity rule: two nodes are on the same network iff their NORMALISED ids compare equal.
     *  Normalisation (FWirelessId::Normalize) folds full-width -> half-width, uppercases ASCII and
     *  trims the ends, so " 一号 " == "一号" == "１号". Comparisons therefore always go through
     *  FWirelessId::Equal -- never through a raw == on the raw entry.
     *
     *  SaveGame: without this specifier the save system (which only serializes CPF_SaveGame
     *  properties, same as vanilla FGBuildableStorage) drops the value on world reload. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, ReplicatedUsing = OnRep_Channel, Category = "WirelessPower")
    FString Channel = TEXT("1");

    /** True once the PLAYER chose this node's 编号 from the panel (ApplyChannel). SaveGame so the
     *  choice survives a reload.
     *
     *  v1.0.58: this is the guard that lets "落地即自动入网" be safe. A freshly built node inherits
     *  the 编号 of the nearest in-range node of the same family (Feyko's `setIDAndValidateConnections`
     *  did the same thing -- the id was auto-assigned, not typed). Without this flag a node whose
     *  player *deliberately* left/set the default "1" next to a "2" network would be silently
     *  re-homed on every save reload. */
    UPROPERTY(SaveGame)
    bool bChannelUserSet = false;

    /** Half-extent (cm) of the CUBIC coverage volume. Same-channel peers + in-range machines
     *  within +/- this distance on X/Y/Z auto-link.
     *  v1.0.53：由球体**改回立方体**（用户决定）。v1.0.52 曾按"完全还原 Feyko"改成球体，
     *  但用户实测后要求保留立方体的手感（斜角处能多连一点，建造时好对位）。
     *  SaveGame so a panel-tuned range survives a reload. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, ReplicatedUsing = OnRep_Range, Category = "WirelessPower")
    float WirelessRange = 3000.f;

    /** Master switch from the control panel. False drops all peer/machine links (POWER DOWN).
     *  SaveGame so a powered-down node stays powered down across a reload. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, ReplicatedUsing = OnRep_PowerEnabled, Category = "WirelessPower")
    bool bPowerEnabled = true;

    /** True while this node takes part in the wireless mesh. A node in POWER DOWN must be skipped
     *  by every other node's peer scan (v1.0.44: the scan is a registry walk, not a world walk,
     *  so this cheap test replaces what the old loop got for free by checking !bPowerEnabled). */
    bool IsPoweredOn() const { return bPowerEnabled; }

    /** True while the node has power (wired port, or the mesh for portless nodes) and is enabled. */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Replicated, Category = "WirelessPower")
    bool bActive = false;

    /** Number of same-channel peer nodes currently linked (server-computed, replicated for the panel). */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Replicated, Category = "WirelessPower")
    int32 LinkedNodeCount = 0;

    /** Number of vanilla machines currently wirelessly powered by this node (Area Power Pole only). */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Replicated, Category = "WirelessPower")
    int32 PoweredMachineCount = 0;

    /** Building family (set by subclass ctor). */
    EWirelessBuildingType GetBuildingType() const { return BuildingType; }
    int32 GetTier() const { return Tier; }
    int32 GetNumVisiblePorts() const { return PowerConnections.Num(); }
    bool BroadcastsToMachines() const { return bBroadcastsToMachines; }

    // ---- Server-side setters, invoked via UWirelessPowerRCO from the control panel ----
    void ApplyChannel(const FString& NewChannel);
    void ApplyRange(float NewRange);
    void ApplyPowerEnabled(bool bEnabled);

    // ---- v1.0.57 (bug #3)：「上次设置过的编号」记忆 ----
    /** 记住玩家最近一次设置成功的编号（内部先规范化）。
     *  面板每次成功提交编号时调用；`ApplyChannel`（服务端权威路径）也会调一次。
     *  新放置的电杆/中继器会在 AWirelessPowerHologram::ConfigureActor 阶段自动继承这个值，
     *  于是"连放一串电杆"不必每个都重选编号。
     *
     *  ★只影响**新放置**的建筑：ConfigureActor 仅在放置那一刻被引擎调用，存档里读出来的
     *  老建筑不走这条路径，所以老存档的编号绝不会被它悄悄改写。
     *  存储 = 进程内固定缓冲 + GameUserSettings.ini 的 [WirelessPowerClient] 段；
     *  **不参与网络复制、不写进存档**。空/纯空白输入不记（保持"没有记忆"语义）。 */
    static void RecordLastUsedChannel(const FString& InChannel);

    /** 取回记忆中的编号（首次调用会从 GameUserSettings.ini 懒加载）。从未设置过则返回空串。 */
    static FString GetLastUsedChannel();

    /** Show/hide the range cage. Driven by the panel's range toggle switch. Hiding also drops the
     *  cage instances so no real geometry lingers on screen. Public so the panel widget can call it. */
    void SetRangeCageVisible(bool bShow);

    /** True while the range cage is currently being drawn (panel switch state, persists after the
     *  panel closes so the player can keep the coverage cube on while building). */
    bool IsRangeCageVisible() const { return bShowCage; }

    /** This building's cage colour. Each of the four building families has its own hue, and the
     *  panel's range-display switch is tinted with the same colour so panel <-> cube obviously match. */
    FLinearColor GetCageColor() const;

    // ---- Laser Tower (无线输电塔) extras ----
    // Feyko's laser tower is the only node the player wires up BY ID: the panel lists the IDs (编号)
    // of the other laser towers that already exist, and "开关连接" connects/disconnects that pair.

    /** Toggle a MANUAL tower-to-tower link by the peer's 编号. Manual links ignore the
     *  channel auto-match but still need line of sight. Server-side (called via the RCO). */
    void ApplyLaserLinkToggle(const FString& TargetChannel);

    /** Show/hide the laser beams drawn between this tower and its linked towers (panel switch).
     *  Replicated so every client renders the same state. */
    void ApplyLaserVisible(bool bVisible);

    /** True while this tower should draw its laser beams. */
    bool IsLaserVisible() const { return bLaserVisible; }

    /** True if this tower currently holds a MANUAL link to the given 编号 (normalised compare). */
    bool HasManualLaserLink(const FString& TargetChannel) const
    {
        return ManualLinkChannels.ContainsByPredicate(
            [&TargetChannel](const FString& S) { return FWirelessId::Equal(S, TargetChannel); });
    }

    /** Towers this laser tower is currently linked to (auto channel match + manual), replicated so
     *  clients can draw the beams. Always empty for the other three building families. */
    const TArray<TObjectPtr<AWirelessPowerBuildable>>& GetLaserPeers() const { return LaserPeers; }

    // ------------------------------------------------------------------------
    // v1.0.44 SCALE FIX -- shared node registry.
    //
    // Problem (user report: "无线电杆一旦多起来后就不稳定"): every node used to walk
    // AFGBuildableSubsystem::GetAllBuildablesRef() -- EVERY buildable in the world, foundations
    // and belts included -- on every validation pass (~3 s) to find same-channel peers, and the
    // Area Power Pole did it again, per machine, calling GetComponents<>() on each. Cost is
    // O(nodes x buildables), i.e. quadratic in base size, and every pass that found a change
    // re-merged circuits through AddHiddenConnection(). In a real base that is what made the grid
    // stutter / flicker once enough poles existed.
    //
    // Fix: keep a process-wide registry of the wireless nodes ONLY (a few dozen at most), so a
    // validation pass is O(nodes) instead of O(buildables). The machine walk the Area Power Pole
    // needs is amortised too: one scan every WP_MACHINE_SCAN_PERIOD per world, shared by all poles
    // (see the FWPMachineCache in WirelessPowerBuildable.cpp).
    //
    // Lifetime: registered from BeginPlay, removed in EndPlay. Entries are weak, so a dismantled
    // node can never dangle. Public because the control panel's laser dropdown also enumerates
    // towers and must not walk the world either.
    // ------------------------------------------------------------------------
    static void RegisterWirelessNode(AWirelessPowerBuildable* Node);
    static void UnregisterWirelessNode(AWirelessPowerBuildable* Node);

    /** Fills OutNodes with every live registered wireless node (clear + fill). Safe to iterate. */
    static void GetRegisteredNodes(TArray<AWirelessPowerBuildable*>& OutNodes);

    // ------------------------------------------------------------------------
    // v1.0.58 -- Feyko parity: "落地即自动入网" (build it and it joins on its own).
    //
    // The original mod's `InitGameWorld` blueprint subscribes to
    // AFGBuildableSubsystem::BuildableConstructedGlobalDelegate --
    // "Broadcast when a buildable or decor has been constructed. Regardless of who built it" --
    // then, for each new buildable, collects the Area Power Poles within range and, after a
    // 0.5 s Delay, calls pole.ValidateAreaConnections(). The judgement is pure DISTANCE; the
    // player never types an id. The player-visible effect is "每新铺的电线杆自动链接上一个
    // 电线杆的电路".
    //
    // Our equivalent is this pair: UWirelessPowerGameWorldModule forwards every constructed
    // buildable to every registered node, and the node decides locally whether it cares (it is
    // the new building itself, or the new building landed inside its coverage).
    // ------------------------------------------------------------------------

    /** Called by UWirelessPowerGameWorldModule for EVERY buildable that the global
     *  BuildableConstructedGlobalDelegate reports -- including this node's own placement.
     *  Server-side only (circuits are server state); no-op otherwise. */
    void NotifyBuildableConstructed(AFGBuildable* NewBuildable);

    /** Set by the placement hologram when it stamped the client-side "remembered 编号" onto a
     *  freshly placed node (v1.0.57's bug #3 fix).
     *
     *  TRANSIENT on purpose -- deliberately NOT a UPROPERTY, so it is false on every actor that came
     *  out of a save, which is exactly the distinction AutoAdoptNearestPeerChannel() needs:
     *    * a non-default 编号 read back from a save  = a decision from an earlier session -> keep it;
     *    * a non-default 编号 just stamped at placement = a convenience hint       -> overwritable.
     *  A 编号 the player typed into the panel is protected separately, by bChannelUserSet. */
    bool bChannelFromHologram = false;

protected:
    UFUNCTION()
    void OnRep_Channel();

    UFUNCTION()
    void OnRep_Range();

    UFUNCTION()
    void OnRep_PowerEnabled();

    UFUNCTION()
    void OnRep_LaserVisible();

    UFUNCTION()
    void OnRep_LaserPeers();

    UPROPERTY()
    TObjectPtr<UStaticMeshComponent> MeshComp = nullptr;

    /** Visible ports the player wires a power cable into. Each owns its own UFGPowerInfoComponent.
     *  Empty for Relay / Area Power Pole (no visible ports). Laser Tower has 4, Converter has 1. */
    UPROPERTY()
    TArray<TObjectPtr<UWirelessPowerConnection>> PowerConnections;

    /** Power infos parallel to PowerConnections (one unique power info per visible port). */
    UPROPERTY()
    TArray<TObjectPtr<UFGPowerInfoComponent>> PowerInfos;

    /** Counter that makes each InitWireless pass in THIS construction chain use fresh subobject
     *  names. A C++ ctor chain runs base-then-derived, and both APowerConverter and
     *  APowerConverterMk2 call InitWireless, so a fixed "PowerConnection0" name would be requested
     *  twice and CreateDefaultSubobject would abort with
     *      Fatal error: Default subobject WirelessPowerConnection PowerConnection0
     *      already exists for PowerConverterMk2
     *  Unregistering the first set does NOT free the name: the duplicate check runs against the
     *  CDO's subobject registry, not our arrays. Bumping this value yields
     *  "PowerConnection0" for the base pass and "PowerConnection1_0" for the derived pass. */
    int32 PortBuildGeneration = 0;

    /** Hidden antenna's power info. Distinct from every visible port's PowerInfo (SDK forbids
     *  two connections sharing one PowerInfo). The antenna is the wireless mesh node. */
    UPROPERTY()
    TObjectPtr<UFGPowerInfoComponent> WirelessPowerInfo = nullptr;

    /** Hidden antenna used purely for the wireless circuit merge (no visible wire). AddHiddenConnection
     *  requires one hidden end, so only the antenna links wirelessly. */
    UPROPERTY()
    TObjectPtr<UWirelessPowerConnection> WirelessConnection = nullptr;

    /** Instanced bar field that draws the CUBIC range cage (v1.0.22).
     *
     *  WHY NOT LINES: v1.0.13-v1.0.21 drew the cage and the laser beams with ULineBatchComponent.
     *  The in-game diagnostic proved the lines were batched (cageLines=240) yet NOTHING appeared on
     *  screen -- this Shipping build simply does not render batched debug lines. So we now emit REAL
     *  geometry: thin instances of /Engine/BasicShapes/Cube (a vanilla mesh that IS cooked into the
     *  game), which render through the normal static-mesh path. Absolute-transform so the cage stays
     *  world-axis-aligned no matter how the player rotated the building. */
    UPROPERTY()
    TObjectPtr<UInstancedStaticMeshComponent> CageISM = nullptr;

    /** Second instanced bar field, used ONLY for the Laser Tower beams (one stretched bar per linked
     *  tower). Separate component so rebuilding beams never disturbs the cage.
     *
     *  v1.0.32: no longer the PRIMARY renderer. The beams now use Feyko's original mechanism (see
     *  BeamFX below); this ISM is kept as the fallback for the case where his P_Laser particle
     *  system cannot be loaded, so a link is never invisible. */
    UPROPERTY()
    TObjectPtr<UInstancedStaticMeshComponent> BeamISM = nullptr;

    /** Feyko's original laser beam (v1.0.32): UParticleSystemComponents playing his own
     *  /WirelessPower/Buildable/LaserTower/P_Laser, exactly the way the original Blueprint did it --
     *  one Cascade Beam2 emitter ("Laser") whose per-particle source/target points we push with
     *  SetBeamSourcePoint(0, WorldSource, BeamIndex) / SetBeamTargetPoint(0, WorldTarget, BeamIndex).
     *
     *  BeamFX.GetLast() is created in the constructor (so it exists on every machine, server AND
     *  client); further entries are added lazily when one component cannot carry all the links --
     *  see BeamPoolSizeFor(). Particle slots per component are bounded by the asset's
     *  ParticleModuleTypeDataBeam2::MaxBeamCount, which is read back at runtime. */
    UPROPERTY()
    TArray<TObjectPtr<UParticleSystemComponent>> BeamFX;

    /** Effective P_Laser template (nullptr when the asset is missing -> cube-bar fallback). */
    UPROPERTY()
    TObjectPtr<UParticleSystem> LaserPSTemplate = nullptr;

    /** Have we already resolved LaserPSTemplate + sanitised its beam modules? */
    bool bLaserPSResolved = false;

    /** How many beams one P_Laser component can show at once (from MaxBeamCount; >= 1). */
    int32 LaserBeamsPerComponent = 1;

    /** Resolve + sanitise Feyko's P_Laser once per actor. Returns nullptr if unavailable.
     *  "Sanitise" = force the beam emitter's source/target method to User Set, which is the only
     *  mode in which SetBeamSourcePoint/SetBeamTargetPoint are honoured, and dump the template's
     *  real configuration to the log (WP_LOGV) so it can be verified without opening the editor. */
    class UParticleSystem* ResolveLaserTemplate();

    /** Get (creating on demand) the BeamFX component at PoolIndex, with the template applied and
     *  the particle emitter live. Returns nullptr if the template is unavailable. */
    class UParticleSystemComponent* GetBeamFXComponent(int32 PoolIndex);

    /** Recolour Feyko's beam to this building's family colour. His P_Laser drives Emissive from
     *  MaterialExpressionParticleColor, which in a particle system comes from ParticleModuleColor --
     *  we push our own colour through SetColorParameter so the laser always matches the building
     *  family (the Laser Tower's red) instead of whatever was baked into the asset. */
    void ApplyBeamFXColor(class UParticleSystemComponent* Comp);

    /** Dynamic material instances tinting CageISM / BeamISM with this building's cage colour
     *  (WirelessPowerCageColor). Both come from /WirelessPower/Shapes/M_WirelessBeam, an unlit
     *  material whose "Color" vector parameter drives Emissive. */
    UPROPERTY()
    TObjectPtr<UMaterialInstanceDynamic> CageMID = nullptr;

    UPROPERTY()
    TObjectPtr<UMaterialInstanceDynamic> BeamMID = nullptr;

    /** Lazily load the shared unlit beam material, create the two MIDs and assign them. Safe to call
     *  repeatedly; degrades gracefully (grey vanilla material) if the asset is missing. */
    void EnsureVisualMaterials();

    /** World-space point the laser beam leaves/enters a tower from: the top of the building's model,
     *  so a beam is never buried inside the mesh. Also used as the line-of-sight trace endpoint. */
    FVector GetBeamPoint() const;

    /** v1.0.28: keep the crosshair/interaction highlight on the BUILDING MESH only.
     *
     *  Super::StartIsLookedAt*() calls UFGOutlineComponent::ShowOutline(this, colour), which
     *  enumerates every mesh component of the actor -- the coverage cage (CageISM) and the laser
     *  beams (BeamISM) included. That is why the cage turned into the outline colour while the
     *  player aimed at the building. This helper (a) forces the visualisation geometry out of the
     *  custom-depth pass the outline is drawn from, and (b) narrows the actor-wide outline state to
     *  MeshComp (the "MainMesh" the build gun previews) via ShowOutlineForStaticMeshComponent, which
     *  is the SDK's API for exactly this. It is a no-op when Super decided not to outline at all
     *  (dismantled building, photo mode), so those semantics are preserved verbatim. */
    void NarrowOutlineToMainMesh(class AFGCharacterPlayer* byCharacter, EOutlineColor Color);

    /** v1.0.28: undo NarrowOutlineToMainMesh(). ShowOutlineForStaticMeshComponent / HideOutline keep
     *  SEPARATE state stores (FActorOutlineState is cached per actor in two arrays, one for whole-actor
     *  outlines and one for component-specified ones), so the vanilla HideOutline(this) that the base
     *  StopIsLookedAt* calls is not guaranteed to clear ours. Without this the building mesh could keep
     *  its custom-depth stencil and stay permanently highlighted after the player looks away. */
    void ClearNarrowedOutline(class AFGCharacterPlayer* byCharacter);

    /** Local offset applied to the visual mesh (cm). Lets subclasses compensate for a source
     *  mesh whose pivot is not at the placement origin (e.g. some Area Power Pole meshes). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WirelessPower")
    FVector MeshLocalOffset = FVector::ZeroVector;

    /** True for Laser Tower: links require line-of-sight (no terrain blocking). */
    bool bNeedsLineOfSight = false;

    /** True ONLY for Area Power Pole: this node broadcasts power to in-range vanilla machines. */
    bool bBroadcastsToMachines = false;

    /** Laser Tower: the panel's "激光的可见性" switch. Replicated. SaveGame so the switch state
     *  survives a world reload. */
    UPROPERTY(SaveGame, ReplicatedUsing = OnRep_LaserVisible)
    bool bLaserVisible = true;

    /** Laser Tower: the towers this one is currently linked to (auto + manual). Replicated so
     *  clients can draw the laser beams without needing the server-only hidden connections. */
    UPROPERTY(ReplicatedUsing = OnRep_LaserPeers)
    TArray<TObjectPtr<AWirelessPowerBuildable>> LaserPeers;

    /** Laser Tower: peer 编号 (ID) the player manually connected from the panel's 开关连接 button.
     *  REPLICATED so the panel can label each dropdown entry 已连/未连 on every client; the actual
     *  hidden links are rebuilt from this list on the server. SaveGame is what keeps the manual
     *  tower-to-tower links across a world reload (without it the list reset to empty).
     *  v1.0.52: strings (was TArray<int32>) -- 编号 is the peer's normalised 编号 text. */
    UPROPERTY(SaveGame, Replicated)
    TArray<FString> ManualLinkChannels;

    /** True only while this building's interaction panel is open: the range cage is drawn then.
     *  Drawing ON DEMAND (not every tick) avoids the Shipping-build exit stall that a
     *  permanently-updating visual component on every wireless actor caused (v1.0.14 bisect). */
    bool bShowCage = false;

    /** Cache of what the cage currently on screen was built from. The instances are persistent
     *  geometry, so we rebuild only when the range/centre really changed instead of
     *  re-emitting the whole dense lattice every frame. */
    bool bCageBuilt = false;
    float CageBuiltRange = -1.f;
    FVector CageBuiltCenter = FVector::ZeroVector;

    /** Laser Tower: draw the beams between this tower and every linked tower. v1.0.32 uses Feyko's
     *  original P_Laser particle beams (BeamFX) whenever that asset is available, and falls back to
     *  the stretched-cube bar field (BeamISM) when it is not. Refreshed at ~2 Hz while the
     *  "激光的可见性" switch is on so a destroyed/re-IDed peer can never leave a stale beam. */
    void DrawLaserBeams();

    /** Fallback beam renderer: one stretched cube instance per linked tower (the pre-v1.0.32 look).
     *  Only used when Feyko's P_Laser cannot be loaded, so a link is never invisible. */
    void DrawLaserBeamBars();

    /** Laser Tower: refresh LaserPeers (the replicated peer list used for beams + the panel's
     *  "已连" marker) from the current hidden-link set. Server-side, end of ValidateWirelessMesh. */
    void RefreshLaserPeers();

    /** Seconds since the last beam refresh (beams are redrawn on a cadence, not every frame). */
    float BeamRefreshAccum = 1.f;

    /** Set at EndPlay so no cage drawing runs during world teardown (belt-and-suspenders vs the
     *  bShowCage gate, which the panel clears on close). */
    bool bEndingPlay = false;

    /** Kind of building (set in subclass ctor before calling InitWireless). */
    EWirelessBuildingType BuildingType = EWirelessBuildingType::Relay;

    /** 1, 2 or 3. Set in subclass ctor. */
    int32 Tier = 1;

    /** Max hidden PEER links per node (machines are tracked separately and not capped here).
     *
     *  v1.0.58: WHICH peers survive the cap is now decided by DISTANCE, nearest first -- the same
     *  rule Feyko's InitGameWorld used to pick poles (GetDistanceTo + Less_FloatFloat). Until now
     *  this was "the first MaxLinks entries in registry order", i.e. registration-history order,
     *  so a busy pole kept an essentially arbitrary peer set that could flip whenever an unrelated
     *  node registered. Laser Tower MANUAL links are explicit player intent and never compete for
     *  this cap. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "WirelessPower")
    int32 MaxLinks = 16;

    /** Call from subclass ctor: loads the building's mesh, applies tier scale + range, and creates
     *  NumVisiblePorts visible wire ports (each with its own power info).
     *
     *  IDEMPOTENT BY CONTRACT (v1.0.55). A C++ ctor chain runs base-then-derived, so a subclass that
     *  also calls this (APowerConverterMk2 : APowerConverter) would otherwise ask
     *  CreateDefaultSubobject for "PowerConnection0" a second time -- which is a fatal error:
     *      Fatal error: Default subobject WirelessPowerConnection PowerConnection0
     *      already exists for PowerConverterMk2
     *  The old ports were already built by the base ctor call, so we tear them down first and let
     *  this call re-create them from scratch with its own tier settings. */
    void InitWireless(EWirelessBuildingType Type, int32 InTier, float Range, bool bLOS,
                      int32 NumVisiblePorts, bool bBroadcastMachines);

    /** Drop the visible ports + their power infos created by an earlier InitWireless call in this
     *  same construction chain, so re-running InitWireless cannot collide on subobject names.
     *  Constructor-scope only: components are unregistered and their UPROPERTY refs cleared, which
     *  is all that is needed (and allowed) while the CDO is still being built. */
    void ResetVisiblePortsForRebuild();

    /** Rebuild the hidden wireless mesh of same-channel, in-range peers, and (for Area Power Pole)
     *  the hidden links to in-range vanilla machines. */
    void ValidateWirelessMesh();

    /** Fuse each visible port's circuit with the antenna's circuit via a hidden internal link.
     *  Idempotent + self-healing; deferred past circuit subsystem init. No-op for portless nodes. */
    void EnsureInternalBridge();

    /** Tear down every hidden link this building holds (peers + machines, but NOT the internal
     *  visible<->antenna bridge) and tell peers to drop us, so no dangling pointers survive. */
    void TearDownWirelessLinks(bool bNotifyPeers);

    /** Make this building interactable (E opens the control panel). mIsUseable and
     *  mInteractWidgetSoftClass are private in AFGBuildable, so write them via reflection. */
    void MakeUseableWithPanel();

    /** (Re)build the CUBIC coverage volume as a THIN-LINE lattice of cube instances (CageISM).
     *  Human-scale families (converter / area pole / relay) follow the user's spec: thin 2.5 cm bars
     *  on a 1 m pitch ("0.5改成1吧"), so the cube reads as a regular grid rather than a solid mesh.
     *  Pitch and thickness are the two named constants at the top of RebuildRangeCage() -- keep the
     *  thickness absolute, never a fraction of the pitch, or a pitch change becomes invisible.
     *  The laser tower is excluded outright (v1.0.27) -- a 4.8 km cube is pure visual noise and its
     *  panel no longer even shows the range row.
     *  Real geometry, so it renders in a Shipping game build (DrawDebug* is compiled out there, and
     *  batched debug lines turned out not to render either -- see CageISM). Instances are submitted
     *  in ONE AddInstances() batch (individual AddInstance() calls are far too slow at this count)
     *  and rebuilt only when the range/centre really changed. */
    void RebuildRangeCage();

    /** True if Other's actor location is within the SPHERE of radius WirelessRange around us.
     *  v1.0.52: sphere (was cube) -- restored to Feyko's original RangeSphere. */
    bool IsWithinRange(const AActor* Other) const;

    /** Line-of-sight test for Laser Tower links (ignores the two endpoints). */
    bool HasLineOfSightTo(const AActor* Other) const;

    /** True if this node currently has power: any visible port HasPower(), or (portless) the
     *  antenna HasPower() (i.e. the wireless mesh is powered). */
    bool HasWiredPower() const;

    /** Gather in-range vanilla machines' power-plug components into OutMachineConns (called only
     *  when bBroadcastsToMachines). Skips already-wirelessly-linked machines to avoid duplicate
     *  cross-pole links. */
    void CollectMachinesInRange(TArray<UFGPowerConnectionComponent*>& OutMachineConns) const;

    /** The link set we last reconciled into the hidden-connection graph (peers + machines).
     *
     *  It doubles as the "already applied" cache that ValidateWirelessMesh() compares the freshly
     *  computed desired set against: when the two match and the periodic re-check is not due, the
     *  pass makes ZERO engine calls. That is the whole point of the field.
     *
     *  Why it matters: the engine's AddHiddenConnection() ALWAYS re-merges the two circuits as its
     *  last action ("Connects both ends of the connection and the circuits", and the shipping
     *  binary confirms the circuit-connect routine is tail-called unconditionally, right after the
     *  two AddUnique() calls). Re-issuing Add for a link that already exists therefore tears the
     *  wireless grid's power down for a beat. The pre-v1.0.34 code did exactly that for every peer
     *  on every validation pass, which is why the game log shows bActive flipping once per pass
     *  (664x 0->1 / 648x 1->0) and why the user sees 供电不稳定. */
    UPROPERTY()
    TArray<TObjectPtr<UFGCircuitConnectionComponent>> MyLinks;

    /** False until a full reconcile has completed since the last teardown. While false, every
     *  pass reconciles (covers the first pass after load). */
    bool bMeshReconciled = false;

    /** World time of the last full reconcile. Even with a perfectly stable mesh we re-verify every
     *  FULL_RECONCILE_PERIOD seconds, so a link the engine dropped behind our back is still
     *  repaired -- just once per period instead of once per pass. */
    float LastReconcileTime = -1000.f;
    static constexpr float FULL_RECONCILE_PERIOD = 15.f;

    /** One entry per visible port: 1 once that port's internal bridge to the antenna is confirmed
     *  fused. Stops EnsureInternalBridge() from calling into the circuit subsystem on every tick
     *  (which it used to do, since the fused state was never cached). Cleared on each slow
     *  reconcile so the bridges get re-verified periodically. */
    TArray<uint8> PortBridged;

    /** World time of the last internal-bridge attempt. Throttles the pre-fusion retry loop so an
     *  attempt that cannot succeed yet is retried at most twice a second, not every tick. */
    float LastBridgeAttemptTime = -1000.f;
    static constexpr float BRIDGE_RETRY_PERIOD = 0.5f;

    /** True if Other is present in Comp's hidden-connection array.
     *
     *  Ground-truth oracle. The engine's own HasHiddenConnection() reads precisely this array
     *  (mHiddenConnections, live at +0x2a8 / Num +0x2b0 / mCircuitID +0x2c8 in the shipping
     *  FactoryGame DLL -- the same offsets this SDK's header layout produces), so going through
     *  the header-inline GetHiddenConnections() is both equivalent and free of the UFUNCTION thunk.
     *  v1.0.34 uses it in place of HasHiddenConnection() everywhere. */
    static bool HasHiddenLink(const UFGCircuitConnectionComponent* Comp,
                              const UFGCircuitConnectionComponent* Other);

    /** True if C is one of our visible ports (i.e. an internal bridge that must survive POWER DOWN
     *  and must never be treated as a wireless peer link). */
    bool IsInternalBridge(const UFGCircuitConnectionComponent* C) const;

    int32 ValidateCounter = 0;
    static constexpr int32 VALIDATE_INTERVAL = 180; // ~3s @ 60fps

    // ---- v1.0.58: 落地即自动入网 ----

    /** World time at which an early validation is due, or <0 when none is pending.
     *
     *  Set by ScheduleEarlyValidate() to `now + 0.5`, mirroring the `Delay (Duration = 0.500000)`
     *  node in Feyko's InitGameWorld: the circuit subsystem hands a fresh connection its private
     *  circuit a little AFTER actor BeginPlay, and AddHiddenConnection() silently no-ops before
     *  that. A half-second head start means a just-placed pole joins the grid immediately instead
     *  of waiting up to VALIDATE_INTERVAL (~3 s). */
    float EarlyValidateAt = -1.f;

    /** True while this node still wants to adopt a neighbour's 编号. Cleared once the adoption has
     *  run, so it happens at most once per actor. Evaluated at the EarlyValidateAt deadline rather
     *  than in BeginPlay so that on a savegame load ALL nodes are registered by then (load order is
     *  not spatial, so an in-BeginPlay scan could see an incomplete neighbourhood). */
    bool bNeedsChannelAdopt = false;

    /** Ask for a validation ~0.5 s from now (see EarlyValidateAt). Idempotent: an already-pending
     *  request is never pushed back, so a burst of constructed buildables cannot starve it. */
    void ScheduleEarlyValidate();

    /** Server-only, one-shot: if nobody has chosen this node's 编号, inherit the nearest in-range
     *  node's (same family preferred, any family accepted -- Feyko's relay validated against poles,
     *  converters AND relays). Feyko parity: `setIDAndValidateConnections` set the id automatically;
     *  the player only ever opened the panel to view/tweak. */
    void AutoAdoptNearestPeerChannel();

    // ---- v1.0.58: repair backoff ----
    /** Per-link count of Remove+Add "repair" attempts made by the slow reconcile.
     *
     *  Why: the engine's AddHiddenConnection() ALWAYS finishes by re-merging the two circuits, so a
     *  Remove+Add of a live link tears the wireless grid's power down for a beat. A link that
     *  exists structurally but whose circuits never merge (a restored SaveGame hidden connection can
     *  sit on its own circuit forever) therefore used to be repaired on EVERY 15 s slow pass --
     *  permanently flickering the whole network's power every 15 s ("周期性掉电", bug #2). After
     *  MAX_REPAIR_TRIES the link is left strictly alone: it is structurally present, and the engine
     *  picks it up by itself the next time either circuit really changes. */
    TMap<TWeakObjectPtr<UFGCircuitConnectionComponent>, int32> RepairTries;
    static constexpr int32 MAX_REPAIR_TRIES = 3;

    /** Feyko's isClassBanned(): a denylist of buildables that must never be auto-wired into the
     *  wireless mesh. He banned the two vanilla WALL power poles (see the original's
     *  `isClassBanned` / the `Build_PowerPoleWall*` references in Build_AreaPowerPole.uasset).
     *  Keyed on the class NAME so it works without an SDK header for every candidate. */
    static bool IsClassBannedForWireless(const AActor* Actor);

    bool bBridgeLogged = false;
};
