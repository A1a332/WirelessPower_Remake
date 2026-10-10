# -*- coding: utf-8 -*-
import re, io

BASE = r"F:/Workbuddy/SMLDev/SMLStarterProject/Mods/WirelessPower"

def parse_po(path):
    d = {}
    cur_key = None
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            m = re.match(r'msgid "(.*)"', line)
            if m:
                cur_key = m.group(1)[1:] if m.group(1).startswith(",") else None
                continue
            m2 = re.match(r'msgstr "(.*)"', line)
            if m2 and cur_key:
                d[cur_key] = m2.group(1)
    return d

en = parse_po(BASE + r"/Content/Localization/WirelessPower/en/WirelessPower.po")
zh = parse_po(BASE + r"/Content/Localization/WirelessPower/zh-Hans/WirelessPower.po")

# Corrected terminology + accurate current ranges. (en, zh)
OVERRIDE = {
    "Milestone_Mk1_Name": ("Wireless Power", "无线电力"),
    "Milestone_Mk1_Desc": ("Wireless Power. The Power Relay Mk1 (60 m), Power Converter and Power Feeder Mk1 are now unlocked in the build menu; this milestone advances the tech tier. Wireless nodes sharing an ID pool power with each other within range, with no cable between them.", "无线电力。电力中继器（60 米）、电力转换器与供电杆现已在建造菜单中解锁；本里程碑提升科技层级。相同 ID 的无线节点在范围内共享同一电力回路，节点之间无需铺设电缆。"),
    "Milestone_Mk2_Name": ("Wireless Power Mk2", "无线电力 Mk2"),
    "Milestone_Mk2_Desc": ("Wireless Power tier 2: the Power Relay Mk2 (120 m) and Power Feeder Mk2 (24 m) are now unlocked in the build menu.", "无线电力 第 2 阶：电力中继器 Mk2（120 米）与供电杆 Mk2（24 米）现已在建造菜单中解锁。"),
    "Milestone_Mk3_Name": ("Wireless Power Mk3", "无线电力 Mk3"),
    "Milestone_Mk3_Desc": ("Unlocks the Power Transmission Tower: a 4800 m line-of-sight link that carries power across canyons. It also advances Wireless Power to tier 3 (Power Relay Mk3 at 160 m and Power Feeder Mk3 at 48 m, both unlocked by this tier).", "解锁激光塔：一条 4800 米的通视输电链路，可跨越峡谷输电。同时将无线电力提升至第 3 阶（电力中继器 Mk3，160 米，与供电杆 Mk3，48 米，均在本阶解锁）。"),
    "Building_RelayMk1": ("Power Relay Mk1", "电力中继器 Mk1"),
    "Desc_RelayMk1": ("An access point for the wireless grid. Wire it into a powered grid and it will share power with same-ID nodes within 60 m -- no cable between the nodes.", "无线电网的接入点。接入通电的电网后，它会与 60 米范围内相同 ID 的节点共享电力，节点之间无需铺设电缆。"),
    "Building_RelayMk2": ("Power Relay Mk2", "电力中继器 Mk2"),
    "Desc_RelayMk2": ("An access point for the wireless grid. Wire it into a powered grid and it will share power with same-ID nodes over a wider range than Mk1 -- no cable between the nodes.", "无线电网的接入点。接入通电的电网后，它会以比 Mk1 更大的范围与相同 ID 的节点共享电力，节点之间无需铺设电缆。"),
    "Building_RelayMk3": ("Power Relay Mk3", "电力中继器 Mk3"),
    "Desc_RelayMk3": ("An access point for the wireless grid. Wire it into a powered grid and it will share power with same-ID nodes over the widest range -- no cable between the nodes.", "无线电网的接入点。接入通电的电网后，它会以最大范围与相同 ID 的节点共享电力，节点之间无需铺设电缆。"),
    "Building_PowerConverter": ("Power Converter", "电力转换器"),
    "Desc_PowerConverter": ("Converts between the wired and the wireless grid. Once wired into a powered grid it supplies same-ID nodes within 48 m.", "在「有线电网」与「无线电网」之间转换。接入通电的电网后，它会向 48 米范围内相同 ID 的节点供电。"),
    "Building_PowerConverterMk2Disabled": ("Power Converter Mk2 (disabled)", "电力转换器 Mk2（未启用）"),
    "Desc_ReservedTier": ("Tier reserved; not registered in the current version.", "该档位为预留档，当前版本未注册。"),
    "Building_PowerConverterMk3Disabled": ("Power Converter Mk3 (disabled)", "电力转换器 Mk3（未启用）"),
    "Building_FeederMk1": ("Power Feeder Mk1", "供电杆 Mk1"),
    "Desc_FeederMk1": ("Connect machines to this node and it draws power from same-ID nodes within 12 m -- no cable between the nodes.", "将机器连接到此节点，它便会从 12 米范围内相同 ID 的节点取电，节点之间无需铺设电缆。"),
    "Building_FeederMk2": ("Power Feeder Mk2", "供电杆 Mk2"),
    "Desc_FeederMk2": ("Connect machines to this node. Range 24 m.", "将机器连接到此节点。范围 24 米。"),
    "Building_FeederMk3": ("Power Feeder Mk3", "供电杆 Mk3"),
    "Desc_FeederMk3": ("Connect machines to this node. Range 48 m.", "将机器连接到此节点。范围 48 米。"),
    "Building_LaserTower": ("Power Transmission Tower", "激光塔"),
    "Desc_LaserTower": ("A long-range wireless power link (4800 m); towers need line of sight to each other. Carries power across canyons and over long distances.", "远距离无线输电链路（4800 米）；塔与塔之间需要互相通视。可跨越峡谷、远距离传输电力。"),
    "Building_AreaPole": ("Power Feeder", "供电杆"),
    "Building_Relay": ("Power Relay", "电力中继器"),
}

PAT = re.compile(r'WP_LOCTEXT\(\s*"([^"\n]+)"\s*,\s*\n?\s*"([^"\n]*)"\s*\)')

def esc(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')

def convert(text, fname, skipped):
    def repl(m):
        key = m.group(1)
        if key in OVERRIDE:
            en_s, zh_s = OVERRIDE[key]
        elif key in en and key in zh:
            en_s, zh_s = en[key], zh[key]
        else:
            skipped.append((fname, key))
            return m.group(0)
        return 'WP_Text(TEXT("%s"), TEXT("%s"))' % (esc(en_s), esc(zh_s))
    return PAT.sub(repl, text)

for rel in [r"Source/WirelessPower/Registration.cpp",
            r"Source/WirelessPower/WirelessPowerPanelWidget.cpp"]:
    p = BASE + "/" + rel
    txt = open(p, encoding="utf-8").read()
    skipped = []
    new = convert(txt, rel, skipped)
    if "WirelessPowerLocalization.h" not in new:
        new = '#include "WirelessPowerLocalization.h"\n' + new
    open(p, "w", encoding="utf-8").write(new)
    print("==", rel, "==")
    print("  remaining WP_LOCTEXT:", new.count("WP_LOCTEXT("))
    print("  WP_Text count:", new.count("WP_Text("))
    if skipped:
        print("  SKIPPED:", skipped)
    else:
        print("  all keys resolved")
