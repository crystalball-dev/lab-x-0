#include "FactoryPresets.h"
#include "Params.h"

namespace labx0
{
    namespace
    {
        std::vector<Preset> build()
        {
            std::vector<Preset> p;

            // 1. The zero point: a plain kit and one D&B pattern to start from.
            {
                Preset z;
                z.name = "Zero Point";
                z.samples[0] = "USER:Drums/Vintage/VT_Kick.wav";
                z.samples[1] = "USER:Drums/Vintage/VT_HandClap.wav";
                z.samples[2] = "USER:Drums/Vintage/VT_CHiHat.wav";
                z.samples[16] = "USER:Jungle Jungle - 1989 to 1999 Samplepack/Breaks/Atlantis Amen.wav";
                z.values = { { "p03_choke", 1.0f } };
                z.patterns = { { 0, "len=16 res=1/16;p1: x.........x.....;p2: ....x.......x...;p3: ..x...x...x...x." } };
                p.push_back (z);
            }

            return p;
        }
    }

    const std::vector<Preset>& factoryPresets()
    {
        static const std::vector<Preset> presets = build();
        return presets;
    }
}
