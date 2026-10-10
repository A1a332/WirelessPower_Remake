#include "WirelessPowerConnection.h"

UWirelessPowerConnection::UWirelessPowerConnection()
{
    // Generous headroom: 1 player cable + 1 internal bridge + up to 16 peer links (MaxLinks).
    mMaxNumConnectionLinks = 64;
}
