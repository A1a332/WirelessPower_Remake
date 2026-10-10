#pragma once

#include "CoreMinimal.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/Culture.h"

/**
 * Multi-language text picker for the Wireless Power mod.
 *
 * The mod's .po / .locres localization is NOT mounted at runtime (the packaged mod ships
 * no Content/Localization, and the uplugin sets the localization target's
 * ConfigGenerationPolicy to Never), so UE's normal key-based localization never loads our
 * translations. To make the mod follow the game's language we therefore pick the string
 * ourselves, at first use, based on the active culture:
 *
 *   - Chinese (culture name starts with "zh", incl. zh-Hans / zh-Hant) -> the Chinese argument
 *   - any other language -> looked up in the translation table; if absent, English
 *
 * Translations are registered once (lazily, on the first WP_Text call) by
 * WP_PopulateTranslations(). Add new languages there; no call site needs to change.
 *
 * `English` is BOTH the canonical source string and the English fallback / table key.
 * `Chinese` is shown when the active culture is Chinese (zh).
 */
FText WP_Text(const TCHAR* English, const TCHAR* Chinese);
