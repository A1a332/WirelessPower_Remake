#pragma once

#include "CoreMinimal.h"
#include "Resources/FGBuildingDescriptor.h"
#include "FGRecipe.h"
#include "Buildables/FGBuildable.h"
#include "Hologram/FGBuildableHologram.h"
#include "FGBuildCategory.h"
#include "FGBuildSubCategory.h"
#include "FGSchematic.h"
#include "Unlocks/FGUnlockRecipe.h"
#include "WirelessPowerTypes.h"
#include "WirelessPowerHologram.h"
#include "Registration.generated.h"

class AWirelessPowerRelay;
class AWirelessPowerRelayMk2;
class AWirelessPowerRelayMk3;
class APowerConverter;
class APowerConverterMk2;
class APowerConverterMk3;
class AAreaPowerPole;
class AAreaPowerPoleMk2;
class AAreaPowerPoleMk3;
class ALaserTower;
class AWirelessPowerHologram;
class UWirelessPowerMilestone;

/**
 * Shared building descriptor for every Wireless Power item. Each item subclass only sets its
 * buildable class, display name/description and icon type, then calls InitCommon() which wires
 * up the build category + sub-category via reflection.
 */
UCLASS()
class WIRELESSPOWER_API UWirelessPowerDescriptor : public UFGBuildingDescriptor
{
    GENERATED_BODY()
public:
    UWirelessPowerDescriptor();

    virtual TSubclassOf<AFGHologram> GetHologramClassInternal() const override;
    virtual FText GetItemNameInternal() const override;
    virtual FText GetItemDescriptionInternal() const override;

protected:
    TSubclassOf<AFGBuildable> BuildableClass;
    TSubclassOf<AFGHologram> HologramClass;
    FText DisplayName;
    FText Desc;
    EWirelessBuildingType IconType = EWirelessBuildingType::Relay;

    /** Sets mBuildableClass + build category / sub-category via reflection. Call at end of ctor. */
    void InitCommon(bool bRegisterInBuildMenu = true);
};

// ---- Relay (Mk1-3) ----
UCLASS() class WIRELESSPOWER_API UWirelessPowerRelayMk1Descriptor : public UWirelessPowerDescriptor { GENERATED_BODY() public: UWirelessPowerRelayMk1Descriptor(); };
UCLASS() class WIRELESSPOWER_API UWirelessPowerRelayMk2Descriptor : public UWirelessPowerDescriptor { GENERATED_BODY() public: UWirelessPowerRelayMk2Descriptor(); };
UCLASS() class WIRELESSPOWER_API UWirelessPowerRelayMk3Descriptor : public UWirelessPowerDescriptor { GENERATED_BODY() public: UWirelessPowerRelayMk3Descriptor(); };

// ---- Power Converter (Mk1-3) ----
UCLASS() class WIRELESSPOWER_API UPowerConverterMk1Descriptor : public UWirelessPowerDescriptor { GENERATED_BODY() public: UPowerConverterMk1Descriptor(); };
UCLASS() class WIRELESSPOWER_API UPowerConverterMk2Descriptor : public UWirelessPowerDescriptor { GENERATED_BODY() public: UPowerConverterMk2Descriptor(); };
UCLASS() class WIRELESSPOWER_API UPowerConverterMk3Descriptor : public UWirelessPowerDescriptor { GENERATED_BODY() public: UPowerConverterMk3Descriptor(); };

// ---- Area Power Pole (Mk1-3) ----
UCLASS() class WIRELESSPOWER_API UAreaPowerPoleMk1Descriptor : public UWirelessPowerDescriptor { GENERATED_BODY() public: UAreaPowerPoleMk1Descriptor(); };
UCLASS() class WIRELESSPOWER_API UAreaPowerPoleMk2Descriptor : public UWirelessPowerDescriptor { GENERATED_BODY() public: UAreaPowerPoleMk2Descriptor(); };
UCLASS() class WIRELESSPOWER_API UAreaPowerPoleMk3Descriptor : public UWirelessPowerDescriptor { GENERATED_BODY() public: UAreaPowerPoleMk3Descriptor(); };

// ---- Laser Tower (single tier) ----
UCLASS() class WIRELESSPOWER_API ULaserTowerDescriptor : public UWirelessPowerDescriptor { GENERATED_BODY() public: ULaserTowerDescriptor(); };

/** Shared build-gun recipe: product = a tier descriptor, produced in the build gun. */
UCLASS()
class WIRELESSPOWER_API UWirelessPowerRecipe : public UFGRecipe
{
    GENERATED_BODY()
public:
    UWirelessPowerRecipe();

    /** Pin mProducedIn onto the resolved Build Gun class (must run after the world is up). */
    void ApplyProducedIn(UClass* BuildGunClass);

    /**
     * v1.0.47: fills mIngredients (the build-gun cost). Previously only mProduct was set, so
     * every wireless building showed as FREE to build. Lines are /Game/... descriptor paths
     * resolved with StaticLoadClass, same as the milestone costs. Idempotent (clears first).
     */
    void ApplyIngredients(const struct FWPCostLine* Lines, int32 Count);

protected:
    /** Sets mProduct to one descriptor and a placeholder mProducedIn. */
    void InitProduct(TSubclassOf<UFGBuildingDescriptor> DescClass);
};

// ---- Relay recipes ----
UCLASS() class WIRELESSPOWER_API UWirelessPowerRelayMk1Recipe : public UWirelessPowerRecipe { GENERATED_BODY() public: UWirelessPowerRelayMk1Recipe(); };
UCLASS() class WIRELESSPOWER_API UWirelessPowerRelayMk2Recipe : public UWirelessPowerRecipe { GENERATED_BODY() public: UWirelessPowerRelayMk2Recipe(); };
UCLASS() class WIRELESSPOWER_API UWirelessPowerRelayMk3Recipe : public UWirelessPowerRecipe { GENERATED_BODY() public: UWirelessPowerRelayMk3Recipe(); };

// ---- Power Converter recipes ----
UCLASS() class WIRELESSPOWER_API UPowerConverterMk1Recipe : public UWirelessPowerRecipe { GENERATED_BODY() public: UPowerConverterMk1Recipe(); };
UCLASS() class WIRELESSPOWER_API UPowerConverterMk2Recipe : public UWirelessPowerRecipe { GENERATED_BODY() public: UPowerConverterMk2Recipe(); };
UCLASS() class WIRELESSPOWER_API UPowerConverterMk3Recipe : public UWirelessPowerRecipe { GENERATED_BODY() public: UPowerConverterMk3Recipe(); };

// ---- Area Power Pole recipes ----
UCLASS() class WIRELESSPOWER_API UAreaPowerPoleMk1Recipe : public UWirelessPowerRecipe { GENERATED_BODY() public: UAreaPowerPoleMk1Recipe(); };
UCLASS() class WIRELESSPOWER_API UAreaPowerPoleMk2Recipe : public UWirelessPowerRecipe { GENERATED_BODY() public: UAreaPowerPoleMk2Recipe(); };
UCLASS() class WIRELESSPOWER_API UAreaPowerPoleMk3Recipe : public UWirelessPowerRecipe { GENERATED_BODY() public: UAreaPowerPoleMk3Recipe(); };

// ---- Laser Tower recipe ----
UCLASS() class WIRELESSPOWER_API ULaserTowerRecipe : public UWirelessPowerRecipe { GENERATED_BODY() public: ULaserTowerRecipe(); };

/** Custom build category so the build gun has a tab for our items. */
UCLASS()
class WIRELESSPOWER_API UWirelessPowerBuildCategory : public UFGBuildCategory
{
    GENERATED_BODY()
public:
    UWirelessPowerBuildCategory();

    /**
     * v1.0.64: assign the cook-time texture to mCategoryIcon so the build-menu
     * left sidebar shows our tab icon. Must run with game content mounted, so it
     * is called from RegisterBuildables() at POST_INITIALIZATION (the same place
     * the descriptor icons are applied). Cannot be done from the CDO constructor
     * because the pak asset is not mounted yet at module-load time.
     */
    static void ApplyIcon();
};

UCLASS()
class WIRELESSPOWER_API UWirelessPowerBuildSubCategory : public UFGBuildSubCategory
{
    GENERATED_BODY()
public:
    UWirelessPowerBuildSubCategory();
};

/**
 * Build-gun recipe unlock that grants exactly one milestone tier's recipes.
 * mRecipes is protected on UFGUnlockRecipe, so it can only be filled from a subclass.
 *
 * v1.0.44: the HUB reward panel's display logic lives in the game's BP_UnlockRecipe BLUEPRINT
 * graphs (UFGUnlock is Blueprintable and spawns its display widget from BP - see the GetWorld()
 * comment on UFGUnlock). A native UFGUnlockRecipe instance has no BP graphs, so the HUB showed
 * an empty Rewards panel (cost/icons don't route through the unlock BP, which is why they worked).
 * Fix, mirroring vanilla/Feyko/ContentLib: instantiate the game's BP_UnlockRecipe_C class and
 * fill its mRecipes via friendship (Mods/WirelessPower/Config/AccessTransformers.ini).
 * This class stays as the friend vehicle + fallback native unlock (unused at runtime now).
 */
UCLASS()
class WIRELESSPOWER_API UWirelessPowerTierUnlock : public UFGUnlockRecipe
{
    GENERATED_BODY()
public:
    UWirelessPowerTierUnlock();

    /**
     * Creates the milestone's reward unlock as a BP_UnlockRecipe_C instance (the only shape the
     * HUB can display), fills its recipes, and installs it on the milestone CDO. Idempotent.
     * Must run with game content mounted (CONSTRUCTION or later).
     */
    static void InstallBlueprintUnlock(UWirelessPowerMilestone* Milestone, const TArray<UClass*>& Recipes);

    /**
     * v1.0.47: watchdog patrol for a milestone's mUnlocks list. Only this class can touch
     * mUnlocks (protected on UFGSchematic; see AccessTransformers.ini), so the free-function
     * watchdog must call through here. Returns true if the list was healthy, false if it found
     * a NULL/foreign entry and re-installed the root-pinned display unlock. Procs = the tier's
     * recipe list used for the re-install.
     */
    static bool PatrolUnlockList(UWirelessPowerMilestone* Milestone, const TArray<UClass*>& Recipes);

    /** Diagnostic: recipe count of THIS unlock (safe to call on a CDO). */
    int32 DebugRecipeCount() const { return mRecipes.Num(); }
};

/** One concrete unlock class per tier, recipes filled in the CDO constructor. */
UCLASS()
class WIRELESSPOWER_API UWirelessPowerTierUnlockMk1 : public UWirelessPowerTierUnlock
{
    GENERATED_BODY()
public:
    UWirelessPowerTierUnlockMk1();
};

UCLASS()
class WIRELESSPOWER_API UWirelessPowerTierUnlockMk2 : public UWirelessPowerTierUnlock
{
    GENERATED_BODY()
public:
    UWirelessPowerTierUnlockMk2();
};

UCLASS()
class WIRELESSPOWER_API UWirelessPowerTierUnlockMk3 : public UWirelessPowerTierUnlock
{
    GENERATED_BODY()
public:
    UWirelessPowerTierUnlockMk3();
};

/**
 * Reproduces Feyko's original milestone progression: three HUB milestones that live in the vanilla
 * Logistics category at tech tiers 2 / 4 / 6, are bought with raw resources, and each unlock only
 * their own tier of build-gun recipes. Because they sit in vanilla tech tiers, the vanilla HUB
 * gates them automatically - you have to finish tier 1 before tier 2 shows up, and so on.
 *
 * The tech tiers, the resource costs and the tier -> recipe mapping were all read straight back out
 * of Feyko's Schematic_WirelessPowerMk1/2/3 .uasset files with the editor, so the numbers are his.
 */
UCLASS()
class WIRELESSPOWER_API UWirelessPowerMilestone : public UFGSchematic
{
    GENERATED_BODY()
public:
    UWirelessPowerMilestone();

    /** 0 = Mk1, 1 = Mk2, 2 = Mk3. Assigned by InitMilestone() from the concrete subclass ctor. */
    int32 MilestoneIndex;

    /**
     * Fills mSchematicCategory + mCost. It resolves /Game/... item descriptor classes, so it must
     * run once the game content is mounted (CONSTRUCTION or later) - never from the CDO constructor.
     */
    void ApplyFeykoCost();

protected:
    /** Common wiring for all three tiers. Call from each concrete subclass constructor. */
    void InitMilestone(int32 InMilestoneIndex, int32 InTechTier, const FText& InName, const FText& InDescription);
};

UCLASS() class WIRELESSPOWER_API UWirelessPowerMilestoneMk1 : public UWirelessPowerMilestone { GENERATED_BODY() public: UWirelessPowerMilestoneMk1(); };
UCLASS() class WIRELESSPOWER_API UWirelessPowerMilestoneMk2 : public UWirelessPowerMilestone { GENERATED_BODY() public: UWirelessPowerMilestoneMk2(); };
UCLASS() class WIRELESSPOWER_API UWirelessPowerMilestoneMk3 : public UWirelessPowerMilestone { GENERATED_BODY() public: UWirelessPowerMilestoneMk3(); };

namespace WirelessPower
{
    /** Register our buildables + recipes with the live AFGRecipeManager. Idempotent. */
    void RegisterBuildables(class UWorld* World);

    /** Resolves the /Game/... references on the three milestones. Needs the game content mounted. */
    void ApplyMilestoneData();

    // v1.0.54: GrantDefaultUnlockedRecipes() was declared here (v1.0.44..v1.0.53). It made the 9
    // Relay / Converter / Area Power Pole recipes available from world start with no HUB milestone.
    // Feyko's original awards every tier through Schematic_WirelessPowerMk1/2/3 instead, and the
    // user asked for that restored, so the declaration is gone along with its definition.
    // Milestone unlocks are now the sole source of our recipes.
}
