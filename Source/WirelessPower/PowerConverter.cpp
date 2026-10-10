#include "PowerConverter.h"

APowerConverter::APowerConverter()
{
    // v1.0.46: the Power Converter is a SINGLE building (no Mk2/Mk3). User (2026-10-05): Mk1 uses
    // the Mk3 range (48 m) even though Mk2/Mk3 are not registered -- the converter is a one-size
    // building but should reach as far as the top tier would have. The other two tiers
    // (APowerConverterMk2/Mk3) are kept compiled but are NOT registered, so they never show up in
    // the build gun / HUB / recipe manager.
    InitWireless(EWirelessBuildingType::Converter, 1, 4800.f, false, 1, false);
}

APowerConverterMk2::APowerConverterMk2()
{
    // Unused legacy tier (see above). 24 m if it is ever re-registered.
    InitWireless(EWirelessBuildingType::Converter, 2, 2400.f, false, 1, false);
}

APowerConverterMk3::APowerConverterMk3()
{
    // Unused legacy tier (see above). 48 m if it is ever re-registered.
    InitWireless(EWirelessBuildingType::Converter, 3, 4800.f, false, 1, false);
}
