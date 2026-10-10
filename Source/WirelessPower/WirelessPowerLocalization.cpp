#include "WirelessPowerLocalization.h"

namespace
{
    void WP_Add(TMap<FString, TMap<FString, FString>>& T, const TCHAR* Key, const TCHAR* Culture, const TCHAR* Text)
    {
        T.FindOrAdd(FString(Key)).Add(FString(Culture), FString(Text));
    }

    // Populate translations here.
    //   Key     = the EXACT English source string passed as the first arg to WP_Text.
    //   Culture = prefix matched against the active culture name
    //             (e.g. "de", "fr", "es", "it", "ja", "ko", "pl", "pt", "ru", "tr", ...).
    // Add as many languages as you like; any language without an entry falls back to English.
    // Empty for now -> all non-Chinese languages show English until translations are added.
    void WP_PopulateTranslations(TMap<FString, TMap<FString, FString>>& T)
    {
        // Translations are injected here from user-provided text.
    }

    // English key -> (CulturePrefix -> Translated text).
    // Lazy-init avoids C++ static-initialization-order issues and guarantees the table exists
    // before the first lookup (even when WP_Text runs during CDO construction).
    TMap<FString, TMap<FString, FString>>& GetTable()
    {
        static TMap<FString, TMap<FString, FString>>* Table = nullptr;
        static bool bInit = false;
        if (!bInit)
        {
            bInit = true;
            Table = new TMap<FString, TMap<FString, FString>>();
            WP_PopulateTranslations(*Table);
        }
        return *Table;
    }
}

FText WP_Text(const TCHAR* English, const TCHAR* Chinese)
{
    const FString Eng(English);
    const FCulturePtr Culture = FInternationalization::Get().GetCurrentCulture();
    if (Culture)
    {
        const FString Name = Culture->GetName();
        if (Name.StartsWith(TEXT("zh"), ESearchCase::IgnoreCase))
        {
            return FText::FromString(Chinese ? FString(Chinese) : Eng);
        }
        const TMap<FString, TMap<FString, FString>>& T = GetTable();
        if (const TMap<FString, FString>* PerCulture = T.Find(Eng))
        {
            for (const TPair<FString, FString>& Pair : *PerCulture)
            {
                if (Name.StartsWith(Pair.Key, ESearchCase::IgnoreCase))
                {
                    return FText::FromString(Pair.Value);
                }
            }
        }
    }
    return FText::FromString(Eng);
}
