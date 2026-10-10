#pragma once

#include "FGRemoteCallObject.h"
#include "WirelessPowerRCO.generated.h"

/**
 * Remote Call Object for the Wireless Power control panel.
 *
 * Buildables are server-owned, so a client CANNOT call Server RPCs on them directly (the RPC
 * is dropped for lack of an owning net connection). The standard Satisfactory pattern is an
 * UFGRemoteCallObject "Within AFGPlayerController": the RCO lives on the player's own
 * controller, so its Server RPCs ride the player's connection, and the parameters carry the
 * target buildable. Registered on the game mode at POST_INITIALIZATION (see the game world
 * module), which spawns an instance on every current and future player controller.
 */
UCLASS()
class WIRELESSPOWER_API UWirelessPowerRCO : public UFGRemoteCallObject
{
    GENERATED_BODY()
public:
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    /** Set the wireless channel of a node (control panel SET button). v1.0.52: 编号是字符串。 */
    UFUNCTION(Server, Reliable)
    void Server_SetChannel(class AFGBuildable* Target, const FString& NewChannel);

    /** Set the wireless range of a node (control panel SET RANGE button). */
    UFUNCTION(Server, Reliable)
    void Server_SetRange(class AFGBuildable* Target, float NewRange);

    /** POWER UP / POWER DOWN a node from the control panel. */
    UFUNCTION(Server, Reliable)
    void Server_SetPowerEnabled(class AFGBuildable* Target, bool bEnabled);

    /** Laser Tower "开关连接": add/remove a MANUAL link to the tower whose ID is TargetChannel. */
    UFUNCTION(Server, Reliable)
    void Server_ToggleLaserLink(class AFGBuildable* Target, const FString& TargetChannel);

    /** Laser Tower "激光的可见性": show/hide the beams (replicated, every client follows). */
    UFUNCTION(Server, Reliable)
    void Server_SetLaserVisible(class AFGBuildable* Target, bool bVisible);

protected:
    /** RCOs need at least one replicated field to participate in the net driver. */
    UPROPERTY(Replicated)
    int32 DummyReplicatedField = 0;
};
