#pragma once

#include <iterator>
#include "Library.h"

namespace labx0
{
    // ZONE KIT: hits picked from the STALKER SPECIMENS library (ZONE) and FL's legacy stalker pack (PACK) by
    // measurement: length, peak and the share of energy below 150 Hz, between 150 Hz and 2.5 kHz and above
    // 5 kHz. Only relative paths live here; this repository contains no game audio.
    //
    // Append new entries at the end only: presets refer to entries by name, and the library's
    // _tools\plugin_paths.py lists every pinned path.
    enum class KitGroup { ordnance = 0, fire, click, perc, vox, fx };
    inline constexpr const char* kitGroupNames[] = { "ORDNANCE", "FIRE", "CLICK", "PERC", "VOX", "FX" };

    struct KitEntry
    {
        const char* name;
        KitGroup group;
        Root root;
        const char* path;
    };

    #define LABX0_S2 "heart_of_chornobyl/sounds/Events/"

    inline constexpr KitEntry kitCatalog[] =
    {
        { "Placeholder", KitGroup::fx, Root::pack, "detectors/geiger_1.ogg" },
    };

    #undef LABX0_S2

    inline constexpr int kitCatalogSize = (int) std::size (kitCatalog);

    inline const KitEntry* findKitEntry (const juce::String& name)
    {
        for (const auto& e : kitCatalog)
            if (name.equalsIgnoreCase (e.name))
                return &e;
        return nullptr;
    }
}
