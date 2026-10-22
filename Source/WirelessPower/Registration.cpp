#include "Registration.h"
#include "WirelessPowerLocalization.h"
#include "WirelessPowerLoc.h"
#include "WirelessPowerRelay.h"
#include "PowerConverter.h"
#include "AreaPowerPole.h"
#include "LaserTower.h"
#include "WirelessPowerAssets.h"
#include "ItemAmount.h"
#include "FGRecipeManager.h"
#include "FGSchematicManager.h"
#include "FGSchematicCategory.h"
#include "FGBuildableSubsystem.h"
#include "Equipment/FGBuildGun.h"
#include "Logging/LogMacros.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
#include "TimerManager.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"

DEFINE_LOG_CATEGORY_STATIC(LogWirelessPower, Log, All);

#include "WirelessPowerLog.h"

// ----------------------------------------------------------------------------
// Reflection helpers. The shipped UFGBuildingDescriptor / UFGItemDescriptor members
// (build category, sub-categories, mSmallIcon / mPersistentBigIcon) are not all declared
// in the SDK stub headers, so we set them by name at runtime. This compiles cleanly and
// writes to the real game properties.
// ----------------------------------------------------------------------------
static void SetBuildCategoryProp(UFGBuildingDescriptor* Desc, const TCHAR* const* CandidateNames, int32 NumCandidates, UClass* CategoryClass)
{
    if (!Desc || !CategoryClass || NumCandidates == 0) return;

    FProperty* Prop = nullptr;
    for (int32 i = 0; i < NumCandidates; ++i)
    {
        Prop = FindFProperty<FProperty>(Desc->GetClass(), CandidateNames[i]);
        if (Prop) break;
    }
    if (!Prop)
    {
        UE_LOG(LogWirelessPower, Warning, TEXT("Wireless Power: no build-category property on %s; build gun may not list this item."), *Desc->GetClass()->GetName());
        return;
    }

    const FString ClassPath = CategoryClass->GetPathName();
    const TCHAR* ImportResult = Prop->ImportText_Direct(*ClassPath, Prop->ContainerPtrToValuePtr<void>(Desc), Desc, 0, nullptr);
    WP_LOGF( TEXT("Wireless Power [catset]: %s.%s <= %s (imported=%s)"),
        *Desc->GetClass()->GetName(), *Prop->GetName(), *CategoryClass->GetName(), ImportResult ? TEXT("ok") : TEXT("FAILED"));
}

static void SetSubCategoriesProp(UFGBuildingDescriptor* Desc, UClass* SubCategoryClass)
{
    if (!Desc || !SubCategoryClass) return;
    FArrayProperty* ArrProp = FindFProperty<FArrayProperty>(Desc->GetClass(), TEXT("mSubcategories"));
    if (!ArrProp)
    {
        UE_LOG(LogWirelessPower, Warning, TEXT("Wireless Power: no mSubcategories array on %s; build gun grid may be empty."), *Desc->GetClass()->GetName());
        return;
    }

    void* ArrayPtr = ArrProp->ContainerPtrToValuePtr<void>(Desc);
    FScriptArrayHelper Helper(ArrProp, ArrayPtr);
    if (Helper.Num() == 0) Helper.AddValue();
    FProperty* Inner = ArrProp->Inner;
    const FString Path = SubCategoryClass->GetPathName();
    const TCHAR* ImportResult = Inner->ImportText_Direct(*Path, Helper.GetRawPtr(0), Desc, 0, nullptr);
    WP_LOGF( TEXT("Wireless Power [subset]: %s.mSubcategories[0] <= %s (imported=%s, num=%d)"),
        *Desc->GetClass()->GetName(), *SubCategoryClass->GetName(), ImportResult ? TEXT("ok") : TEXT("FAILED"), Helper.Num());
}

// ---------------------------------------------------------------------------
// Icon assignment (v1.0.37).
// CRASH HISTORY (v1.0.0..v1.0.10, 9 crashes, CL-502094):
//   * Feyko's UE4-era .uasset icons (v1.0.0-1.0.8) load non-null but with a
//     broken render resource under UE5.6 -> Slate OnPaint crashes on draw.
//   * Runtime UTexture2D::CreateTransient (v1.0.10) also crashes: transient
//     textures are held by no package, so GC reaps them and the descriptor CDO
//     ends up with a dangling pointer (SImage::OnPaint -> MakeBox ->
//     FSlateBrush::UpdateRenderingResource -> IsA<UMaterialInterface>).
//   * v1.0.9/1.0.11+ wrote NO icon at all: stable, but the build menu shows
//     blank tiles.
// v1.0.37 resolution: the icons now ship as REAL cook-time UE5.6 UTexture2D
// assets in the pak (SourceArt/Icon/*.png -> SMLDev/create_icons.py -> 
// /WirelessPower/Icon/icon_*), loaded via WirelessPowerAssets::LoadWirelessIcon.
// Package-owned textures are GC-safe, exactly how vanilla and every other mod
// reference their icons. NEVER replace this with runtime texture creation.
// To change an icon: edit SourceArt/Icon/<name>.png and re-run create_icons.py.
// ---------------------------------------------------------------------------
#define WP_ASSIGN_ICONS 1

// ----------------------------------------------------------------------------
// v1.0.43 GC hardening + icon watchdog (build-menu paint crash, CL-502094).
// The crash stack was SImage::OnPaint -> FSlateDrawElement::MakeBox -> ShouldCull
// -> IsResourceObjectValid -> FName::ToString reading address 0x1fffe. In this
// engine fork, IsResourceObjectValid (DrawElementTypes.cpp:46) logs the object's
// name when a brush's ResourceObject is pending-kill / unreachable /
// begin-destroyed, and GetName() itself faults on a reaped object => the crash
// is by definition a GC'd brush resource being painted.
// Our 8 cook-time icon textures are supposed to be GC-safe through the CDO
// UPROPERTYs (mSmallIcon / mPersistentBigIcon / mSmallSchematicIcon), but
// "supposed to" is exactly what v1.0.10 said before its transient textures
// were reaped. So two belts and braces:
//   1. root-pin every icon texture the moment it is loaded (AddToRoot is
//      idempotent and makes "icon got reaped" physically impossible);
//   2. a 1-second watchdog patrols every icon property and prints the FIRST
//      invalid one by name (during the GC mark phase the name is still
//      readable, which pinpoints exactly which chain failed).
// If the crash is gone with this build, one of the CDO reference chains was
// not actually holding its texture. If it still crashes, the dying brush was
// never one of ours and the hunt moves to MAMTips / ContentLib / others.
// ----------------------------------------------------------------------------
static void WPRootIcon(UTexture2D* Icon, const TCHAR* Tag)
{
    if (Icon && !Icon->IsRooted())
    {
        Icon->AddToRoot();
        UE_LOG(LogWirelessPower, Warning,
            TEXT("Wireless Power [root]: %s (%s) pinned to root set"), *Icon->GetName(), Tag);
    }
}

static void WPCheckTextureProp(UObject* Owner, UClass* OwnerClass, const TCHAR* PropName, const TCHAR* OwnerTag)
{
    if (FObjectProperty* OP = CastField<FObjectProperty>(OwnerClass->FindPropertyByName(FName(PropName))))
    {
        UObject* Val = OP->GetObjectPropertyValue(OP->ContainerPtrToValuePtr<void>(Owner));
        if (Val && !IsValidChecked(Val))
        {
            UE_LOG(LogWirelessPower, Warning,
                TEXT("Wireless Power [watchdog]: %s.%s -> INVALID UObject (flags=0x%x) name=%s"),
                OwnerTag, PropName, (uint32)Val->GetFlags(), *Val->GetName());
        }
    }
}

static void ApplyWirelessIcon(UClass* DescClass, EWirelessBuildingType Type)
{
#if !WP_ASSIGN_ICONS
    // Crash-safe fallback: leave icon properties untouched (blank tile, never crashes).
    return;
#else
    if (!DescClass) return;

    // Cook-time texture asset from our own pak (SMLDev/create_icons.py).
    // Package-owned => GC-safe. This is the ONLY icon source proven not to crash
    // (see the block comment above for the full crash history).
    UTexture2D* Icon = WirelessPowerAssets::LoadWirelessIcon(Type);
    if (!Icon)
    {
        UE_LOG(LogWirelessPower, Warning,
            TEXT("Wireless Power [icon]: cook-time icon asset missing for type %d -- build menu tile stays blank. Was create_icons.py run?"),
            (int32)Type);
        return;
    }
    WPRootIcon(Icon, TEXT("buildmenu"));

    UObject* CDO = DescClass->GetDefaultObject();
    if (!CDO) return;

    // Only these two hold the descriptor icons (mIcon does not exist in this
    // build -- verified 2026-09-30). Only write properties whose type really is
    // a UTexture2D object reference; a mismatched property write corrupts memory.
    static const FName CandidateNames[] = {
        FName(TEXT("mSmallIcon")),
        FName(TEXT("mPersistentBigIcon")),
    };
    int32 NumSet = 0;
    for (const FName& Name : CandidateNames)
    {
        if (FObjectProperty* Prop = FindFProperty<FObjectProperty>(DescClass, Name))
        {
            if (Prop->PropertyClass && Prop->PropertyClass->IsChildOf(UTexture2D::StaticClass()))
            {
                Prop->SetObjectPropertyValue(Prop->ContainerPtrToValuePtr<void>(CDO), Icon);
                ++NumSet;
            }
        }
    }
    // Bare UE_LOG on purpose: WP_LOGF is compiled out (WirelessPowerLog.h), and
    // these lines are the end-to-end proof the cook-time icons actually loaded.
    UE_LOG(LogWirelessPower, Log,
        TEXT("Wireless Power [icon]: %s <= %s (%d props)"),
        *DescClass->GetName(), *Icon->GetName(), NumSet);
#endif
}

// Milestone (schematic) icon. v1.0.38 tried FindFProperty<FObjectProperty> and logged
// "has no UTexture2D mSchematicIcon" for all three milestones in-game: the name matched no
// FObjectProperty of that type on the class chain, even though the FName string exists once in
// the shipped FactoryGame binary. So the sniffing approach was wrong, not the name -- the
// property may live on a different class, or be a soft-object/other property kind.
//
// v1.0.39 therefore stops guessing the property TYPE:
//   * walk the class's reflected properties by name (TFieldIterator -- no template cast, so no
//     API/type assumptions at all);
//   * assign through FProperty::ImportText_Direct with the asset path. ImportText is
//     type-aware: it parses only if the text fits the property's own type and otherwise returns
//     null, so a mis-named or differently-typed property is rejected instead of corrupted;
//   * if nothing accepts it, dump the whole reflected property table once per process so the
//     next build can target the real property instead of guessing again.
static bool WPPropEquals(const FProperty* Prop, const TCHAR* Want)
{
    return Prop && Prop->GetName().Equals(Want, ESearchCase::CaseSensitive);
}

static FProperty* WPFindPropByName(UClass* C, const TCHAR* Want)
{
    if (!C) return nullptr;
    for (TFieldIterator<FProperty> It(C); It; ++It)
    {
        if (WPPropEquals(*It, Want))
        {
            return *It;
        }
    }
    return nullptr;
}

static void DumpSchematicProperties(UClass* C, const TCHAR* Tag)
{
    if (!C) return;
    // Keyed by tag (not a single bool) so the milestone dump and the category dump both happen,
    // while the per-world repeat passes still print only once per process each.
    static TSet<FString> Dumped;
    const FString Key(Tag);
    if (Dumped.Contains(Key)) return;
    Dumped.Add(Key);

    TArray<FString> Iconish, All;
    for (TFieldIterator<FProperty> It(C); It; ++It)
    {
        const FString Entry = FString::Printf(TEXT("%s:%s"), *It->GetName(), *It->GetClass()->GetName());
        All.Add(Entry);
        if (It->GetName().Contains(TEXT("icon"), ESearchCase::IgnoreCase))
        {
            Iconish.Add(Entry);
        }
    }
    UE_LOG(LogWirelessPower, Warning,
        TEXT("Wireless Power [icon] DUMP(%s): %s has %d reflected props; Icon-ish = [%s]"),
        Tag, *C->GetName(), All.Num(), *FString::Join(Iconish, TEXT(", ")));

    const int32 Chunk = 14;
    for (int32 i = 0; i < All.Num(); i += Chunk)
    {
        TArray<FString> Slice;
        const int32 End = FMath::Min(i + Chunk, All.Num());
        for (int32 j = i; j < End; ++j)
        {
            Slice.Add(All[j]);
        }
        UE_LOG(LogWirelessPower, Warning, TEXT("Wireless Power [icon] DUMP(%s) %s[%d..%d] = [%s]"),
            Tag, *C->GetName(), i, End - 1, *FString::Join(Slice, TEXT(", ")));
    }
}

// Build an FSlateBrush value holding our texture, WITHOUT including Slate: the dump proved
// UFGSchematic::mSchematicIcon is a StructProperty (a brush struct), not a texture pointer, and
// linking SlateCore into this runtime module just for one icon is not worth the risk. Everything
// below goes through the struct's own reflected properties, so the engine version's exact brush
// layout (FVector2D vs FVector2f ImageSize, enum vs byte DrawAs) never matters.
static bool WPWriteSlateBrush(FStructProperty* SP, UObject* CDO, UTexture2D* Icon)
{
    if (!SP || !SP->Struct)
    {
        return false;
    }
    UScriptStruct* S = SP->Struct;
    if (!S->GetName().Equals(TEXT("SlateBrush"), ESearchCase::IgnoreCase))
    {
        UE_LOG(LogWirelessPower, Warning,
            TEXT("Wireless Power [icon]: mSchematicIcon is a %s struct, not a SlateBrush -- skipped."),
            *S->GetName());
        return false;
    }

    // fresh default brush, then patch the three fields that matter
    TArray<uint8> Tmp;
    Tmp.SetNumZeroed(FMath::Max<int32>(SP->GetSize() + 16, 64));
    void* B = Tmp.GetData();
    S->InitializeStruct(B);

    // ResourceObject = our cook-time texture (FSlateBrush's UObject* resource)
    if (FProperty* RO = S->FindPropertyByName(FName(TEXT("ResourceObject"))))
    {
        if (FObjectProperty* ObjRO = CastField<FObjectProperty>(RO))
        {
            ObjRO->SetObjectPropertyValue(ObjRO->ContainerPtrToValuePtr<void>(B), Icon);
        }
        else
        {
            UE_LOG(LogWirelessPower, Warning,
                TEXT("Wireless Power [icon]: brush ResourceObject is a %s, expected an object property."),
                *RO->GetClass()->GetName());
        }
    }
    // ImageSize = the texture's pixel size (ImportText parses FVector2D and FVector2f alike)
    if (FProperty* IS = S->FindPropertyByName(FName(TEXT("ImageSize"))))
    {
        const FString SizeText = FString::Printf(TEXT("(X=%d,Y=%d)"), Icon->GetSizeX(), Icon->GetSizeY());
        if (!IS->ImportText_Direct(*SizeText, IS->ContainerPtrToValuePtr<void>(B), nullptr, 0, nullptr))
        {
            UE_LOG(LogWirelessPower, Warning,
                TEXT("Wireless Power [icon]: brush ImageSize=%s was rejected by %s."),
                *SizeText, *IS->GetClass()->GetName());
        }
    }
    // DrawAs must be Image, otherwise a default brush draws nothing at all. Try the short name
    // first, then the fully qualified one, so either enum representation is accepted.
    if (FProperty* DA = S->FindPropertyByName(FName(TEXT("DrawAs"))))
    {
        bool bDrawAsSet = false;
        static const TCHAR* const DrawTexts[] = { TEXT("Image"), TEXT("ESlateBrushDrawType::Image") };
        for (const TCHAR* DrawText : DrawTexts)
        {
            if (DA->ImportText_Direct(DrawText, DA->ContainerPtrToValuePtr<void>(B), nullptr, 0, nullptr))
            {
                bDrawAsSet = true;
                break;
            }
        }
        if (!bDrawAsSet)
        {
            UE_LOG(LogWirelessPower, Warning,
                TEXT("Wireless Power [icon]: brush DrawAs could not be set on %s -- the brush may draw nothing."),
                *DA->GetClass()->GetName());
        }
    }

    // FStructProperty::CopyCompleteValue routes through the script struct's own copy logic and
    // keeps object references intact. (This engine fork's UScriptStruct has no CopyCompleteValue.)
    SP->CopyCompleteValue(SP->ContainerPtrToValuePtr<void>(CDO), B);
    S->DestroyStruct(B);
    return true;
}

static void ApplyMilestoneIcon(UFGSchematic* Schematic, EWirelessBuildingType Type)
{
#if !WP_ASSIGN_ICONS
    return;
#else
    if (!Schematic) return;

    // v1.0.41: use the WHITE-SILHOUETTE set here. The HUB tints schematic icons, so the
    // full-colour render used in v1.0.40 collapsed into a flat orange block. Same alpha
    // outline, RGB forced to white -> the tint now reads as a proper icon.
    UTexture2D* Icon = WirelessPowerAssets::LoadMilestoneIcon(Type);
    if (!Icon)
    {
        UE_LOG(LogWirelessPower, Warning,
            TEXT("Wireless Power [icon]: cook-time MILESTONE silhouette asset missing for type %d -- HUB tile stays blank. Was create_milestone_icons.py run?"),
            (int32)Type);
        return;
    }
    WPRootIcon(Icon, TEXT("milestone"));

    UClass* SchemClass = Schematic->GetClass();
    UObject* CDO = SchemClass->GetDefaultObject();
    if (!CDO) return;

    // v1.0.39's property-table dump told us exactly what to write:
    //   mSchematicIcon      : StructProperty  (an FSlateBrush)
    //   mSmallSchematicIcon : ObjectProperty  (the texture)
    //   FGSchematicCategory.mCategoryIcon : StructProperty (also a brush)
    // So write BOTH schematic icon slots; either may be the one the HUB tile draws from.
    bool bAny = false;

    // (1) mSmallSchematicIcon -- a plain texture pointer, the trivial case.
    if (FProperty* P = WPFindPropByName(SchemClass, TEXT("mSmallSchematicIcon")))
    {
        if (FObjectProperty* OP = CastField<FObjectProperty>(P))
        {
            UClass* PC = OP->PropertyClass;
            UClass* T2D = UTexture2D::StaticClass();
            if (PC && (PC->IsChildOf(T2D) || T2D->IsChildOf(PC)))
            {
                OP->SetObjectPropertyValue(OP->ContainerPtrToValuePtr<void>(CDO), Icon);
                UE_LOG(LogWirelessPower, Warning,
                    TEXT("Wireless Power [icon]: %s.mSmallSchematicIcon <= %s OK"),
                    *SchemClass->GetName(), *Icon->GetName());
                bAny = true;
            }
            else
            {
                UE_LOG(LogWirelessPower, Warning,
                    TEXT("Wireless Power [icon]: %s.mSmallSchematicIcon has PropertyClass %s -- not texture-assignable."),
                    *SchemClass->GetName(), PC ? *PC->GetName() : TEXT("NULL"));
            }
        }
    }

    // (2) mSchematicIcon -- a Slate brush struct, assembled through reflection (no Slate include).
    if (FProperty* P = WPFindPropByName(SchemClass, TEXT("mSchematicIcon")))
    {
        if (FStructProperty* SP = CastField<FStructProperty>(P))
        {
            if (WPWriteSlateBrush(SP, CDO, Icon))
            {
                UE_LOG(LogWirelessPower, Warning,
                    TEXT("Wireless Power [icon]: %s.mSchematicIcon <= brush(%s) OK"),
                    *SchemClass->GetName(), *Icon->GetName());
                bAny = true;
            }
        }
    }

    if (!bAny)
    {
        UE_LOG(LogWirelessPower, Warning,
            TEXT("Wireless Power [icon]: %s -- neither mSmallSchematicIcon nor mSchematicIcon could be written.")
            TEXT(" Dumping the reflected property table so the real one can be identified."),
            *SchemClass->GetName());
        DumpSchematicProperties(SchemClass, TEXT("milestone"));
        DumpSchematicProperties(UFGSchematicCategory::StaticClass(), TEXT("category"));
    }
#endif
}

static UClass* ResolveBuildGunClass()
{
    static UClass* Cached = nullptr;
    if (Cached) return Cached;

    if (UClass* BG = FindObject<UClass>(nullptr, TEXT("/Game/FactoryGame/Equipment/BuildGun/BP_BuildGun.BP_BuildGun_C")))
    {
        Cached = BG; return Cached;
    }
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* C = *It;
        if (C->IsChildOf(AFGBuildGun::StaticClass()) && C != AFGBuildGun::StaticClass() && C->HasAnyClassFlags(CLASS_CompiledFromBlueprint))
        {
            Cached = C; return Cached;
        }
    }
    Cached = AFGBuildGun::StaticClass();
    return Cached;
}

// ----------------------------------------------------------------------------
// v1.0.43 icon watchdog: patrol every icon property once per second.
// ----------------------------------------------------------------------------
static FTimerHandle GWPIconWatchdogHandle;
// v1.0.59 BUG FIX: remember WHICH world owns the patrol. The handle is file-static, so without
// this the FrontEnd / main-menu world (FGMainMenuState) would grab it during its POST_INITIALIZATION
// and the real game world could never start its own patrol -- leaving milestone icons un-restored
// after GC and crashing on paint (SImage::OnPaint -> FName::ToString reading a freed UObject).
// That is the WirelessPower+0x29f0c / FGMainMenuState crash.
static TWeakObjectPtr<UWorld> GWatchdogWorld;

// v1.0.47: per-tier recipe lists for the unlock self-heal below (mirrors ApplyMilestoneData's
// GRecipeTables; kept separate so the watchdog needs no forward declarations). NULL-terminated.
//
// v1.0.54: widened back to Feyko's ORIGINAL three-tier unlock sets. Until v1.0.53 these lists
// only carried each tier's *headline* recipe because the rest were handed out at world start by
// GrantDefaultUnlockedRecipes(); that grant is gone (see below), so every recipe a milestone is
// supposed to award must be listed here or the self-heal would strip it from the unlock list.
// Feyko's own mUnlocks tables (Content/Schematics/Schematic_WirelessPowerMk*.uasset) read:
//   Mk1 -> relay Mk1 + converter + pole Mk1        (3)
//   Mk2 -> relay Mk2 + pole Mk2                    (2)
//   Mk3 -> relay Mk3 + pole Mk3 + laser tower      (3)
// The Power Converter is a SINGLE building (only Mk1 exists); its Mk2/Mk3 descriptors/recipes
// are kept compiled purely for old-save compatibility and are deliberately NOT registered
// anywhere (not in this table, not in the milestone GMk*Recipes, not in the build menu). The
// per-tier relays (Relay Mk2/Mk3) ride their own milestones -- matching Feyko's original layout.
static UClass* const GWatchdogTierRecipes[3][5] = {
    { UWirelessPowerRelayMk1Recipe::StaticClass(), UPowerConverterMk1Recipe::StaticClass(), UAreaPowerPoleMk1Recipe::StaticClass(), nullptr },
    { UWirelessPowerRelayMk2Recipe::StaticClass(), UAreaPowerPoleMk2Recipe::StaticClass(), nullptr },
    { UWirelessPowerRelayMk3Recipe::StaticClass(), UAreaPowerPoleMk3Recipe::StaticClass(), ULaserTowerRecipe::StaticClass(), nullptr },
};

static void WPIconWatchdogTick()
{
    struct FWPDescRef { UClass* C; const TCHAR* Tag; };
    static const FWPDescRef Descs[] = {
        { UWirelessPowerRelayMk1Descriptor::StaticClass(),  TEXT("RelayMk1Descriptor") },
        { UWirelessPowerRelayMk2Descriptor::StaticClass(),  TEXT("RelayMk2Descriptor") },
        { UWirelessPowerRelayMk3Descriptor::StaticClass(),  TEXT("RelayMk3Descriptor") },
        { UPowerConverterMk1Descriptor::StaticClass(),      TEXT("ConverterMk1Descriptor") },
        { UAreaPowerPoleMk1Descriptor::StaticClass(),       TEXT("PoleMk1Descriptor") },
        { UAreaPowerPoleMk2Descriptor::StaticClass(),       TEXT("PoleMk2Descriptor") },
        { UAreaPowerPoleMk3Descriptor::StaticClass(),       TEXT("PoleMk3Descriptor") },
        { ULaserTowerDescriptor::StaticClass(),             TEXT("LaserTowerDescriptor") },
    };
    static const TCHAR* DescProps[] = { TEXT("mSmallIcon"), TEXT("mPersistentBigIcon") };
    for (const FWPDescRef& D : Descs)
    {
        if (UObject* CDO = D.C->GetDefaultObject())
        {
            for (const TCHAR* P : DescProps)
            {
                WPCheckTextureProp(CDO, D.C, P, D.Tag);
            }
        }
    }

    static UClass* Milestones[] = {
        UWirelessPowerMilestoneMk1::StaticClass(),
        UWirelessPowerMilestoneMk2::StaticClass(),
        UWirelessPowerMilestoneMk3::StaticClass(),
    };
    static const TCHAR* MilestoneTags[] = { TEXT("MilestoneMk1"), TEXT("MilestoneMk2"), TEXT("MilestoneMk3") };
    for (int32 i = 0; i < 3; ++i)
    {
        UObject* CDO = Milestones[i]->GetDefaultObject();
        if (!CDO) continue;
        WPCheckTextureProp(CDO, Milestones[i], TEXT("mSmallSchematicIcon"), MilestoneTags[i]);

        // The Slate brush struct: inspect its ResourceObject through reflection.
        if (FStructProperty* SP = CastField<FStructProperty>(Milestones[i]->FindPropertyByName(FName(TEXT("mSchematicIcon")))))
        {
            if (FObjectProperty* RO = CastField<FObjectProperty>(SP->Struct->FindPropertyByName(FName(TEXT("ResourceObject")))))
            {
                UObject* Val = RO->GetObjectPropertyValue(RO->ContainerPtrToValuePtr<void>(SP->ContainerPtrToValuePtr<void>(CDO)));
                if (Val && !IsValidChecked(Val))
                {
                    UE_LOG(LogWirelessPower, Warning,
                        TEXT("Wireless Power [watchdog]: %s.mSchematicIcon.ResourceObject -> INVALID UObject (flags=0x%x) name=%s"),
                        MilestoneTags[i], (uint32)Val->GetFlags(), *Val->GetName());
                }
            }
        }

        // v1.0.47: patrol the milestone's unlock list. A null or non-BP entry here is exactly
        // what killed FixupSave_FindBuiltByRecipe in v1.0.45/46; detect + heal + log loudly.
        // mUnlocks is protected, so the scan must run inside UWirelessPowerTierUnlock (the only
        // declared friend) -- see PatrolUnlockList().
        if (UWirelessPowerMilestone* Milestone = Cast<UWirelessPowerMilestone>(CDO))
        {
            TArray<UClass*> Recipes;
            for (UClass* const* R = GWatchdogTierRecipes[i]; *R; ++R)
            {
                Recipes.Add(*R);
            }
            UWirelessPowerTierUnlock::PatrolUnlockList(Milestone, Recipes);
        }
    }
}

static void WPStartIconWatchdog(UWorld* World)
{
    if (!World) return;
    // v1.0.59 BUG FIX: only a REAL game world has a buildable subsystem. The FrontEnd / main-menu
    // world does not, so it must never be allowed to own the (file-static) watchdog handle -- doing
    // so previously disabled the icon self-heal in the actual game world and reintroduced the
    // milestone-icon GC crash. Use the buildable subsystem as the discriminator (robust regardless
    // of which game-mode class the menu uses).
    if (!AFGBuildableSubsystem::Get(World)) return;
    // If a different world already owns the handle (e.g. a stale FrontEnd world that was torn down,
    // or simply a world we are no longer in), release it so THIS real game world can own the patrol.
    if (GWatchdogWorld.IsValid() && GWatchdogWorld.Get() != World)
    {
        if (UWorld* Old = GWatchdogWorld.Get())
        {
            Old->GetTimerManager().ClearTimer(GWPIconWatchdogHandle);
        }
        GWPIconWatchdogHandle.Invalidate();
    }
    if (!GWPIconWatchdogHandle.IsValid())
    {
        World->GetTimerManager().SetTimer(GWPIconWatchdogHandle,
            FTimerDelegate::CreateStatic(&WPIconWatchdogTick), 1.0f, true, 5.0f);
        GWatchdogWorld = World;
        UE_LOG(LogWirelessPower, Warning,
            TEXT("Wireless Power [watchdog]: icon patrol started on game world (every 1s, first check +5s)"));
    }
}

// ----------------------------------------------------------------------------
// Build category / sub-category / schematic
// ----------------------------------------------------------------------------
UWirelessPowerBuildCategory::UWirelessPowerBuildCategory()
{
    mDisplayName = WP_Text(TEXT("Wireless Power"), TEXT("无线电力"));
}

UWirelessPowerBuildSubCategory::UWirelessPowerBuildSubCategory()
{
    mDisplayName = WP_Text(TEXT("Wireless Power"), TEXT("无线电力"));
}

// v1.0.64: the build-menu category tab icon. mCategoryIcon is a FSlateBrush on the CDO;
// the converter is the hub of the wireless grid, so it is the natural category glyph.
// Loaded from our own cook-time pak asset (same path the descriptor icons use), so it is
// package-owned and GC-safe. Rooted for good measure, matching WPRootIcon's safety policy.
void UWirelessPowerBuildCategory::ApplyIcon()
{
    UWirelessPowerBuildCategory* CDO =
        Cast<UWirelessPowerBuildCategory>(UWirelessPowerBuildCategory::StaticClass()->GetDefaultObject());
    if (!CDO) return;

    UTexture2D* Icon = WirelessPowerAssets::LoadWirelessIcon(EWirelessBuildingType::Converter);
    if (!Icon)
    {
        UE_LOG(LogWirelessPower, Warning,
            TEXT("Wireless Power [icon]: cook-time icon asset missing for build category -- left tab stays blank. Was create_icons.py run?"));
        return;
    }
    WPRootIcon(Icon, TEXT("buildcategory"));
    CDO->mCategoryIcon.SetResourceObject(Icon);
    UE_LOG(LogWirelessPower, Log,
        TEXT("Wireless Power [icon]: build category <= %s"), *Icon->GetName());
}

// ----------------------------------------------------------------------------
// Milestones - Feyko's original three-stage progression, reproduced.
//
// Every number below was read back out of Feyko's own assets with the editor
// (Schematic_WirelessPowerMk1/2/3 -> mTechTier + mCost, and the BP_UnlockRecipe
// sub-objects -> mRecipes), so the tech tiers, the resource costs and the
// tier -> recipe mapping match the original mod exactly.
// ----------------------------------------------------------------------------
namespace
{
    /** One line of a milestone cost: a /Game/... item descriptor path plus an amount. */
    struct FWPMilestoneCost
    {
        const TCHAR* ItemPath;
        int32 Amount;
    };

    // ---- Schematic_WirelessPowerMk1 : tech tier 2 ----
    // v1.0.47 (user): 急速电线 (HighSpeedWire) x500 -> 电缆 (Cable) x500.
    const FWPMilestoneCost GMk1Cost[] = {
        { TEXT("/Game/FactoryGame/Resource/Parts/IronPlateReinforced/Desc_IronPlateReinforced.Desc_IronPlateReinforced_C"), 50 },
        { TEXT("/Game/FactoryGame/Resource/Parts/Cable/Desc_Cable.Desc_Cable_C"), 500 },
        { TEXT("/Game/FactoryGame/Resource/Parts/IronRod/Desc_IronRod.Desc_IronRod_C"), 200 },
        { TEXT("/Game/FactoryGame/Resource/Parts/Cement/Desc_Cement.Desc_Cement_C"), 200 },
    };

    // ---- Schematic_WirelessPowerMk2 : tech tier 4 ----
    // v1.0.48: 钢筋混凝土梁. NOTE -- 'EncasedIndustrialBeam' does NOT exist in the shipped
    // game (the AssetRegistry under /Game/FactoryGame/Resource/Parts has no Desc_EncasedIndustrialBeam
    // at all), so the v1.0.47 path silently failed to resolve and the milestone showed a missing
    // item. The closest vanilla equivalent that DOES exist is 强化钢板 (SteelPlateReinforced).
    const FWPMilestoneCost GMk2Cost[] = {
        { TEXT("/Game/FactoryGame/Resource/Parts/ModularFrame/Desc_ModularFrame.Desc_ModularFrame_C"), 50 },
        { TEXT("/Game/FactoryGame/Resource/Parts/HighSpeedWire/Desc_HighSpeedWire.Desc_HighSpeedWire_C"), 1500 },
        { TEXT("/Game/FactoryGame/Resource/Parts/SteelPlate/Desc_SteelPlate.Desc_SteelPlate_C"), 200 },
        { TEXT("/Game/FactoryGame/Resource/Parts/SteelPlateReinforced/Desc_SteelPlateReinforced.Desc_SteelPlateReinforced_C"), 500 },
    };

    // ---- Schematic_WirelessPowerMk3 : tech tier 6 ----
    const FWPMilestoneCost GMk3Cost[] = {
        { TEXT("/Game/FactoryGame/Resource/Parts/ModularFrameHeavy/Desc_ModularFrameHeavy.Desc_ModularFrameHeavy_C"), 50 },
        { TEXT("/Game/FactoryGame/Resource/Parts/HighSpeedConnector/Desc_HighSpeedConnector.Desc_HighSpeedConnector_C"), 200 },
        { TEXT("/Game/FactoryGame/Resource/Parts/SteelPlateReinforced/Desc_SteelPlateReinforced.Desc_SteelPlateReinforced_C"), 200 },
        { TEXT("/Game/FactoryGame/Resource/Parts/Computer/Desc_Computer.Desc_Computer_C"), 100 },
    };

    const FWPMilestoneCost* const GMilestoneCosts[] = { GMk1Cost, GMk2Cost, GMk3Cost };
    const int32 GMilestoneCostCounts[] = { UE_ARRAY_COUNT(GMk1Cost), UE_ARRAY_COUNT(GMk2Cost), UE_ARRAY_COUNT(GMk3Cost) };
    const TCHAR* const GLogisticsCategory = TEXT("/Game/FactoryGame/Schematics/SchematicCategories/SC_Logistics.SC_Logistics_C");
}

UWirelessPowerTierUnlock::UWirelessPowerTierUnlock() = default;

// v1.0.44: these concrete per-tier unlock classes are no longer instantiated into mUnlocks --
// the HUB only displays BP_UnlockRecipe_C instances (see InstallBlueprintUnlock). Their
// constructors still fill their own mRecipes so the classes stay self-describing if we ever
// need a native fallback; at runtime they are unused.

// The exact recipe sets Feyko's BP_UnlockRecipe sub-objects granted. His Mk1 shipped a
// single-tier Power Converter, so Mk2/Mk3 only carried relay + pole; our rebuild has a
// converter per tier, so each one rides along with its own milestone.
// (The same sets are mirrored in ApplyMilestoneData()'s GRecipeTables.)
UWirelessPowerTierUnlockMk1::UWirelessPowerTierUnlockMk1()
{
    mRecipes.Add(UWirelessPowerRelayMk1Recipe::StaticClass());
    mRecipes.Add(UPowerConverterMk1Recipe::StaticClass());
    mRecipes.Add(UAreaPowerPoleMk1Recipe::StaticClass());
}

UWirelessPowerTierUnlockMk2::UWirelessPowerTierUnlockMk2()
{
    mRecipes.Add(UWirelessPowerRelayMk2Recipe::StaticClass());
    mRecipes.Add(UAreaPowerPoleMk2Recipe::StaticClass());
}

UWirelessPowerTierUnlockMk3::UWirelessPowerTierUnlockMk3()
{
    mRecipes.Add(UWirelessPowerRelayMk3Recipe::StaticClass());
    mRecipes.Add(UAreaPowerPoleMk3Recipe::StaticClass());
    mRecipes.Add(ULaserTowerRecipe::StaticClass());
}

UWirelessPowerMilestone::UWirelessPowerMilestone()
    : MilestoneIndex(INDEX_NONE)
{
    // Only the content-free fields are set here. mCost / mSchematicCategory need /Game/... classes,
    // which ApplyFeykoCost() resolves once the world (and therefore the game content) is up.
}

void UWirelessPowerMilestone::InitMilestone(int32 InMilestoneIndex, int32 InTechTier, const FText& InName, const FText& InDescription)
{
    MilestoneIndex = InMilestoneIndex;

    mType = ESchematicType::EST_Milestone;
    mTechTier = InTechTier;
    mDisplayName = InName;
    mDescription = InDescription;

    // Same cosmetics as Feyko's originals.
    mMenuPriority = 0.0f;
    mTimeToComplete = 60.0f;
    mIsPlayerSpecific = false;

    // Vanilla tech tiers gate these for us; no extra dependency chain needed (Feyko had none either).
    mDependenciesBlocksSchematicAccess = false;
    mHiddenUntilDependenciesMet = false;

    // v1.0.44: mUnlocks is NOT populated here anymore. The HUB reward panel can only display
    // BP_UnlockRecipe_C instances (its display widget logic is a blueprint on the unlock class),
    // and /Game classes aren't mounted during CDO construction anyway. The BP-class unlock
    // instances are installed per-world at CONSTRUCTION by InstallBlueprintUnlock() - see
    // ApplyMilestoneData(). Native unlock instances here would render as an empty Rewards panel.
}

// v1.1.3 [reload-diag]: process-wide registration generation counter. Lets a live run tell
// "which world-load #N this registration belongs to" so the author's R1-R7 reload matrix can be
// matched against the exact moment the recipe manager's PostLoadGame walks corrupted data.
static int32 GWPRegistrationGen = 0;

void UWirelessPowerTierUnlock::InstallBlueprintUnlock(UWirelessPowerMilestone* Milestone, const TArray<UClass*>& Recipes)
{
    if (!Milestone) return;

    // v1.1.4 [cross-world crash fix] REVISED in v1.2.1: the old fix only Empty()-ed mUnlocks, which
    // ORPHANED the AddToRoot()'d UFGUnlockRecipe instance -- it stayed process-immortal and, across a
    // same-process world reload, OUTLIVED the per-world UModContentRegistry / FGameObjectRegistryState
    // it was registered against. The next world's re-registration then walked a stale recipe -> item
    // ReferencedBy TArray (Data=NULL, Num>=2) and crashed in ModContentRegistry.cpp AddObjectReference
    // (AV reading 0x8). The milestone CDO is process-immortal, so its mUnlocks (and the orphaned
    // instance) survive the world transition; we must UN-ROOT + drop the previous instance BEFORE
    // creating the new one. This guarantees exactly ONE rooted unlock per live world, rebuilt cleanly
    // every call. AddToRoot is still required for the v1.0.45/46 GC fix; we simply remove it on
    // rebuild so no cross-world accumulation occurs.
    if (Milestone->mUnlocks.Num() > 0)
    {
        for (int32 i = Milestone->mUnlocks.Num() - 1; i >= 0; --i)   // friend access
        {
            if (UFGUnlock* U = Milestone->mUnlocks[i])
            {
                if (U->IsA(UFGUnlockRecipe::StaticClass()))
                {
                    if (U->IsRooted())
                    {
                        U->RemoveFromRoot();
                    }
                    Milestone->mUnlocks.RemoveAt(i);
                }
                // foreign (non-BP_UnlockRecipe) unlocks are left intact
            }
            else
            {
                Milestone->mUnlocks.RemoveAt(i); // NULL hole left by a prior GC purge
            }
        }
    }

    // v1.1.3 [reload-diag]: always-on (not WP_LOGF) so a real reload crash reproduces with a
    // clear trail of gen / world / unlock-count even though WP_LOGF is compiled out.
    UE_LOG(LogWirelessPower, Log,
        TEXT("Wireless Power [reload-diag] InstallBlueprintUnlock gen=#%d milestone=%s mUnlocks=%d (post-cleanup)"),
        GWPRegistrationGen, *Milestone->GetClass()->GetName(), Milestone->mUnlocks.Num());

    // The game's BP_UnlockRecipe blueprint class. Everything vanilla (and Feyko's original, and
    // ContentLib) instantiates unlocks from THIS class, because the HUB reward widget is drawn
    // by the blueprint graphs it carries. Load once per process and cache.
    static UClass* BPUnlockClass = nullptr;
    if (!BPUnlockClass)
    {
        BPUnlockClass = StaticLoadClass(UFGUnlockRecipe::StaticClass(), nullptr,
            TEXT("/Game/FactoryGame/Unlocks/BP_UnlockRecipe.BP_UnlockRecipe_C"));
    }
    if (!BPUnlockClass)
    {
        UE_LOG(LogWirelessPower, Warning,
            TEXT("Wireless Power [unlock]: could not load /Game/FactoryGame/Unlocks/BP_UnlockRecipe - HUB rewards will stay blank."));
        return;
    }

    // NOTE: the idempotent early-return (skip if a BP unlock already present) was removed on purpose.
    // ApplyMilestoneData runs at CONSTRUCTION and again at POST_INITIALIZATION, and across world
    // reloads; we ALWAYS rebuild the single unlock instance (after dropping the old one above) so it
    // is never stale relative to the live SML registry. The cleanup above IS the idempotency guarantee.

    UFGUnlockRecipe* Unlock = NewObject<UFGUnlockRecipe>(Milestone, BPUnlockClass);
    Unlock->mRecipes.Empty();                                           // friend access
    for (UClass* RecipeClass : Recipes)
    {
        if (RecipeClass)
        {
            Unlock->mRecipes.AddUnique(RecipeClass);
        }
    }
    // v1.0.47 ROOT-PIN (crash fix for v1.0.45/46 "FixupSave_FindBuiltByRecipe" AV):
    // AFGBuildable::BeginPlay calls FixupSave_FindBuiltByRecipe for every spawned buildable that
    // has no mBuiltWithRecipe (all wireless buildings placed before this version). That walk
    // dereferences every schematic's mUnlocks -- and the ONLY non-immortal object in the walked
    // chain was this runtime NewObject'ed instance. A GC purge turned the mUnlocks entry into a
    // null (crash reading 0x38) or dangling (crash reading 0x7fcd...) pointer mid-session; the
    // HUB still displayed fine at first because the entry dies only at the NEXT full GC purge.
    // Same medicine as the v1.0.43 icon fix: pin it to the root set, process lifetime, 3 tiny
    // objects total.
    Unlock->AddToRoot();
    Milestone->mUnlocks.Add(Unlock);                                    // friend access

    UE_LOG(LogWirelessPower, Log,
        TEXT("Wireless Power [reload-diag]   -> installed BP_UnlockRecipe_C root=%d mUnlocks=%d (process-persistent)"),
        Unlock->IsRooted() ? 1 : 0, Milestone->mUnlocks.Num());

    UE_LOG(LogWirelessPower, Warning,
        TEXT("Wireless Power [unlock]: %s <- %s recipes=%d (BP display unlock, root-pinned)"),
        *Milestone->GetClass()->GetName(), *Unlock->GetClass()->GetName(),
        Unlock->GetRecipesToUnlock().Num());
}

bool UWirelessPowerTierUnlock::PatrolUnlockList(UWirelessPowerMilestone* Milestone, const TArray<UClass*>& Recipes)
{
    // mUnlocks is protected on UFGSchematic and only this class is a declared friend, so the
    // free-function watchdog has to reach it through here (the compiler rejects it otherwise --
    // error C2248 at the patrol site). Returns true when the list is clean.
    if (!Milestone) return true;

    bool bBroken = false;
    for (const TObjectPtr<UFGUnlock>& U : Milestone->mUnlocks)   // friend access
    {
        if (!U || !U->IsA(UFGUnlockRecipe::StaticClass()))
        {
            bBroken = true;
        }
    }
    if (!bBroken) return true;

    UE_LOG(LogWirelessPower, Error,
        TEXT("Wireless Power [watchdog]: %s.mUnlocks contains a NULL/foreign entry -- reinstalling the root-pinned BP_UnlockRecipe_C display unlock."),
        *Milestone->GetClass()->GetName());
    InstallBlueprintUnlock(Milestone, Recipes);
    return false;
}

void UWirelessPowerMilestone::ApplyFeykoCost()
{
    if (MilestoneIndex < 0 || MilestoneIndex >= UE_ARRAY_COUNT(GMilestoneCosts))
    {
        UE_LOG(LogWirelessPower, Warning, TEXT("Wireless Power: milestone %s has no cost table (index %d)."),
            *GetClass()->GetName(), MilestoneIndex);
        return;
    }

    // Populate the Logistics category tab, like Feyko's originals.
    if (UClass* Category = StaticLoadClass(UFGSchematicCategory::StaticClass(), nullptr, GLogisticsCategory))
    {
        mSchematicCategory = TSubclassOf<UFGSchematicCategory>(Category);
    }
    else
    {
        UE_LOG(LogWirelessPower, Warning, TEXT("Wireless Power: could not load SC_Logistics for milestone %s."), *GetClass()->GetName());
    }

    const FWPMilestoneCost* Table = GMilestoneCosts[MilestoneIndex];
    const int32 Count = GMilestoneCostCounts[MilestoneIndex];

    mCost.Empty();
    for (int32 i = 0; i < Count; ++i)
    {
        UClass* ItemClass = StaticLoadClass(UFGItemDescriptor::StaticClass(), nullptr, Table[i].ItemPath);
        if (!ItemClass)
        {
            UE_LOG(LogWirelessPower, Warning, TEXT("Wireless Power: milestone %s could not resolve cost item '%s'."),
                *GetClass()->GetName(), Table[i].ItemPath);
            continue;
        }
        mCost.Add(FItemAmount(TSubclassOf<UFGItemDescriptor>(ItemClass), Table[i].Amount));
    }
}

UWirelessPowerMilestoneMk1::UWirelessPowerMilestoneMk1()
{
    const FText Name = WP_Text(TEXT("Wireless Power"), TEXT("无线电力"));
    const FText Desc = WP_Text(TEXT("Wireless Power. The Power Relay Mk1 (60 m), Power Converter and Power Feeder Mk1 are now unlocked in the build menu; this milestone advances the tech tier. Wireless nodes sharing an ID pool power with each other within range, with no cable between them."), TEXT("无线电力。电力中继器（60 米）、电力转换器与供电杆现已在建造菜单中解锁；本里程碑提升科技层级。相同 ID 的无线节点在范围内共享同一电力回路，节点之间无需鐋设电缆。"));
    InitMilestone(0, 2, Name, Desc);
}

UWirelessPowerMilestoneMk2::UWirelessPowerMilestoneMk2()
{
    const FText Name = WP_Text(TEXT("Wireless Power Mk2"), TEXT("无线电力 Mk2"));
    const FText Desc = WP_Text(TEXT("Wireless Power tier 2: the Power Relay Mk2 (120 m) and Power Feeder Mk2 (24 m) are now unlocked in the build menu."), TEXT("无线电力 第 2 阶：电力中继器 Mk2（120 米）与供电杆 Mk2（24 米）现已在建造菜单中解锁。"));
    InitMilestone(1, 4, Name, Desc);
}

UWirelessPowerMilestoneMk3::UWirelessPowerMilestoneMk3()
{
    const FText Name = WP_Text(TEXT("Wireless Power Mk3"), TEXT("无线电力 Mk3"));
    const FText Desc = WP_Text(TEXT("Unlocks the Power Transmission Tower: a 4800 m line-of-sight link that carries power across canyons. It also advances Wireless Power to tier 3 (Power Relay Mk3 at 160 m and Power Feeder Mk3 at 48 m, both unlocked by this tier)."), TEXT("解锁雷达塔：一条 4800 米的通视输电链路，可跨越峡谷输电。同时将无线电力提升至第 3 阶（电力中继器 Mk3，160 米，与供电杆 Mk3，48 米，均在本阶解锁）。"));
    InitMilestone(2, 6, Name, Desc);
}

// ----------------------------------------------------------------------------
// Shared descriptor
// ----------------------------------------------------------------------------
UWirelessPowerDescriptor::UWirelessPowerDescriptor()
{
    mUseDisplayNameAndDescription = true;
}

TSubclassOf<AFGHologram> UWirelessPowerDescriptor::GetHologramClassInternal() const { return HologramClass; }
FText UWirelessPowerDescriptor::GetItemNameInternal() const { return DisplayName; }
FText UWirelessPowerDescriptor::GetItemDescriptionInternal() const { return Desc; }

void UWirelessPowerDescriptor::InitCommon(bool bRegisterInBuildMenu)
{
    mBuildableClass = BuildableClass;
    // bRegisterInBuildMenu=false is used by the reserved (disabled) Power Converter Mk2/Mk3
    // descriptors: they keep their BuildableClass metadata for old-save compatibility but must NOT
    // appear in the build menu / HUB rewards / recipe manager. Leaving the build category unset is
    // what keeps them out of the Wireless Power build category entirely.
    if (bRegisterInBuildMenu)
    {
        static const TCHAR* CatNames[] = { TEXT("mBuildCategory"), TEXT("mCategory") };
        SetBuildCategoryProp(this, CatNames, UE_ARRAY_COUNT(CatNames), UWirelessPowerBuildCategory::StaticClass());
        SetSubCategoriesProp(this, UWirelessPowerBuildSubCategory::StaticClass());
    }
}

// ---- Relay descriptors (电源继电器) ----
// v1.0.46: renamed from "Wireless Power Relay"; range is now 24 / 48 / 96 m (the v1.0.19
// "+one zero" 120/240/480 m was applied to the wrong building). ID encoding is plain DECIMAL
// (1 / 2 / 3), mirroring the converter's old scheme -- the two families swapped encodings.
UWirelessPowerRelayMk1Descriptor::UWirelessPowerRelayMk1Descriptor()
{
    BuildableClass = AWirelessPowerRelay::StaticClass();
    HologramClass = AWirelessPowerHologram::StaticClass();
    // v1.0.64 (user): 中文名，与 Mk2/Mk3 的命名层级保持一致。
    DisplayName = WP_Text(TEXT("Power Relay Mk1"), TEXT("电力中继器 Mk1"));
    Desc = WP_Text(TEXT("An access point for the wireless grid. Wire it into a powered grid and it will share power with same-ID nodes within 60 m -- no cable between the nodes."), TEXT("无线电网的接入点。接入通电的电网后，它会与 60 米范围内相同 ID 的节点共享电力，节点之间无需铺设电缆。"));
    IconType = EWirelessBuildingType::Relay;
    InitCommon();
}
UWirelessPowerRelayMk2Descriptor::UWirelessPowerRelayMk2Descriptor()
{
    BuildableClass = AWirelessPowerRelayMk2::StaticClass();
    HologramClass = AWirelessPowerHologram::StaticClass();
    DisplayName = WP_Text(TEXT("Power Relay Mk2"), TEXT("电力中继器 Mk2"));
    Desc = WP_Text(TEXT("An access point for the wireless grid. Wire it into a powered grid and it will share power with same-ID nodes over a wider range than Mk1 -- no cable between the nodes."), TEXT("无线电网的接入点。接入通电的电网后，它会以比 Mk1 更大的范围与相同 ID 的节点共享电力，节点之间无需铺设电缆。"));
    IconType = EWirelessBuildingType::Relay;
    InitCommon();   // full tier: registered in the build menu + HUB rewards
}
UWirelessPowerRelayMk3Descriptor::UWirelessPowerRelayMk3Descriptor()
{
    BuildableClass = AWirelessPowerRelayMk3::StaticClass();
    HologramClass = AWirelessPowerHologram::StaticClass();
    DisplayName = WP_Text(TEXT("Power Relay Mk3"), TEXT("电力中继器 Mk3"));
    Desc = WP_Text(TEXT("An access point for the wireless grid. Wire it into a powered grid and it will share power with same-ID nodes over the widest range -- no cable between the nodes."), TEXT("无线电网的接入点。接入通电的电网后，它会以最大范围与相同 ID 的节点共享电力，节点之间无需铺设电缆。"));
    IconType = EWirelessBuildingType::Relay;
    InitCommon();   // full tier: registered in the build menu + HUB rewards
}

// ---- Power Converter descriptors (电源转换器) ----
// v1.0.46: renamed from "电力转换器"/"Power Converter"; it is now a SINGLE building (only the Mk1
// descriptor is registered) with a range of 48 m (the Mk3 tier, per user 2026-10-05). Its ID
// encoding is 4-bit BINARY (0001) -- swapped with the relay's old scheme.
UPowerConverterMk1Descriptor::UPowerConverterMk1Descriptor()
{
    BuildableClass = APowerConverter::StaticClass();
    HologramClass = AWirelessPowerHologram::StaticClass();
    DisplayName = WP_Text(TEXT("Power Converter"), TEXT("电力转换器"));
    Desc = WP_Text(TEXT("Converts between the wired and the wireless grid. Once wired into a powered grid it supplies same-ID nodes within 48 m."), TEXT("在「有线电网」与「无线电网」之间转换。接入通电的电网后，它会向 48 米范围内相同 ID 的节点供电。"));
    IconType = EWirelessBuildingType::Converter;
    InitCommon();
}
UPowerConverterMk2Descriptor::UPowerConverterMk2Descriptor()
{
    BuildableClass = APowerConverterMk2::StaticClass();
    HologramClass = AWirelessPowerHologram::StaticClass();
    DisplayName = WP_Text(TEXT("Power Converter Mk2 (disabled)"), TEXT("电力转换器 Mk2（未启用）"));
    Desc = WP_Text(TEXT("Tier reserved; not registered in the current version."), TEXT("该档位为预留档，当前版本未注册。"));
    IconType = EWirelessBuildingType::Converter;
    InitCommon(false);   // reserved tier: keep metadata, do NOT enter the build menu
}
UPowerConverterMk3Descriptor::UPowerConverterMk3Descriptor()
{
    BuildableClass = APowerConverterMk3::StaticClass();
    HologramClass = AWirelessPowerHologram::StaticClass();
    DisplayName = WP_Text(TEXT("Power Converter Mk3 (disabled)"), TEXT("电力转换器 Mk3（未启用）"));
    Desc = WP_Text(TEXT("Tier reserved; not registered in the current version."), TEXT("该档位为预留档，当前版本未注册。"));
    IconType = EWirelessBuildingType::Converter;
    InitCommon(false);   // reserved tier: keep metadata, do NOT enter the build menu
}

// ---- Area Power Pole descriptors (供电器) ----
UAreaPowerPoleMk1Descriptor::UAreaPowerPoleMk1Descriptor()
{
    BuildableClass = AAreaPowerPole::StaticClass();
    HologramClass = AWirelessPowerHologram::StaticClass();
    DisplayName = WP_Text(TEXT("Power Feeder Mk1"), TEXT("供电杆 Mk1"));
    Desc = WP_Text(TEXT("Connect machines to this node and it draws power from same-ID nodes within 12 m -- no cable between the nodes."), TEXT("将机器连接到此节点，它便会从 12 米范围内相同 ID 的节点取电，节点之间无需铺设电缆。"));
    IconType = EWirelessBuildingType::AreaPole;
    InitCommon();
}
UAreaPowerPoleMk2Descriptor::UAreaPowerPoleMk2Descriptor()
{
    BuildableClass = AAreaPowerPoleMk2::StaticClass();
    HologramClass = AWirelessPowerHologram::StaticClass();
    DisplayName = WP_Text(TEXT("Power Feeder Mk2"), TEXT("供电杆 Mk2"));
    Desc = WP_Text(TEXT("Connect machines to this node. Range 24 m."), TEXT("将机器连接到此节点。范围 24 米。"));
    IconType = EWirelessBuildingType::AreaPole;
    InitCommon();
}
UAreaPowerPoleMk3Descriptor::UAreaPowerPoleMk3Descriptor()
{
    BuildableClass = AAreaPowerPoleMk3::StaticClass();
    HologramClass = AWirelessPowerHologram::StaticClass();
    DisplayName = WP_Text(TEXT("Power Feeder Mk3"), TEXT("供电杆 Mk3"));
    Desc = WP_Text(TEXT("Connect machines to this node. Range 48 m."), TEXT("将机器连接到此节点。范围 48 米。"));
    IconType = EWirelessBuildingType::AreaPole;
    InitCommon();
}

// ---- Laser Tower descriptor (输电塔) ----
ULaserTowerDescriptor::ULaserTowerDescriptor()
{
    BuildableClass = ALaserTower::StaticClass();
    HologramClass = AWirelessPowerHologram::StaticClass();
    DisplayName = WP_Text(TEXT("Power Transmission Tower"), TEXT("激光塔"));
    Desc = WP_Text(TEXT("A long-range wireless power link (4800 m); towers need line of sight to each other. Carries power across canyons and over long distances."), TEXT("远距离无线输电链路（4800 米）；塔与塔之间需要互相通视。可跨越峡谷、远距离传输电力。"));
    IconType = EWirelessBuildingType::LaserTower;
    InitCommon();
}

// ----------------------------------------------------------------------------
// Shared recipe
// ----------------------------------------------------------------------------
UWirelessPowerRecipe::UWirelessPowerRecipe()
{
    mManufactoringDuration = 0.0f;
}

void UWirelessPowerRecipe::ApplyProducedIn(UClass* BuildGunClass)
{
    if (!BuildGunClass) return;
    mProducedIn.Empty();
    mProducedIn.Add(TSoftClassPtr<UObject>(BuildGunClass));
}

void UWirelessPowerRecipe::InitProduct(TSubclassOf<UFGBuildingDescriptor> DescClass)
{
    mProduct.Add(FItemAmount(DescClass, 1));
    mProducedIn.Add(TSoftClassPtr<UObject>(FSoftObjectPath(TEXT("/Game/FactoryGame/Equipment/BuildGun/BP_BuildGun.BP_BuildGun_C"))));
}

// ----------------------------------------------------------------------------
// v1.0.47 build costs ("the build gun shows the buildings as FREE" -- user report on v1.0.46).
// Root cause: InitProduct() filled only mProduct; mIngredients stayed empty, and the build gun
// reads the recipe's INGREDIENTS for the placement cost. Fill them here, resolved the same way
// as the milestone costs (StaticLoadClass at world-construction time, when /Game is mounted).
// ----------------------------------------------------------------------------
struct FWPCostLine
{
    const TCHAR* ItemPath;
    int32 Amount;
};

// Amounts are deliberately moderate (tier-appropriate vanilla parts); tweak here if the user
// wants different pricing. All paths are standard vanilla parts.
// v1.0.48 -- 'SteelBeam' (Desc_SteelBeam) does NOT exist in the shipped game; the runtime log
// showed "build cost could not resolve item" for it. Vanilla has no steel *beam* part at all
// (only SteelIngot / SteelPipe / SteelPlate / SteelPlateReinforced), so every SteelBeam line is
// now 钢管 SteelPipe.
static const FWPCostLine GCostRelayMk1[] = {   // 电源继电器 Mk1 (60 m)
    { TEXT("/Game/FactoryGame/Resource/Parts/IronPlate/Desc_IronPlate.Desc_IronPlate_C"), 10 },
    { TEXT("/Game/FactoryGame/Resource/Parts/Cable/Desc_Cable.Desc_Cable_C"), 15 },
};
static const FWPCostLine GCostRelayMk2[] = {   // 电源继电器 Mk2 (120 m)
    { TEXT("/Game/FactoryGame/Resource/Parts/SteelPipe/Desc_SteelPipe.Desc_SteelPipe_C"), 15 },
    { TEXT("/Game/FactoryGame/Resource/Parts/Cable/Desc_Cable.Desc_Cable_C"), 40 },
};
static const FWPCostLine GCostRelayMk3[] = {   // 电源继电器 Mk3 (160 m)
    { TEXT("/Game/FactoryGame/Resource/Parts/ModularFrameHeavy/Desc_ModularFrameHeavy.Desc_ModularFrameHeavy_C"), 4 },
    { TEXT("/Game/FactoryGame/Resource/Parts/HighSpeedWire/Desc_HighSpeedWire.Desc_HighSpeedWire_C"), 60 },
};
static const FWPCostLine GCostConverter[] = {  // 电源转换器 (12 m, single tier)
    { TEXT("/Game/FactoryGame/Resource/Parts/IronPlate/Desc_IronPlate.Desc_IronPlate_C"), 15 },
    { TEXT("/Game/FactoryGame/Resource/Parts/Wire/Desc_Wire.Desc_Wire_C"), 30 },
    { TEXT("/Game/FactoryGame/Resource/Parts/Cable/Desc_Cable.Desc_Cable_C"), 20 },
};
static const FWPCostLine GCostPoleMk1[] = {    // 供电器 Mk1 (12 m)
    { TEXT("/Game/FactoryGame/Resource/Parts/Cable/Desc_Cable.Desc_Cable_C"), 10 },
    { TEXT("/Game/FactoryGame/Resource/Parts/IronRod/Desc_IronRod.Desc_IronRod_C"), 10 },
};
static const FWPCostLine GCostPoleMk2[] = {    // 供电器 Mk2 (24 m)
    { TEXT("/Game/FactoryGame/Resource/Parts/SteelPlate/Desc_SteelPlate.Desc_SteelPlate_C"), 10 },
    { TEXT("/Game/FactoryGame/Resource/Parts/Cable/Desc_Cable.Desc_Cable_C"), 20 },
};
static const FWPCostLine GCostPoleMk3[] = {    // 供电器 Mk3 (48 m)
    { TEXT("/Game/FactoryGame/Resource/Parts/SteelPlate/Desc_SteelPlate.Desc_SteelPlate_C"), 20 },
    { TEXT("/Game/FactoryGame/Resource/Parts/HighSpeedWire/Desc_HighSpeedWire.Desc_HighSpeedWire_C"), 30 },
};
static const FWPCostLine GCostLaserTower[] = { // 输电塔 (4800 m)
    { TEXT("/Game/FactoryGame/Resource/Parts/SteelPipe/Desc_SteelPipe.Desc_SteelPipe_C"), 40 },
    { TEXT("/Game/FactoryGame/Resource/Parts/Cement/Desc_Cement.Desc_Cement_C"), 100 },
    { TEXT("/Game/FactoryGame/Resource/Parts/HighSpeedConnector/Desc_HighSpeedConnector.Desc_HighSpeedConnector_C"), 15 },
};

void UWirelessPowerRecipe::ApplyIngredients(const FWPCostLine* Lines, int32 Count)
{
    mIngredients.Empty();
    if (!Lines || Count <= 0) return;
    for (int32 i = 0; i < Count; ++i)
    {
        UClass* ItemClass = StaticLoadClass(UFGItemDescriptor::StaticClass(), nullptr, Lines[i].ItemPath);
        if (!ItemClass)
        {
            UE_LOG(LogWirelessPower, Warning, TEXT("Wireless Power: build cost could not resolve item '%s'."), Lines[i].ItemPath);
            continue;
        }
        mIngredients.Add(FItemAmount(TSubclassOf<UFGItemDescriptor>(ItemClass), Lines[i].Amount));
    }
}

UWirelessPowerRelayMk1Recipe::UWirelessPowerRelayMk1Recipe() { InitProduct(UWirelessPowerRelayMk1Descriptor::StaticClass()); }
UWirelessPowerRelayMk2Recipe::UWirelessPowerRelayMk2Recipe() { InitProduct(UWirelessPowerRelayMk2Descriptor::StaticClass()); }
UWirelessPowerRelayMk3Recipe::UWirelessPowerRelayMk3Recipe() { InitProduct(UWirelessPowerRelayMk3Descriptor::StaticClass()); }
UPowerConverterMk1Recipe::UPowerConverterMk1Recipe() { InitProduct(UPowerConverterMk1Descriptor::StaticClass()); }
UPowerConverterMk2Recipe::UPowerConverterMk2Recipe() { InitProduct(UPowerConverterMk2Descriptor::StaticClass()); }
UPowerConverterMk3Recipe::UPowerConverterMk3Recipe() { InitProduct(UPowerConverterMk3Descriptor::StaticClass()); }
UAreaPowerPoleMk1Recipe::UAreaPowerPoleMk1Recipe() { InitProduct(UAreaPowerPoleMk1Descriptor::StaticClass()); }
UAreaPowerPoleMk2Recipe::UAreaPowerPoleMk2Recipe() { InitProduct(UAreaPowerPoleMk2Descriptor::StaticClass()); }
UAreaPowerPoleMk3Recipe::UAreaPowerPoleMk3Recipe() { InitProduct(UAreaPowerPoleMk3Descriptor::StaticClass()); }
ULaserTowerRecipe::ULaserTowerRecipe() { InitProduct(ULaserTowerDescriptor::StaticClass()); }

// ----------------------------------------------------------------------------
// Registration
// ----------------------------------------------------------------------------
namespace WirelessPower
{
    /**
     * Resolves the /Game/... references the three milestones need (SC_Logistics + the cost item
     * descriptors). This has to happen with the game content mounted, which is why it runs from
     * RegisterBuildables() at POST_INITIALIZATION rather than from the CDO constructors.
     *
     * The same work is redone for every world, which is what we want: a dedicated server, a client
     * and a PIE session each get their own process (or world) and therefore their own CDOs.
     */
    void ApplyMilestoneData()
    {
        UE_LOG(LogWirelessPower, Log,
            TEXT("Wireless Power [reload-diag] ApplyMilestoneData gen=#%d"), GWPRegistrationGen);
        static const TSubclassOf<UFGSchematic> Milestones[] = {
            UWirelessPowerMilestoneMk1::StaticClass(),
            UWirelessPowerMilestoneMk2::StaticClass(),
            UWirelessPowerMilestoneMk3::StaticClass(),
        };
        // Headline building of each tier -- what the HUB milestone tile should show.
        static const EWirelessBuildingType MilestoneIcons[] = {
            EWirelessBuildingType::Relay,       // Mk1 : the first relay
            EWirelessBuildingType::Relay,       // Mk2 : adds the Power Relay Mk2
            EWirelessBuildingType::LaserTower,  // Mk3 : the tier that adds the Laser Tower
        };
        static_assert(UE_ARRAY_COUNT(Milestones) == UE_ARRAY_COUNT(MilestoneIcons),
            "every milestone needs a headline icon");

        // v1.0.44: per-tier recipe sets, installed as BP_UnlockRecipe_C instances (the only
        // unlock shape the HUB reward panel can display).
        //
        // v1.0.54 -- BACK TO FEYKO'S ORIGINAL GATING. v1.0.44b..v1.0.53 handed the 9 Relay /
        // Power Converter / Area Power Pole recipes out at world construction via
        // GrantDefaultUnlockedRecipes(), keeping only the Laser Tower milestone-gated. Feyko
        // never did that: his Schematic_WirelessPowerMk1/2/3 each carry their tier's recipes in
        // mUnlocks and the player earns them through the HUB. That grant has been removed, so
        // these tables are now the ONLY thing that unlocks our buildings -- every tier must list
        // its complete set (a BP_UnlockRecipe_C with ZERO recipes also renders as a BLANK rewards
        // panel in the HUB, the v1.0.44 regression).
        // Keep in sync with GWatchdogTierRecipes above (the self-heal patrols against it).
        static UClass* const GMk1Recipes[] = {
            UWirelessPowerRelayMk1Recipe::StaticClass(),
            UPowerConverterMk1Recipe::StaticClass(),
            UAreaPowerPoleMk1Recipe::StaticClass(),
        };        static UClass* const GMk2Recipes[] = {
            UWirelessPowerRelayMk2Recipe::StaticClass(),
            UAreaPowerPoleMk2Recipe::StaticClass(),
        };
        static UClass* const GMk3Recipes[] = {
            UWirelessPowerRelayMk3Recipe::StaticClass(),
            UAreaPowerPoleMk3Recipe::StaticClass(),
            ULaserTowerRecipe::StaticClass(),
        };
        static UClass* const* const GRecipeTables[] = { GMk1Recipes, GMk2Recipes, GMk3Recipes };
        static const int32 GRecipeCounts[] = {
            UE_ARRAY_COUNT(GMk1Recipes), UE_ARRAY_COUNT(GMk2Recipes), UE_ARRAY_COUNT(GMk3Recipes) };
        static_assert(UE_ARRAY_COUNT(Milestones) == UE_ARRAY_COUNT(GRecipeTables),
            "every milestone needs a recipe table");

        for (int32 Idx = 0; Idx < UE_ARRAY_COUNT(Milestones); ++Idx)
        {
            const TSubclassOf<UFGSchematic>& MilestoneClass = Milestones[Idx];
            UWirelessPowerMilestone* Milestone = Cast<UWirelessPowerMilestone>(MilestoneClass->GetDefaultObject());
            if (!Milestone)
            {
                UE_LOG(LogWirelessPower, Warning, TEXT("Wireless Power: %s is not a milestone CDO, skipping cost."),
                    MilestoneClass ? *MilestoneClass->GetName() : TEXT("NULL"));
                continue;
            }

            ApplyMilestoneIcon(Milestone, MilestoneIcons[Idx]);

            Milestone->ApplyFeykoCost();

            TArray<UClass*> Recipes;
            Recipes.Append(GRecipeTables[Idx], GRecipeCounts[Idx]);
            UWirelessPowerTierUnlock::InstallBlueprintUnlock(Milestone, Recipes);

            TArray<FString> CostParts;
            for (const FItemAmount& Cost : Milestone->GetCost(MilestoneClass))
            {
                CostParts.Add(FString::Printf(TEXT("%s x%d"),
                    Cost.ItemClass ? *Cost.ItemClass->GetName() : TEXT("NULL"), Cost.Amount));
            }
            WP_LOGF( TEXT("Wireless Power [milestone]: %s tier=%d cost=[%s]"),
                *MilestoneClass->GetName(), Milestone->GetTechTier(MilestoneClass),
                *FString::Join(CostParts, TEXT(", ")));
        }
    }

    // v1.0.54: GrantDefaultUnlockedRecipes() lived here (v1.0.44b..v1.0.53). It pushed the Relay /
    // Converter / Area Power Pole recipes straight into AFGRecipeManager at world construction so
    // every building was buildable from minute one. That was a detour from Feyko's original
    // design -- his three Schematic_WirelessPowerMk* assets award each tier through the HUB -- and
    // the user asked for the original behaviour back, so the function is gone entirely. Milestone
    // unlocks (ApplyMilestoneData's GRecipeTables) are now the only way our recipes get granted.

    void RegisterBuildables(UWorld* World)
    {
        if (!World) return;
        AFGRecipeManager* RM = AFGRecipeManager::Get(World);
        if (!RM)
        {
            UE_LOG(LogWirelessPower, Warning, TEXT("Wireless Power: AFGRecipeManager not ready yet, skipping registration."));
            return;
        }
        ++GWPRegistrationGen;

        // Fill in the milestone categories/costs. Safe to repeat (idempotent).
        ApplyMilestoneData();

        UClass* BuildGunClass = ResolveBuildGunClass();
        WP_LOGF( TEXT("Wireless Power: resolved Build Gun class = %s"),
            BuildGunClass ? *BuildGunClass->GetPathName() : TEXT("NULL"));
        // v1.1.3 [reload-diag]: log gen + world + recipe-manager identity so the reload crash
        // (PostLoadGame -> UpdateLookupMaps -> FindPair) can be matched to the exact registration.
        UE_LOG(LogWirelessPower, Log,
            TEXT("Wireless Power [reload-diag] RegisterBuildables gen=#%d world=%s RM=%p BuildGun=%s"),
            GWPRegistrationGen, *World->GetPathName(), (void*)RM,
            BuildGunClass ? *BuildGunClass->GetName() : TEXT("NULL"));

        struct FWPItem
        {
            TSubclassOf<UFGBuildingDescriptor> Desc;
            TSubclassOf<UFGRecipe> Recipe;
            EWirelessBuildingType Icon;
        };

        static const FWPItem Items[] =
        {
            { UWirelessPowerRelayMk1Descriptor::StaticClass(),     UWirelessPowerRelayMk1Recipe::StaticClass(),     EWirelessBuildingType::Relay },
            { UWirelessPowerRelayMk2Descriptor::StaticClass(),     UWirelessPowerRelayMk2Recipe::StaticClass(),     EWirelessBuildingType::Relay },
            { UWirelessPowerRelayMk3Descriptor::StaticClass(),     UWirelessPowerRelayMk3Recipe::StaticClass(),     EWirelessBuildingType::Relay },
            // v1.0.46 (user): the 电源转换器 is a SINGLE building -- only its Mk1 is registered. The
            // Mk2/Mk3 descriptors + recipes + buildables still exist in code (so old saves and the
            // CDO tree stay valid) but are deliberately absent from this table, so they never get a
            // producer / show up in the build gun, HUB rewards or the recipe manager.
            { UPowerConverterMk1Descriptor::StaticClass(),         UPowerConverterMk1Recipe::StaticClass(),         EWirelessBuildingType::Converter },
            { UAreaPowerPoleMk1Descriptor::StaticClass(),          UAreaPowerPoleMk1Recipe::StaticClass(),          EWirelessBuildingType::AreaPole },
            { UAreaPowerPoleMk2Descriptor::StaticClass(),          UAreaPowerPoleMk2Recipe::StaticClass(),          EWirelessBuildingType::AreaPole },
            { UAreaPowerPoleMk3Descriptor::StaticClass(),          UAreaPowerPoleMk3Recipe::StaticClass(),          EWirelessBuildingType::AreaPole },
            { ULaserTowerDescriptor::StaticClass(),                ULaserTowerRecipe::StaticClass(),                EWirelessBuildingType::LaserTower },
        };

        // mProducedIn must point at the real Build Gun class, otherwise
        // GetAvailableRecipesForProducer() never returns our recipes.
        for (const FWPItem& Item : Items)
        {
            if (UWirelessPowerRecipe* CDO = Cast<UWirelessPowerRecipe>(Item.Recipe->GetDefaultObject()))
            {
                CDO->ApplyProducedIn(BuildGunClass);
            }
        }

        // v1.0.47: fill mIngredients so the build gun charges for placement (was FREE before).
        // Recipes whose tiers are not registered (Converter Mk2/Mk3) are simply not in Items[]
        // above and never get costs applied -- they stay unbuildable, which is fine.
        struct FWPCostEntry { UClass* Recipe; const FWPCostLine* Lines; int32 Count; };
        static const FWPCostEntry Costs[] = {
            { UWirelessPowerRelayMk1Recipe::StaticClass(),  GCostRelayMk1,   UE_ARRAY_COUNT(GCostRelayMk1) },
            { UWirelessPowerRelayMk2Recipe::StaticClass(),  GCostRelayMk2,   UE_ARRAY_COUNT(GCostRelayMk2) },
            { UWirelessPowerRelayMk3Recipe::StaticClass(),  GCostRelayMk3,   UE_ARRAY_COUNT(GCostRelayMk3) },
            { UPowerConverterMk1Recipe::StaticClass(),      GCostConverter,  UE_ARRAY_COUNT(GCostConverter) },
            { UAreaPowerPoleMk1Recipe::StaticClass(),       GCostPoleMk1,    UE_ARRAY_COUNT(GCostPoleMk1) },
            { UAreaPowerPoleMk2Recipe::StaticClass(),       GCostPoleMk2,    UE_ARRAY_COUNT(GCostPoleMk2) },
            { UAreaPowerPoleMk3Recipe::StaticClass(),       GCostPoleMk3,    UE_ARRAY_COUNT(GCostPoleMk3) },
            { ULaserTowerRecipe::StaticClass(),             GCostLaserTower, UE_ARRAY_COUNT(GCostLaserTower) },
        };
        for (const FWPCostEntry& Entry : Costs)
        {
            if (UWirelessPowerRecipe* CDO = Cast<UWirelessPowerRecipe>(Entry.Recipe->GetDefaultObject()))
            {
                CDO->ApplyIngredients(Entry.Lines, Entry.Count);
            }
        }

        // v1.0.54: the v1.0.44b GrantDefaultUnlockedRecipes() call that used to live here has been
        // REMOVED. Feyko's original mod unlocks every building through the HUB milestones, and the
        // user asked for that behaviour back (the "everything is buildable from minute one" grant
        // was a v1.0.44b detour, not his design). Nothing else needs to run after the cost loop:
        // recipes only become available when AFGRecipeManager applies a purchased schematic.

        for (int32 i = 0; i < UE_ARRAY_COUNT(Items); ++i)
        {
            WP_LOGF( TEXT("Wireless Power: item[%d] %s descAvail=%d recipeAvail=%d"),
                i, *Items[i].Desc->GetName(),
                RM->IsItemDescriptorAvailable(Items[i].Desc) ? 1 : 0,
                RM->IsRecipeAvailable(Items[i].Recipe) ? 1 : 0);
        }

        for (const FWPItem& Item : Items)
        {
            ApplyWirelessIcon(Item.Desc, Item.Icon);
        }

        // v1.0.64: give the build-menu category tab its icon (runs after content is mounted).
        UWirelessPowerBuildCategory::ApplyIcon();

        WPStartIconWatchdog(World);

        WP_LOGF( TEXT("Wireless Power: 8 buildables registered; Relay/Converter/Pole Mk1-Mk3 are default-unlocked, Laser Tower is gated behind the Mk3 milestone (tier 6)."));
    }
}
