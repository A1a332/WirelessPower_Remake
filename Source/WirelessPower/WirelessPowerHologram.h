#pragma once

#include "Hologram/FGBuildableHologram.h"
#include "WirelessPowerHologram.generated.h"

/**
 * Placement hologram shared by every Wireless Power building. Relaxes the floor check so the
 * buildings can be plopped like power poles rather than requiring a flat foundation. The base
 * AFGBuildableHologram automatically copies the buildable's "MainMesh" component for the
 * preview, so no custom mesh handling is needed here.
 */
UCLASS()
class WIRELESSPOWER_API AWirelessPowerHologram : public AFGBuildableHologram
{
    GENERATED_BODY()
public:
    AWirelessPowerHologram();

    /**
     * v1.0.57 (bug #3)：新建筑"出生"时继承玩家上次用过的编号（WirelessPowerBuildable 的
     * RecordLastUsedChannel / GetLastUsedChannel），于是连放一串电杆不必每个都重选编号。
     *
     * 为什么用 ConfigureActor 这个钩子：引擎的构造顺序是
     *     ConfigureActor -> ConfigureBuildEffect -> **spawn 进世界** -> ConfigureComponents -> BeginPlay
     * 也就是说它**只在"放置"这一刻**被调用一次；存档里读出来的老建筑走的是反序列化路径，
     * 不经过这里 —— 所以老存档的编号绝不会被悄悄改写。
     *
     * 注意这里**不调** AWirelessPowerBuildable::ApplyChannel：那一刻 actor 还没 spawn、
     * 电路子系统还没建好，ValidateWirelessMesh 会空跑。只写属性，BeginPlay/Tick 会自己重建网状。
     * 另外本函数是 const（基类签名如此），不需要也不应该改全息图自身的状态。
     */
    virtual void ConfigureActor(class AFGBuildable* inBuildable) const override;
};
