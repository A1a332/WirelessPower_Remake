#pragma once

#include "CoreMinimal.h"

/**
 * Localization shorthand for the WirelessPower module.
 *
 * The ENGLISH text passed here IS the source language. GatherText harvests it into
 * Content/Localization/WirelessPower/, and it is also the fallback shown when the player's game
 * language has no matching .locres entry. Chinese lives in
 * Content/Localization/WirelessPower/zh-Hans/WirelessPower.po.
 *
 * Never wrap a string that is not shown to a player (log lines, internal ids, asset paths):
 * those stay as plain TEXT() so they do not pollute the translation target.
 */
#define WP_LOCTEXT(Key, Source) NSLOCTEXT("WirelessPower", Key, Source)
