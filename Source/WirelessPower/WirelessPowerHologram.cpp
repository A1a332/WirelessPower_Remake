#include "WirelessPowerHologram.h"
#include "WirelessPowerBuildable.h"
#include "Buildables/FGBuildable.h"
#include "WirelessPowerId.h"

// 与 Registration.cpp / WirelessPowerBuildable.cpp 同名的 file-local 分类。
// 这游戏的 Shipping 版把 LogTemp 在 Log 级静默丢掉了（2026-10-01 实测），必须自带分类。
DEFINE_LOG_CATEGORY_STATIC(LogWirelessPower, Log, All);

AWirelessPowerHologram::AWirelessPowerHologram()
{
    mNeedsValidFloor = false;
    mMaxPlacementFloorAngle = 90.f;
}

void AWirelessPowerHologram::ConfigureActor(AFGBuildable* inBuildable) const
{
    Super::ConfigureActor(inBuildable);

    AWirelessPowerBuildable* Node = Cast<AWirelessPowerBuildable>(inBuildable);
    if (Node == nullptr)
    {
        return; // 理论上不会发生（本全息图只服务 WirelessPower 的建筑），防御性返回
    }

    const FString Remembered = AWirelessPowerBuildable::GetLastUsedChannel();
    if (Remembered.IsEmpty())
    {
        // 还没记忆过任何编号（全新存档 / 首次运行）-> 保持类默认值，玩家设一次之后就有了。
        return;
    }

    const FString Previous = Node->Channel;
    if (FWirelessId::Equal(Previous, Remembered))
    {
        return; // 出厂值恰巧相同，不必动
    }

    Node->Channel = Remembered;
    // v1.0.58: mark it as a HINT rather than a decision. This flag is transient (not a UPROPERTY),
    // so an actor that comes out of a save never carries it -- which is how
    // AWirelessPowerBuildable::AutoAdoptNearestPeerChannel() can tell "the player placed this next
    // to an existing pole, so let distance decide" apart from "this node's 编号 was chosen in an
    // earlier session, so leave it alone".
    Node->bChannelFromHologram = true;
    UE_LOG(LogWirelessPower, Log,
           TEXT("[WirelessPower] %s placed with the remembered 编号 '%s' (default was '%s')"),
           *Node->GetClass()->GetName(), *Remembered, *Previous);
}
