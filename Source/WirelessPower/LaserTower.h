#pragma once

#include "WirelessPowerBuildable.h"
#include "LaserTower.generated.h"

/**
 * Laser Tower -- long-range wireless link that requires line-of-sight between towers
 * (terrain/cliffs block the beam). Use pairs (or chains) of towers to bridge power across
 * canyons and long distances that normal relays can't reach.
 */
UCLASS()
class WIRELESSPOWER_API ALaserTower : public AWirelessPowerBuildable
{
    GENERATED_BODY()
public:
    ALaserTower();
};
