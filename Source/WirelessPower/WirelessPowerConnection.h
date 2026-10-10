#pragma once

#include "FGPowerConnectionComponent.h"
#include "WirelessPowerConnection.generated.h"

/**
 * A UFGPowerConnectionComponent tuned for the Wireless Power buildables.
 *
 * The shipped SDK default for UFGCircuitConnectionComponent::mMaxNumConnectionLinks is 1,
 * which only allows a single link on a port. Our nodes need far more:
 *   - The visible ports (Laser Tower has up to 4, Converter has 1) each need 1 player cable
 *     PLUS 1 internal "bridge" hidden link to the antenna (so a grid wired into the visible
 *     port feeds the wireless mesh and vice-versa).
 *   - The hidden antenna needs: 1 internal bridge per visible port + up to MaxLinks peer
 *     hidden links + (for Area Power Pole only) hidden links to every in-range machine.
 * Because mMaxNumConnectionLinks is protected with no public setter, we raise it here (a
 * subclass may write its own protected base members) and expose SetMaxNumConnectionLinks().
 */
UCLASS()
class WIRELESSPOWER_API UWirelessPowerConnection : public UFGPowerConnectionComponent
{
    GENERATED_BODY()
public:
    UWirelessPowerConnection();

    /** Bias used by the wire tool when picking the "closest" connection. The value is ADDED
     *  to the measured distance, so HIGHER = LESS favored. Give the hidden antenna a large
     *  bias so the wire tool never snaps to it. */
    void SetDistanceBias(float B) { mConnectionDistanceBias = B; }

    /** Raise the engine connection-link cap. Protected base member, settable from this subclass. */
    void SetMaxNumConnectionLinks(int32 N) { mMaxNumConnectionLinks = FMath::Max(1, N); }
};
