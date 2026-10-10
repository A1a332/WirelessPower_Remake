#pragma once

#include "WirelessPowerBuildable.h"
#include "AreaPowerPole.generated.h"

/**
 * Area Power Pole -- a wireless node you wire your machines into. Within its range it links
 * (hidden connection) to same-channel peers, so a machine wired to this pole is powered by
 * whatever grid any peer on the channel is feeding. Larger tiers reach further.
 *
 * v1.0.58 (Feyko parity): it also carries ONE visible wire terminal, and it is the only node that
 * broadcasts power to in-range vanilla machines. See AreaPowerPole.cpp for the evidence.
 */
UCLASS()
class WIRELESSPOWER_API AAreaPowerPole : public AWirelessPowerBuildable
{
    GENERATED_BODY()
public:
    AAreaPowerPole();
};

UCLASS()
class WIRELESSPOWER_API AAreaPowerPoleMk2 : public AAreaPowerPole
{
    GENERATED_BODY()
public:
    AAreaPowerPoleMk2();
};

UCLASS()
class WIRELESSPOWER_API AAreaPowerPoleMk3 : public AAreaPowerPole
{
    GENERATED_BODY()
public:
    AAreaPowerPoleMk3();
};
