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
        // ORDNANCE: kicks and booms (most energy below 150 Hz, fast attack)
        { "Pseudogiant Step",          KitGroup::ordnance, Root::zone, LABX0_S2 "Mutants/pseudogiant/Pseudogiant_Locomotion/SFX_Pseudogiant_Step_179.flac" },
        { "Pseudogiant Step II",       KitGroup::ordnance, Root::zone, LABX0_S2 "Mutants/pseudogiant/Pseudogiant_Locomotion/SFX_Pseudogiant_Step_411.flac" },
        { "Pseudogiant Step III",      KitGroup::ordnance, Root::zone, LABX0_S2 "Mutants/pseudogiant/Pseudogiant_Locomotion/SFX_Pseudogiant_Step_615.flac" },
        { "Pseudogiant Step IV",       KitGroup::ordnance, Root::zone, LABX0_S2 "Mutants/pseudogiant/Pseudogiant_Locomotion/SFX_Pseudogiant_Step_834.flac" },
        { "Pseudogiant Fist",          KitGroup::ordnance, Root::zone, LABX0_S2 "Mutants/pseudogiant/Pseudogiant_Locomotion/SFX_Pseudogiant_Hand_Hit_07.flac" },
        { "Stalker Landing",           KitGroup::ordnance, Root::zone, LABX0_S2 "human/Movement/SFX_Human_Landing_659.flac" },
        { "Stalker Landing II",        KitGroup::ordnance, Root::zone, LABX0_S2 "human/Movement/SFX_Human_Landing_616.flac" },
        { "Body Blow",                 KitGroup::ordnance, Root::zone, LABX0_S2 "human/GettingDamage/SFX_Human_GettingDamage_55.flac" },
        { "Body Blow II",              KitGroup::ordnance, Root::zone, LABX0_S2 "human/GettingDamage/SFX_Human_GettingDamage_62.flac" },
        { "Barrel Blast",              KitGroup::ordnance, Root::zone, LABX0_S2 "Explosion/SFX_Explosion_Barrel_19.flac" },
        { "Gas Cylinder Blast",        KitGroup::ordnance, Root::zone, LABX0_S2 "Explosion/SFX_Explosion_GasCylinder_10.flac" },
        { "Snork Kick",                KitGroup::ordnance, Root::zone, LABX0_S2 "Mutants/snork/Snork_SFX/SFX/SFX_Snork_Kick_Attack_Two_Hand_22.flac" },
        { "Flesh Punch",               KitGroup::ordnance, Root::zone, LABX0_S2 "Mutants/Flesh/Flesh_Voice/SFX_Flesh_Hit_Punch_04.flac" },
        { "Mutant Fall",               KitGroup::ordnance, Root::zone, LABX0_S2 "PhysicsInteraction/Collision/DeadBody/SFX_PhysicsInteraction_Collision_DeadBody_Mutant_S_07.flac" },
        { "Zombie Blow",               KitGroup::ordnance, Root::zone, "shadow_of_chornobyl/sounds/monsters/zombie/zombie_attack_hit_0.ogg" },
        { "F1 Grenade",                KitGroup::ordnance, Root::zone, "shadow_of_chornobyl/sounds/weapons/f1_explode2.ogg" },
        { "Dead Body Drop",            KitGroup::ordnance, Root::pack, "material/dead-body/collide/hithard05hl.ogg" },

        // FIRE: snares from gunshots (broadband, attack under 3 ms)
        { "Rhino",                     KitGroup::fire,     Root::zone, LABX0_S2 "Weapons/HG/rhino/Rhino_Fire/SFX_Rhino_Oneshot_104.flac" },
        { "Bucket SMG",                KitGroup::fire,     Root::zone, LABX0_S2 "Weapons/smg/bucket/Bucket_Fire/SFX_Bucket_Oneshot_34.flac" },
        { "Grim",                      KitGroup::fire,     Root::zone, LABX0_S2 "Weapons/ar/grim/Grim_Fire/SFX_Grim_Oneshot_145.flac" },
        { "APB",                       KitGroup::fire,     Root::zone, LABX0_S2 "Weapons/HG/apb/APB_Fire/SFX_APB_Oneshot_114.flac" },
        { "M10 Pistol",                KitGroup::fire,     Root::zone, LABX0_S2 "Weapons/HG/m10/M10_Fire/SFX_M10_Oneshot_100.flac" },
        { "D12 Shotgun",               KitGroup::fire,     Root::zone, LABX0_S2 "Weapons/SG/d12/D12_Fire/SFX_D12_Oneshot_113.flac" },
        { "SVDM",                      KitGroup::fire,     Root::zone, LABX0_S2 "Weapons/sr/svdm/SVDM_Fire/SFX_SVDM_Oneshot_06.flac" },
        { "M701",                      KitGroup::fire,     Root::zone, LABX0_S2 "Weapons/sr/m701/M701_Fire/SFX_M701_Oneshot_100.flac" },
        { "TOZ-34",                    KitGroup::fire,     Root::zone, LABX0_S2 "Weapons/SG/toz34/TOZ34_Fire/SFX_TOZ34_Oneshot_06.flac" },
        { "GP37",                      KitGroup::fire,     Root::zone, LABX0_S2 "Weapons/ar/gp37/GP37_Fire/SFX_GP37_Oneshot_148.flac" },
        { "Integral",                  KitGroup::fire,     Root::zone, LABX0_S2 "Weapons/smg/integral/Integral_Fire/SFX_Integral_Oneshot_117.flac" },
        { "M860",                      KitGroup::fire,     Root::zone, LABX0_S2 "Weapons/SG/m860/M860_Fire/SFX_M860_Oneshot_13.flac" },
        { "Viper",                     KitGroup::fire,     Root::zone, LABX0_S2 "Weapons/smg/viper/Viper_Fire/SFX_Viper_Oneshot_57.flac" },
        { "SPSA",                      KitGroup::fire,     Root::zone, LABX0_S2 "Weapons/SG/spsa/SPSA_Fire/SFX_SPSA_Oneshot_121.flac" },
        { "SVU",                       KitGroup::fire,     Root::zone, LABX0_S2 "Weapons/sr/svu/SVU_Fire/SFX_SVU_Oneshot_126.flac" },
        { "Three-Line",                KitGroup::fire,     Root::zone, LABX0_S2 "Weapons/sr/ThreeLine/ThreeLine_Fire/SFX_ThreeLine_Oneshot_10.flac" },
        { "Zubr",                      KitGroup::fire,     Root::zone, LABX0_S2 "Weapons/smg/zubr/Zubr_Fire/SFX_Zubr_Oneshot_174.flac" },
        { "Dnipro",                    KitGroup::fire,     Root::zone, LABX0_S2 "Weapons/ar/dnipro/Dnipro_FIre/SFX_Dnipro_Oneshot_43.flac" },
        { "AK-74 SoC",                 KitGroup::fire,     Root::zone, "shadow_of_chornobyl/sounds/weapons/ak74_shot_0.ogg" },
        { "Fort SoC",                  KitGroup::fire,     Root::zone, "shadow_of_chornobyl/sounds/weapons/fort_shoot.ogg" },

        // CLICK: hats and ticks (energy above 5 kHz, short)
        { "Shell Casing",              KitGroup::click,    Root::zone, LABX0_S2 "Weapons/Weapon_Related/Bullets/SFX_Shell_Collision_130.flac" },
        { "Shell Casing II",           KitGroup::click,    Root::zone, LABX0_S2 "Weapons/Weapon_Related/Bullets/SFX_Shell_Collision_139.flac" },
        { "Shell Casing III",          KitGroup::click,    Root::zone, LABX0_S2 "Weapons/Weapon_Related/Bullets/SFX_Shell_Collision_229.flac" },
        { "Shell Casing IV",           KitGroup::click,    Root::zone, LABX0_S2 "Weapons/Weapon_Related/Bullets/SFX_Shell_Collision_359.flac" },
        { "Shell Casing V",            KitGroup::click,    Root::zone, LABX0_S2 "Weapons/Weapon_Related/Bullets/SFX_Shell_Collision_479.flac" },
        { "Detector Beep",             KitGroup::click,    Root::pack, "detectors/da-2_beep1.ogg" },
        { "Dry Fire",                  KitGroup::click,    Root::zone, "clear_sky/sounds/weapons/gen_empty.ogg" },
        { "Dry Fire II",               KitGroup::click,    Root::zone, "clear_sky/sounds/weapons/generic_empty.ogg" },
        { "Geiger",                    KitGroup::click,    Root::zone, "shadow_of_chornobyl/sounds/detectors/geiger_1.ogg" },
        { "Geiger II",                 KitGroup::click,    Root::zone, "shadow_of_chornobyl/sounds/detectors/geiger_3.ogg" },
        { "Geiger Click",              KitGroup::click,    Root::zone, "clear_sky/sounds/detectors/geiger_click3.ogg" },
        { "Contact Beep",              KitGroup::click,    Root::zone, "shadow_of_chornobyl/sounds/detectors/contact_1.ogg" },
        { "Veles Signal",              KitGroup::click,    Root::zone, LABX0_S2 "detectors/DetectorVeles/SFX_DetectorVeles_Signal_01.flac" },
        { "Bear Signal",               KitGroup::click,    Root::zone, LABX0_S2 "detectors/DetectorBear/SFX_DetectorBear_Signal_01.flac" },
        { "Mag Out",                   KitGroup::click,    Root::zone, LABX0_S2 "Weapons/SG/toz34/TOZ34_Reload/SFX_TOZ34_Mag_Out_Tactical_02.flac" },
        { "Inventory Pick",            KitGroup::click,    Root::zone, LABX0_S2 "UI/Inventory/SFX_Inventory_Pickup_Item_56.flac" },
        { "AK Unload",                 KitGroup::click,    Root::zone, LABX0_S2 "UI/InteractTrigger/Weapon_Unload/ar/SFX_WeaponUnload_Auto_AK74_02.flac" },

        // PERC: impacts, footsteps and metal (mid band)
        { "Grenade Bounce",            KitGroup::perc,     Root::zone, LABX0_S2 "PhysicsInteraction/Collision/Items/Equipment/SFX_PhysicsInteraction_Collision_Grenade_21.flac" },
        { "Grenade Bounce II",         KitGroup::perc,     Root::zone, LABX0_S2 "PhysicsInteraction/Collision/Items/Equipment/SFX_PhysicsInteraction_Collision_Grenade_20.flac" },
        { "Bolt Toss",                 KitGroup::perc,     Root::zone, LABX0_S2 "PhysicsInteraction/Collision/Items/Equipment/SFX_PhysicsInteraction_Collision_Bolt_55.flac" },
        { "Phone Drop",                KitGroup::perc,     Root::zone, LABX0_S2 "PhysicsInteraction/Collision/PhysicalObjects/SFX_PhysicsInteraction_Collision_Phone_07.flac" },
        { "Kitchen Metal",             KitGroup::perc,     Root::zone, LABX0_S2 "PhysicsInteraction/Collision/PhysicalObjects/SFX_PhysicsInteraction_Collision_MetalKitchenUtensils_04.flac" },
        { "Ammo Box",                  KitGroup::perc,     Root::zone, LABX0_S2 "PhysicsInteraction/Collision/Items/Ammo/SFX_PhysicsInteraction_Collision_Ammo_Grenade_24.flac" },
        { "Boot Step",                 KitGroup::perc,     Root::pack, "material/human/step/n_default_2.ogg" },
        { "Metal Plate Step",          KitGroup::perc,     Root::pack, "material/actor/step/n_metal_plate_1.ogg" },
        { "Wood Step",                 KitGroup::perc,     Root::pack, "material/actor/step/n_wood_3.ogg" },
        { "Hoof",                      KitGroup::perc,     Root::pack, "material/hoof/hoof_hard4.ogg" },
        { "Paw on Earth",              KitGroup::perc,     Root::pack, "material/medium/paw_earth4.ogg" },
        { "Magazine Drop",             KitGroup::perc,     Root::zone, LABX0_S2 "PhysicsInteraction/Collision/Items/Magazines/SFX_PhysicsInteraction_Collision_Magazine_Plastic_Medium_29.flac" },
        { "Device Knock",              KitGroup::perc,     Root::zone, LABX0_S2 "PhysicsInteraction/Collision/Items/SFX_PhysicsInteraction_Collision_Device_02.flac" },
        { "Rifle Bounce",              KitGroup::perc,     Root::pack, "material/large_weapon/collide/bounce03hl.ogg" },
        { "Big Box Break",             KitGroup::perc,     Root::zone, LABX0_S2 "PhysicsInteraction/Destruct/SFX_PhysicsInteraction_Destruct_BigBox_22.flac" },
        { "Bullet in Water",           KitGroup::perc,     Root::pack, "material/bullet/collide/water01gr.ogg" },

        // VOX: barks, named by faction and situation (the words are not transcribed), and grunts
        { "Duty Attack EN",            KitGroup::vox,      Root::zone, "clear_sky/sounds/localization/eng/characters_voice/human_01/dolg/fight/attack/script_attack_14.ogg" },
        { "Duty Attack RU",            KitGroup::vox,      Root::zone, "clear_sky/sounds/localization/rus/characters_voice/human_01/dolg/fight/attack/script_attack_13.ogg" },
        { "Monolith Drop Weapon RU",   KitGroup::vox,      Root::zone, "shadow_of_chornobyl/sounds/localization/rus/characters_voice/human_01/monolith/threat/drop_weapon/dropweapon_4.ogg" },
        { "Monolith Attack RU",        KitGroup::vox,      Root::zone, "clear_sky/sounds/localization/rus/characters_voice/human_01/monolith/fight/attack/script_attack_4.ogg" },
        { "Monolith Attack EN",        KitGroup::vox,      Root::zone, "clear_sky/sounds/localization/eng/characters_voice/human_01/monolith/fight/attack/attack_one_2.ogg" },
        { "Bandit Drop Weapon EN",     KitGroup::vox,      Root::zone, "clear_sky/sounds/localization/eng/characters_voice/human_01/bandit/threat/drop_weapon/dropweapon_4.ogg" },
        { "Bandit Drop Weapon RU",     KitGroup::vox,      Root::zone, "shadow_of_chornobyl/sounds/localization/rus/characters_voice/human_01/bandit/threat/drop_weapon/dropweapon_6.ogg" },
        { "Military Attack EN",        KitGroup::vox,      Root::zone, "clear_sky/sounds/localization/eng/characters_voice/human_01/military/fight/attack/attack_1.ogg" },
        { "Military Attack RU",        KitGroup::vox,      Root::zone, "clear_sky/sounds/localization/rus/characters_voice/human_01/military/fight/attack/attack_one_4.ogg" },
        { "Mercenary Stop RU",         KitGroup::vox,      Root::zone, "shadow_of_chornobyl/sounds/localization/rus/characters_voice/human_01/killer/threat/stop/stop_3.ogg" },
        { "Mercenary Attack RU",       KitGroup::vox,      Root::zone, "clear_sky/sounds/localization/rus/characters_voice/human_01/killer/fight/attack/script_attack_1.ogg" },
        { "Freedom Attack RU",         KitGroup::vox,      Root::zone, "clear_sky/sounds/localization/rus/characters_voice/human_01/freedom/fight/attack/attack_many_2.ogg" },
        { "Stalker Back Off EN",       KitGroup::vox,      Root::zone, "clear_sky/sounds/localization/eng/characters_voice/human_01/stalker/threat/back_off/backoff_2.ogg" },
        { "Ecologist Back Off RU",     KitGroup::vox,      Root::zone, "shadow_of_chornobyl/sounds/localization/rus/characters_voice/human_01/ecolog/threat/back_off/backoff_2.ogg" },
        { "Zombie Attack EN",          KitGroup::vox,      Root::zone, "clear_sky/sounds/localization/eng/characters_voice/human_01/zombied/fight/attack/script_attack_2.ogg" },
        { "Zombie Groan RU",           KitGroup::vox,      Root::zone, "shadow_of_chornobyl/sounds/localization/rus/characters_voice/human_01/zombied/fight/attack/attack_1.ogg" },
        { "Knockdown Grunt",           KitGroup::vox,      Root::zone, LABX0_S2 "Voiceover/Effects/knockdown/VO_PC_Knockdown_Front_Fall_22.flac" },
        { "Fall Grunt",                KitGroup::vox,      Root::zone, LABX0_S2 "Voiceover/Effects/HitReactions/VO_PC_HitReaction_FallDamage_05.flac" },

        // FX: anomalies, the emission, artifacts, static
        { "Fire Breath",               KitGroup::fx,       Root::zone, LABX0_S2 "Anomalies/FireBreath/SFX_FireBreath_13.flac" },
        { "Bubble Burst",              KitGroup::fx,       Root::zone, LABX0_S2 "Anomalies/Bubble/SFX_Anomaly_BubbleBig_Destroy_04.flac" },
        { "Small Bubble Burst",        KitGroup::fx,       Root::zone, LABX0_S2 "Anomalies/Bubble/SFX_Anomaly_BubbleSmall_Destroy_09.flac" },
        { "Electro Discharge",         KitGroup::fx,       Root::zone, LABX0_S2 "Anomalies/Electro/SFX_Anomaly_Electro_17.flac" },
        { "Artifact Jump",             KitGroup::fx,       Root::zone, LABX0_S2 "Artifacts/SFX_Artifact_Jump_02.flac" },
        { "Body Tear",                 KitGroup::fx,       Root::pack, "anomaly/anomaly_body_tear_1.ogg" },
        { "Razor",                     KitGroup::fx,       Root::zone, LABX0_S2 "Anomalies/Razor/SFX_Anomaly_Razor_09.flac" },
        { "Emission Rise",             KitGroup::fx,       Root::zone, LABX0_S2 "Emission/SFX_Emission_Pre_Start_16.flac" },
        { "Emission Stop",             KitGroup::fx,       Root::zone, LABX0_S2 "Emission/SFX_Emission_Active_Stop_11.flac" },
        { "Safebox Lightning",         KitGroup::fx,       Root::zone, LABX0_S2 "Anomalies/Visual_Anomalies/Safebox/SFX_Anomaly_Safebox_Lightning_05.flac" },
        { "Clicker Impact",            KitGroup::fx,       Root::zone, LABX0_S2 "Anomalies/Clicker/SFX_Anomaly_Clicker_Impact_04.flac" },
        { "Vortex",                    KitGroup::fx,       Root::zone, LABX0_S2 "Anomalies/Vortex/SFX_Vortex_15.flac" },
        { "Gravity Blast",             KitGroup::fx,       Root::zone, "call_of_prypiat/sounds/anomaly/anomaly_gravy_blast01.ogg" },
        { "Lightning Ball",            KitGroup::fx,       Root::zone, LABX0_S2 "Anomalies/LightningBall/SFX_LightningBall_06.flac" },
        { "Electra Hit",               KitGroup::fx,       Root::zone, "shadow_of_chornobyl/sounds/anomaly/electra_hit.ogg" },
        { "Psy Phantom",               KitGroup::fx,       Root::zone, LABX0_S2 "Effects/Psy/SFX_Psy_PhantomDeath_02.flac" },
        { "Radio Static",              KitGroup::fx,       Root::pack, "soundtrack/static/static_01.ogg" },
        { "Controller Hit",            KitGroup::fx,       Root::zone, "shadow_of_chornobyl/sounds/monsters/controller/controller_final_hit.ogg" },
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
