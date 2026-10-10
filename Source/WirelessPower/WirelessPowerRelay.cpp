#include "WirelessPowerRelay.h"

AWirelessPowerRelay::AWirelessPowerRelay()
{
    // Relay: NO visible port (pure wireless mesh node), does NOT broadcast to machines.
    // v1.0.6x: consolidated to a SINGLE tier per user request; the Mk2/Mk3 relay tiers are
    // disabled (see Registration.cpp -- their descriptors call InitCommon(false) and are dropped
    // from the milestone recipe tables). The one remaining relay keeps its ORIGINAL Mk1 range
    // (6000 cm = 60 m); the briefly-used Mk3 160 m was reverted at user request. The other three
    // machines (feeder / converter / laser tower) are intentionally untouched.
    InitWireless(EWirelessBuildingType::Relay, 1, 6000.f, false, 0, false);
}

AWirelessPowerRelayMk2::AWirelessPowerRelayMk2()
{
    // v1.0.48: Feyko original = 12000.0 cm -> 120 m (Mk2 uasset offset 104485).
    InitWireless(EWirelessBuildingType::Relay, 2, 12000.f, false, 0, false);
}

AWirelessPowerRelayMk3::AWirelessPowerRelayMk3()
{
    // v1.0.48: Feyko original = 16000.0 cm -> 160 m (Mk3 uasset offset 104485).
    // NOTE: 160 breaks the doubling pattern (60 -> 120 -> 160); this is what Feyko
    // actually shipped -- the only value that varies across the three uassets at the
    // same property-stream offset, so it is taken as the authoritative original.
    InitWireless(EWirelessBuildingType::Relay, 3, 16000.f, false, 0, false);
}
