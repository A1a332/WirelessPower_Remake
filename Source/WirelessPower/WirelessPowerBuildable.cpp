#include "WirelessPowerBuildable.h"
#include "FGBuildableSubsystem.h"
#include "WirelessPowerAssets.h"
#include "WirelessPowerPanelWidget.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "CollisionQueryParams.h"
#include "DrawDebugHelpers.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"
#include "Net/UnrealNetwork.h"
#include "UObject/UnrealType.h"
// v1.0.57 (bug #3): 记住"上次用过的编号"要落盘（GameUserSettings.ini）。FConfigCacheIni 提供
// GConfig->GetString/SetString/Flush；GConfig 与 GGameUserSettingsIni 本身在 CoreGlobals.h（CoreMinimal 已带）。
#include "Misc/ConfigCacheIni.h"
// v1.0.28: interaction-outline control. FGOutlineComponent.h gives us the component-scoped
// ShowOutlineForStaticMeshComponent + GetImmutableOutlineStateForActor; FGCharacterPlayer.h gives us
// AFGCharacterPlayer::GetOutline().
#include "FGOutlineComponent.h"
#include "FGCharacterPlayer.h"
// v1.0.32: Feyko's original laser beams are a Cascade Beam2 particle system, not geometry.
// ParticleSystemComponent.h is needed for SetBeamSourcePoint/SetBeamTargetPoint; the module headers
// are needed to read back (and if necessary re-assert) the beam emitter's source/target method.
#include "Particles/ParticleSystemComponent.h"
#include "Particles/TypeData/ParticleModuleTypeDataBeam2.h"
#include "Particles/Beam/ParticleModuleBeamSource.h"
#include "Particles/Beam/ParticleModuleBeamTarget.h"
#include "Particles/Beam/ParticleModuleBeamNoise.h"
#include "Particles/Color/ParticleModuleColor.h"
#include "Particles/Size/ParticleModuleSize.h"
#include "Particles/ParticleLODLevel.h"
#include "Particles/ParticleEmitter.h"
#include "Particles/ParticleModuleRequired.h"
#include "Distributions/DistributionVector.h"
#include "Distributions/DistributionVectorConstant.h"
#include "Distributions/DistributionFloat.h"

// v1.0.20: diagnostics MUST NOT use LogTemp. Measured fact (2026-10-01): this game's Shipping
// build filters LogTemp at Log level -- every UE_LOG(LogTemp, Log, ...) is silently dropped, so
// the [WirelessPower] runtime lines were invisible in every session log. A file-local category
// declared with DEFINE_LOG_CATEGORY_STATIC (same pattern as Registration.cpp) IS printed.
// (A static category is per-TU, so the same name in several source files does not clash.)
DEFINE_LOG_CATEGORY_STATIC(LogWirelessPower, Log, All);

#include "WirelessPowerLog.h"

// ----------------------------------------------------------------------------
// v1.0.44 SCALE FIX -- the shared wireless-node registry.
//
// User report: "无线电杆一旦多起来后就不稳定" (the grid gets unstable once you have many poles).
//
// Measured cause: ValidateWirelessMesh() walked AFGBuildableSubsystem::GetAllBuildablesRef() --
// EVERY buildable in the world -- once per node per validation pass (~3 s) just to find
// same-channel peers, and the Area Power Pole path repeated that walk inside the same loop while
// calling GetComponents<UFGPowerConnectionComponent>() on every buildable it saw. With a real base
// (hundreds-to-thousands of buildables) and more than a handful of wireless nodes that is
// O(nodes x buildables) work per pass, all on the game thread, and every pass that found a change
// re-merged circuits through AddHiddenConnection().
//
// The fix keeps one process-wide list of the wireless nodes (a few dozen at most) and iterates
// THAT instead of the world. Weak pointers, so a dismantled node can never dangle. Registration
// happens in BeginPlay/EndPlay; the list is also compacted lazily on every read.
//
// Note on threading: all of this runs on the game thread (BeginPlay/EndPlay/Tick), so a plain
// static TArray is safe here -- no locking needed, and none is added (a lock would just cost).
// ----------------------------------------------------------------------------
namespace
{
    TArray<TWeakObjectPtr<AWirelessPowerBuildable>> GWirelessNodes;

    /** Shared cache for the machine walk (Area Power Pole broadcast), so several poles validating in
     *  the same slice share ONE buildable scan instead of each doing their own. */
    struct FWPMachineCache
    {
        float LastScanTime = -1000.f;
        UWorld* CachedWorld = nullptr;
        /** One entry per candidate power port on a vanilla machine (weak: a dismantled machine drops out). */
        struct FMachineEntry
        {
            TWeakObjectPtr<UFGPowerConnectionComponent> Conn;
            /** Nearest *active, machine-broadcasting* wireless pole whose (symmetric) cube covers this
             *  machine. Recomputed every cache refresh. Channel-independent: a machine is owned by the
             *  geometrically nearest pole regardless of 编号 (machines never carry a channel). This is
             *  what makes "a machine sitting in the overlap of two poles joins the closer one"
             *  deterministic instead of first-come-first-served. */
            TWeakObjectPtr<AWirelessPowerBuildable> NearestPole;
        };
        TArray<FMachineEntry> Entries;
    };
    FWPMachineCache GMachineCache;

    /** Machine walk is expensive, so at most one every this many seconds per world. */
    constexpr float WP_MACHINE_SCAN_PERIOD = 2.0f;

    /** CUBIC range test, factored out so the peer scan, the machine cache and the cache's
     *  per-pole filter all use the exact same rule (the cache works on raw locations rather than
     *  on an AActor, so it cannot call IsWithinRange()).
     *
     *  判据：|dx| <= Range && |dy| <= Range && |dz| <= Range —— 以建筑为中心的正方体，
     *  Range 是**半边长**（"前后左右上下各延伸 Range"）。对角线可达 1.73 x Range。
     *
     *  v1.0.53：由球体**改回立方体**（用户决定）。v1.0.52 曾按"完全还原 Feyko"改成球体
     *  （Feyko 资产里确有 RangeSphere/SphereComponent），但用户实测后要求保留立方体手感：
     *  立方体在斜角处能多连一点，建造时更好对位。**这是用户的明确取舍，不是遗漏。** */
    struct FWPRangeHelper
    {
        static bool WithinCubic(const FVector& Centre, float Range, const FVector& Point)
        {
            const FVector D = (Point - Centre).GetAbs();
            return D.X <= Range && D.Y <= Range && D.Z <= Range;
        }
    };

    void CompactWirelessNodes()
    {
        GWirelessNodes.RemoveAll([](const TWeakObjectPtr<AWirelessPowerBuildable>& N)
        {
            return !N.IsValid();
        });
    }

    /** Resolved node list, rebuilt ONLY when the registry changes (Register/Unregister) so the
     *  thousands of BuildableConstructedGlobalDelegate firings during a savegame load each get a
     *  cheap array copy instead of a full RemoveAll+walk every single time. */
    TArray<AWirelessPowerBuildable*> GCachedNodes;
    bool bNodeCacheDirty = true;

    void RebuildNodeCache()
    {
        CompactWirelessNodes();
        GCachedNodes.Reset();
        GCachedNodes.Reserve(GWirelessNodes.Num());
        for (const TWeakObjectPtr<AWirelessPowerBuildable>& N : GWirelessNodes)
        {
            if (AWirelessPowerBuildable* P = N.Get())
            {
                GCachedNodes.Add(P);
            }
        }
        bNodeCacheDirty = false;
    }
}

void AWirelessPowerBuildable::RegisterWirelessNode(AWirelessPowerBuildable* Node)
{
    if (!Node)
    {
        return;
    }
    for (const TWeakObjectPtr<AWirelessPowerBuildable>& Existing : GWirelessNodes)
    {
        if (Existing.Get() == Node)
        {
            return; // already registered
        }
    }
    CompactWirelessNodes();
    GWirelessNodes.Add(Node);
    bNodeCacheDirty = true;
    WP_LOGF( TEXT("[WirelessPower] node registry: + %s (%s) -> %d node(s)"),
        *Node->GetClass()->GetName(),
        Node->GetWorld() ? *Node->GetWorld()->GetName() : TEXT("no-world"),
        GWirelessNodes.Num());
}

void AWirelessPowerBuildable::UnregisterWirelessNode(AWirelessPowerBuildable* Node)
{
    if (!Node)
    {
        return;
    }
    const int32 Removed = GWirelessNodes.RemoveAll(
        [Node](const TWeakObjectPtr<AWirelessPowerBuildable>& N)
        {
            return N.Get() == Node;
        });
    if (Removed > 0)
    {
        bNodeCacheDirty = true;
        WP_LOGF( TEXT("[WirelessPower] node registry: - %s -> %d node(s)"),
            *Node->GetClass()->GetName(), GWirelessNodes.Num());
    }
}

void AWirelessPowerBuildable::GetRegisteredNodes(TArray<AWirelessPowerBuildable*>& OutNodes)
{
    if (bNodeCacheDirty)
    {
        RebuildNodeCache();
    }
    OutNodes = GCachedNodes;
}

// ----------------------------------------------------------------------------
// v1.0.58 -- Feyko parity: "落地即自动入网".
//
// The original mod's `InitGameWorld` blueprint binds AFGBuildableSubsystem::
// BuildableConstructedGlobalDelegate ("Regardless of who built it"), and for every newly
// constructed buildable collects the Area Power Poles within range, waits `Delay (Duration =
// 0.500000)`, then calls pole.ValidateAreaConnections(). Its judgement is pure DISTANCE -- the
// player never types an id, and the visible effect is "每新铺的电线杆自动链接上一个电线杆".
//
// UWirelessPowerGameWorldModule forwards every constructed buildable to NotifyBuildableConstructed()
// below; each node decides locally whether it cares. Three pieces:
//   ScheduleEarlyValidate()          -- the 0.5 s beat
//   AutoAdoptNearestPeerChannel()    -- Feyko's `setIDAndValidateConnections` (id auto-assigned)
//   NotifyBuildableConstructed()     -- the entry point
// ----------------------------------------------------------------------------

void AWirelessPowerBuildable::ScheduleEarlyValidate()
{
    // The delay is functional, not cosmetic: the circuit subsystem hands a fresh connection its
    // private circuit a little AFTER actor BeginPlay, and AddHiddenConnection() silently no-ops
    // before that. Nothing here is time-critical, so Tick() merely notices the deadline instead of
    // standing up a timer -- no extra lifetime to manage, and it behaves identically in single
    // player and on a listen server.
    const UWorld* W = GetWorld();
    if (!W)
    {
        return;
    }
    if (EarlyValidateAt < 0.f) // never push an already-pending request further out
    {
        EarlyValidateAt = W->GetTimeSeconds() + 0.5f;
    }
}

void AWirelessPowerBuildable::NotifyBuildableConstructed(AFGBuildable* NewBuildable)
{
    // Power circuits are server state, and the global delegate can also fire on a client, where
    // there is nothing for us to do.
    if (!HasAuthority() || bEndingPlay || !IsValid(NewBuildable))
    {
        return;
    }

    if (NewBuildable == this)
    {
        // We were just placed (or just loaded): join the neighbourhood. The 编号 adoption happens at
        // the deadline, not here -- see the note on bNeedsChannelAdopt.
        ScheduleEarlyValidate();
        return;
    }

    // T07 fix (WP-T07): a freshly-built MACHINE inside our coverage must be visible to the very next
    // validation pass. The shared machine scan is cached for WP_MACHINE_SCAN_PERIOD seconds (see
    // CollectMachinesInRange), so a build event landing between scans would otherwise be invisible
    // until the cache expired -- the new machine would show "not powered" for up to ~2 s, easily
    // mistaken for intermittent coverage failure. Force the cache stale so the next pass re-scans.
    // This is a cheap, merged invalidation: batch-building N machines just re-stamps the same
    // timestamp, and the actual re-scan runs at most once per pass per world.
    const bool bNewMachine = !Cast<AWirelessPowerBuildable>(NewBuildable)
        && !IsClassBannedForWireless(NewBuildable);
    if (bNewMachine && IsValid(WirelessConnection) && IsWithinRange(NewBuildable))
    {
        GMachineCache.LastScanTime = -1000.f; // force re-scan on next validation
        ScheduleEarlyValidate();
        return;
    }

    // Somebody else's wireless buildable (peer node) landed in range: re-validate soon instead of
    // up to ~3 s later.
    if (IsValid(WirelessConnection) && IsWithinRange(NewBuildable))
    {
        ScheduleEarlyValidate();
    }
}

void AWirelessPowerBuildable::AutoAdoptNearestPeerChannel()
{
    if (!HasAuthority())
    {
        return;
    }

    // Never touch a node whose 编号 somebody actually chose. Two separate guards:
    //   * bChannelUserSet       -- the player typed one into the panel (SaveGame, so it survives).
    //   * a non-default value read back from a save -- a decision from an earlier session.
    // The ONE non-default value we may overwrite is the one the placement hologram just stamped on
    // from its client-side "last used 编号" memory: that is a convenience hint, and the
    // distance rule the user asked for outranks it. (That is also why the flag is transient: it is
    // false on anything that came out of a save, which is precisely the case we must not touch.)
    if (bChannelUserSet)
    {
        return;
    }
    const AWirelessPowerBuildable* Cdo = Cast<AWirelessPowerBuildable>(GetClass()->GetDefaultObject());
    const bool bAtClassDefault = Cdo && FWirelessId::Equal(Channel, Cdo->Channel);
    if (!bAtClassDefault && !bChannelFromHologram)
    {
        return; // carries a saved, deliberate 编号
    }

    TArray<AWirelessPowerBuildable*> Nodes;
    GetRegisteredNodes(Nodes);

    AWirelessPowerBuildable* Nearest = nullptr;
    int32 BestFamilyRank = 2; // 0 = same family (preferred), 1 = any other family
    float BestDistSq = TNumericLimits<float>::Max();
    const FVector MyLocation = GetActorLocation();

    for (AWirelessPowerBuildable* Other : Nodes)
    {
        if (!IsValid(Other) || Other == this)
        {
            continue;
        }
        // Symmetric reach, exactly like the peer scan: either cube covering the other counts, so a
        // new Mk1 placed near an Mk3 still adopts the Mk3's network (see the peer-scan comment).
        if (!Other->IsPoweredOn() || (!Other->IsWithinRange(this) && !IsWithinRange(Other)))
        {
            continue; // only a node that could actually form a link with us
        }
        // T04 fix (WP-T04): only adopt a channel from a node we could actually LINK to. Mirror the
        // peer-scan eligibility in ValidateWirelessMesh (the LOS gate at the link-formation step):
        // if EITHER side needs line of sight, the candidate must have it, otherwise we would inherit
        // an occluded tower's ID, fail the LOS check at link time, and leave the new node orphaned
        // while a clear-LOS, differently-ID'd tower was available. Area Power Poles / Relays have
        // bNeedsLineOfSight == false, so this never blocks them.
        const bool bNeedsLosWithOther = bNeedsLineOfSight || Other->bNeedsLineOfSight;
        if (bNeedsLosWithOther && !HasLineOfSightTo(Other))
        {
            continue; // would fail the LOS gate at link time; pick a reachable candidate instead
        }
        // Prefer our own family (an Area Power Pole should join poles, not a stray laser tower),
        // but accept any family when nothing of our own kind is around -- Feyko's relay validated
        // against area poles, power converters AND relays.
        const int32 FamilyRank = (Other->GetBuildingType() == BuildingType) ? 0 : 1;
        const float DistSq = FVector::DistSquared(MyLocation, Other->GetActorLocation());
        if (FamilyRank < BestFamilyRank || (FamilyRank == BestFamilyRank && DistSq < BestDistSq))
        {
            BestFamilyRank = FamilyRank;
            BestDistSq = DistSq;
            Nearest = Other;
        }
    }

    if (!Nearest || FWirelessId::Equal(Channel, Nearest->Channel))
    {
        return;
    }

    Channel = Nearest->Channel;

    // Deliberately ALWAYS logged, not WP_LOGF: this is the line that proves "落地即自动入网"
    // actually happened, and it fires at most once per building -- exactly the kind of one-shot
    // evidence the mod keeps on by default (cf. the load banner in WirelessPowerModule.cpp).
    UE_LOG(LogWirelessPower, Log,
           TEXT("[WirelessPower] %s auto-joined 编号 '%s' (inherited from nearest %s)"),
           *GetClass()->GetName(), *Channel, *Nearest->GetClass()->GetName());
}

bool AWirelessPowerBuildable::IsClassBannedForWireless(const AActor* Actor)
{
    if (!Actor)
    {
        return true;
    }
    // Feyko's isClassBanned() denylist. His original banned the two vanilla WALL power poles --
    // Build_AreaPowerPole.uasset carries exactly those two class references (import[1] / import[2],
    // right next to the `isClassBanned` name), because they must never be auto-wired into the
    // wireless mesh. Matched by class NAME so no SDK header is needed for every candidate;
    // "Build_PowerPoleWall" is a prefix of "...WallDouble", so one Contains() covers both.
    // Extend this list if a buildable ever turns up that must not be auto-wired.
    const FString ClassName = Actor->GetClass()->GetName();
    return ClassName.Contains(TEXT("Build_PowerPoleWall"));
}

AWirelessPowerBuildable::AWirelessPowerBuildable()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = true;

    // The component MUST be named "MainMesh": AFGBuildableHologram copies the buildable's
    // visual mesh by this exact name for the build-gun preview. Any other name is invisible
    // to the placement pipeline.
    MeshComp = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MainMesh"));
    MeshComp->SetupAttachment(RootComponent);

    // The hidden ANTENNA. It owns its OWN power info (WirelessPowerInfo) -- the SDK forbids two
    // connections sharing one power info (v1.0.6 did exactly that and the merge never formed).
    // The antenna is the wireless mesh node: peer antennas and (Area Power Pole only) in-range
    // machines are fused into its circuit via AddHiddenConnection().
    WirelessPowerInfo = CreateDefaultSubobject<UFGPowerInfoComponent>(TEXT("WirelessPowerInfo"));
    WirelessConnection = CreateDefaultSubobject<UWirelessPowerConnection>(TEXT("WirelessConnection"));
    WirelessConnection->SetPowerInfo(WirelessPowerInfo);
    WirelessConnection->SetIsHidden(true);
    WirelessConnection->SetupAttachment(RootComponent);
    WirelessConnection->SetRelativeLocation(FVector(0.f, 0.f, 60.f));
    // mConnectionDistanceBias is ADDED to the measured distance, so a large value makes the
    // hidden antenna the LEAST attractive snap target for the wire tool.
    WirelessConnection->SetDistanceBias(1000000.f);
    // Generous headroom: internal bridges (per visible port) + up to MaxLinks peers + (pole only)
    // every in-range machine. 2048 is comfortably above any sane factory density.
    WirelessConnection->SetMaxNumConnectionLinks(2048);

    // ---- Real-geometry visualisation (v1.0.22) ----
    // The range cage and the laser beams used to be ULineBatchComponent lines. The in-game
    // diagnostic proved those lines were batched (cageLines=240) yet never appeared on screen: this
    // Shipping build does not render batched debug lines at all. Both features are therefore drawn
    // as thin instances of /Engine/BasicShapes/Cube -- a vanilla mesh that IS cooked into the game --
    // which go through the ordinary static-mesh path and are guaranteed to render.
    // Absolute-transform so world-aligned geometry is not skewed by the player's building rotation.
    static ConstructorHelpers::FObjectFinder<UStaticMesh> BarMeshFinder(TEXT("/Engine/BasicShapes/Cube.Cube"));
    UStaticMesh* BarMesh = BarMeshFinder.Succeeded() ? BarMeshFinder.Object : nullptr;

    auto SetupBarISM = [this, BarMesh](UInstancedStaticMeshComponent* ISM)
    {
        ISM->SetupAttachment(RootComponent);
        ISM->SetUsingAbsoluteLocation(true);
        ISM->SetUsingAbsoluteRotation(true);
        ISM->SetUsingAbsoluteScale(true);
        ISM->SetMobility(EComponentMobility::Movable);
        ISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        ISM->SetGenerateOverlapEvents(false);
        ISM->SetCastShadow(false);
        ISM->SetCanEverAffectNavigation(false);
        if (BarMesh)
        {
            ISM->SetStaticMesh(BarMesh);
        }
        ISM->SetVisibility(false); // hidden until the range switch / laser link asks for it
    };

    CageISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("CageISM"));
    SetupBarISM(CageISM);

    BeamISM = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("BeamISM"));
    SetupBarISM(BeamISM);

    // v1.0.32: Feyko's own beam renderer. One UParticleSystemComponent playing his P_Laser; created
    // as a default subobject so it exists on clients as well as the server (the beams are pure
    // visuals, but every machine ticks DrawLaserBeams). The template is applied at RUNTIME, not here:
    // a mod asset cannot be resolved from a constructor, and a ConstructorHelpers::FObjectFinder on
    // mod content silently yields null. Extra components (for towers with more links than one P_Laser
    // can carry) are created lazily in GetBeamFXComponent().
    {
        UParticleSystemComponent* FirstFX = CreateDefaultSubobject<UParticleSystemComponent>(TEXT("BeamFX0"));
        FirstFX->SetupAttachment(RootComponent);
        FirstFX->SetMobility(EComponentMobility::Movable);
        FirstFX->bAutoActivate = false;      // stays dormant until a link actually exists
        FirstFX->SetCastShadow(false);
        FirstFX->SetGenerateOverlapEvents(false);
        FirstFX->SetCanEverAffectNavigation(false);
        FirstFX->SetVisibility(false);
        // A beam reaches a tower hundreds of metres away, far outside this actor's own bounds. Without
        // a large bounds scale the component's proxy is culled as soon as the TOWER leaves the view,
        // and the beam would pop out of existence while it is still on screen.
        FirstFX->SetBoundsScale(2000.f);
        BeamFX.Add(FirstFX);
    }

    // Visible wire ports are created in InitWireless() (called from the subclass ctor) because the
    // count differs per building (Laser Tower 4, Converter 1, Relay/Area Pole 0).

    MakeUseableWithPanel();
}

void AWirelessPowerBuildable::MakeUseableWithPanel()
{
    // mIsUseable and mInteractWidgetSoftClass are private in AFGBuildable, so set them via
    // reflection. This is the documented pair the game checks to open the interact UI on E.
    if (FBoolProperty* UseableProp = FindFProperty<FBoolProperty>(AFGBuildable::StaticClass(), TEXT("mIsUseable")))
    {
        UseableProp->SetPropertyValue(UseableProp->ContainerPtrToValuePtr<void>(this), true);
    }
    if (FSoftClassProperty* WidgetProp = FindFProperty<FSoftClassProperty>(AFGBuildable::StaticClass(), TEXT("mInteractWidgetSoftClass")))
    {
        if (UClass* PanelClass = UWirelessPowerPanelWidget::StaticClass())
        {
            const FString Path = PanelClass->GetPathName();
            WidgetProp->ImportText_Direct(*Path, WidgetProp->ContainerPtrToValuePtr<void>(this), this, 0, nullptr);
        }
    }
}

void AWirelessPowerBuildable::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(AWirelessPowerBuildable, Channel);
    DOREPLIFETIME(AWirelessPowerBuildable, WirelessRange);
    DOREPLIFETIME(AWirelessPowerBuildable, bPowerEnabled);
    DOREPLIFETIME(AWirelessPowerBuildable, bActive);
    DOREPLIFETIME(AWirelessPowerBuildable, LinkedNodeCount);
    DOREPLIFETIME(AWirelessPowerBuildable, LaserPeers);
    DOREPLIFETIME(AWirelessPowerBuildable, bLaserVisible);
    DOREPLIFETIME(AWirelessPowerBuildable, ManualLinkChannels);
    DOREPLIFETIME(AWirelessPowerBuildable, PoweredMachineCount);
}

// ---------------------------------------------------------------------------------------------
// v1.0.57 (bug #3): "上次设置过的编号" 记忆
// ---------------------------------------------------------------------------------------------
// 用户报告：每放一个无线电杆/中继器，面板里的编号都会回到默认值，必须重新去下拉里找一次继电器
// 的编号 —— 布置一整片电网要重复几十次。
// 修法：玩家每次设置成功就记住这个编号，新放置的同类建筑自动继承。
// 为什么用固定 TCHAR 缓冲而不是 static FString：FString 是带堆分配的对象，进程退出时它的析构
// 顺序不可控（mod 的 DLL 可能在引擎已经拆掉内存分配器之后才被卸载），静态 FString 有在退出阶段
// 崩的风险。零初始化的字符数组没有析构，最稳。它只是个缓存，权威值永远在建筑的 Channel 上。
namespace
{
    // 比 FWirelessId::Validate 的 MaxLen(24) 留出余量，末尾还有 NUL。
    constexpr int32 kLastUsedChannelCap = 40;

    TCHAR GLastUsedChannelBuf[kLastUsedChannelCap] = { 0 };
    bool  GLastUsedChannelLoaded = false; // 是否已经从 ini 读过一次（懒加载）

    const TCHAR* const kLastUsedChannelSection = TEXT("WirelessPowerClient");
    const TCHAR* const kLastUsedChannelKey     = TEXT("LastUsedChannel");

    void EnsureLastUsedChannelLoaded()
    {
        if (GLastUsedChannelLoaded)
        {
            return;
        }
        GLastUsedChannelLoaded = true; // 先置位：读失败也只在启动时读一次，不会每次调用都碰磁盘
        if (GConfig == nullptr)
        {
            return;
        }
        FString Stored;
        if (!GConfig->GetString(kLastUsedChannelSection, kLastUsedChannelKey, Stored,
                                GGameUserSettingsIni))
        {
            return;
        }
        // 走一遍规范化+校验，脏数据（超长/纯空白）一律丢弃，不污染缓存。
        FString Norm;
        if (FWirelessId::Validate(Stored, Norm, kLastUsedChannelCap - 1))
        {
            FCString::Strncpy(GLastUsedChannelBuf, *Norm, kLastUsedChannelCap);
        }
    }
}

void AWirelessPowerBuildable::RecordLastUsedChannel(const FString& InChannel)
{
    FString Norm;
    if (!FWirelessId::Validate(InChannel, Norm, kLastUsedChannelCap - 1))
    {
        return; // 空 / 纯空白：不记（"没有记忆"才让新建筑保持出厂默认）
    }

    // 值没变就别写盘（面板刷新路径可能反复调到）。
    if (FCString::Strlen(GLastUsedChannelBuf) > 0
        && FCString::Stricmp(GLastUsedChannelBuf, *Norm) == 0)
    {
        GLastUsedChannelLoaded = true;
        return;
    }

    FCString::Strncpy(GLastUsedChannelBuf, *Norm, kLastUsedChannelCap);
    GLastUsedChannelLoaded = true;

    if (GConfig != nullptr)
    {
        GConfig->SetString(kLastUsedChannelSection, kLastUsedChannelKey, *Norm,
                           GGameUserSettingsIni);
        GConfig->Flush(false, GGameUserSettingsIni);
    }

    UE_LOG(LogWirelessPower, Log,
           TEXT("[WirelessPower] remembered 编号 '%s' -- the next AreaPole/Relay placed will inherit it"),
           *Norm);
}

FString AWirelessPowerBuildable::GetLastUsedChannel()
{
    EnsureLastUsedChannelLoaded();
    return FString(GLastUsedChannelBuf);
}

void AWirelessPowerBuildable::ApplyChannel(const FString& NewChannel)
{
    // v1.0.52: 编号是字符串（还原 Feyko 原版；汉字合法）。存规范化后的值，比较一律用
    // FWirelessId::Equal，这样 " 一号 " / "１号" / "一号" 视为同一编号。
    FString Norm;
    FWirelessId::Validate(NewChannel, Norm); // 纯空白输入保留为空串（= 默认网络）
    Channel = Norm;
    // v1.0.58: 玩家（或面板）亲自选过编号了。AutoAdoptNearestPeerChannel 从此不再插手这个节点，
    // 否则"玩家故意把一台设备留在 1 号网、紧邻 2 号网"的布置会在每次读档时被悄悄改掉。
    // 这个标记进存档（SaveGame），所以老存档读进来是 false、且 Channel 若已非默认值本就不会被改。
    bChannelUserSet = true;
    // v1.0.57 (bug #3) + WP-T06 fix: the LOCAL panel already records the channel
    // (WirelessPowerPanelWidget.cpp, OnSetChannelClicked) so the player's own next placement inherits
    // it. We deliberately do NOT record here on the authority path: ApplyChannel runs on the server,
    // and "last used channel" is meant to be a PER-PLAYER local hologram memory. On a listen server the
    // server process is also the host's client, so a remote player's Server_SetChannel would otherwise
    // overwrite the host's shared GLastUsedChannelBuf / GameUserSettings.ini with the remote player's
    // ID -- the exact cross-player contamination reported in WP-T06. Removing this keeps each player's
    // "last used" memory in their own client process (a dedicated server has no hologram to feed anyway).
    if (HasAuthority())
    {
        ValidateWirelessMesh();
    }
}

void AWirelessPowerBuildable::ApplyRange(float NewRange)
{
    // v1.0.36: the CEILING is locked per tier -- the range a building gets from its ctor is the
    // most it can ever have, and the panel may only LOWER it. Read off the class default object so
    // the ctor stays the single source of truth (no duplicated table that could drift).
    // Was: a flat 0..500000 clamp (v1.0.19), which let an Mk1 Area Power Pole (default 12 m) be
    // cranked to 5000 m, making the Mk1/Mk2/Mk3 tiers meaningless.
    const AWirelessPowerBuildable* Cdo = Cast<AWirelessPowerBuildable>(GetClass()->GetDefaultObject());
    const float Ceiling = Cdo ? Cdo->WirelessRange : 500000.f;
    WirelessRange = FMath::Clamp(NewRange, 0.f, Ceiling);
    if (HasAuthority())
    {
        ValidateWirelessMesh();
    }
}

void AWirelessPowerBuildable::ApplyPowerEnabled(bool bEnabled)
{
    bPowerEnabled = bEnabled;
    if (HasAuthority())
    {
        if (!bPowerEnabled)
        {
            // POWER DOWN: isolate this node from the wireless mesh. Downstream machines
            // connected through us lose wireless power until POWER UP is pressed.
            TearDownWirelessLinks(true);
        }
        else
        {
            ValidateWirelessMesh();
        }
    }
}

void AWirelessPowerBuildable::OnRep_Channel()
{
    WP_LOGF( TEXT("[WirelessPower] %s channel replicated: %s"), *GetClass()->GetName(), *Channel);
}

void AWirelessPowerBuildable::OnRep_Range()
{
    WP_LOGF( TEXT("[WirelessPower] %s range replicated: %.0f"), *GetClass()->GetName(), WirelessRange);
}

void AWirelessPowerBuildable::OnRep_PowerEnabled()
{
    WP_LOGF( TEXT("[WirelessPower] %s power-enabled replicated: %d"), *GetClass()->GetName(), bPowerEnabled ? 1 : 0);
}

void AWirelessPowerBuildable::ResetVisiblePortsForRebuild()
{
    // Only ever called from a constructor (base ctor's InitWireless, then a derived ctor's).
    // CreateDefaultSubobject refuses a duplicate name, so the previously built ports must go.
    //
    // Note on GC: we are still inside the object's construction, so the old components have never
    // been exposed to the GC and are not rooted. Unregistering them is enough; the engine cleans
    // up any subobject that ends up unreferenced once construction finishes. The UPROPERTY arrays
    // are cleared last so nothing holds a stale pointer.
    //
    // DetachFromComponent lives on USceneComponent, NOT on UActorComponent (the base has no
    // transform to detach from). Calling it through a UActorComponent* is a hard compile error
    // (C2039), so detach only when the component is actually a scene component.
    auto DropOne = [](UActorComponent* Comp)
    {
        if (!Comp)
        {
            return;
        }
        if (USceneComponent* Scene = Cast<USceneComponent>(Comp))
        {
            Scene->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
        }
        Comp->UnregisterComponent();
    };

    for (TObjectPtr<UFGPowerInfoComponent>& Info : PowerInfos)
    {
        DropOne(Info.Get());
    }
    PowerInfos.Reset();

    for (TObjectPtr<UWirelessPowerConnection>& Port : PowerConnections)
    {
        // The port owns its power info (passed in via SetPowerInfo). Drop the port itself; the
        // info was already handled above.
        DropOne(Port.Get());
    }
    PowerConnections.Reset();
}

void AWirelessPowerBuildable::InitWireless(EWirelessBuildingType Type, int32 InTier, float Range, bool bLOS,
                                          int32 NumVisiblePorts, bool bBroadcastMachines)
{
    // Idempotent: a derived ctor calling this a second time (converter Mk2/Mk3) must not collide
    // with the ports the base ctor already created under the same fixed names.
    if (PowerConnections.Num() > 0 || PowerInfos.Num() > 0)
    {
        ResetVisiblePortsForRebuild();
    }

    BuildingType = Type;
    Tier = InTier;
    WirelessRange = Range;
    bNeedsLineOfSight = bLOS;
    bBroadcastsToMachines = bBroadcastMachines;

    // Create the visible wire ports. Each port is a distinct UWirelessPowerConnection that owns its
    // OWN UFGPowerInfoComponent (SDK contract: unique power info per connection). The ports are placed
    // in a small ring so multiple ports (Laser Tower = 4) don't overlap and are each wire-able.
    //
    // ★子对象名必须每次 InitWireless 都不同。
    // CreateDefaultSubobject 的重名检查查的是 CDO 的子对象注册表，而我们那套
    // UnregisterComponent + 清数组 的做法并不能把名字还回去。构造链又是「基类先跑」，
    // APowerConverter 与 APowerConverterMk2 都会调到这里，于是 "PowerConnection0" 会被要两次，
    // 直接 Fatal error（cook 阶段表现为 ExitCode=25 Error_UnknownCookFailure）。
    // 计数器让基类那轮拿 "PowerConnection0"，派生类那轮拿 "PowerConnection1_0"，永不撞名。
    const int32 Gen = PortBuildGeneration++;
    for (int32 i = 0; i < NumVisiblePorts; ++i)
    {
        const FString PortName = (Gen == 0)
            ? FString::Printf(TEXT("PowerConnection%d"), i)
            : FString::Printf(TEXT("PowerConnection%d_%d"), Gen, i);
        const FString InfoName = (Gen == 0)
            ? FString::Printf(TEXT("PowerInfo%d"), i)
            : FString::Printf(TEXT("PowerInfo%d_%d"), Gen, i);
        UWirelessPowerConnection* PC = CreateDefaultSubobject<UWirelessPowerConnection>(*PortName);
        UFGPowerInfoComponent* InfoComp = CreateDefaultSubobject<UFGPowerInfoComponent>(*InfoName);
        PC->SetPowerInfo(InfoComp);
        PC->SetupAttachment(RootComponent);
        const float Ang = (NumVisiblePorts > 1)
            ? (static_cast<float>(i) / static_cast<float>(NumVisiblePorts)) * 2.f * PI
            : 0.f;
        const float Rad = (NumVisiblePorts > 1) ? 70.f : 0.f;
        PC->SetRelativeLocation(FVector(FMath::Cos(Ang) * Rad, FMath::Sin(Ang) * Rad, 120.f));
        // 1 player cable + 1 internal bridge to the antenna (+ a little headroom).
        PC->SetMaxNumConnectionLinks(8);
        PowerConnections.Add(PC);
        PowerInfos.Add(InfoComp);
    }

    if (MeshComp)
    {
        if (UStaticMesh* M = WirelessPowerAssets::LoadWirelessMesh(Type, InTier))
        {
            MeshComp->SetStaticMesh(M);

            // Auto-compensate for source meshes whose pivot is not at the visual center/base
            // (Feyko's SF_PowerPoleMk1 exports with a lopsided origin, which rendered Mk1
            // several metres away from the placement point). For a well-behaved mesh the X/Y
            // bounds center is ~0 and the base sits at Z=0, so this is a no-op there.
            // The offset is applied in actor space, so it must be scaled by the tier scale.
            const FBoxSphereBounds B = M->GetBounds();
            const float S = WirelessPowerTierScale(InTier);
            FVector Fix(
                MeshLocalOffset.X - S * B.Origin.X,
                MeshLocalOffset.Y - S * B.Origin.Y,
                MeshLocalOffset.Z + S * (B.BoxExtent.Z - B.Origin.Z));
            MeshComp->SetRelativeLocation(Fix);
            WP_LOGF(
                TEXT("[WirelessPower] %s mesh pivot fix: boundsCenter=(%.1f,%.1f,%.1f) extent Z=%.1f -> relLoc=(%.1f,%.1f,%.1f)"),
                *GetClass()->GetName(), B.Origin.X, B.Origin.Y, B.Origin.Z, B.BoxExtent.Z, Fix.X, Fix.Y, Fix.Z);
        }
        else
        {
            MeshComp->SetRelativeLocation(MeshLocalOffset);
        }
        MeshComp->SetWorldScale3D(FVector(WirelessPowerTierScale(InTier)));
    }
}

void AWirelessPowerBuildable::BeginPlay()
{
    Super::BeginPlay();

    // v1.0.44 scale fix: join the shared node registry. Validation passes iterate this list instead
    // of walking every buildable in the world (see the registry block at the top of this file).
    RegisterWirelessNode(this);

    // v1.0.36: the per-tier ceiling is locked, so a save written by an older build can hold a range
    // above it (ApplyRange used to accept anything up to 500000 cm). Pull those back down once --
    // authority only; the property is replicated (OnRep_Range) so clients follow.
    if (HasAuthority())
    {
        const AWirelessPowerBuildable* Cdo = Cast<AWirelessPowerBuildable>(GetClass()->GetDefaultObject());
        const float Ceiling = Cdo ? Cdo->WirelessRange : 500000.f;
        if (WirelessRange > Ceiling)
        {
            WP_LOGF(TEXT("[WirelessPower] %s range %.0f exceeds the tier ceiling %.0f -> clamped"),
                    *GetClass()->GetName(), WirelessRange, Ceiling);
            WirelessRange = Ceiling;
        }
    }

    // v1.0.52 (阶段 A) 老存档迁移：v1.0.51 及以前 Channel 是 int32（1/2/3...，转换器是 4 位二进制），
    // 现在是字符串。UE 的 SaveGame 属性序列化在两个类型间**不会**自动转换：老存档里 Channel 是
    // IntProperty 的字节流，新类期望 StrProperty，读进来会丢弃/失败，于是这里读到的是**类默认值**。
    // 因此迁移必须靠"看起来像什么"判定，且只在**纯数字**（0..9 组成的串）时才当作老编号，
    // 汉字/字母编号一律不动。幂等：迁移后再走一遍结果不变。
    //   数字 n（"1"/"12"）-> 十进制字符串（与原面板显示一致）；
    //   数字 0（"0"）    -> 空串（老版 0 = 默认网络）。
    // 只在服务端做：Channel 是 ReplicatedUsing，客户端会被 OnRep 覆盖。
    if (HasAuthority())
    {
        const FString Trimmed = Channel.TrimStartAndEnd();
        bool bAllDigits = !Trimmed.IsEmpty();
        for (int32 i = 0; i < Trimmed.Len(); ++i)
        {
            if (!FChar::IsDigit(Trimmed[i]))
            {
                bAllDigits = false;
                break;
            }
        }
        if (bAllDigits)
        {
            // 退档丢失编号 bug 修正：v1.0.52+ 编号已是合法字符串，玩家完全可以手输纯数字（"001"/
            // "01"/"007"）当编号。老 int32 编号经 FString::FromInt 序列化后**不存在前导零**，所以凡是
            // 「带前导零、或与自身整数形式不一致」的纯数字串，必定是玩家自定义字符串，严禁当成老编号
            // 去做 Atoi 归一化（否则 "001"->"1"、"01"->"1"、"0"->""，退档即丢编号）。
            // 只有恰好等于其整数形式（"0"/"1"/"12"…无前导零）的纯数字才可能是老编号，而这类的迁移
            // 结果与自身相同（no-op），既兼容老存档又不会破坏玩家输入。
            const FString IntForm = FString::FromInt(FCString::Atoi(*Trimmed));
            if (Trimmed != IntForm)
            {
                WP_LOGF(TEXT("[WirelessPower] %s channel '%s' kept verbatim (player string, not legacy int)"),
                        *GetClass()->GetName(), *Channel);
            }
            else
            {
                const int32 Legacy = FCString::Atoi(*Trimmed);
                const FString Migrated = FWirelessId::FromLegacyInt(Legacy);
                if (Migrated != Channel)
                {
                    WP_LOGF(TEXT("[WirelessPower] %s legacy channel '%s' migrated to '%s'"),
                            *GetClass()->GetName(), *Channel, *Migrated);
                    Channel = Migrated;
                }
            }
        }
    }

    // v1.0.58 -- Feyko parity ("落地即自动入网").
    // A newly built node joins the neighbourhood WITHOUT the player typing a 编号: half a second
    // from now it adopts the nearest in-range node's 编号 and runs a validation, which is
    // exactly the original's `Delay 0.500000 -> ValidateAreaConnections()` beat. The 0.5 s is not
    // cosmetic: AddHiddenConnection() is a silent no-op until the circuit subsystem has handed the
    // fresh connection its private circuit, which happens a little after BeginPlay.
    //
    // The adoption itself is deferred to the deadline (not done here) because on a savegame load
    // the nodes' BeginPlay order is NOT spatial -- an in-BeginPlay scan can only see the nodes that
    // happen to have loaded already. By the time Tick runs, every node is registered.
    if (HasAuthority())
    {
        bNeedsChannelAdopt = true;
        ScheduleEarlyValidate();
    }

    // Infrastructure: the node itself does not consume power -- it only bridges it (like a pole).
    if (WirelessPowerInfo)
    {
        WirelessPowerInfo->SetTargetConsumption(0.0f);
    }
    for (UFGPowerInfoComponent* InfoComp : PowerInfos)
    {
        if (IsValid(InfoComp))
        {
            InfoComp->SetTargetConsumption(0.0f);
        }
    }

    // NOTE: the internal port<->antenna bridge is NOT established here. The circuit subsystem
    // creates each connection's private circuit only AFTER actor BeginPlay, so AddHiddenConnection
    // called this early silently no-ops. EnsureInternalBridge() runs every Tick until the link
    // exists AND both ends report the same circuit ID.
    WP_LOGF(
        TEXT("[WirelessPower] %s ready: mesh=%s ports=%d broadcast=%d channel='%s' range=%.0f LOS=%d"),
        *GetClass()->GetName(),
        (MeshComp && MeshComp->GetStaticMesh()) ? TEXT("OK") : TEXT("MISSING"),
        PowerConnections.Num(), bBroadcastsToMachines ? 1 : 0,
        *Channel, WirelessRange, bNeedsLineOfSight ? 1 : 0);
}

void AWirelessPowerBuildable::EndPlay(const EEndPlayReason::Type Reason)
{
    // v1.0.44 scale fix: leave the shared node registry before anything else is torn down, so no
    // other node's validation pass can still be holding us as a peer candidate.
    UnregisterWirelessNode(this);

    // Drop the visualization geometry immediately. v1.0.22: the cage and the beams are real
    // UInstancedStaticMeshComponent instances now, so clearing the instances (not Flush()ing a line
    // batcher) is what releases the geometry before the actor goes away.
    bEndingPlay = true;
    bShowCage = false;
    bCageBuilt = false;
    if (IsValid(CageISM))
    {
        CageISM->ClearInstances();
        CageISM->SetVisibility(false);
    }
    if (IsValid(BeamISM))
    {
        BeamISM->ClearInstances();
        BeamISM->SetVisibility(false);
    }
    // v1.0.32: kill the beam particle systems explicitly. They are auto-updating visual components,
    // and this build is known to stall on shutdown when such a component is still live while the
    // world tears down (same reason the cage is cleared on demand rather than left running).
    for (const TObjectPtr<UParticleSystemComponent>& FX : BeamFX)
    {
        if (IsValid(FX))
        {
            FX->DeactivateSystem();
            FX->SetVisibility(false);
        }
    }

    // Critical: drop all hidden links BEFORE the actor (and its connection components) are
    // destroyed. Otherwise peer buildings keep dangling pointers into our WirelessConnection,
    // which corrupts the circuit subsystem and later crashes the renderer (Slate OnPaint 0x8).
    TearDownWirelessLinks(true);

    // Full teardown for demolition: also sever the internal bridges. At runtime (POWER DOWN)
    // the bridges are preserved by TearDownWirelessLinks -- only here do we drop everything.
    if (IsValid(WirelessConnection))
    {
        WirelessConnection->ClearHiddenConnections();
    }
    for (UWirelessPowerConnection* P : PowerConnections)
    {
        if (IsValid(P))
        {
            P->ClearHiddenConnections();
        }
    }

    Super::EndPlay(Reason);
}

// ----------------------------------------------------------------------------
// Interaction outline (v1.0.28).
//
// What the player sees: aim at a placed building and Satisfactory draws a coloured highlight around
// it. The trigger is AFGBuildable::StartIsLookedAt_Implementation -> UFGOutlineComponent::
// ShowOutline(this, OC_USABLE), "Shows outline using actor's mesh components" -- i.e. it walks EVERY
// mesh component of the actor. Our coverage cage (CageISM) and the laser beams (BeamISM) are mesh
// components of the actor too, so they were swept into the highlight and the thin bars were
// repainted in the outline colour (user: "指向它时，显示出来的是描边颜色").
//
// The fix has two independent layers, because the SDK outline can work either way internally:
//   1. force the visualisation geometry OUT of the custom-depth/stencil pass the outline is built
//      from (covers the "the actor's own meshes are rendered into the outline" path), and
//   2. narrow the actor-wide outline state to MeshComp alone with ShowOutlineForStaticMeshComponent,
//      the SDK's dedicated "outline only this mesh" API (covers the "proxies mirror every mesh
//      component" path, which would otherwise duplicate the cage into the outline).
//
// Super::StartIsLookedAt* is still called first so the vanilla semantics are preserved (photo mode
// suppresses the outline, a dismantled building is skipped, dismantle mode also swaps materials).
// NarrowOutlineToMainMesh() then only acts when an outline state really exists for us. The three
// StopIsLookedAt* hooks mirror it, because the component-scoped call keeps its own state store.
void AWirelessPowerBuildable::NarrowOutlineToMainMesh(AFGCharacterPlayer* byCharacter, EOutlineColor Color)
{
    UFGOutlineComponent* Outline = (byCharacter ? byCharacter->GetOutline() : nullptr);
    if (!Outline)
    {
        return;
    }
    // No state == Super did not ask for an outline (dismantled / photo mode): leave everything as is.
    // This also means we never ADD an outline where the vanilla code decided not to draw one.
    if (!Outline->GetImmutableOutlineStateForActor(this))
    {
        return;
    }

    // Diagnostic (transition-driven, not per-frame): did the engine put our geometry into the
    // outline pass before we cleared it? depth=1 confirms the actor-mesh path, depth=0 the proxy path.
    const int32 CageDepth = IsValid(CageISM) ? (CageISM->bRenderCustomDepth ? 1 : 0) : -1;
    const int32 BeamDepth = IsValid(BeamISM) ? (BeamISM->bRenderCustomDepth ? 1 : 0) : -1;

    // (1) Hard guarantee for the custom-depth pass.
    if (IsValid(CageISM))
    {
        CageISM->SetRenderCustomDepth(false);
        CageISM->SetCustomDepthStencilValue(0);
    }
    if (IsValid(BeamISM))
    {
        BeamISM->SetRenderCustomDepth(false);
        BeamISM->SetCustomDepthStencilValue(0);
    }

    // (2) Narrow the outline to the real building mesh ("MainMesh", the one the build gun previews).
    if (!IsValid(MeshComp))
    {
        return; // nothing sensible to outline -- keep whatever Super set up
    }
    Outline->HideOutline(this); // drops the whole-actor state (and its proxies)
    Outline->ShowOutlineForStaticMeshComponent(this, MeshComp, Color);

    WP_LOGF(
        TEXT("[WirelessPower] %s outline scoped to MainMesh (color=%d, cageWasInOutlinePass=%d, beamWasInOutlinePass=%d)"),
        *GetClass()->GetName(), static_cast<int32>(Color), CageDepth, BeamDepth);
}

void AWirelessPowerBuildable::StartIsLookedAt_Implementation(AFGCharacterPlayer* byCharacter, const FUseState& state)
{
    Super::StartIsLookedAt_Implementation(byCharacter, state);
    NarrowOutlineToMainMesh(byCharacter, EOutlineColor::OC_USABLE);
}

void AWirelessPowerBuildable::StopIsLookedAt_Implementation(AFGCharacterPlayer* byCharacter, const FUseState& state)
{
    Super::StopIsLookedAt_Implementation(byCharacter, state); // vanilla HideOutline(this)
    ClearNarrowedOutline(byCharacter);
}

void AWirelessPowerBuildable::StartIsLookedAtForDismantle_Implementation(AFGCharacterPlayer* byCharacter)
{
    Super::StartIsLookedAtForDismantle_Implementation(byCharacter);
    NarrowOutlineToMainMesh(byCharacter, EOutlineColor::OC_DISMANTLE);
}

void AWirelessPowerBuildable::StopIsLookedAtForDismantle_Implementation(AFGCharacterPlayer* byCharacter)
{
    Super::StopIsLookedAtForDismantle_Implementation(byCharacter); // also restores the materials
    ClearNarrowedOutline(byCharacter);
}

void AWirelessPowerBuildable::StartIsLookedAtForConnection(AFGCharacterPlayer* byCharacter,
                                                          UFGCircuitConnectionComponent* overlappingConnection)
{
    Super::StartIsLookedAtForConnection(byCharacter, overlappingConnection);
    NarrowOutlineToMainMesh(byCharacter, EOutlineColor::OC_HOLOGRAMLINE);
}

void AWirelessPowerBuildable::StopIsLookedAtForConnection(AFGCharacterPlayer* byCharacter)
{
    Super::StopIsLookedAtForConnection(byCharacter);
    ClearNarrowedOutline(byCharacter);
}

void AWirelessPowerBuildable::ClearNarrowedOutline(AFGCharacterPlayer* byCharacter)
{
    UFGOutlineComponent* Outline = (byCharacter ? byCharacter->GetOutline() : nullptr);
    if (!Outline || !IsValid(MeshComp))
    {
        return;
    }
    // Symmetric counterpart of ShowOutlineForStaticMeshComponent. Runs AFTER Super so it can never
    // resurrect a state the base class just cleared.
    Outline->HideOutlineForStaticMeshComponent(this, MeshComp);
}

// ----------------------------------------------------------------------------
// Range visualization: draw the CUBIC coverage volume as a DENSE bar lattice (CageISM).
//
// Style: Feyko's original drew a gridded box, not a bare 12-edge wireframe. So we emit the 12
// outer edges (thick) PLUS an interior face lattice (thinner), with the lattice spacing scaled to
// the range so the cube still reads as "dense" at any size. The colour comes from
// WirelessPowerCageColor(BuildingType), i.e. every building family gets its own hue.
//
// HISTORY OF TWO DEAD ENDS (v1.0.13-v1.0.21) -- do NOT go back to either:
//   1) DrawDebugBox/DrawDebugLine: their bodies are wrapped in a shipping guard, so they are
//      compiled into NO-OPS in a Shipping game build. Invisible by construction.
//   2) ULineBatchComponent::DrawLine: this game's Shipping renderer simply does not draw batched
//      debug lines. The in-game diagnostic proved they WERE submitted (cageLines=240) yet not one
//      appeared on screen. Link traced end-to-end (DrawLine -> FBatchedElements -> PDI->
//      DrawTranslucentLine -> GWhiteTexture) and it is self-consistent -- the game build just
//      filters that proxy out.
// v1.0.22 therefore uses REAL geometry: thin instances of /Engine/BasicShapes/Cube (a vanilla mesh
// that IS cooked into the game), which render through the normal static-mesh path. Rebuild only
// when the range/centre really changed, and ClearInstances() before every rebuild so instances do
// not accumulate.
// ----------------------------------------------------------------------------
FLinearColor AWirelessPowerBuildable::GetCageColor() const
{
    return WirelessPowerCageColor(BuildingType);
}

void AWirelessPowerBuildable::EnsureVisualMaterials()
{
    // Both MIDs come from the same unlit material; only the "Color" parameter differs (one hue per
    // building family, so panel <-> cube <-> beam obviously belong together). If the material asset
    // is missing we simply keep the vanilla BasicShapeMaterial: grey, but still visible.
    UMaterialInterface* Base = nullptr;
    if (IsValid(CageISM) && !CageMID)
    {
        Base = WirelessPowerAssets::LoadBeamMaterial();
        if (Base)
        {
            CageMID = UMaterialInstanceDynamic::Create(Base, this);
            if (CageMID)
            {
                CageMID->SetVectorParameterValue(TEXT("Color"), GetCageColor());
                CageISM->SetMaterial(0, CageMID);
            }
        }
    }
    if (IsValid(BeamISM) && !BeamMID)
    {
        if (!Base)
        {
            Base = WirelessPowerAssets::LoadBeamMaterial();
        }
        if (Base)
        {
            BeamMID = UMaterialInstanceDynamic::Create(Base, this);
            if (BeamMID)
            {
                // v1.0.30: the beam used to be LerpUsingHSV(cageColor, White, 0.45) -- a deliberate
                // "hotter than the cage" tint that turned the laser tower's red into a washed-out
                // pink/white. The user wants the laser RED ("颜色应该是红色"), and it should read as
                // the same family colour as everything else on the building, so use the cage colour
                // verbatim: LaserTower -> (1.00, 0.25, 0.20).
                BeamMID->SetVectorParameterValue(TEXT("Color"), GetCageColor());
                BeamISM->SetMaterial(0, BeamMID);
            }
        }
    }
}

FVector AWirelessPowerBuildable::GetBeamPoint() const
{
    // Beam emitters (and the line-of-sight trace) sit at the model's RED EMITTER LAMP. Using the
    // actor origin (the building's base) buried both the trace and the bar inside the foundation --
    // that was the v1.0.21 bug where the laser link could never form and no beam was ever drawn.
    //
    // v1.0.25 fix: Bounds.BoxExtent.Z is only the HALF-height, so reading it alone landed the beam
    // at the building's WAIST whenever the mesh's bounds centre is not at its base.
    //
    // v1.0.30 fix: stop adding +40 cm on top of the bounds top (the beam sat entirely above the
    // light) and instead come down by a constant.
    //
    // v1.0.34 fix ("没有在塔顶红色灯柱范围内"): stop guessing entirely and anchor to the LAMP'S OWN
    // GEOMETRY, measured off WP_Mesh_LaserTower:
    //   * material slot 5, `Custom_EmissiveBlendToFactoryMat`, is the mod's emissive/lamp material
    //     (confirmed section 3 of LOD0 via StaticMeshEditorSubsystem.get_lod_material_slot);
    //   * that section's geometry in the lantern region is two rings around the lantern axis, at
    //     mesh Z 6713..6800 and 7100..7117, radius 126..227 cm, centred on mesh (X -0.3, Y +22.0)
    //     -- i.e. they are the top and bottom rim of the lantern's glowing core panel;
    //   * so the core panel spans mesh Z 6800..7100 and its centre is mesh Z ~6950 on the axis
    //     (0, +22);
    //   * the previous anchor (bounds top 7334.63 - 200 = 7134.63) sits 185 cm ABOVE the core's top
    //     rim -- outside the lamp, exactly what the screenshot showed.
    // Going through MeshComp's transform keeps this correct when the pivot fix or the tier scale
    // changes, which a hard-coded height below the bounds top would not.
    static constexpr float kLampCentreMeshZ = 6950.f;
    static constexpr float kLampCentreMeshY = 22.f;

    if (BuildingType == EWirelessBuildingType::LaserTower && IsValid(MeshComp)
        && MeshComp->GetStaticMesh())
    {
        // Guard against the fallback cube being in the slot: only the real 73 m tower gets the
        // lamp anchor.
        const FBoxSphereBounds MeshBounds = MeshComp->GetStaticMesh()->GetBounds();
        if (MeshBounds.BoxExtent.Z * 2.f > 5000.f)
        {
            const FVector MeshLoc = MeshComp->GetComponentLocation();
            const float S = MeshComp->GetComponentScale().Z;
            return MeshLoc + FVector(0.f, kLampCentreMeshY * S, kLampCentreMeshZ * S);
        }
    }

    const FVector Loc = GetActorLocation();
    float HeightAboveOrigin = 0.f;
    if (IsValid(MeshComp))
    {
        const FBoxSphereBounds& B = MeshComp->Bounds; // world space
        HeightAboveOrigin = (B.Origin.Z + B.BoxExtent.Z) - Loc.Z;
    }
    if (HeightAboveOrigin < 50.f)
    {
        // Bounds not computed yet (pre-registration) or a degenerate mesh -- fall back to a sane
        // tower height so the endpoint is never left sitting inside the base collider.
        HeightAboveOrigin = 400.f;
    }
    return FVector(Loc.X, Loc.Y, Loc.Z + HeightAboveOrigin - 200.f);
}

void AWirelessPowerBuildable::RebuildRangeCage()
{
    if (!IsValid(CageISM) || bEndingPlay)
    {
        return;
    }

    // Drop the previous cage: instances are persistent geometry, so without ClearInstances every
    // rebuild (range edit, toggle, move) would stack another lattice on top of the old one.
    CageISM->ClearInstances();
    bCageBuilt = false;

    if (!bShowCage || WirelessRange <= 0.f)
    {
        CageISM->SetVisibility(false);
        return;
    }
    // v1.0.27: the Laser Tower has NO range display at all (user: "输电塔去除范围显示"). Its
    // coverage is a fixed, self-evident 4.8 km, and a cube that size is pure visual noise. This
    // guard is the hard backstop: even if something flips bShowCage on a tower, nothing is drawn.
    if (BuildingType == EWirelessBuildingType::LaserTower)
    {
        CageISM->SetVisibility(false);
        return;
    }
    const UWorld* W = GetWorld();
    if (!W || W->GetNetMode() == NM_DedicatedServer)
    {
        return; // no renderer there
    }
    UStaticMesh* Bar = CageISM->GetStaticMesh();
    if (!Bar)
    {
        return;
    }

    EnsureVisualMaterials();

    // The unit cube's own box, so bar scale/origin are derived from the real asset instead of
    // assuming "100 uu centred on the pivot".
    const FBoxSphereBounds MB = Bar->GetBounds();
    const FVector Half = MB.BoxExtent.GetAbs();
    const FVector Pivot = MB.Origin;
    if (Half.X <= 0.f || Half.Y <= 0.f || Half.Z <= 0.f)
    {
        return;
    }

    const FVector C = GetActorLocation();
    const float R = WirelessRange;                              // half extent (cm), per the 半径 rule
    // v1.0.53：由球体**改回立方体**（用户决定，v1.0.52 的球体只存在了一版）。
    // 画法沿用 v1.0.31 的立方体网格：1 m 间距的细线 lattice，12 条外棱 + 六个面上的内格。
    // 三个人类尺度族（转换器 / 无线供电器 / 继电器）都走这条路径；
    //   * 输电塔永远不会到这里（v1.0.27 有硬守卫），bDense 的兜底只防任何一族出现荒诞大范围。
    const bool bDense = (R <= 30000.f);                         // <= 300 m
    const float Span = 2.f * R;                                 // full edge length (cm)

    // ---- The two knobs that define the cage look. Both are single constants on purpose: the user
    //      is tuning this by eye, so each round trip must be a one-number edit. ----
    // Pitch between parallel lines. Every standard range (12/24/36/48/120/160 m) is a whole number
    // of metres, so the span is always an exact multiple of 100 cm and the pitch comes out exactly
    // as asked, with no rounding drift.
    static constexpr float kCagePitchCm = 100.f;
    // Line thickness: a FIXED absolute value, never a fraction of the pitch -- see the v1.0.30 note
    // below for why that rule matters.
    static constexpr float kCageBarThicknessCm = 2.5f;

    // Cells per axis, capped by an instance budget so a pathological range coarsens gracefully
    // instead of spawning tens of thousands of bars. At a 1 m pitch the budget is far away: a
    // maxed-out 160 m relay -> 320 cells/axis -> 12 + 6*319*2 = 3840 bars.
    // bars = 12 edges + 6 faces * (N-1) * 2 -> N <= (Budget-12)/12.
    const int32 MaxCells = (20000 - 12) / 12;                   // 1665
    const int32 N = bDense ? FMath::Clamp(FMath::RoundToInt(Span / kCagePitchCm), 2, MaxCells) : 1;
    // v1.0.30: THIN LINE at a FIXED thickness. v1.0.28 doubled the pitch to 50 cm but let the bar
    // grow with it (10% of the pitch), so the solid/void ratio stayed at 10% and the grid looked
    // exactly like the 25 cm one -- the user could not see any change ("这范围怎么感觉跟没改一样").
    // Pinning the bar at the same 2.5 cm the 25 cm grid used keeps the "细线" look while cutting the
    // line count, so the pitch change finally reads. Spacing stays uniform: the interior lines sit
    // at exact multiples of Span/N and both outer edges are one full step in from them.
    const float T = bDense ? kCageBarThicknessCm
                           : FMath::Clamp(R / 3000.f, 20.f, 150.f);

    // Bulk-submit: AddInstance() marks the render state dirty every call, which is far too slow
    // for the several-thousand bars a dense cage needs. Collect first, then AddInstances() once.
    TArray<FTransform> Xforms;
    Xforms.Reserve(bDense ? (12 + 6 * (N - 1) * 2) : 12);

    auto PushBar = [&](const FVector& Center, const FVector& Size)
    {
        const FVector Scale(Size.X / (2.f * Half.X), Size.Y / (2.f * Half.Y), Size.Z / (2.f * Half.Z));
        const FVector Pos = Center - Scale * Pivot; // compensate a non-centred mesh pivot
        Xforms.Emplace(FRotator::ZeroRotator, Pos, Scale);
    };

    // ---- 12 outer edges (full extent along one axis, T x T cross-section) ----
    for (int32 Axis = 0; Axis < 3; ++Axis)
    {
        const int32 A = (Axis + 1) % 3;
        const int32 B = (Axis + 2) % 3;
        for (int32 SignA = -1; SignA <= 1; SignA += 2)
        {
            for (int32 SignB = -1; SignB <= 1; SignB += 2)
            {
                FVector Center = C;
                Center[A] += SignA * R;
                Center[B] += SignB * R;
                FVector Size(T, T, T);
                Size[Axis] = Span;
                PushBar(Center, Size);
            }
        }
    }

    // ---- interior lattice on all six faces (the fine-mesh part) -- human-scale cubes only ----
    if (bDense)
    {
        for (int32 Step = 1; Step < N; ++Step)
        {
            const float O = -R + Span * (static_cast<float>(Step) / static_cast<float>(N));
            for (int32 Axis = 0; Axis < 3; ++Axis) // Axis is the face normal
            {
                const int32 U = (Axis + 1) % 3;
                const int32 V = (Axis + 2) % 3;
                for (int32 Sign = -1; Sign <= 1; Sign += 2)
                {
                    FVector Face = C;
                    Face[Axis] += Sign * R;

                    FVector C1 = Face;
                    C1[V] += O; // bar running along U
                    FVector S1(T, T, T);
                    S1[U] = Span;
                    PushBar(C1, S1);

                    FVector C2 = Face;
                    C2[U] += O; // bar running along V
                    FVector S2(T, T, T);
                    S2[V] = Span;
                    PushBar(C2, S2);
                }
            }
        }
    }

    if (Xforms.Num() > 0)
    {
        CageISM->AddInstances(Xforms, false); // false = instance space == component space (see ctor)
    }
    CageISM->SetVisibility(Xforms.Num() > 0);
    bCageBuilt = true;
    CageBuiltRange = WirelessRange;
    CageBuiltCenter = C;
}

void AWirelessPowerBuildable::SetRangeCageVisible(bool bShow)
{
    // v1.0.27: the Laser Tower has no range display; never latch the switch on for it.
    bShowCage = (BuildingType == EWirelessBuildingType::LaserTower) ? false : bShow;
    if (!IsValid(CageISM))
    {
        return;
    }
    if (bShowCage)
    {
        CageISM->SetVisibility(true);
        RebuildRangeCage();
    }
    else
    {
        // Hide AND drop the instances: they are real geometry, so leaving them would keep the cage
        // on screen (and cost draw calls) after the switch is turned off.
        CageISM->SetVisibility(false);
        CageISM->ClearInstances();
        bCageBuilt = false;
    }
}

// ----------------------------------------------------------------------------
// Laser Tower: manual ID links + beam rendering.
//
// Feyko's laser tower is the only node wired up BY ID: the panel lists the IDs (编号) of the laser
// towers that already exist in the world, and "开关连接" connects/disconnects that pair. Manual
// links deliberately bypass the channel auto-match (they still require line of sight), so two
// towers on different IDs can be bridged - that is the whole point of the feature.
//
// v1.0.32 - the beams are drawn by FEYKO'S OWN particle system, the way his Blueprint did it. The
// cube-instance bars survive only as a fallback for the case where his asset cannot be loaded.
// Everything is rebuilt at ~2 Hz, so a destroyed/changed peer can never leave a stale beam.
// LaserPeers is replicated, so clients draw the same beams without server-only circuit data.
// ----------------------------------------------------------------------------
void AWirelessPowerBuildable::ApplyLaserLinkToggle(const FString& TargetChannel)
{
    if (!HasAuthority())
    {
        return;
    }
    if (BuildingType != EWirelessBuildingType::LaserTower)
    {
        return;
    }
    // v1.0.52: 编号是字符串，比较走规范化（汉字/全角/大小写都能正确对上）。
    const FString Target = FWirelessId::Normalize(TargetChannel);
    if (Target.IsEmpty() || FWirelessId::Equal(Target, Channel))
    {
        return; // a tower cannot be manually linked to its own ID
    }
    // Find an existing entry that normalises to the same id (so toggling off works even if the
    // stored spelling differs in width/case from what the panel sent).
    const int32 Existing = ManualLinkChannels.IndexOfByPredicate(
        [&Target](const FString& S) { return FWirelessId::Equal(S, Target); });
    if (Existing != INDEX_NONE)
    {
        ManualLinkChannels.RemoveAt(Existing);
        WP_LOGF( TEXT("[WirelessPower] %s manual laser link removed: id=%s"),
               *GetClass()->GetName(), *Target);
    }
    else
    {
        ManualLinkChannels.Add(Target);
        WP_LOGF( TEXT("[WirelessPower] %s manual laser link added: id=%s"),
               *GetClass()->GetName(), *Target);
    }
    // Re-evaluate immediately so the link (and the beam) appears/disappears without waiting for
    // the 3s validation tick.
    ValidateWirelessMesh();
    BeamRefreshAccum = 1.f;
}

void AWirelessPowerBuildable::ApplyLaserVisible(bool bVisible)
{
    bLaserVisible = bVisible; // replicated -> every client follows
    BeamRefreshAccum = 1.f;   // redraw on the next tick
}

void AWirelessPowerBuildable::OnRep_LaserVisible()
{
    BeamRefreshAccum = 1.f;
}

void AWirelessPowerBuildable::OnRep_LaserPeers()
{
    BeamRefreshAccum = 1.f;
}

void AWirelessPowerBuildable::RefreshLaserPeers()
{
    TArray<TObjectPtr<AWirelessPowerBuildable>> NewPeers;
    if (BuildingType == EWirelessBuildingType::LaserTower && IsValid(WirelessConnection))
    {
        for (UFGCircuitConnectionComponent* L : MyLinks)
        {
            if (!IsValid(L))
            {
                continue;
            }
            AWirelessPowerBuildable* P = Cast<AWirelessPowerBuildable>(L->GetOwner());
            if (P && P != this && P->BuildingType == EWirelessBuildingType::LaserTower)
            {
                NewPeers.Add(P);
            }
        }
    }
    if (NewPeers.Num() != LaserPeers.Num())
    {
        LaserPeers = NewPeers;
        return;
    }
    // Same length: compare element-wise so an unchanged list does not dirty the replicated property.
    for (int32 i = 0; i < NewPeers.Num(); ++i)
    {
        if (NewPeers[i] != LaserPeers[i])
        {
            LaserPeers = NewPeers;
            return;
        }
    }
}

// ----------------------------------------------------------------------------
// v1.0.32 - Feyko's ORIGINAL beam renderer.
//
// The original mod is Blueprint-only. Its Build_LaserTower graph holds a ParticleSystemComponent
// playing /WirelessPower/Buildable/LaserTower/P_Laser and drives every link with
// SetBeamSourcePoint / SetBeamTargetPoint -- those two function names (together with NewSourcePoint,
// NewTargetPoint, SourceIndex, TargetIndex, TargetArray) are literally in that asset's name table,
// which is how we know the mechanism. P_Laser is ONE Cascade Beam2 emitter called "Laser"
// (ParticleModuleTypeDataBeam2 + BeamSource / BeamTarget + BeamNoise), and M_Laser is MSM_UNLIT /
// BLEND_TRANSLUCENT, building a hair-thin glowing core out of a cross-section falloff
// (Sine + Power over TextureCoordinate, driven by ParticleColor) -- no texture at all. That is why
// the original reads as a thin line rather than a solid bar, and it is why nothing assembled out of
// /Engine/BasicShapes/Cube can ever look like it.
//
// Why this still works in Satisfactory 1.1 (UE5.6-CSS): Cascade is fully present
// (Classes/Particles/TypeData/ParticleModuleTypeDataBeam2.h, Private/Particles/ParticleBeam2EmitterInstance.cpp),
// and the asset loads (verified headlessly: Class 'ParticleSystem', IsValid=True). On the engine side
// UParticleSystemComponent::SetBeamSourcePoint() forwards to
// FParticleBeam2EmitterInstance::SetBeamSourcePoint(), which stores the point into
// UserSetSourceArray[SourceIndex]; ParticleBeamModules.cpp consumes that array ONLY when the module's
// SourceMethod == PEB2STM_UserSet (the target likewise). We therefore force that flag on the loaded
// template below: a no-op when Feyko already set it, and otherwise the difference between a beam that
// joins two towers and one that just hangs off the component's own location.
//
// Coordinate space: on the UserSet path the engine stores the point verbatim, with NO component
// transform applied (BeamData->SourcePoint = BeamInst->UserSetSourceArray[ParticleIndex]), so we hand
// it WORLD coordinates -- exactly what GetBeamPoint() already returns.
//
// Particle index == link index: the emitter's particle N is the beam that reads
// UserSetSourceArray[N], so link i is written into slot i. How many slots one component can carry is
// bounded by ParticleModuleTypeDataBeam2::MaxBeamCount (read back at runtime); a tower with more
// links than that gets a second P_Laser component instead of silently dropping beams.
// ----------------------------------------------------------------------------
namespace
{
    /** Read a vector distribution's value when it is a plain constant. */
    bool ReadConstantVector(const FRawDistributionVector& Dist, FVector& OutValue)
    {
        if (const UDistributionVectorConstant* Const = Cast<UDistributionVectorConstant>(Dist.Distribution))
        {
            OutValue = Const->Constant;
            return true;
        }
        return false;
    }

    /** Recolour a constant vector distribution in place (used for ParticleModuleColor::StartColor,
     *  which is what M_Laser's MaterialExpressionParticleColor ends up showing). Distributions that
     *  are curves are left alone -- overriding those at runtime would fight Feyko's own animation. */
    void ForceConstantVector(FRawDistributionVector& Dist, const FVector& NewValue, bool bReport,
                             const TCHAR* What)
    {
        UDistributionVectorConstant* Const = Cast<UDistributionVectorConstant>(Dist.Distribution);
        if (!Const)
        {
            if (bReport)
            {
                WP_LOGV(TEXT("[WP-PLASER]   %s: not a constant distribution (%s) -- left as authored"),
                    What, Dist.Distribution ? *Dist.Distribution->GetClass()->GetName() : TEXT("<null>"));
            }
            return;
        }
        const FVector Before = Const->Constant;
        Const->Constant = NewValue;
        if (bReport)
        {
            WP_LOGV(TEXT("[WP-PLASER]   %s: constant %s -> %s"), What, *Before.ToString(), *NewValue.ToString());
        }
    }
}

UParticleSystem* AWirelessPowerBuildable::ResolveLaserTemplate()
{
    if (bLaserPSResolved)
    {
        return LaserPSTemplate;
    }
    bLaserPSResolved = true;

    LaserPSTemplate = WirelessPowerAssets::LoadLaserParticleSystem();
    if (!LaserPSTemplate)
    {
        UE_LOG(LogWirelessPower, Warning,
            TEXT("[WirelessPower] %s: Feyko's P_Laser could not be loaded -- laser beams stay on the cube-bar fallback."),
            *GetClass()->GetName());
        LaserBeamsPerComponent = 1;
        return nullptr;
    }

    // Dump the template's real configuration exactly once per process (we cannot read a particle
    // system's emitter tree from Python -- UParticleSystem::Emitters is not editor-exposed -- so this
    // log is the only way to see what the asset actually contains), then force the two method flags.
    static TWeakObjectPtr<UParticleSystem> ReportedTemplate;
    const bool bReport = (ReportedTemplate.Get() != LaserPSTemplate);
    ReportedTemplate = LaserPSTemplate;

    const FLinearColor BeamColor = GetCageColor();
    const FVector BeamColorV(BeamColor.R, BeamColor.G, BeamColor.B);

    int32 MaxBeams = 1;
    int32 PeakActive = 0;
    int32 EmitterIndex = 0;
    for (UParticleEmitter* Emitter : LaserPSTemplate->Emitters)
    {
        if (IsValid(Emitter))
        {
            for (UParticleLODLevel* LOD : Emitter->LODLevels)
            {
                if (!IsValid(LOD))
                {
                    continue;
                }
                PeakActive = FMath::Max(PeakActive, LOD->PeakActiveParticles);

                UParticleModuleTypeDataBeam2* BeamTD = Cast<UParticleModuleTypeDataBeam2>(LOD->TypeDataModule);
                if (BeamTD)
                {
                    MaxBeams = FMath::Max(MaxBeams, BeamTD->MaxBeamCount);
                }
                if (bReport)
                {
                    WP_LOGV(TEXT("[WP-PLASER] emitter[%d]='%s' lod=%d beam2=%d maxBeamCount=%d beamMethod=%d textureTile=%d sheets=%d peakActive=%d material=%s"),
                        EmitterIndex, *Emitter->EmitterName.ToString(), LOD->Level,
                        BeamTD ? 1 : 0,
                        BeamTD ? BeamTD->MaxBeamCount : -1,
                        BeamTD ? static_cast<int32>(BeamTD->BeamMethod.GetValue()) : -1,
                        BeamTD ? BeamTD->TextureTile : -1,
                        BeamTD ? BeamTD->Sheets : -1,
                        LOD->PeakActiveParticles,
                        (LOD->RequiredModule && LOD->RequiredModule->Material)
                            ? *LOD->RequiredModule->Material->GetName() : TEXT("<null>"));
                }

                for (UParticleModule* Mod : LOD->Modules)
                {
                    if (UParticleModuleBeamSource* Src = Cast<UParticleModuleBeamSource>(Mod))
                    {
                        const bool bWasUserSet = (Src->SourceMethod.GetValue() == PEB2STM_UserSet);
                        Src->SourceMethod = PEB2STM_UserSet;
                        if (bReport)
                        {
                            WP_LOGV(TEXT("[WP-PLASER]   BeamSource: wasUserSet=%d absolute=%d locked=%d -> forced UserSet"),
                                bWasUserSet ? 1 : 0, Src->bSourceAbsolute ? 1 : 0, Src->bLockSource ? 1 : 0);
                        }
                    }
                    else if (UParticleModuleBeamTarget* Tgt = Cast<UParticleModuleBeamTarget>(Mod))
                    {
                        const bool bWasUserSet = (Tgt->TargetMethod.GetValue() == PEB2STM_UserSet);
                        Tgt->TargetMethod = PEB2STM_UserSet;
                        if (bReport)
                        {
                            WP_LOGV(TEXT("[WP-PLASER]   BeamTarget: wasUserSet=%d name='%s' absolute=%d -> forced UserSet"),
                                bWasUserSet ? 1 : 0, *Tgt->TargetName.ToString(),
                                Tgt->bTargetAbsolute ? 1 : 0);
                        }
                    }
                    else if (UParticleModuleBeamNoise* Noise = Cast<UParticleModuleBeamNoise>(Mod))
                    {
                        // v1.0.34 (为什么会抖动): P_Laser ships a Beam Noise module, which is what
                        // makes the beam wander by design. Feyko's asset is left untouched on disk,
                        // but the loaded template is patched here so the beam is a straight line.
                        // Turning bEnabled off as well keeps the module out of the update path.
                        if (bReport)
                        {
                            WP_LOGV(TEXT("[WP-PLASER]   BeamNoise: lowFreq=%d frequency=%d -> disabled"),
                                Noise->bLowFreq_Enabled ? 1 : 0, Noise->Frequency);
                        }
                        Noise->bLowFreq_Enabled = 0;
                        Noise->bTargetNoise = 0;
                        Noise->bEnabled = 0;
                    }
                    else if (UParticleModuleColor* Col = Cast<UParticleModuleColor>(Mod))
                    {
                        ForceConstantVector(Col->StartColor, BeamColorV, bReport, TEXT("Color.StartColor"));
                    }
                    else if (UParticleModuleSize* Sz = Cast<UParticleModuleSize>(Mod))
                    {
                        FVector SizeValue = FVector::ZeroVector;
                        if (bReport)
                        {
                            WP_LOGV(TEXT("[WP-PLASER]   Size.StartSize: constant=%d value=%s"),
                                ReadConstantVector(Sz->StartSize, SizeValue) ? 1 : 0, *SizeValue.ToString());
                        }
                    }
                }
            }
        }
        ++EmitterIndex;
    }

    // One component per MaxBeamCount links, but never claim more slots than the emitter actually
    // keeps alive: a high MaxBeamCount with a low spawn rate would otherwise strand the extra links.
    LaserBeamsPerComponent = FMath::Clamp(FMath::Min(MaxBeams, FMath::Max(PeakActive, 1)), 1, 16);
    if (bReport)
    {
        WP_LOGV(TEXT("[WP-PLASER] P_Laser resolved: maxBeams=%d peakActive=%d -> beamsPerComponent=%d (emitters=%d)"),
            MaxBeams, PeakActive, LaserBeamsPerComponent, LaserPSTemplate->Emitters.Num());
    }
    return LaserPSTemplate;
}

UParticleSystemComponent* AWirelessPowerBuildable::GetBeamFXComponent(int32 PoolIndex)
{
    if (PoolIndex < 0)
    {
        return nullptr;
    }
    UParticleSystem* PS = ResolveLaserTemplate();
    if (!PS)
    {
        return nullptr;
    }

    while (BeamFX.Num() <= PoolIndex)
    {
        UParticleSystemComponent* NewFX = NewObject<UParticleSystemComponent>(this);
        if (!NewFX)
        {
            return nullptr;
        }
        NewFX->SetupAttachment(RootComponent);
        NewFX->SetMobility(EComponentMobility::Movable);
        NewFX->bAutoActivate = false;
        NewFX->SetCastShadow(false);
        NewFX->SetGenerateOverlapEvents(false);
        NewFX->SetCanEverAffectNavigation(false);
        NewFX->SetVisibility(false);
        NewFX->SetBoundsScale(2000.f);
        NewFX->RegisterComponent();
        BeamFX.Add(NewFX);
    }

    UParticleSystemComponent* Comp = BeamFX[PoolIndex];
    if (!IsValid(Comp))
    {
        return nullptr;
    }
    if (Comp->Template != PS)
    {
        Comp->SetTemplate(PS);
        ApplyBeamFXColor(Comp);
    }
    // Must be re-entered on every "come back from hidden" path, not only when the template changes:
    // DrawLaserBeams() calls DeactivateSystem() as soon as a tower has no links left (the panel's
    // 激光的可见性 switch, or the last peer removed). Without this the component would stay inactive
    // forever and the beams could never reappear. Activate(true) also (re)creates EmitterInstances,
    // which SetBeamSourcePoint needs -- it silently does nothing while that list is empty.
    if (!Comp->IsActive())
    {
        Comp->Activate(true);
    }
    Comp->SetVisibility(true);
    return Comp;
}

void AWirelessPowerBuildable::ApplyBeamFXColor(UParticleSystemComponent* Comp)
{
    if (!IsValid(Comp))
    {
        return;
    }
    // M_Laser drives Emissive from MaterialExpressionParticleColor (i.e. the particle's own colour),
    // which is why forcing ParticleModuleColor::StartColor on the template is what really decides the
    // beam hue. These two calls are belt-and-braces: they publish the same value as component-level
    // instance parameters, so a material that samples a named "Color" parameter also agrees.
    const FLinearColor C = GetCageColor();
    Comp->SetColorParameter(TEXT("Color"), C);
    Comp->SetVectorParameter(TEXT("Color"), FVector(C.R, C.G, C.B));
}

void AWirelessPowerBuildable::DrawLaserBeams()
{
    const bool bEnabled = !bEndingPlay && bLaserVisible && BuildingType == EWirelessBuildingType::LaserTower;
    const int32 PeerCount = bEnabled ? LaserPeers.Num() : 0;

    // ---- Feyko's particle beams (the original mechanism, preferred) ----
    if (ResolveLaserTemplate())
    {
        const int32 PerComponent = FMath::Max(1, LaserBeamsPerComponent);
        const int32 ComponentsNeeded = (PeerCount + PerComponent - 1) / PerComponent;
        const FVector SourcePoint = GetBeamPoint();

        for (int32 CompIndex = 0; CompIndex < BeamFX.Num(); ++CompIndex)
        {
            if (CompIndex >= ComponentsNeeded)
            {
                if (IsValid(BeamFX[CompIndex]))
                {
                    BeamFX[CompIndex]->DeactivateSystem();
                    BeamFX[CompIndex]->SetVisibility(false);
                }
                continue;
            }
            UParticleSystemComponent* Comp = GetBeamFXComponent(CompIndex);
            if (!IsValid(Comp))
            {
                continue;
            }
            for (int32 Slot = 0; Slot < PerComponent; ++Slot)
            {
                const int32 PeerIndex = CompIndex * PerComponent + Slot;
                if (PeerIndex >= PeerCount)
                {
                    break;
                }
                const AWirelessPowerBuildable* Peer = LaserPeers[PeerIndex];
                if (!IsValid(Peer))
                {
                    continue;
                }
                // Emitter 0 is P_Laser's single "Laser" Beam2 emitter; Slot is the beam (particle)
                // index inside it. Both points are WORLD space -- no component transform is applied.
                Comp->SetBeamSourcePoint(0, SourcePoint, Slot);
                Comp->SetBeamTargetPoint(0, Peer->GetBeamPoint(), Slot);
            }
        }

        // The geometry fallback must never render at the same time as the particle beams.
        if (IsValid(BeamISM))
        {
            BeamISM->ClearInstances();
            BeamISM->SetVisibility(false);
        }
        return;
    }

    // ---- fallback: stretched cube bars, used only when P_Laser is unavailable ----
    DrawLaserBeamBars();
}

void AWirelessPowerBuildable::DrawLaserBeamBars()
{
    if (!IsValid(BeamISM))
    {
        return;
    }
    // Real geometry: clear the old instances first, or every 2 Hz refresh stacks another bar.
    BeamISM->ClearInstances();

    if (bEndingPlay || !bLaserVisible || BuildingType != EWirelessBuildingType::LaserTower)
    {
        BeamISM->SetVisibility(false);
        return;
    }
    const UWorld* W = GetWorld();
    if (!W || W->GetNetMode() == NM_DedicatedServer)
    {
        return;
    }
    UStaticMesh* Bar = BeamISM->GetStaticMesh();
    if (!Bar)
    {
        return;
    }

    EnsureVisualMaterials();

    const FBoxSphereBounds MB = Bar->GetBounds();
    const FVector Half = MB.BoxExtent.GetAbs();
    const FVector Pivot = MB.Origin;
    if (Half.X <= 0.f || Half.Y <= 0.f || Half.Z <= 0.f)
    {
        return;
    }

    const FVector A = GetBeamPoint(); // roof of this tower, so the beam is never buried in the mesh
    int32 Drawn = 0;
    for (const AWirelessPowerBuildable* P : LaserPeers)
    {
        if (!IsValid(P))
        {
            continue;
        }
        const FVector B = P->GetBeamPoint();
        const FVector D = B - A;
        const float Len = static_cast<float>(D.Size());
        if (Len < 1.f)
        {
            continue;
        }
        // A square cross-section bar: rotate the cube's +Z onto the beam direction. Thickness grows
        // with length so a 2 km beam is never a sub-pixel sliver.
        const FQuat Q = FQuat::FindBetweenNormals(FVector::UpVector, D / Len);
        const float Thick = FMath::Clamp(Len * 0.006f, 20.f, 600.f);
        const FVector Size(Thick, Thick, Len);
        const FVector Scale(Size.X / (2.f * Half.X), Size.Y / (2.f * Half.Y), Size.Z / (2.f * Half.Z));
        const FVector Mid = (A + B) * 0.5f;
        const FVector Pos = Mid - Q.RotateVector(Scale * Pivot);
        BeamISM->AddInstance(FTransform(Q, Pos, Scale));
        ++Drawn;
    }
    BeamISM->SetVisibility(Drawn > 0);
}

bool AWirelessPowerBuildable::IsWithinRange(const AActor* Other) const
{
    if (!Other)
    {
        return false;
    }
    // Single definition of the (cubic) rule lives in FWPRangeHelper (top of this file).
    return FWPRangeHelper::WithinCubic(GetActorLocation(), WirelessRange, Other->GetActorLocation());
}

void AWirelessPowerBuildable::TearDownWirelessLinks(bool bNotifyPeers)
{
    // Remove every hidden link EXCEPT the internal visible<->antenna bridges (infrastructure that
    // must survive POWER DOWN). AddHiddenConnection/RemoveHiddenConnection are engine-symmetric, so
    // removing from our end also clears the peer/machine end -- no dangling pointers remain there.
    if (IsValid(WirelessConnection))
    {
        TArray<UFGCircuitConnectionComponent*> Current;
        WirelessConnection->GetHiddenConnections(Current);
        for (UFGCircuitConnectionComponent* C : Current)
        {
            if (!IsValid(C))
            {
                continue;
            }
            bool bIsInternalBridge = false;
            for (UWirelessPowerConnection* P : PowerConnections)
            {
                if (P == C)
                {
                    bIsInternalBridge = true;
                    break;
                }
            }
            if (bIsInternalBridge)
            {
                continue;
            }
            WirelessConnection->RemoveHiddenConnection(C);
        }
    }
    MyLinks.Empty();

    // v1.0.58: every link is gone, so the per-link repair history is meaningless -- and keeping it
    // would suppress the repair of a link that legitimately comes back after a POWER DOWN cycle.
    RepairTries.Reset();

    // Force the next ValidateWirelessMesh() to do a full reconcile rather than take the
    // "nothing changed" fast path.
    bMeshReconciled = false;
    LastReconcileTime = -1000.f;

    // Also purge our tracker entries from peers' MyLinks, otherwise their next
    // ValidateWirelessMesh pass would dereference our (dying) components.
    //
    // v1.0.56 -- "放多个继电器电网就罢工" fix. This used to walk AFGBuildableSubsystem's
    // GetAllBuildablesRef() -- i.e. EVERY buildable in the world -- and Cast each one, for
    // every relay the player placed or dismantled, and on every POWER DOWN. On a real base
    // (hundreds-to-thousands of buildables) that is an O(buildables) stall on the game thread
    // per event, plus an O(MyLinks) linear TArray::Remove per candidate, so a handful of relays
    // was enough to make the whole grid look dead. The peer scan was already moved onto the
    // wireless-node registry in v1.0.44 to kill exactly this quadratic cost -- this path was
    // simply missed. Peers can only ever be wireless nodes, so iterate that same
    // GWirelessNodes registry (a few dozen entries) instead of the world.
    if (bNotifyPeers)
    {
        TArray<AWirelessPowerBuildable*> Peers;
        GetRegisteredNodes(Peers);
        for (AWirelessPowerBuildable* Other : Peers)
        {
            if (Other && Other != this)
            {
                Other->MyLinks.Remove(WirelessConnection);
            }
        }
    }
}

void AWirelessPowerBuildable::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

    // Visualize the coverage cube when the panel's range-display switch is ON (bShowCage), and
    // never during teardown (bEndingPlay). The switch state persists after the panel closes.
    // The instances are persistent geometry, so we only rebuild when something actually changed --
    // re-emitting the dense lattice every frame would be pure waste.
    if (bShowCage && !bEndingPlay)
    {
        if (!bCageBuilt
            || !FMath::IsNearlyEqual(CageBuiltRange, WirelessRange)
            || !CageBuiltCenter.Equals(GetActorLocation(), 1.f))
        {
            RebuildRangeCage();
        }
    }

    // Laser Tower beams: redrawn at ~2 Hz (flush + rebuild) so a destroyed or re-IDed peer can
    // never leave a stale beam behind, while costing almost nothing per frame.
    if (BuildingType == EWirelessBuildingType::LaserTower && !bEndingPlay)
    {
        BeamRefreshAccum += DeltaSeconds;
        if (BeamRefreshAccum >= 0.5f)
        {
            BeamRefreshAccum = 0.f;
            DrawLaserBeams();
        }
    }

    // Server-authoritative: power circuits live on the server.
    if (!HasAuthority())
    {
        return;
    }

    // Fuse each visible port with the antenna every tick until the merge sticks. Cheap no-op
    // once fused, but self-heals after POWER DOWN cycles.
    EnsureInternalBridge();

    // v1.0.58 -- the "just landed" fast path (Feyko parity). When a buildable was constructed on
    // or next to this node, NotifyBuildableConstructed() pointed EarlyValidateAt at now+0.5 s;
    // when that deadline arrives we (a) inherit the neighbourhood's 编号 if the player never chose
    // one, then (b) validate immediately instead of waiting out the regular ~3 s cadence. Result:
    // a pole placed at the end of a row of poles is on the grid before the player finishes the
    // next build, with no panel interaction.
    const UWorld* TickWorld = GetWorld();
    const float NowTime = TickWorld ? TickWorld->GetTimeSeconds() : 0.f;
    bool bValidatedThisTick = false;
    if (EarlyValidateAt >= 0.f && NowTime >= EarlyValidateAt)
    {
        EarlyValidateAt = -1.f;
        if (bNeedsChannelAdopt)
        {
            bNeedsChannelAdopt = false;
            AutoAdoptNearestPeerChannel();
        }
        ValidateCounter = 0;
        ValidateWirelessMesh();
        bValidatedThisTick = true;
    }

    const bool bWiredPower = HasWiredPower();
    const bool bNowActive = bPowerEnabled && bWiredPower;
    if (bNowActive != bActive)
    {
        WP_LOGF(
            TEXT("[WirelessPower] %s active %d->%d (wiredPower=%d, peers=%d, machines=%d, channel='%s')"),
            *GetClass()->GetName(), bActive ? 1 : 0, bNowActive ? 1 : 0,
            bWiredPower ? 1 : 0, LinkedNodeCount, PoweredMachineCount, *Channel);
    }
    bActive = bNowActive;

    // Regular cadence. Skipped on a tick that already ran the early validation, so a freshly built
    // node never validates twice in one frame.
    if (!bValidatedThisTick && ++ValidateCounter >= VALIDATE_INTERVAL)
    {
        ValidateCounter = 0;
        ValidateWirelessMesh();
    }
}

bool AWirelessPowerBuildable::HasLineOfSightTo(const AActor* Other) const
{
    if (!Other || !GetWorld())
    {
        return false;
    }
    // Trace between the ROOFS of the two buildings, not their origins.
    // v1.0.21 bug this fixes: Start was GetActorLocation(), i.e. the building's base -- which sits
    // inside the foundation/terrain collider. A complex-collision trace that begins inside geometry
    // reports a hit immediately, so "line of sight" was false for EVERY pair and a laser link could
    // never be established (diagnostic showed peers=1 same-channel but laserPeers=0).
    const AWirelessPowerBuildable* O = Cast<AWirelessPowerBuildable>(Other);
    const FVector Start = GetBeamPoint();
    const FVector End = O ? O->GetBeamPoint() : (Other->GetActorLocation() + FVector(0.f, 0.f, 400.f));

    FHitResult Hit;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(WirelessLOS), true);
    Params.AddIgnoredActor(this);
    Params.AddIgnoredActor(Other);
    // Block on world geometry only; we want terrain/cliffs to break the beam.
    const bool bBlocked = GetWorld()->LineTraceSingleByChannel(Hit, Start, End, ECC_WorldStatic, Params);
    // A hit that merely means "the trace started inside something" must NOT count as blocked, or a
    // tower standing next to a hill keeps losing its links.
    return !bBlocked || Hit.bStartPenetrating;
}

bool AWirelessPowerBuildable::HasHiddenLink(const UFGCircuitConnectionComponent* Comp,
                                            const UFGCircuitConnectionComponent* Other)
{
    if (!IsValid(Comp) || !IsValid(Other))
    {
        return false;
    }
    // GetHiddenConnections() is header-inline and appends mHiddenConnections verbatim -- the very
    // array the engine's HasHiddenConnection() walks (verified in the shipping binary: Data +0x2a8,
    // Num +0x2b0). Equivalent answer, no UFUNCTION thunk.
    TArray<UFGCircuitConnectionComponent*> Hidden;
    Comp->GetHiddenConnections(Hidden);
    for (const UFGCircuitConnectionComponent* H : Hidden)
    {
        if (H == Other)
        {
            return true;
        }
    }
    return false;
}

bool AWirelessPowerBuildable::IsInternalBridge(const UFGCircuitConnectionComponent* C) const
{
    for (const UWirelessPowerConnection* P : PowerConnections)
    {
        if (P == C)
        {
            return true;
        }
    }
    return false;
}

void AWirelessPowerBuildable::EnsureInternalBridge()
{
    if (!HasAuthority() || !IsValid(WirelessConnection))
    {
        return;
    }
    // Portless nodes (Relay, Area Power Pole) have no internal bridge: the antenna's circuit IS
    // the node's circuit. Nothing to fuse.
    if (PowerConnections.Num() == 0)
    {
        return;
    }
    if (PortBridged.Num() != PowerConnections.Num())
    {
        PortBridged.SetNumZeroed(PowerConnections.Num());
    }

    const UWorld* W = GetWorld();
    const float Now = W ? W->GetTimeSeconds() : 0.f;

    for (int32 i = 0; i < PowerConnections.Num(); ++i)
    {
        UWirelessPowerConnection* P = PowerConnections[i];
        if (!IsValid(P))
        {
            continue;
        }
        // v1.0.34: once the bridge is confirmed fused we never touch it again. Previously this ran
        // for EVERY port on EVERY tick, because the "fused" test used HasHiddenConnection() and so
        // re-issued AddHiddenConnection() forever -- and AddHiddenConnection always ends by
        // re-merging the two circuits in the engine. Caching the confirmation removes that
        // per-tick circuit churn outright.
        if (PortBridged[i])
        {
            continue;
        }
        // Throttle the not-yet-fused retry: a fresh buildable's connections get their circuits a
        // little after BeginPlay, and there is no point hammering the circuit subsystem at frame
        // rate until that happens.
        if (Now - LastBridgeAttemptTime < BRIDGE_RETRY_PERIOD)
        {
            continue;
        }

        const int32 PortCircuit = P->GetCircuitID();
        const int32 AntCircuit = WirelessConnection->GetCircuitID();
        const bool bLinked = HasHiddenLink(P, WirelessConnection);
        const bool bMerged = bLinked && PortCircuit != INDEX_NONE && PortCircuit == AntCircuit;
        if (bMerged)
        {
            PortBridged[i] = 1; // confirmed for good (re-verified on each slow reconcile)
            continue;           // fused, nothing to do
        }

        // A link that exists but whose circuits never merged (a restored SaveGame hidden
        // connection can sit on a different circuit forever) has to be dropped before re-adding:
        // AddHiddenConnection is a no-op on an entry that is already there, and it is the
        // circuit-merge step that actually fuses them.
        if (bLinked)
        {
            P->RemoveHiddenConnection(WirelessConnection);
        }
        // "Connects both ends of the connection and the circuits" -- but only works once both ends
        // actually HAVE circuits, hence the Tick-time deferral (see BeginPlay note).
        P->AddHiddenConnection(WirelessConnection);
        LastBridgeAttemptTime = Now;

        if (!bBridgeLogged)
        {
            WP_LOGF(
                TEXT("[WirelessPower] %s internal bridge (port %s): linked=%d portCircuit=%d antCircuit=%d"),
                *GetClass()->GetName(), *P->GetName(), bLinked ? 1 : 0, PortCircuit, AntCircuit);
            bBridgeLogged = true;
        }
    }
}

bool AWirelessPowerBuildable::HasWiredPower() const
{
    // v1.0.58: "any visible port OR the antenna". Previously a port-bearing node was powered iff a
    // VISIBLE port had power, and only a portless node consulted the antenna. That was fine while
    // the Area Power Pole had no visible port at all, but it now has one (Feyko parity -- his pole
    // ships a real `FGPowerConnection`), and a pole with nothing plugged into that port but an
    // energized wireless mesh behind it must NOT read as dead.
    //
    // The two are the same circuit anyway: EnsureInternalBridge() fuses every visible port to the
    // antenna with a hidden link, so in practice this can only ever turn a false negative into a
    // correct positive.
    for (const UWirelessPowerConnection* P : PowerConnections)
    {
        if (IsValid(P) && P->HasPower())
        {
            return true;
        }
    }
    return IsValid(WirelessConnection) && WirelessConnection->HasPower();
}

void AWirelessPowerBuildable::CollectMachinesInRange(TArray<UFGPowerConnectionComponent*>& OutMachineConns) const
{
    if (!bBroadcastsToMachines)
    {
        return;
    }
    UWorld* W = GetWorld();
    if (!W)
    {
        return;
    }
    AFGBuildableSubsystem* Sub = AFGBuildableSubsystem::Get(W);
    if (!Sub)
    {
        return;
    }

    // v1.0.44 scale fix: this walk is O(all buildables) and every Area Power Pole used to do its
    // own, on every validation pass. Refresh it at most once per WP_MACHINE_SCAN_PERIOD per world
    // and share the result between all poles in that window. The cache holds weak pointers, so a
    // dismantled machine simply drops out of it.
    const float Now = W->GetTimeSeconds();
    const bool bCacheValid = (GMachineCache.CachedWorld == W)
        && (Now - GMachineCache.LastScanTime) < WP_MACHINE_SCAN_PERIOD;

    if (!bCacheValid)
    {
        GMachineCache.CachedWorld = W;
        GMachineCache.LastScanTime = Now;
        GMachineCache.Entries.Reset();

        for (AFGBuildable* B : Sub->GetAllBuildablesRef())
        {
            // Only vanilla (non-WirelessPower) machines get wireless power.
            if (Cast<AWirelessPowerBuildable>(B))
            {
                continue;
            }
            // v1.0.58 -- Feyko's isClassBanned(): a denylist of buildables that must never be
            // auto-wired into the mesh (his original banned the vanilla wall power poles). Applied
            // to every candidate rather than to the caller, so the ban holds wherever the target
            // sits relative to whichever pole happens to be scanning.
            if (IsClassBannedForWireless(B))
            {
                continue;
            }
            // v1.0.45 BUGFIX ("杆子连不上机器"): NO range filtering in the scan! This cache is
            // SHARED by every pole in the world -- v1.0.44 filtered by the SCANNING pole's cube
            // here, so every OTHER pole read a cache full of somebody else's machines and
            // published nothing ("已连 0 节点 · 供电 0 机器"). The per-pole cube test belongs to
            // the publish loop below (which already does it against each pole's own location).
            TArray<UFGPowerConnectionComponent*> Comps;
            B->GetComponents<UFGPowerConnectionComponent>(Comps);
            for (UFGPowerConnectionComponent* C : Comps)
            {
                if (!IsValid(C))
                {
                    continue;
                }
                if (C->IsHidden())
                {
                    continue; // skip hidden ports (e.g. powered walls)
                }
                if (C->GetPowerInfo() == nullptr)
                {
                    continue; // not an actual power port
                }
                GMachineCache.Entries.Add({C, nullptr});
            }
        }

        // NEAREST-POLE OWNERSHIP: decide, per machine, which pole owns it. A machine covered by
        // several poles joins the geometrically NEAREST active pole that broadcasts to machines
        // (Area Power Pole), NOT whichever validated first. Channel-independent (machines never
        // carry a 编号). Recomputed every cache refresh (~2 s); cost is machines x poles and poles
        // are only a few dozen, so this is negligible. This makes overlap ownership deterministic
        // and stable across reloads -- the old first-come rule made it arbitrary and flip-floppy.
        TArray<AWirelessPowerBuildable*> Nodes;
        AWirelessPowerBuildable::GetRegisteredNodes(Nodes);
        for (FWPMachineCache::FMachineEntry& E : GMachineCache.Entries)
        {
            UFGPowerConnectionComponent* C = E.Conn.Get();
            if (!IsValid(C))
            {
                continue;
            }
            AActor* PortActor = C->GetOwner();
            if (!PortActor)
            {
                continue;
            }
            AWirelessPowerBuildable* Best = nullptr;
            float BestD = TNumericLimits<float>::Max();
            for (AWirelessPowerBuildable* P : Nodes)
            {
                if (!IsValid(P) || !P->BroadcastsToMachines() || !P->IsPoweredOn())
                {
                    continue;
                }
                if (!FWPRangeHelper::WithinCubic(P->GetActorLocation(), P->WirelessRange, PortActor->GetActorLocation()))
                {
                    continue;
                }
                const float D = FVector::DistSquared(P->GetActorLocation(), PortActor->GetActorLocation());
                if (D < BestD)
                {
                    BestD = D;
                    Best = P;
                }
            }
            E.NearestPole = Best;
        }

        int32 Owned = 0;
        for (const FWPMachineCache::FMachineEntry& E : GMachineCache.Entries)
        {
            if (E.NearestPole.IsValid())
            {
                ++Owned;
            }
        }
        WP_LOGF( TEXT("[WirelessPower] machine cache refreshed: %d candidate port(s), %d owned by a pole"),
            GMachineCache.Entries.Num(), Owned);
    }

    // Publish the cached connectors that are inside OUR cube. Ownership is decided by NEAREST pole
    // (see cache refresh), so this pole only feeds a machine when it is that machine's recorded
    // nearest covering pole. If a different pole is nearer we step back -- and if we currently hold
    // the link, our removal pass below releases it so the nearer pole takes over. This replaces the
    // old first-come-first-served rule, which made overlap ownership arbitrary and (across reloads)
    // unstable. Channel-independent: a machine never carries a 编号.
    for (const FWPMachineCache::FMachineEntry& E : GMachineCache.Entries)
    {
        UFGPowerConnectionComponent* C = E.Conn.Get();
        if (!IsValid(C))
        {
            continue;
        }
        AActor* PortActor = C->GetOwner();
        if (!PortActor || PortActor == this)
        {
            continue;
        }
        if (!FWPRangeHelper::WithinCubic(GetActorLocation(), WirelessRange, PortActor->GetActorLocation()))
        {
            continue;
        }
        // Only the recorded nearest covering pole feeds this machine.
        const AWirelessPowerBuildable* Nearest = E.NearestPole.Get();
        if (Nearest != this)
        {
            // If we currently hold this machine but are no longer the nearest pole, the removal
            // pass below releases it so the nearer pole can take over. Log that handoff (Verbose,
            // off by default -- enable verbose logging to debug "machine in my cage but no power").
            if (HasHiddenLink(WirelessConnection, C))
            {
                UE_LOG(LogWirelessPower, Verbose,
                       TEXT("[WirelessPower] %s releasing machine %s: nearer pole %s now owns it"),
                       *GetClass()->GetName(), *PortActor->GetClass()->GetName(),
                       Nearest ? *Nearest->GetClass()->GetName() : TEXT("(none)"));
            }
            continue;
        }
        OutMachineConns.Add(C);
    }
}

void AWirelessPowerBuildable::ValidateWirelessMesh()
{
    // Server-authoritative: hidden connections exist only on the server; they are not
    // replicated and building them on a client would corrupt its local circuit state.
    if (!HasAuthority() || !IsValid(WirelessConnection))
    {
        return;
    }

    // POWER DOWN: keep the *external* mesh empty until the panel re-enables this node. The internal
    // port<->antenna bridges are infrastructure and always stay up; only the peer/machine links go.
    if (!bPowerEnabled)
    {
        // v1.0.56: only tear down while links actually remain. This runs from Tick() via
        // ValidateWirelessMesh(), and a node sitting in POWER DOWN is its normal resting state --
        // re-walking GetHiddenConnections() every tick to remove nothing was pure waste.
        // Test the connection itself rather than the MyLinks cache, so a stale tracker can never
        // strand a real hidden link.
        TArray<UFGCircuitConnectionComponent*> StillLinked;
        if (IsValid(WirelessConnection))
        {
            WirelessConnection->GetHiddenConnections(StillLinked);
        }
        bool bAnyExternal = false;
        for (const UFGCircuitConnectionComponent* C : StillLinked)
        {
            if (!IsValid(C))
            {
                continue;
            }
            bool bIsInternalBridge = false;
            for (const UWirelessPowerConnection* P : PowerConnections)
            {
                if (P == C)
                {
                    bIsInternalBridge = true;
                    break;
                }
            }
            if (!bIsInternalBridge)
            {
                bAnyExternal = true;
                break;
            }
        }
        if (bAnyExternal)
        {
            TearDownWirelessLinks(false);
        }
        LinkedNodeCount = 0;
        PoweredMachineCount = 0;
        LaserPeers.Empty(); // no links -> no beams
        return;
    }

    // Keep the internal port<->antenna bridges alive (infrastructure, always on).
    EnsureInternalBridge();

    // ---- Incremental mesh update ----
    // Compute the DESIRED connection set (peers + machines), then add missing / remove stale.
    // Tearing everything down and rebuilding every pass split + re-merged the circuit and briefly
    // dropped power each cycle.
    TSet<UFGCircuitConnectionComponent*> Desired;
    TArray<UFGPowerConnectionComponent*> DesiredMachines;

    // ---- PEER SCAN (v1.0.44) ----
    // Iterate the shared registry of WIRELESS NODES only, not every buildable in the world. This is
    // the fix for "无线电杆多起来后不稳定": the old loop was O(nodes x buildables) per pass, and a
    // real base has hundreds-to-thousands of buildables that can never be a wireless peer.
    TArray<AWirelessPowerBuildable*> Nodes;
    GetRegisteredNodes(Nodes);

    // v1.0.58 -- which peers survive MaxLinks is decided by DISTANCE, nearest first, exactly the
    // rule Feyko used to pick poles (`GetDistanceTo` + `Less_FloatFloat`). The pre-v1.0.58 code
    // took the first MaxLinks entries in REGISTRY ORDER -- i.e. in the order nodes happened to be
    // built/loaded -- so a busy pole's kept set was arbitrary and could be reshuffled by an
    // unrelated registration, which then showed up as the mesh re-merging for no visible reason.
    //
    // Manual Laser Tower links are explicit player intent and never compete for the cap, so they
    // are tracked separately and always kept.
    struct FPeerCandidate
    {
        AWirelessPowerBuildable* Node = nullptr;
        float DistSq = 0.f;
        bool bManual = false;
    };
    TArray<FPeerCandidate> AutoPeers;
    TArray<FPeerCandidate> ManualPeers;

    const FVector MyLocation = GetActorLocation();
    for (AWirelessPowerBuildable* Other : Nodes)
    {
        if (!IsValid(Other) || Other == this)
        {
            continue;
        }
        // v1.0.58 -- SYMMETRIC range test: either node's cube covering the other is enough.
        //
        // This is both what Feyko's original does and a hard requirement for stability. His
        // InitGameWorld asks "is the NEW buildable within Range of a pole?", i.e. the newcomer's
        // reach counts, while each pole's own ValidateAreaConnections() asks about its own reach --
        // a union of the two directions.
        //
        // Stability: the reconcile pass below REMOVES any hidden link that is not in this node's
        // desired set, so a one-directional test makes two nodes of DIFFERENT tiers fight forever.
        // Place an Mk3 (48 m) beside an Mk1 (12 m) at 30 m: the Mk3 sees the Mk1 and links, then
        // the Mk1 -- whose 12 m cube does not contain the Mk3 -- deletes that link on its next pass,
        // then the Mk3 re-adds it, and so on: a permanent link/power flicker. With the union the
        // relation is symmetric, both nodes compute the SAME set, and neither ever deletes what the
        // other wants.
        if (!IsWithinRange(Other) && !Other->IsWithinRange(this))
        {
            continue;
        }
        if (!Other->IsPoweredOn())
        {
            continue; // a node in POWER DOWN leaves the mesh entirely
        }
        // Peer wireless node: link antenna<->antenna on the same channel -- PLUS, for the
        // Laser Tower, any ID the player wired up manually from the panel (manual links
        // deliberately ignore the auto channel match; line of sight is still enforced below).
        // v1.0.52: 编号比较改为字符串规范化比较（FWirelessId::Equal），汉字/全角/大小写均可对上。
        // v1.1.1 (T02 fix): 手动链路判定改为对称 —— A 手动连 B **或** B 手动连 A 任一成立即可。
        // 旧逻辑只查本塔 ManualLinkChannels：单边配对时 A 校验加边、B 校验删边无限循环，
        // B 端间歇断电（约 1/15 时间有电）而光束因 LaserPeers 复制与电路解耦仍可见。
        // 与 v1.0.44 自动链路的 union 修复同理：两端算出同一个集合，谁也不删对方想要的边。
        // 注意：这里绝不引入"对方当前有电"作为条件——那会造成接收入站连接的死锁。
        const bool bManualLink = bNeedsLineOfSight
            && !FWirelessId::Equal(Other->Channel, Channel)
            && (HasManualLaserLink(Other->Channel) || Other->HasManualLaserLink(Channel));
        if (!bManualLink && !FWirelessId::Equal(Other->Channel, Channel))
        {
            continue;
        }
        if (!IsValid(Other->WirelessConnection))
        {
            continue;
        }
        // Laser Tower links (either end) require an unobstructed line of sight.
        if ((bNeedsLineOfSight || Other->bNeedsLineOfSight) && !HasLineOfSightTo(Other))
        {
            continue;
        }

        FPeerCandidate Cand;
        Cand.Node = Other;
        Cand.DistSq = FVector::DistSquared(MyLocation, Other->GetActorLocation());
        Cand.bManual = bManualLink;
        (bManualLink ? ManualPeers : AutoPeers).Add(Cand);
    }

    // Nearest first. Only the AUTO set is capped, and only when it is actually over the cap, so a
    // normal cluster (fewer than MaxLinks peers) keeps every in-range peer and this ordering never
    // changes which links exist.
    AutoPeers.Sort([](const FPeerCandidate& A, const FPeerCandidate& B)
    {
        return A.DistSq < B.DistSq;
    });

    for (const FPeerCandidate& Cand : AutoPeers)
    {
        if (Desired.Num() >= MaxLinks)
        {
            break; // auto cap reached; the FARTHEST peers are the ones dropped
        }
        Desired.Add(Cand.Node->WirelessConnection);
    }
    // Manual links are additive: they are kept even when the auto set is already at the cap.
    for (const FPeerCandidate& Cand : ManualPeers)
    {
        Desired.Add(Cand.Node->WirelessConnection);
    }

    // ---- MACHINE SCAN (Area Power Pole only) ----
    // Machines are NOT wireless nodes, so they still need a walk over the buildables. That walk is
    // shared: one scan per WP_MACHINE_SCAN_PERIOD per world, cached, instead of one per pole per
    // pass (which was the other half of the quadratic cost).
    if (bBroadcastsToMachines)
    {
        CollectMachinesInRange(DesiredMachines);
    }

    // ---- Decide whether the link graph needs touching at all ----
    // v1.0.34 (供电不稳定 fix). The destructive part of validation is NOT the scan above -- it is
    // the Add/Remove calls below, because the engine's AddHiddenConnection() ALWAYS finishes by
    // re-merging the two circuits ("Connects both ends of the connection and the circuits"; the
    // shipping binary confirms the circuit-connect routine is tail-called unconditionally, after
    // the two AddUnique() calls). Re-issuing Add for a link that already exists therefore splits
    // and re-merges the wireless grid, dropping its power for a beat.
    //
    // The old code did that for every peer, every pass. That is exactly the reported symptom: the
    // game log shows bActive flipping once per validation pass (664x 0->1 / 648x 1->0 across four
    // node types, i.e. one flicker per pass per node) and every single peer re-linked with
    // repaired=0 on every single pass.
    //
    // So reconcile ONLY when the desired set actually changed -- a peer was built, dismantled,
    // came into range, switched channel, or line of sight opened/closed -- or when the slow
    // periodic re-check comes due. In between, validation costs the scan and nothing else.
    TSet<UFGCircuitConnectionComponent*> Want;
    for (UFGCircuitConnectionComponent* L : Desired)
    {
        if (IsValid(L))
        {
            Want.Add(L);
        }
    }
    for (UFGPowerConnectionComponent* M : DesiredMachines)
    {
        if (IsValid(M))
        {
            Want.Add(M);
        }
    }

    // MyLinks is the tracker we wrote on the last reconcile, so it doubles as the "already applied"
    // cache (no second array to keep in sync).
    bool bSameAsApplied = (Want.Num() == MyLinks.Num());
    if (bSameAsApplied)
    {
        for (UFGCircuitConnectionComponent* A : MyLinks)
        {
            if (!IsValid(A) || !Want.Contains(A))
            {
                bSameAsApplied = false;
                break;
            }
        }
    }

    const UWorld* W = GetWorld();
    const float Now = W ? W->GetTimeSeconds() : 0.f;
    const bool bSlowDue = !bMeshReconciled || (Now - LastReconcileTime) >= FULL_RECONCILE_PERIOD;

    LinkedNodeCount = Desired.Num();
    PoweredMachineCount = DesiredMachines.Num();

    if (bSameAsApplied && !bSlowDue)
    {
        // Steady state: make NO engine calls at all. RefreshLaserPeers() is element-wise and
        // idempotent, so it costs nothing and keeps the beams/panel in step.
        RefreshLaserPeers();
        return;
    }

    // A slow pass re-verifies the internal bridges as well (they are cached as fused until then).
    if (bSlowDue)
    {
        for (uint8& Bridged : PortBridged)
        {
            Bridged = 0;
        }
        EnsureInternalBridge();
    }

    // Remove links that are no longer desired (internal bridges are never in Want, and are
    // explicitly preserved).
    TArray<UFGCircuitConnectionComponent*> Current;
    WirelessConnection->GetHiddenConnections(Current);
    for (UFGCircuitConnectionComponent* C : Current)
    {
        if (!IsValid(C) || IsInternalBridge(C))
        {
            continue;
        }
        if (!Want.Contains(C))
        {
            WirelessConnection->RemoveHiddenConnection(C);
        }
    }

    // Add missing PEER links. A link that is already present is left strictly alone (re-issuing Add
    // would re-merge the circuits; see the note above). On a slow pass we additionally repair a
    // link that exists structurally but whose circuits never actually merged -- a restored
    // SaveGame hidden connection can sit on a different circuit forever -- and the only way to fix
    // that is to drop it and let Add merge it.
    for (UFGCircuitConnectionComponent* L : Desired)
    {
        if (!IsValid(L))
        {
            continue;
        }
        const bool bHasLink = HasHiddenLink(WirelessConnection, L);
        bool bFix = !bHasLink;
        if (bHasLink && bSlowDue)
        {
            const int32 SelfCircuit = WirelessConnection->GetCircuitID();
            const int32 PeerCircuit = L->GetCircuitID();
            if (SelfCircuit == INDEX_NONE || PeerCircuit == INDEX_NONE || SelfCircuit != PeerCircuit)
            {
                bFix = true;
            }
        }
        if (!bFix)
        {
            // Healthy link: forget any earlier repair failures so a link that later breaks gets a
            // full set of attempts again.
            RepairTries.Remove(L);
            continue;
        }
        // v1.0.58 -- repair BACKOFF. Give up on a link that just will not merge.
        //
        // A structurally present link whose two ends never share a circuit (a restored SaveGame
        // hidden connection can sit on its own circuit forever) used to be dropped and re-added on
        // EVERY slow reconcile, i.e. every FULL_RECONCILE_PERIOD. AddHiddenConnection() always ends
        // by re-merging the two circuits, so each attempt briefly tears the whole wireless grid's
        // power down -- a single unmergeable link therefore flickered the entire network every
        // 15 s. That is bug #2 (周期性掉电). After MAX_REPAIR_TRIES we stop touching it: the link
        // is still there, and the engine picks it up by itself the next time either circuit really
        // changes. We only skip the DESTRUCTIVE retry -- the link is never removed for this reason.
        int32& Attempts = RepairTries.FindOrAdd(L);
        if (bHasLink && Attempts >= MAX_REPAIR_TRIES)
        {
            continue;
        }
        ++Attempts;

        if (bHasLink)
        {
            WirelessConnection->RemoveHiddenConnection(L);
        }
        WirelessConnection->AddHiddenConnection(L);
        const int32 SelfCircuit = WirelessConnection->GetCircuitID();
        const int32 PeerCircuit = L->GetCircuitID();
        // v1.1.3 [C1]: L->GetOwner() 可能为 null（组件在 UnregisterComponent / 引擎早期销毁的瞬态窗口里被遍历到），
        // 直接解引用会裸 nullptr 崩溃。先取本地指针再判空。
        AActor* PeerOwner = L->GetOwner();
        WP_LOGF(
            TEXT("[WirelessPower] %s linked peer %s (ch='%s' selfCircuit=%d peerCircuit=%d repaired=%d attempt=%d)"),
            *GetClass()->GetName(), PeerOwner ? *PeerOwner->GetClass()->GetName() : TEXT("?"), *Channel,
            SelfCircuit, PeerCircuit, bHasLink ? 1 : 0, Attempts);
    }

    // Add missing MACHINE links (broadcast nodes only).
    for (UFGPowerConnectionComponent* M : DesiredMachines)
    {
        if (!IsValid(M))
        {
            continue;
        }
        if (HasHiddenLink(WirelessConnection, M))
        {
            continue;
        }
        // Diagnostic: if the machine was already linked to a DIFFERENT pole's antenna, we are
        // taking it over (nearest-pole ownership). Report the handoff at Log level so a "machine
        // inside my cage but no power" case is explainable from the log. Otherwise log the normal
        // "broadcasting to machine" line (was WP_LOGF, now always-on and enriched).
        bool bTakenOver = false;
        FString PrevOwner;
        {
            TArray<UFGCircuitConnectionComponent*> MH;
            M->GetHiddenConnections(MH);
            for (UFGCircuitConnectionComponent* H : MH)
            {
                if (H != WirelessConnection)
                {
                    bTakenOver = true;
                    if (AActor* O = H->GetOwner())
                    {
                        PrevOwner = O->GetClass()->GetName();
                    }
                    break;
                }
            }
        }
        WirelessConnection->AddHiddenConnection(M);
        if (bTakenOver)
        {
            // v1.1.3 [C1]: M->GetOwner() 判空守卫。
            AActor* MachOwner = M->GetOwner();
            UE_LOG(LogWirelessPower, Log,
                   TEXT("[WirelessPower] %s took over machine %s from %s (nearest-pole ownership, ch='%s')"),
                   *GetClass()->GetName(), MachOwner ? *MachOwner->GetClass()->GetName() : TEXT("?"),
                   PrevOwner.IsEmpty() ? TEXT("?") : *PrevOwner, *Channel);
        }
        else
        {
            // v1.1.3 [C1]: M->GetOwner() 判空守卫。
            AActor* MachOwner2 = M->GetOwner();
            UE_LOG(LogWirelessPower, Log,
                   TEXT("[WirelessPower] %s broadcasting to machine %s (ch='%s')"),
                   *GetClass()->GetName(), MachOwner2 ? *MachOwner2->GetClass()->GetName() : TEXT("?"), *Channel);
        }
    }

    // Tracker mirrors the desired set exactly (peers + machines), for clean teardown + HUD counts.
    // This doubles as the cache the next pass compares against.
    MyLinks.Reset();
    for (UFGCircuitConnectionComponent* L : Desired)
    {
        MyLinks.Add(L);
    }
    for (UFGPowerConnectionComponent* M : DesiredMachines)
    {
        MyLinks.Add(M);
    }

    // Drop repair bookkeeping for links we no longer keep, so the map cannot grow without bound and
    // a peer that leaves and comes back starts from a clean slate.
    for (auto It = RepairTries.CreateIterator(); It; ++It)
    {
        UFGCircuitConnectionComponent* Link = It.Key().Get();
        if (!IsValid(Link) || !Want.Contains(Link))
        {
            It.RemoveCurrent();
        }
    }

    bMeshReconciled = true;
    LastReconcileTime = Now;

    // Keep the replicated laser-peer list in step (drives the beams + the panel's 已连 marker).
    RefreshLaserPeers();

    const int32 PortCircuit = (PowerConnections.Num() > 0 && IsValid(PowerConnections[0]))
        ? PowerConnections[0]->GetCircuitID() : -1;
    const int32 AntCircuit = (IsValid(WirelessConnection)) ? WirelessConnection->GetCircuitID() : -1;
    const bool bSameCircuit = PortCircuit != INDEX_NONE && PortCircuit == AntCircuit;
    UE_LOG(LogWirelessPower, Verbose,
        TEXT("[WirelessPower] %s ch='%s' validated: %d peer(s), %d machine(s), portCircuit=%d antCircuit=%d internalMerged=%d hasPower=%d"),
        *GetClass()->GetName(), *Channel, Desired.Num(), DesiredMachines.Num(),
        PortCircuit, AntCircuit, bSameCircuit ? 1 : 0, bActive ? 1 : 0);
}
