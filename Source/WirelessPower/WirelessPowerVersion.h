#pragma once

// Single source of truth for the runtime banner version (WP-D01).
//
// This MUST be bumped in lock-step with WirelessPower.uplugin (VersionName /
// SemVersion) and mod.json (version) by the release pipeline. The startup banner
// in WirelessPowerModule.cpp prints WIRELESS_POWER_VERSION_TEXT so the actually
// loaded build can be fingerprinted from FactoryGame.log -- a mismatch there used
// to make maintainers misattribute features/bugs to the wrong build.
//
// Keeping the number in exactly one committed C++ constant (rather than the .uplugin
// XML, which C++ cannot read at runtime without file IO) means the banner can never
// silently drift from the declared plugin version again.
//
// Wrapped in TEXT() so it concatenates correctly with the other TEXT(...) literals in the
// startup banner (a bare narrow "1.1.2" would fail to combine with wide TCHAR strings).
#define WIRELESS_POWER_VERSION_TEXT TEXT("1.2.1")
