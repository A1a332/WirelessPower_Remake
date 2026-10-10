#include "AreaPowerPole.h"

// v1.0.46 (user): the "Area Power Pole" family is renamed 供电器 (Power Supplier).
// Class names stay AAreaPowerPole* to keep save-game class references stable; only the
// display names (descriptors / panel title) change. Ranges unchanged.

// v1.0.58 -- Feyko parity: the Area Power Pole regains its REAL wire terminal.
//
// Evidence (uasset ImportTable, see _archive/feyko_gh/FEYKO_POLE_LOGIC.md §3.1/§3.1b): the
// original `Build_AreaPowerPole` does NOT inherit AFGBuildablePowerPole -- its parent is plain
// AFGBuildable -- but it does attach its own SCS components, and among them is a genuine
// `FGPowerConnection` (a UFGPowerConnectionComponent), a second hidden `Hidden`, and an
// `FGPowerInfo` wired up with SetPowerInfo(). So "原作者没有电线接口" is not the case: he built
// an equivalent terminal himself.
//
// That terminal is what makes "只铺电线杆即可" work: you run ONE cable from your generators into
// the first pole, and every further pole in the row joins it wirelessly (distance, no 编号).
//
// So the pole now asks InitWireless for 1 VISIBLE port (it used to ask for 0). The antenna path is
// untouched -- EnsureInternalBridge() fuses the new port to the hidden antenna exactly as it does
// for the Converter, and HasWiredPower() consults both ends, so the pole stays "active" whether it
// is fed by that cable or by the wireless mesh behind it.

AAreaPowerPole::AAreaPowerPole()
{
    // Area Power Pole (供电器): 1 visible wire port (the real terminal, Feyko parity) and it is the
    // ONLY node that broadcasts power to in-range vanilla machines (true).
    // Mk1 range = 12 m (cubic half-extent).
    InitWireless(EWirelessBuildingType::AreaPole, 1, 1200.f, false, 1, true);
}

AAreaPowerPoleMk2::AAreaPowerPoleMk2()
{
    // Mk2 range = 24 m.
    InitWireless(EWirelessBuildingType::AreaPole, 2, 2400.f, false, 1, true);
}

AAreaPowerPoleMk3::AAreaPowerPoleMk3()
{
    // Mk3 range = 48 m.
    InitWireless(EWirelessBuildingType::AreaPole, 3, 4800.f, false, 1, true);
}
