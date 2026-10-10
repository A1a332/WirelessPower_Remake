# -*- coding: utf-8 -*-
import os

BASE = r"F:/Workbuddy/SMLDev/SMLStarterProject/Mods/WirelessPower/Source/WirelessPower"
for fn, mod in [("Registration.cpp", "Registration.h"),
                ("WirelessPowerPanelWidget.cpp", "WirelessPowerPanelWidget.h")]:
    p = os.path.join(BASE, fn)
    data = open(p, "rb").read()
    text = data.decode("utf-8")
    text = text.replace("\ufeff", "")          # drop any stray BOM char (mid-file or leading)
    lines = text.split("\n")
    lines = [l for l in lines if l.strip() != '#include "WirelessPowerLocalization.h"']
    idx = next(i for i, l in enumerate(lines) if ('#include "%s"' % mod) in l)
    lines.insert(idx + 1, '#include "WirelessPowerLocalization.h"')
    out = "\n".join(lines)
    open(p, "wb").write(b"\xef\xbb\xbf" + out.encode("utf-8"))
    print(fn, "-> module header at", idx, "; localization include inserted after; BOM restored")
