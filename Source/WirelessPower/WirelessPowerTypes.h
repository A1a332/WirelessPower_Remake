#pragma once

#include "CoreMinimal.h"

/**
 * The four building families of the Wireless Power mod (revival of Feyko's design).
 * All four are peer nodes on one wireless power circuit: any two nodes that share the
 * same Channel and are within each other's WirelessRange auto-link (hidden circuit
 * connection), so a powered grid wired into one node feeds any machine wired into
 * another node on the same channel. Laser Tower additionally requires line-of-sight.
 */
UENUM(BlueprintType)
enum class EWirelessBuildingType : uint8
{
    Relay      UMETA(DisplayName = "Wireless Power Relay"),
    Converter   UMETA(DisplayName = "Power Converter"),
    AreaPole    UMETA(DisplayName = "Area Power Pole"),
    LaserTower  UMETA(DisplayName = "Laser Tower"),
};

/**
 * Unreal world units per in-game metre (1 Unreal unit == 1 cm in this game).
 *
 * v1.0.21: every range constant and the whole engine side stay in centimetres, but the CONTROL
 * PANEL speaks metres -- the player never has to type or read a centimetre value. Convert in the
 * panel only: display = UU / WIRELESS_UU_PER_METRE, entry = metres * WIRELESS_UU_PER_METRE.
 */
inline constexpr float WIRELESS_UU_PER_METRE = 100.f;

/** A WirelessRange in Unreal units (cm) expressed as whole metres, for display. */
inline int32 WirelessRangeToMetres(float RangeUU)
{
    return FMath::RoundToInt(RangeUU / WIRELESS_UU_PER_METRE);
}

/** Per-tier scale multipliers (Mk1=1.0, Mk2=1.3, Mk3=1.7) applied to the mesh. */
inline float WirelessPowerTierScale(int32 Tier)
{
    switch (Tier)
    {
        case 3:  return 1.7f;
        case 2:  return 1.3f;
        default: return 1.0f;
    }
}

/**
 * Per-building colour of the coverage cage (also used to tint the panel's range-display switch).
 *
 * Four clearly separated hues, one per building family, so a player standing in a base with
 * several cubes on screen can tell at a glance which building each cube belongs to. The amber
 * used for the Area Power Pole is the original mod's accent colour.
 */
inline FLinearColor WirelessPowerCageColor(EWirelessBuildingType Type)
{
    switch (Type)
    {
        case EWirelessBuildingType::Converter:  return FLinearColor(0.62f, 0.38f, 1.00f, 1.f); // 紫 · 电力转换器
        case EWirelessBuildingType::AreaPole:   return FLinearColor(1.00f, 0.55f, 0.10f, 1.f); // 橙 · 区域电线杆
        case EWirelessBuildingType::LaserTower: return FLinearColor(1.00f, 0.25f, 0.20f, 1.f); // 红 · 无线输电塔
        default:                                return FLinearColor(0.15f, 0.78f, 1.00f, 1.f); // 青 · 无线中继器
    }
}

#include "WirelessPowerTypes.generated.h"
