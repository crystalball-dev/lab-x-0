#include "FactoryPresets.h"
#include "Params.h"

namespace labx0
{
    namespace
    {
        // Sample sources. KIT: is the ZONE KIT catalogue; USER: is the configurable library (FL Studio's legacy Packs
        // folder by default). A missing file only leaves its pad empty.
        const juce::String amen1 = "USER:rhythm-lab.com_amen_vol.1/rhythm-lab.com_amen_vol.1/WAV/";
        const juce::String amen2 = "USER:rhythm-lab.com_amen_vol.2/rhythm-lab.com_amen_vol.2/WAV/";
        const juce::String amen3 = "USER:rhythm-lab.com_amen_vol.3/rhythm-lab.com_amen_vol.3/WAV/";
        const juce::String hiphop = "USER:rhythm-lab.com_hiphop_kicking_breaks/rhythm-lab.com_hiphop_kicking_breaks/WAV/";
        const juce::String jungle = "USER:Jungle Jungle - 1989 to 1999 Samplepack/";
        const juce::String explorer = "USER:Bass Explorer Presents 101 Drum & Bass Drum Loops/";
        const juce::String breaks = "USER:All The Breaks-20250923T180149Z-1-001/All The Breaks/";
        const juce::String fpc = "USER:FPC/";
        const juce::String real = "USER:Drums/RealDrumkits/";
        const juce::String vintage = "USER:Drums/Vintage/";
        const juce::String k808 = "USER:808_drum_kit/";
        const juce::String nthnl = "USER:NTHNL Industrial Kit/Misc_Patterns/";

        juce::String kit (const char* name) { return juce::String ("KIT:") + name; }

        using Values = std::vector<std::pair<juce::String, float>>;

        // Hats choke each other; everything else plays out.
        Values base (Values extra)
        {
            Values v { { "p03_choke", 1.0f }, { "p04_choke", 1.0f }, { "p04_mode", 1.0f } };
            v.insert (v.end(), extra.begin(), extra.end());
            return v;
        }

        Preset make (const char* name, std::array<juce::String, numPads + numBreaks> samples, Values values,
                     std::vector<std::pair<int, juce::String>> patterns)
        {
            Preset p;
            p.name = name;
            p.samples = std::move (samples);
            p.values = std::move (values);
            p.patterns = std::move (patterns);
            return p;
        }

        // Shared grooves (1/16 steps). Spaces only group the steps for reading.
        const char* twoStep    = "p1: X... .... ..x. ....;p2: .... X... .... X...;p3: x.x. x.x. x.x. x.x.";
        const char* twoStepHat = "p3: ..x. ..x. ..x. ..x.;p4: .... .... .... ..x.";

        std::vector<Preset> build()
        {
            std::vector<Preset> p;

            // Every kit's master_volume is set so its own patterns play at about -11 LUFS momentary maximum at 172 BPM
            // (Tests/run_tests.py test 25 holds them inside -14..-8).

            // 1. ZERO POINT: a clean STALKER kit and an amen to learn the machine with.
            p.push_back (make ("Zero Point",
                { kit ("Pseudogiant Step III"), kit ("Grim"), kit ("Shell Casing II"), vintage + "VT_OHiHat.wav",
                  real + "RD_Clap.wav", fpc + "SideStick_Rims/FPC_SdSt_B_001.wav", kit ("Bolt Toss"), kit ("Grenade Bounce"),
                  k808 + "kicks/808-Kicks03.wav", fpc + "Cymbals/FPC_Crash_G16InLite_01.wav", kit ("Electro Discharge"), kit ("Emission Rise"),
                  kit ("Stalker Back Off EN"), kit ("Duty Attack EN"), kit ("Detector Beep"), kit ("Radio Static"),
                  amen1 + "cw_amen15_174.wav", explorer + "Ben Kama - ABC [174bpm].wav", jungle + "Breaks/Atlantis Amen.wav",
                  breaks + "The Winstons - Amen Brother.wav" },
                base ({ { "master_volume", 3.2f }, { "p02_level", -2.0f }, { "p03_level", -6.0f }, { "p05_level", -4.0f }, { "b1_slicing", 2.0f },
                        { "b1_level", -3.0f }, { "tr_attack", 0.2f }, { "mb_depth", 0.2f } }),
                { { 0, juce::String ("len=16 res=1/16;") + twoStep + ";b1: x... .... .... ...." },
                  { 1, "len=32 res=1/16;p1: X... .... ..x. .... X... .... ..x. ...."
                       ";b1: 0123 4567 89ab cdef ghij klmn 45ab cdef;p3: x.x. x.x. x.x. x.x. x.x. x.x. x.x. x..."
                       ";t: .... .... .... .... .... .... .... ssss" },
                  { 2, "len=16 res=1/16;p1: X... .... .... ....;p2: .... .... X... ....;p3: x.x. x.x. x.x. x.x.;b2: x... .... .... ...." },
                  { 3, juce::String ("len=16 res=1/16;") + twoStep + ";p13: .... .... .... ..X.;v: .... .... ssss ssss" } }));

            // 2. EMISSION FRONT: heavy rolling D&B; the last bar stutters into the next.
            p.push_back (make ("Emission Front",
                { k808 + "kicks/808-Kicks01.wav", kit ("D12 Shotgun"), kit ("Shell Casing"), vintage + "VT_OHiHat_2.wav",
                  real + "RD_Snare_4.wav", kit ("Dry Fire"), kit ("Ammo Box"), kit ("Phone Drop"),
                  kit ("Body Blow"), fpc + "Cymbals/FPC_Crash_G16InLite_03.wav", kit ("Emission Rise"), kit ("Emission Stop"),
                  kit ("Military Attack EN"), kit ("Monolith Drop Weapon RU"), kit ("Geiger Click"), kit ("Safebox Lightning"),
                  explorer + "Fearful (Diffrent Music) - Break 1 [172bpm].wav", explorer + "Dexta (Diffrent Music) - JungleSteppin [170bpm].wav",
                  amen1 + "cw_amen07_172.wav", jungle + "Breaks/Bulldozer Amen.wav" },
                base ({ { "master_volume", -7.9f }, { "p05_level", -6.0f }, { "b1_level", -2.0f }, { "tr_attack", 0.35f }, { "mb_depth", 0.35f },
                        { "slam_bark", 0.25f }, { "slam_bite", 0.3f }, { "clip_drive", 3.0f }, { "emission", 0.15f } }),
                { { 0, "len=64 res=1/16"
                       ";p1: X... .... ..x. .... X... .... ..x. .... X... .... ..x. .... X... .... ..x. .x.."
                       ";p2: .... X... .... X... .... X... .... X... .... X... .... X... .... X... .... X.xX"
                       ";p5: .... X... .... X... .... X... .... X... .... X... .... X... .... X... .... ...."
                       ";p3: ..x. ..x. ..x. ..x. ..x. ..x. ..x. ..x. ..x. ..x. ..x. ..x. ..x. ..x. ..x. ...."
                       ";b1: x... .... .... .... .... .... .... .... x... .... .... .... .... .... .... ...."
                       ";t: .... .... .... .... .... .... .... .... .... .... .... .... .... .... ssss tttt" },
                  { 1, "len=32 res=1/16;p1: X... .... ..x. .... X... .... ..x. ....;p2: .... X... .... X..o .... X... .... X...;b2: x... .... .... .... .... .... .... ....;p3: x.x. x.x. x.x. x.x. x.x. x.x. x.x. x.x.;p11: X... .... .... .... .... .... .... ...." } }));

            // 3. CHEEKI BREEKI: jungle amen chops with bandit barks over the top.
            p.push_back (make ("Cheeki Breeki",
                { k808 + "kicks/808-Kicks05.wav", fpc + "Snare Drums/FPC_Snr_2L002.wav", kit ("Shell Casing III"), vintage + "VT_OHiHat.wav",
                  real + "RD_Snare_7.wav", fpc + "SideStick_Rims/FPC_SdSt_C_002.wav", kit ("Hoof"), kit ("Wood Step"),
                  kit ("Zombie Blow"), fpc + "Cymbals/FPC_Crash_G18InLite_01.wav", kit ("F1 Grenade"), kit ("Gravity Blast"),
                  kit ("Bandit Drop Weapon RU"), kit ("Bandit Drop Weapon EN"), kit ("Mercenary Stop RU"), kit ("Freedom Attack RU"),
                  amen1 + "cw_amen04_170.wav", amen2 + "cw_amen_classic.wav", jungle + "Breaks/Control Amen.wav", jungle + "Breaks/Cool Amen.wav" },
                base ({ { "master_volume", -5.1f }, { "b1_slicing", 2.0f }, { "b2_slicing", 2.0f }, { "b1_level", -1.0f }, { "swing", 0.08f },
                        { "tr_attack", 0.25f }, { "slam_bite", 0.2f } }),
                { { 0, "len=32 res=1/16;b1: 0123 4567 89ab cdef ghij klmn opqr stuv;p1: X... .... .... .... X... .... .... ....;p13: .... .... .... .... .... .... .... X..." },
                  { 1, "len=32 res=1/16;b1: 0123 4545 89ab cdcd 0123 klmn 4545 cdef;b1.rat: .... .... .... ...2 .... .... .... 2.22"
                       ";p1: X... .... ..x. .... X... .... ..x. ....;p14: .... .... .... .... .... .... .... ..X." },
                  { 2, "len=32 res=1/16;b1: 0..3 ..67 89.b cd.f g..j ..mn op.r 4567;p2: .... X... .... X... .... X... .... X...;t: .... .... .... .... .... .... .... eeee" },
                  { 3, "len=32 res=1/16;b2: x... .... .... .... .... .... .... ....;p1: X... .... ..x. .... X... .... ..x. ....;p15: X... .... .... .... .... .... .... ...." } }));

            // 4. PSEUDOGIANT STOMP: half-time weight, every hit a body falling.
            p.push_back (make ("Pseudogiant Stomp",
                { kit ("Pseudogiant Step"), kit ("SVDM"), kit ("Shell Casing IV"), vintage + "VT_OHiHat_2.wav",
                  kit ("Rhino"), kit ("Mag Out"), kit ("Metal Plate Step"), kit ("Rifle Bounce"),
                  kit ("Pseudogiant Step II"), kit ("Gas Cylinder Blast"), kit ("Pseudogiant Fist"), kit ("Body Tear"),
                  kit ("Zombie Groan RU"), kit ("Knockdown Grunt"), kit ("Bear Signal"), kit ("Bubble Burst"),
                  explorer + "Flite - Recall (Half Time) [174bpm].wav", explorer + "Rameses B - Dreaming (Half Time) [175bpm].wav",
                  amen1 + "cw_amen14_175.wav", jungle + "Breaks/Brut Force Break.wav" },
                base ({ { "master_volume", -7.6f }, { "p01_tune", -2.0f }, { "p09_tune", -5.0f }, { "slam_bark", 0.5f }, { "slam_bite", 0.4f }, { "sub_safe", 3.0f },
                        { "mb_depth", 0.3f }, { "clip_drive", 4.0f }, { "emission", 0.1f } }),
                { { 0, "len=32 res=1/16;p1: X... .... .... ..x. .... ..x. .... ....;p2: .... .... X... .... .... .... X... ....;p3: ..x. ..x. ..x. ..x. ..x. ..x. ..x. ..x.;p9: .... .... .... .... X... .... .... ...."
                       ";b1: x... .... .... .... .... .... .... ...." },
                  { 1, "len=32 res=1/16;p1: X... .... ..x. .... X... .x.. .... ....;p2: .... .... X... .... .... .... X... ..o.;p10: X... .... .... .... .... .... .... ....;b2: x... .... .... .... .... .... .... ....;v: .... .... .... .... .... .... dddd dddd" } }));

            // 5. MONOLITH CALL: breakcore at full emission; the break shatters in Bubble and Burner.
            p.push_back (make ("Monolith Call",
                { k808 + "kicks/808-Kicks02.wav", kit ("Bucket SMG"), kit ("Shell Casing V"), vintage + "VT_OHiHat.wav",
                  kit ("M10 Pistol"), kit ("Dry Fire II"), kit ("Kitchen Metal"), kit ("Device Knock"),
                  kit ("Snork Kick"), fpc + "Cymbals/FPC_Crash_G16InLite_05.wav", kit ("Razor"), kit ("Electra Hit"),
                  kit ("Monolith Attack RU"), kit ("Monolith Attack EN"), kit ("Monolith Drop Weapon RU"), kit ("Psy Phantom"),
                  amen1 + "cw_amen18_178.wav", amen2 + "cw_amen_freak.wav", amen2 + "cw_amen_chopper.wav", jungle + "Breaks/Chopper Break.wav" },
                base ({ { "master_volume", -9.6f }, { "b1_slicing", 3.0f }, { "b1_gate", 1.0f }, { "emission", 0.4f }, { "slam_bark", 0.35f }, { "slam_bite", 0.45f },
                        { "clip_drive", 5.0f }, { "mb_depth", 0.45f }, { "scratch_depth", 0.8f } }),
                { { 0, "len=32 res=1/32;b1: 0123 4567 89ab cdef 0123 4567 89ab cdef;b1.rat: .... ...2 .... ..34 .... ...2 .2.. 4444"
                       ";p1: X... .... X... .... X... .... X.X. ....;t: .... .... .... .... oooo oooo bbbb bbbb" },
                  { 1, "len=32 res=1/32;b1: 0011 2233 4455 6677 8899 aabb ccdd eeff;p2: .... .... X... .... .... .... X... X...;p13: X... .... .... .... .... .... .... ....;t: .... .... .... .... .... .... uuuu uuuu" },
                  { 2, "len=64 res=1/32;b1: x...;t: .... .... .... .... .... .... .... .... oooo oooo oooo oooo vvvv vvvv cccc cccc" } }));

            // 6. ARENA AT ROSTOK: a classic funk break rolling at D&B tempo.
            p.push_back (make ("Arena at Rostok",
                { real + "RD_Kick.wav", real + "RD_Snare.wav", real + "RD_Hat_2.wav", real + "RD_Hat_6.wav",
                  real + "RD_Clap.wav", real + "RD_Wood.wav", real + "RD_Shaker.wav", real + "RD_Tambhit.wav",
                  real + "RD_Kick_2.wav", real + "RD_Crash.wav", kit ("Artifact Jump"), kit ("Clicker Impact"),
                  kit ("Duty Attack RU"), kit ("Stalker Back Off EN"), kit ("Geiger"), kit ("Radio Static"),
                  breaks + "Lyn Collins - Think About It (Break 1).wav", breaks + "James Brown - Funky Drummer.wav",
                  breaks + "Skull Snaps - It_s A New Day.wav", breaks + "Melvin Bliss - Synthetic Substitution.wav" },
                base ({ { "master_volume", -2.2f }, { "b1_level", -1.0f }, { "p01_level", -3.0f }, { "tr_attack", 0.3f }, { "mb_depth", 0.25f }, { "swing", 0.05f } }),
                { { 0, juce::String ("len=16 res=1/16;b1: xxxx xxxx xxxx xxxx;") + "p1: X... .... ..x. ....;p2: .... X... .... X..." },
                  { 1, "len=16 res=1/16;b2: xxxx xxxx xxxx xxxx;p1: X... .... ..x. ....;p2: .... X... .... X...;p7: x.x. x.x. x.x. x.x." },
                  { 2, "len=16 res=1/16;b3: xxxx xxxx xxxx xxxx;p2: .... X... .... X...;t: .... .... .... wwww" } }));

            // 7. AMEN AT YANOV: the original amen, sped up the jungle way, with soundsystem vocals.
            p.push_back (make ("Amen at Yanov",
                { k808 + "kicks/808-Kicks04.wav", fpc + "Snare Drums/FPC_SnrC_3_3.wav", fpc + "Hi Hats/FPC_ClHH_GCsta_001.wav", vintage + "VT_OHiHat.wav",
                  real + "RD_Snare_2.wav", fpc + "SideStick_Rims/FPC_SdSt_C_001.wav", jungle + "FX/Dred Guns.wav", jungle + "FX/Champion Scratch.wav",
                  k808 + "kicks/808-Kicks06.wav", fpc + "Cymbals/FPC_Crash_G16InLite_02.wav", jungle + "FX/Aura Boom.wav", jungle + "FX/Cool Ping.wav",
                  jungle + "Vocals/Champion Badman.wav", jungle + "Vocals/Bad Boy.wav", jungle + "Vocals/Let Roll Vox.wav", kit ("Duty Attack EN"),
                  breaks + "The Winstons - Amen Brother.wav", amen1 + "cw_amen06_169.wav", amen3 + "Riddim Break 168.wav", jungle + "Breaks/Control Amen 2.wav" },
                base ({ { "master_volume", -5.2f }, { "b2_slicing", 2.0f }, { "b1_level", -1.0f }, { "tr_attack", 0.2f }, { "slam_bark", 0.15f } }),
                { { 0, "len=16 res=1/16;b1: x... .... .... ....;p1: X... .... ..x. ....;p13: X... .... .... ...." },
                  { 1, "len=32 res=1/16;b2: 0123 4567 89ab cdef ghij klmn 4567 cdef;p1: X... .... ..x. .... X... .... ..x. ....;p8: .... .... .... .... .... .... .... ..X." },
                  { 2, "len=32 res=1/16;b2: 0123 0123 89ab 89ab ghij ghij 4567 cdcd;b2.rat: .... .... .... .... .... .... .... ..22;p14: .... .... .... .... X... .... .... ...." },
                  { 3, "len=16 res=1/16;b3: x... .... .... ....;p1: X... .... ..x. ....;p2: .... X... .... X..." } }));

            // 8. RED FOREST STATIC: glitch breakcore; scratches, reversals and gates on everything.
            p.push_back (make ("Red Forest Static",
                { kit ("Body Blow II"), kit ("Viper"), kit ("Geiger Click"), kit ("Shell Casing"),
                  kit ("APB"), kit ("Contact Beep"), kit ("Veles Signal"), kit ("Detector Beep"),
                  kit ("Mutant Fall"), kit ("Radio Static"), kit ("Razor"), kit ("Electro Discharge"),
                  kit ("Electra Hit"), kit ("Lightning Ball"), kit ("Small Bubble Burst"), kit ("Fire Breath"),
                  jungle + "Breaks/Control Stutter Break.wav", amen2 + "cw_amen_distorted.wav", jungle + "Breaks/Darkside Break.wav", amen2 + "cw_amen_highpass.wav" },
                base ({ { "master_volume", -9.6f }, { "b1_slicing", 3.0f }, { "scratch_depth", 0.9f }, { "slam_bark", 0.4f }, { "clip_drive", 6.0f }, { "emission", 0.25f } }),
                { { 0, "len=32 res=1/16;b1: 0123 4567 89ab cdef 0123 4567 89ab cdef;p1: X... ..X. .... X... X... ..X. ..X. ....;t: .... .... .... WWWW .... .... vvvv vvvv;v: .... .... .... .... tttt tttt .... ...." },
                  { 1, "len=32 res=1/32;b2: x... .... .... .... .... .... .... ....;p3: x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.;t: .... .... wwww wwww .... .... WWWW WWWW" } }));

            // 9. JUPITER PLANT: industrial D&B from the plant's metal.
            p.push_back (make ("Jupiter Plant",
                { nthnl + "Hits/dum.wav", kit ("Big Box Break"), kit ("Shell Casing II"), kit ("Kitchen Metal"),
                  kit ("Metal Plate Step"), nthnl + "Hits/synpulse1.wav", nthnl + "Hits/whoop.wav", kit ("Bolt Toss"),
                  nthnl + "Hits/dum2.wav", kit ("Rifle Bounce"), kit ("Clicker Impact"), kit ("Safebox Lightning"),
                  kit ("Ecologist Back Off RU"), kit ("Fall Grunt"), kit ("Inventory Pick"), kit ("AK Unload"),
                  nthnl + "Loops/bdpattern5.wav", explorer + "Ben Kama - Riot [172bpm].wav", amen1 + "cw_amen19_172.wav", jungle + "Breaks/Detroit Break.wav" },
                base ({ { "master_volume", -3.9f }, { "mb_depth", 0.5f }, { "mb_up", 0.6f }, { "slam_bite", 0.5f }, { "slam_sc", 2.0f }, { "tr_attack", 0.4f } }),
                { { 0, "len=16 res=1/16;p1: X... .... ..x. ....;p2: .... X... .... X...;p4: x.x. x.x. x.x. x.x.;p5: .... ..x. .x.. ..x.;b2: x... .... .... ...." },
                  { 1, "len=16 res=1/16;p1: X... .... ..x. ....;p2: .... X... .... X...;p6: ..x. ..x. ..x. ..x.;p8: .... .... ...x .x..;v: eeee eeee eeee eeee" } }));

            // 10. SKADOVSK DECK: liquid rollers on the old ship.
            p.push_back (make ("Skadovsk Deck",
                { fpc + "Kick Drums/FPC_Kick_GiReal_002.wav", fpc + "Snare Drums/FPC_Snr_2L003.wav", fpc + "Hi Hats/FPC_ClHH_GSab_001.wav", vintage + "VT_OHiHat_2.wav",
                  fpc + "Snare Drums/FPC_SnrC_3_6.wav", fpc + "SideStick_Rims/FPC_SdSt_C_003.wav", fpc + "Other Percussion/FPC_Tamb_B04.wav", fpc + "Other Percussion/FPC_CowB_G_02.wav",
                  fpc + "Kick Drums/FPC_Kick_Gretc_001.wav", vintage + "VT_Ride.wav", jungle + "FX/Enchanted Water.wav", jungle + "FX/Blue Water Spin.wav",
                  jungle + "Vocals/Dream Vox.wav", jungle + "Vocals/Heaven Vox.wav", kit ("Bullet in Water"), kit ("Wood Step"),
                  jungle + "Breaks/Adrift Break.wav", jungle + "Breaks/Breeze Break.wav", amen3 + "Hydro Break 170.wav", jungle + "Breaks/Cumulus Break.wav" },
                base ({ { "master_volume", -0.8f }, { "swing", 0.12f }, { "mb_depth", 0.15f }, { "tr_attack", 0.1f }, { "b1_level", -2.0f } }),
                { { 0, "len=16 res=1/16;b1: xxxx xxxx xxxx xxxx;p1: X... .... ..x. ....;p2: .... X... .... X...;p7: ..x. ..x. ..x. ..x." },
                  { 1, "len=32 res=1/16;b2: xxxx xxxx xxxx xxxx xxxx xxxx xxxx xxxx;p1: X... .... ..x. .... X... .... ..x. ....;p10: x... x... x... x... x... x... x... x...;p13: .... .... .... .... X... .... .... ...." } }));

            // 11. GARBAGE RUN: a hip-hop break stretched up to speed without the chipmunk.
            p.push_back (make ("Garbage Run",
                { k808 + "kicks/808-Kicks03.wav", real + "RD_Snare_9.wav", real + "RD_Hat_4.wav", real + "RD_Hat_7.wav",
                  real + "RD_Clap.wav", kit ("Dry Fire"), kit ("Boot Step"), kit ("Paw on Earth"),
                  kit ("Dead Body Drop"), real + "RD_Crash_2.wav", kit ("Barrel Blast"), kit ("Vortex"),
                  kit ("Bandit Drop Weapon RU"), kit ("Zombie Attack EN"), kit ("Geiger II"), kit ("Psy Phantom"),
                  hiphop + "Dirty_Break_107.wav", hiphop + "FilthyDrumer_Break_100.wav", hiphop + "Banggang_Break_090.wav", hiphop + "Crowd_Break_088.wav" },
                base ({ { "master_volume", -2.9f }, { "b1_fit", 2.0f }, { "b2_fit", 2.0f }, { "b3_fit", 0.0f }, { "b3_beats", 9.0f }, { "slam_bark", 0.3f }, { "clip_drive", 2.0f } }),
                { { 0, "len=16 res=1/16;b1: x... .... .... ....;p1: X... .... ..x. ....;p2: .... X... .... X..." },
                  { 1, "len=16 res=1/16;b2: x... .... .... ....;p2: .... X... .... X...;t: .... .... .... jjjj" },
                  { 2, "len=32 res=1/16;b3: x... .... .... .... .... .... .... ....;p1: X... .... .... .... .... .... .... ...." } }));

            // 12. BRAIN SCORCHER BUS: the punch chain flat out, X-10 style.
            p.push_back (make ("Brain Scorcher Bus",
                { k808 + "kicks/808-Kicks06.wav", kit ("Flesh Punch"), kit ("Shell Casing III"), vintage + "VT_OHiHat.wav",
                  k808 + "snares/808-Clap03.wav", kit ("Mag Out"), kit ("Grenade Bounce II"), kit ("Ammo Box"),
                  kit ("Snork Kick"), vintage + "VT_Crash.wav", kit ("Electro Discharge"), kit ("Controller Hit"),
                  kit ("Monolith Attack EN"), kit ("Military Attack RU"), kit ("Dry Fire II"), kit ("Radio Static"),
                  explorer + "Fox Stevenson - Big Punch 1 [175bpm].wav", explorer + "Livewire - Tin Hat [174bpm].wav", amen1 + "cw_amen09_175.wav", amen2 + "cw_amen_comp.wav" },
                base ({ { "mb_depth", 0.9f }, { "mb_up", 0.8f }, { "mb_down", 0.7f }, { "slam_bark", 0.6f }, { "slam_bite", 0.5f },
                        { "clip_drive", 6.0f }, { "tr_attack", 0.45f }, { "emission", 0.2f }, { "master_volume", -9.7f } }),
                { { 0, juce::String ("len=16 res=1/16;") + twoStep + ";b1: x... .... .... ...." },
                  { 1, "len=16 res=1/16;p1: X... ..x. ..x. ....;p2: .... X... .... X.xX;p3: xxxx xxxx xxxx xxxx;b2: x... .... .... ...." } }));

            // 13. OASIS RATCHET: psy rolls and rattling hats, X-3 style.
            p.push_back (make ("Oasis Ratchet",
                { fpc + "Kick Drums/FPC_Kick_GiReal_003.wav", kit ("GP37"), kit ("Shell Casing IV"), vintage + "VT_OHiHat_2.wav",
                  kit ("Integral"), kit ("Geiger"), kit ("Veles Signal"), kit ("Contact Beep"),
                  kit ("Stalker Landing"), fpc + "Cymbals/FPC_Crash_G16InLite_04.wav", kit ("Psy Phantom"), kit ("Fire Breath"),
                  kit ("Knockdown Grunt"), kit ("Fall Grunt"), kit ("Detector Beep"), kit ("Lightning Ball"),
                  jungle + "Breaks/Cosmik Break.wav", amen2 + "cw_amen_deeptouch.wav", amen3 + "Orb Break 170.wav", jungle + "Breaks/Centuries Break.wav" },
                base ({ { "master_volume", -2.6f }, { "b1_fit", 2.0f }, { "tr_attack", 0.3f }, { "mb_depth", 0.3f }, { "slam_bark", 0.2f } }),
                { { 0, "len=16 res=1/16;p1: X... .... ..x. ....;p2: .... X... .... X...;p3: x2x. x2x. x3x. x2x4;p3.prob: .... ..5. .... ..7.;b1: x... .... .... ...." },
                  { 1, "len=16 res=1/16;p6: 2.2. 2.2. 2.2. 4444;p7: .... ..x. .... ..x.;p1: X... .... ..x. ....;p2: .... X... .... X..." } }));

            // 14. WISH GRANTER ROLL: rolls into tape stops, X-19 style.
            p.push_back (make ("Wish Granter Roll",
                { real + "RD_Kick_3.wav", real + "RD_Snare_5.wav", real + "RD_Hat_3.wav", real + "RD_Hat_8.wav",
                  real + "RD_Snare_11.wav", real + "RD_Tom_2.wav", real + "RD_Tom_4.wav", real + "RD_Tom_6.wav",
                  k808 + "kicks/808-Kicks02.wav", real + "RD_Crash.wav", kit ("Emission Stop"), kit ("Artifact Jump"),
                  kit ("Duty Attack EN"), kit ("Ecologist Back Off RU"), kit ("Geiger Click"), kit ("Emission Rise"),
                  amen3 + "Riddim Break 168.wav", amen3 + "Ruffa Break 170.wav", amen1 + "cw_amen13_173.wav", jungle + "Breaks/Champion Break.wav" },
                base ({ { "master_volume", -2.6f }, { "b1_slicing", 2.0f }, { "tr_attack", 0.25f }, { "mb_depth", 0.2f } }),
                { { 0, "len=64 res=1/16;b1: x"
                       ";p2: .... X... .... X... .... X... .... X... .... X... .... X... .... X... XXXX 4444"
                       ";p2.rat: .... .... .... .... .... .... .... .... .... .... .... .... .... .... 2222 ...."
                       ";t: .... .... .... .... .... .... .... .... .... .... .... .... .... .... .... cccc" },
                  { 1, "len=32 res=1/16;b1: 0123 4567 89ab cdef 0123 4567 cccc dddd;b1.rat: .... .... .... .... .... .... 2222 3344;p1: X... .... ..x. .... X... .... ..x. ...." } }));

            // 15. DUTY CHECKPOINT: half-time drill on the checkpoint's guns, gated.
            p.push_back (make ("Duty Checkpoint",
                { k808 + "kicks/808-Kicks01.wav", kit ("Rhino"), kit ("Shell Casing"), kit ("Shell Casing V"),
                  kit ("SVDM"), kit ("M701"), kit ("TOZ-34"), kit ("Three-Line"),
                  kit ("Body Blow"), kit ("Gas Cylinder Blast"), kit ("Emission Rise"), kit ("Razor"),
                  kit ("Military Attack EN"), kit ("Military Attack RU"), kit ("Mercenary Attack RU"), kit ("Mercenary Stop RU"),
                  explorer + "Livewire - Straight Talk [174bpm].wav", amen1 + "cw_amen17_175.wav", amen2 + "cw_amen_breaking.wav", jungle + "Breaks/Bronx Break.wav" },
                base ({ { "master_volume", -2.8f }, { "slam_bite", 0.35f }, { "clip_drive", 3.0f }, { "tr_attack", 0.3f } }),
                { { 0, "len=32 res=1/16;p1: X... .... .... ..x. ..x. .... .... ....;p2: .... .... X... .... .... .... X... ....;p3: x.x. x.x. x.x. x.x. x.x. x.x. x.x. x.x.;p5: .... .... .... .... .... .... .... ..X.;v: .... .... .... .... .... .... ssss ssss" },
                  { 1, "len=32 res=1/16;b1: x... .... .... .... .... .... .... ....;p6: x... x... x... x... .... .... .... ....;p13: X... .... .... .... .... .... .... ...." } }));

            // 16. SPATIAL BUBBLE: experimental breakcore; the break folds through every anomaly.
            p.push_back (make ("Spatial Bubble",
                { kit ("Pseudogiant Step IV"), kit ("Zubr"), kit ("Shell Casing II"), kit ("Shell Casing IV"),
                  kit ("Dnipro"), kit ("Dry Fire"), kit ("Phone Drop"), kit ("Kitchen Metal"),
                  kit ("Stalker Landing II"), kit ("Bubble Burst"), kit ("Small Bubble Burst"), kit ("Vortex"),
                  kit ("Monolith Attack RU"), kit ("Zombie Groan RU"), kit ("Veles Signal"), kit ("Psy Phantom"),
                  jungle + "Breaks/Bulldozer Amen.wav", amen2 + "cw_amen_downshift.wav", amen1 + "cw_amen20_164.wav", jungle + "Breaks/Chopper Break.wav" },
                base ({ { "master_volume", -3.0f }, { "b1_slicing", 2.0f }, { "scratch_depth", 0.7f }, { "emission", 0.3f }, { "slam_bark", 0.3f }, { "mb_depth", 0.4f } }),
                { { 0, "len=32 res=1/16;b1: x... .... .... .... .... .... .... ....;t: oooo oooo .... .... jjjj jjjj vvvv vvvv" },
                  { 1, "len=32 res=1/16;b1: 0123 4567 89ab cdef ghij klmn opqr stuv;t: .... .... oooo oooo .... .... bbbb bbbb;p1: X... .... ..x. .... X... .... ..x. ...." } }));

            return p;
        }
    }

    const std::vector<Preset>& factoryPresets()
    {
        static const std::vector<Preset> presets = build();
        return presets;
    }
}
