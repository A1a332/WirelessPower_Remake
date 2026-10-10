#pragma once

#include "Module/GameWorldModule.h"
#include "WirelessPowerGameWorldModule.generated.h"

/** UFUNCTION parameter of the global construction hook (forward declaration is enough -- the
 *  handler only forwards the pointer). */
class AFGBuildable;

/**
 * SML auto-discovers this native class as a root game-world module (bRootModule) and
 * calls DispatchLifecycleEvent during world initialization. POST_INITIALIZATION is used
 * so the FactoryGame subsystems (e.g. AFGRecipeManager) are guaranteed to exist.
 */
UCLASS()
class UWirelessPowerGameWorldModule : public UGameWorldModule
{
    GENERATED_BODY()
public:
    UWirelessPowerGameWorldModule();

    virtual void DispatchLifecycleEvent(ELifecyclePhase Phase) override;

    /** v1.0.58 -- "落地即自动入网", exactly Feyko's `InitGameWorld` wiring.
     *
     *  Bound to AFGBuildableSubsystem::BuildableConstructedGlobalDelegate:
     *      DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam( FOnBuildableConstructedGlobal,
     *                                                   AFGBuildable*, buildable );
     *      // "Broadcast when a buildable or decor has been constructed. Regardless of who built it"
     *
     *  We forward every constructed buildable to every registered wireless node; each node then
     *  decides locally whether it cares (it is the new building itself, or the new building landed
     *  inside its coverage) and schedules a validation ~0.5 s out -- the same beat as the original's
     *  `Delay (Duration = 0.500000) -> ValidateAreaConnections()`.
     *
     *  The delegate has ONE parameter, so this signature is fixed; it must be a UFUNCTION(). */
    UFUNCTION()
    void OnBuildableConstructedGlobal(AFGBuildable* Buildable);

private:
    /** Guards against a second AddDynamic (POST_INITIALIZATION is only dispatched once today, but a
     *  duplicate binding would make every construction notify twice). */
    bool bConstructedDelegateBound = false;

    /** v1.0.59 -- PERF: during a savegame load the global construction delegate fires for EVERY
     *  buildable (see OnBuildableConstructedGlobal). We skip that storm until the world has settled,
     *  because every node already self-validates from its own BeginPlay. Armed a few seconds after
     *  POST_INITIALIZATION so live building still auto-joins the grid. */
    bool bWorldLoadComplete = false;

    /** Timer that flips bWorldLoadComplete on once the initial load storm is over. */
    FTimerHandle LoadCompleteTimer;

    void MarkWorldLoadComplete();
};
