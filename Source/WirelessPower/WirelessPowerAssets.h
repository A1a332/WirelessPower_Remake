#pragma once

#include "CoreMinimal.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInterface.h"
#include "Particles/ParticleSystem.h"
#include "WirelessPowerTypes.h"

/**
 * Asset loaders for the Wireless Power buildables.
 *
 * Feyko's original (discontinued, MIT) mod shipped its own modelling. We reuse his
 * meshes / icons, copied verbatim into our mod's Content/ so the internal references
 * resolve. The .uasset files came from UE4; if they fail to cook/load under Satisfactory
 * 1.1 (UE5.6-CSS), re-import the bundled .fbx / .png via the headless importer and the
 * loaders here will pick up the new assets at the same paths. If nothing resolves we fall
 * back to a confirmed-existing vanilla power pole so the building is never invisible.
 *
 * Mount-root candidates cover the ways SML can expose a mod's Content root
 * (/WirelessPower/, /Game/WirelessPower/, /wirelesspower/, /Game/wirelesspower/). Only one
 * of them resolves at runtime, so we try them all.
 */
namespace WirelessPowerAssets
{
    inline UStaticMesh* LoadWirelessMesh(EWirelessBuildingType Type, int32 Tier)
    {
        // Relative asset paths (without extension / root) keyed by building + tier.
        FString RelPath;
        switch (Type)
        {
            case EWirelessBuildingType::Relay:
                RelPath = TEXT("Buildable/WirelessPowerRelay/Mesh/SM_anim_relay.SM_anim_relay");
                break;
            case EWirelessBuildingType::Converter:
                RelPath = TEXT("Buildable/PowerConverter/Mesh/Mesh_Converter.Mesh_Converter");
                break;
            case EWirelessBuildingType::AreaPole:
                // All three Area Power Pole tiers share Feyko's original Mk1 model (the user
                // confirmed Mk1 is the author's model, while polemk2/polemk3 are not). Tier
                // scale (1.0 / 1.3 / 1.7) in InitWireless gives the size progression.
                RelPath = TEXT("Buildable/AreaPowerPoles/AreaPoles/AreaPowerPoleMk1/SF_PowerPoleMk1.SF_PowerPoleMk1");
                break;
            case EWirelessBuildingType::LaserTower:
                RelPath = TEXT("Buildable/LaserTower/WP_Mesh_LaserTower.WP_Mesh_LaserTower");
                break;
            default:
                RelPath = TEXT("Buildable/WirelessPowerRelay/Mesh/SM_anim_relay.SM_anim_relay");
                break;
        }

        static const TCHAR* Roots[] =
        {
            // ★2026-10-11: 插件改名 WirelessPower_Remake 后，UE 按 uplugin 名挂内容根
            // /Game/<uplugin名>/，故新根必须排最前；旧根保留兜底（旧身份部署仍可用）。
            TEXT("/Game/WirelessPower_Remake/"),
            TEXT("/WirelessPower_Remake/"),
            TEXT("/Game/WirelessPower/"),
            TEXT("/WirelessPower/"),
        };

        for (const TCHAR* Root : Roots)
        {
            const FString Full = FString(Root) + RelPath;
            if (UStaticMesh* M = LoadObject<UStaticMesh>(nullptr, *Full))
            {
                return M;
            }
        }

        // Fallback: a vanilla power pole of the MATCHING tier (Mk1->Mk1, Mk2->Mk2, Mk3->Mk3)
        // so a missing/uncookable Feyko mesh still yields a distinct, correct model per tier,
        // instead of every tier silently showing Mk1.
        const TCHAR* Vanilla = nullptr;
        switch (Tier)
        {
            case 3:  Vanilla = TEXT("/Game/FactoryGame/Buildable/Factory/PowerPoleMk3/Mesh/SM_PowerPole_Mk3.SM_PowerPole_Mk3"); break;
            case 2:  Vanilla = TEXT("/Game/FactoryGame/Buildable/Factory/PowerPoleMk2/Mesh/SM_PowerPole_Mk2.SM_PowerPole_Mk2"); break;
            default: Vanilla = TEXT("/Game/FactoryGame/Buildable/Factory/PowerPoleMk1/Mesh/SM_PowerPole_Mk1.SM_PowerPole_Mk1"); break;
        }
        if (UStaticMesh* M = LoadObject<UStaticMesh>(nullptr, Vanilla))
        {
            UE_LOG(LogTemp, Warning,
                TEXT("[WirelessPower] Feyko mesh for building type %d tier %d not found -- using vanilla power-pole placeholder. Fix/re-import the original .uasset if you want the Feyko model."),
                (int32)Type, Tier);
            return M;
        }
        return nullptr;
    }

    /**
     * Shared UNLIT material used by the range cage and the laser beams (v1.0.22).
     *
     * /WirelessPower/Shapes/M_WirelessBeam is an unlit material whose Emissive is driven by a
     * vector parameter named "Color", so each building can tint its own cage/beam through a
     * MaterialInstanceDynamic (see WirelessPowerBuildable::EnsureVisualMaterials). Created
     * headlessly by SMLDev/create_beam_material.py.
     *
     * Returns nullptr if the asset is missing -- callers then keep the vanilla BasicShapeMaterial,
     * which is grey but still renders, so the geometry never silently disappears.
     */
    inline UMaterialInterface* LoadBeamMaterial()
    {
        static TWeakObjectPtr<UMaterialInterface> Cached;
        if (Cached.IsValid())
        {
            return Cached.Get();
        }

        static const TCHAR* Roots[] =
        {
            // ★2026-10-11: 插件改名 WirelessPower_Remake 后，UE 按 uplugin 名挂内容根
            // /Game/<uplugin名>/，故新根必须排最前；旧根保留兜底（旧身份部署仍可用）。
            TEXT("/Game/WirelessPower_Remake/"),
            TEXT("/WirelessPower_Remake/"),
            TEXT("/Game/WirelessPower/"),
            TEXT("/WirelessPower/"),
        };
        for (const TCHAR* Root : Roots)
        {
            const FString Full = FString(Root) + TEXT("Shapes/M_WirelessBeam.M_WirelessBeam");
            if (UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, *Full))
            {
                Cached = M;
                return M;
            }
        }
        return nullptr;
    }

    /**
     * Feyko's ORIGINAL laser beam (v1.0.32).
     *
     * The original mod is Blueprint-only: its Build_LaserTower graph drives a
     * UParticleSystemComponent playing /WirelessPower/Buildable/LaserTower/P_Laser, and feeds every
     * link with SetBeamSourcePoint / SetBeamTargetPoint (verified from the asset's own name table:
     * "ParticleSystemComponent | P_Laser | SetBeamSourcePoint | SetBeamTargetPoint | NewSourcePoint |
     * NewTargetPoint | SourceIndex | TargetIndex"). P_Laser is a single Cascade Beam2 emitter called
     * "Laser" -- ParticleModuleTypeDataBeam2 + ParticleModuleBeamSource/Target + BeamNoise -- whose
     * material M_Laser is MSM_UNLIT / BLEND_TRANSLUCENT and builds a hair-thin glowing core out of a
     * cross-section falloff (Sine + Power over TextureCoordinate + ParticleColor), i.e. it needs NO
     * texture at all. That is why the original reads as a thin line instead of a solid bar.
     *
     * Cascade is still fully present in Satisfactory's UE5.6-CSS engine
     * (Engine/.../Particles/TypeData/ParticleModuleTypeDataBeam2.h + ParticleBeam2EmitterInstance.cpp),
     * and the asset itself loads (verified headlessly: Class 'ParticleSystem', IsValid=True), so we
     * simply reuse it verbatim instead of faking the look with geometry.
     *
     * Returns nullptr if the asset is missing -- callers then fall back to the cube-instance bars so
     * a link is never invisible.
     */
    inline UParticleSystem* LoadLaserParticleSystem()
    {
        static TWeakObjectPtr<UParticleSystem> Cached;
        if (Cached.IsValid())
        {
            return Cached.Get();
        }

        static const TCHAR* Roots[] =
        {
            // ★2026-10-11: 插件改名 WirelessPower_Remake 后，UE 按 uplugin 名挂内容根
            // /Game/<uplugin名>/，故新根必须排最前；旧根保留兜底（旧身份部署仍可用）。
            TEXT("/Game/WirelessPower_Remake/"),
            TEXT("/WirelessPower_Remake/"),
            TEXT("/Game/WirelessPower/"),
            TEXT("/WirelessPower/"),
        };
        for (const TCHAR* Root : Roots)
        {
            const FString Full = FString(Root) + TEXT("Buildable/LaserTower/P_Laser.P_Laser");
            if (UParticleSystem* P = LoadObject<UParticleSystem>(nullptr, *Full))
            {
                Cached = P;
                return P;
            }
        }
        return nullptr;
    }

    inline UTexture2D* LoadWirelessIcon(EWirelessBuildingType Type)    {
        FString RelPath;
        switch (Type)
        {
            case EWirelessBuildingType::Relay:      RelPath = TEXT("Icon/icon_relay.icon_relay"); break;
            case EWirelessBuildingType::Converter:   RelPath = TEXT("Icon/icon_converter.icon_converter"); break;
            case EWirelessBuildingType::AreaPole:    RelPath = TEXT("Icon/icon_power_pole.icon_power_pole"); break;
            case EWirelessBuildingType::LaserTower:  RelPath = TEXT("Icon/icon_laser_tower.icon_laser_tower"); break;
            default:                                 RelPath = TEXT("Icon/icon_relay.icon_relay"); break;
        }

        static const TCHAR* Roots[] =
        {
            // ★2026-10-11: 插件改名 WirelessPower_Remake 后，UE 按 uplugin 名挂内容根
            // /Game/<uplugin名>/，故新根必须排最前；旧根保留兜底（旧身份部署仍可用）。
            TEXT("/Game/WirelessPower_Remake/"),
            TEXT("/WirelessPower_Remake/"),
            TEXT("/Game/WirelessPower/"),
            TEXT("/WirelessPower/"),
        };
        for (const TCHAR* Root : Roots)
        {
            const FString Full = FString(Root) + RelPath;
            if (UTexture2D* T = LoadObject<UTexture2D>(nullptr, *Full))
            {
                return T;
            }
        }
        return nullptr;
    }

    /**
     * WHITE-SILHOUETTE sibling of LoadWirelessIcon, for HUB milestone (schematic) slots (v1.0.41).
     *
     * The HUB applies a tint to schematic icons -- a full-colour render collapses into a flat
     * orange block (observed v1.0.40). The icon_milestone_* assets carry the SAME alpha outline
     * but pure-white RGB, so the tint yields a readable silhouette. The four build-menu
     * description icons keep using the full-colour LoadWirelessIcon path.
     */
    inline UTexture2D* LoadMilestoneIcon(EWirelessBuildingType Type)
    {
        FString RelPath;
        switch (Type)
        {
            case EWirelessBuildingType::Relay:
                RelPath = TEXT("Icon/icon_milestone_relay.icon_milestone_relay"); break;
            case EWirelessBuildingType::Converter:
                RelPath = TEXT("Icon/icon_milestone_converter.icon_milestone_converter"); break;
            case EWirelessBuildingType::AreaPole:
                RelPath = TEXT("Icon/icon_milestone_power_pole.icon_milestone_power_pole"); break;
            case EWirelessBuildingType::LaserTower:
                RelPath = TEXT("Icon/icon_milestone_laser_tower.icon_milestone_laser_tower"); break;
            default:
                RelPath = TEXT("Icon/icon_milestone_relay.icon_milestone_relay"); break;
        }

        static const TCHAR* Roots[] =
        {
            // ★2026-10-11: 插件改名 WirelessPower_Remake 后，UE 按 uplugin 名挂内容根
            // /Game/<uplugin名>/，故新根必须排最前；旧根保留兜底（旧身份部署仍可用）。
            TEXT("/Game/WirelessPower_Remake/"),
            TEXT("/WirelessPower_Remake/"),
            TEXT("/Game/WirelessPower/"),
            TEXT("/WirelessPower/"),
        };
        for (const TCHAR* Root : Roots)
        {
            const FString Full = FString(Root) + RelPath;
            if (UTexture2D* T = LoadObject<UTexture2D>(nullptr, *Full))
            {
                return T;
            }
        }
        return nullptr;
    }
}
