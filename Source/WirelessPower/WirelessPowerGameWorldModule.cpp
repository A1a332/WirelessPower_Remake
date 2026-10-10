#include "WirelessPowerGameWorldModule.h"
#include "Registration.h"
#include "WirelessPowerRCO.h"
#include "WirelessPowerBuildable.h"
#include "FGBuildableSubsystem.h"
#include "FGGameMode.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogWirelessPower, Log, All);

#include "WirelessPowerLog.h"

UWirelessPowerGameWorldModule::UWirelessPowerGameWorldModule()
{
    // Mark as the root module for this mod so SML discovers and loads it.
    bRootModule = true;

    // Feyko's original progression: one milestone per tier, living in the vanilla Logistics
    // category at tech tiers 2 / 4 / 6. Registering them here puts them into the HUB, where the
    // vanilla tier gating makes the player buy them in order and pay resources for each one.
    mSchematics.Add(UWirelessPowerMilestoneMk1::StaticClass());
    mSchematics.Add(UWirelessPowerMilestoneMk2::StaticClass());
    mSchematics.Add(UWirelessPowerMilestoneMk3::StaticClass());
}

void UWirelessPowerGameWorldModule::DispatchLifecycleEvent(ELifecyclePhase Phase)
{
    // CRITICAL: the SML base implementation is what pushes mSchematics into UModContentRegistry,
    // and it does that from its own INITIALIZATION branch. Overriding this function without
    // chaining up means the registry never learns about our milestones, so they would never show
    // up in the HUB. Always let the base class run first.
    Super::DispatchLifecycleEvent(Phase);

    // CONSTRUCTION is dispatched before INITIALIZATION, and INITIALIZATION is when SML pushes
    // mSchematics into UModContentRegistry. That registration path reads UFGSchematic::GetCost() to
    // auto-register the items a schematic references, so the categories and costs have to be on the
    // CDOs before that happens. Doing it here also keeps the HUB's own "available schematics" query
    // (which runs at WorldBeginPlay) seeing fully populated milestones.
    if (Phase == ELifecyclePhase::CONSTRUCTION)
    {
        WirelessPower::ApplyMilestoneData();
    }

    // POST_INITIALIZATION: world + FactoryGame subsystems are fully up. Safe to register.
    if (Phase == ELifecyclePhase::POST_INITIALIZATION)
    {
        UWorld* World = GetWorld();
        if (World)
        {
            WP_LOGF( TEXT("Wireless Power: registering buildables…"));
            WirelessPower::RegisterBuildables(World);

            // Register the control-panel RCO on the game mode. This spawns an instance on
            // every current and future player controller so panel clicks can reach the
            // server (buildables are server-owned and can't take client RPCs directly).
            if (AFGGameMode* GM = Cast<AFGGameMode>(World->GetAuthGameMode()))
            {
                GM->RegisterRemoteCallObjectClass(UWirelessPowerRCO::StaticClass());
                WP_LOGF( TEXT("Wireless Power: RCO registered on game mode."));
            }

            // v1.0.58 -- Feyko parity: subscribe to the global "a buildable was constructed" hook,
            // which is exactly what the original mod's `InitGameWorld` blueprint does. This is what
            // makes a freshly placed pole join the grid on its own instead of waiting for its own
            // ~3 s validation cadence.
            if (AFGBuildableSubsystem* BuildableSubsystem = AFGBuildableSubsystem::Get(World))
            {
                // v1.0.59 -- make the auto-join hook binding idempotent. If SML reuses this module
                // instance when a world is destroyed and re-created, the old world's subsystem is gone
                // but bConstructedDelegateBound would still be true, so AddDynamic would never run again
                // and freshly placed poles would silently fail to auto-join on reload. Always clear first.
                BuildableSubsystem->BuildableConstructedGlobalDelegate.RemoveDynamic(
                    this, &UWirelessPowerGameWorldModule::OnBuildableConstructedGlobal);
                BuildableSubsystem->BuildableConstructedGlobalDelegate.AddDynamic(
                    this, &UWirelessPowerGameWorldModule::OnBuildableConstructedGlobal);
                bConstructedDelegateBound = true;

                // One-shot, always logged (not WP_LOGF): this line is the ground truth for
                // "did the自动入网 hook actually get installed in the build the game loaded?"
                // It fires once per world, so it cannot flood the log.
                UE_LOG(LogWirelessPower, Log,
                       TEXT("Wireless Power: subscribed to BuildableConstructedGlobalDelegate (落地即自动入网 armed)."));
            }
            else
            {
                UE_LOG(LogWirelessPower, Warning,
                       TEXT("Wireless Power: FGBuildableSubsystem unavailable, auto-join hook NOT installed."));
            }

            // v1.0.59 -- PERF: arm the load-complete flag a few seconds out. Until then,
            // OnBuildableConstructedGlobal short-circuits, so the savegame-load construction storm
            // (thousands of buildables x node list) does not stall the load. Nodes self-validate from
            // BeginPlay, so coverage is unaffected; live building after the timer works as before.
            World->GetTimerManager().SetTimer(
                LoadCompleteTimer, this,
                &UWirelessPowerGameWorldModule::MarkWorldLoadComplete, 4.0f, false);
        }
        else
        {
            UE_LOG(LogWirelessPower, Warning, TEXT("Wireless Power: no world at POST_INITIALIZATION, skip registration."));
        }
    }
}

void UWirelessPowerGameWorldModule::MarkWorldLoadComplete()
{
    bWorldLoadComplete = true;
}

void UWirelessPowerGameWorldModule::OnBuildableConstructedGlobal(AFGBuildable* Buildable)
{
    // v1.0.59 -- PERF: skip the redundant per-buildable notification storm during a savegame load.
    // Every node already self-validates from its own BeginPlay, so re-doing it here for all the
    // thousands of loaded buildables only costs load time. After bWorldLoadComplete (a few seconds
    // post POST_INITIALIZATION) this hook is fully live for newly placed buildings.
    if (!bWorldLoadComplete)
    {
        return;
    }

    // The delegate is global and fires for EVERY buildable or decor, on whichever side it was
    // constructed -- including during a savegame load. We only hand it to the wireless nodes, which
    // are a few dozen at most and do the cheap "does this concern me?" test themselves. Client
    // nodes no-op inside NotifyBuildableConstructed (power circuits are server state).
    if (!IsValid(Buildable))
    {
        return;
    }

    TArray<AWirelessPowerBuildable*> Nodes;
    AWirelessPowerBuildable::GetRegisteredNodes(Nodes);
    for (AWirelessPowerBuildable* Node : Nodes)
    {
        if (IsValid(Node))
        {
            Node->NotifyBuildableConstructed(Buildable);
        }
    }
}
