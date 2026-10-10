#include "WirelessPowerModule.h"
#include "Logging/LogMacros.h"
#include "WirelessPowerVersion.h"

DEFINE_LOG_CATEGORY_STATIC(LogWirelessPower, Log, All);

void FWirelessPowerModule::StartupModule()
{
    // v1.0.29: QUIET LOG. This banner is the ONLY per-session Log line the mod still prints -- it is
    // the fingerprint we use to confirm which build the game actually loaded, so it must stay cheap
    // and it must name the version. Every other Log-level call now goes through WP_LOGF() in
    // WirelessPowerLog.h (compiled out by default). See that header for the reasoning.
    UE_LOG(LogWirelessPower, Log, TEXT("Wireless Power ") WIRELESS_POWER_VERSION_TEXT TEXT(" (build fingerprint; remainder is historical design notes, NOT a current feature list. milestone gating restored, by user request. v1.0.44b..v1.0.53 handed the Relay / Power Converter / Area Power Pole recipes out at world construction (GrantDefaultUnlockedRecipes) so every building was buildable from minute one, with only the Laser Tower left behind the Mk3 milestone. That was a self-inflicted detour, NOT Feyko's design: his Schematic_WirelessPowerMk1/2/3 each carry their tier's recipes in mUnlocks and the player earns them through the HUB (verified by parsing the three .uasset files: Mk1 -> relay Mk1 + converter + pole Mk1, Mk2 -> relay Mk2 + pole Mk2, Mk3 -> relay Mk3 + pole Mk3 + laser tower). GrantDefaultUnlockedRecipes is deleted outright and the milestone recipe tables are widened to their full original sets, so a fresh save starts with NOTHING unlocked and must buy Mk1 at HUB tech tier 2. NOTE saves that already ran v1.0.44b..v1.0.53 keep their unlock: mAvailableRecipes is SaveGame and we do not revoke. Our one deliberate departure from Feyko stands: we ship a Power Converter per tier, so Mk2/Mk3 each award their own converter (his converter was single-tier and rode Mk1). keeps v1.0.53: cube coverage volume (deliberate user trade-off, do not sphere it again); keeps v1.0.52 (phase A): IDs are STRINGS again, exactly like Feyko's original -- his Relay blueprint compares with EqualEqual_StriStri and his panel literal reads 'Set ID here', so our int32 encoding (and the v1.0.46 4-bit-binary display for the converter) was a self-inflicted detour that produced the 'converter cannot set its ID' bug. Channel is now a replicated/saved FString compared through FWirelessId::Equal (trim ends + full-width->half-width + ASCII uppercase), so Chinese IDs are natively legal, and the panel input filter only enforces a 24-char cap instead of a digits-only whitelist (that whitelist was exactly what ate the converter's ID). Old saves are migrated once on BeginPlay: an all-digit id n becomes its decimal string, 0 becomes the empty default network, and any id containing non-digits is left untouched; keeps v1.0.51: converter-ID input filter fix; keeps v1.0.50: removed the bogus Alpakit dependency from the mod .uplugin that made the game pop the UE Missing Plugin dialog at every launch (Alpakit exists only in the dev project, never in the shipped game); keeps v1.0.49: cost-item fix (Desc_SteelBeam / Desc_EncasedIndustrialBeam do not exist in the shipped game -> SteelPipe / SteelPlateReinforced); keeps v1.0.48: Relay Mk1/Mk2/Mk3 restored to Feyko's original 60/120/160 m."));;
}

void FWirelessPowerModule::ShutdownModule()
{
    // Also keep the exit line: one line per session, and it is the only way to tell from the log
    // whether an unclean shutdown left the mod loaded.
    UE_LOG(LogWirelessPower, Log, TEXT("Wireless Power unloaded."));
}

IMPLEMENT_MODULE(FWirelessPowerModule, WirelessPower)
