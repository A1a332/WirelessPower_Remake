#include "LaserTower.h"

ALaserTower::ALaserTower()
{
    // Laser Tower (输电塔, v1.0.46 renamed from 无线输电塔): 4 visible wire ports (different towers
    // interconnect via different channel numbers), does NOT broadcast to machines. Very long range
    // (4800 m), line-of-sight enforced.
    InitWireless(EWirelessBuildingType::LaserTower, 1, 480000.f, true, 4, false);
}
