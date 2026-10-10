#pragma once

#include "WirelessPowerBuildable.h"
#include "WirelessPowerRelay.generated.h"

/**
 * Wireless Power Relay -- a pure peer node on the wireless grid.
 * Wire it into any power grid; it shares that grid with every same-channel peer within
 * range (hidden connection, no cable). Relays are the backbone that ties a Converter's
 * grid to Area Power Poles feeding machines.
 */
UCLASS()
class WIRELESSPOWER_API AWirelessPowerRelay : public AWirelessPowerBuildable
{
    GENERATED_BODY()
public:
    AWirelessPowerRelay();
};

UCLASS()
class WIRELESSPOWER_API AWirelessPowerRelayMk2 : public AWirelessPowerRelay
{
    GENERATED_BODY()
public:
    AWirelessPowerRelayMk2();
};

UCLASS()
class WIRELESSPOWER_API AWirelessPowerRelayMk3 : public AWirelessPowerRelay
{
    GENERATED_BODY()
public:
    AWirelessPowerRelayMk3();
};
