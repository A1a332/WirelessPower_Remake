#include "WirelessPowerRCO.h"
#include "Buildables/FGBuildable.h"
#include "WirelessPowerBuildable.h"
#include "Net/UnrealNetwork.h"

void UWirelessPowerRCO::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(UWirelessPowerRCO, DummyReplicatedField);
}

void UWirelessPowerRCO::Server_SetChannel_Implementation(AFGBuildable* Target, const FString& NewChannel)
{
    if (AWirelessPowerBuildable* Node = Cast<AWirelessPowerBuildable>(Target))
    {
        Node->ApplyChannel(NewChannel);
    }
}

void UWirelessPowerRCO::Server_SetRange_Implementation(AFGBuildable* Target, float NewRange)
{
    if (AWirelessPowerBuildable* Node = Cast<AWirelessPowerBuildable>(Target))
    {
        Node->ApplyRange(NewRange);
    }
}

void UWirelessPowerRCO::Server_SetPowerEnabled_Implementation(AFGBuildable* Target, bool bEnabled)
{
    if (AWirelessPowerBuildable* Node = Cast<AWirelessPowerBuildable>(Target))
    {
        Node->ApplyPowerEnabled(bEnabled);
    }
}

void UWirelessPowerRCO::Server_ToggleLaserLink_Implementation(AFGBuildable* Target, const FString& TargetChannel)
{
    if (AWirelessPowerBuildable* Node = Cast<AWirelessPowerBuildable>(Target))
    {
        Node->ApplyLaserLinkToggle(TargetChannel);
    }
}

void UWirelessPowerRCO::Server_SetLaserVisible_Implementation(AFGBuildable* Target, bool bVisible)
{
    if (AWirelessPowerBuildable* Node = Cast<AWirelessPowerBuildable>(Target))
    {
        Node->ApplyLaserVisible(bVisible);
    }
}
