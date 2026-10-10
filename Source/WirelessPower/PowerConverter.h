#pragma once

#include "WirelessPowerBuildable.h"
#include "PowerConverter.generated.h"

/**
 * Power Converter -- the gateway between a WIRED grid and the wireless network.
 * Wire it into your powered grid; it brings that grid into the wireless mesh so any
 * same-channel peer (Relay / Area Power Pole / Laser Tower) on the same channel can
 * draw from it without cables.
 */
UCLASS()
class WIRELESSPOWER_API APowerConverter : public AWirelessPowerBuildable
{
    GENERATED_BODY()
public:
    APowerConverter();
};

UCLASS()
class WIRELESSPOWER_API APowerConverterMk2 : public APowerConverter
{
    GENERATED_BODY()
public:
    APowerConverterMk2();
};

UCLASS()
class WIRELESSPOWER_API APowerConverterMk3 : public APowerConverter
{
    GENERATED_BODY()
public:
    APowerConverterMk3();
};
