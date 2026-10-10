# Paths pinned by LAB X-0

The ZONE KIT catalogue (`Source/KitCatalog.h`) and the factory kits that use it read these files by relative path.
Moving or renaming one breaks that kit entry: it then reports FILE NOT FOUND, and test 15 fails.

## ZONE: STALKER SPECIMENS (95 files)

Relative to the STALKER SPECIMENS folder.

```
call_of_prypiat/sounds/anomaly/anomaly_gravy_blast01.ogg
clear_sky/sounds/detectors/geiger_click3.ogg
clear_sky/sounds/localization/eng/characters_voice/human_01/bandit/threat/drop_weapon/dropweapon_4.ogg
clear_sky/sounds/localization/eng/characters_voice/human_01/dolg/fight/attack/script_attack_14.ogg
clear_sky/sounds/localization/eng/characters_voice/human_01/military/fight/attack/attack_1.ogg
clear_sky/sounds/localization/eng/characters_voice/human_01/monolith/fight/attack/attack_one_2.ogg
clear_sky/sounds/localization/eng/characters_voice/human_01/stalker/threat/back_off/backoff_2.ogg
clear_sky/sounds/localization/eng/characters_voice/human_01/zombied/fight/attack/script_attack_2.ogg
clear_sky/sounds/localization/rus/characters_voice/human_01/dolg/fight/attack/script_attack_13.ogg
clear_sky/sounds/localization/rus/characters_voice/human_01/freedom/fight/attack/attack_many_2.ogg
clear_sky/sounds/localization/rus/characters_voice/human_01/killer/fight/attack/script_attack_1.ogg
clear_sky/sounds/localization/rus/characters_voice/human_01/military/fight/attack/attack_one_4.ogg
clear_sky/sounds/localization/rus/characters_voice/human_01/monolith/fight/attack/script_attack_4.ogg
clear_sky/sounds/weapons/gen_empty.ogg
clear_sky/sounds/weapons/generic_empty.ogg
heart_of_chornobyl/sounds/Events/Anomalies/Bubble/SFX_Anomaly_BubbleBig_Destroy_04.flac
heart_of_chornobyl/sounds/Events/Anomalies/Bubble/SFX_Anomaly_BubbleSmall_Destroy_09.flac
heart_of_chornobyl/sounds/Events/Anomalies/Clicker/SFX_Anomaly_Clicker_Impact_04.flac
heart_of_chornobyl/sounds/Events/Anomalies/Electro/SFX_Anomaly_Electro_17.flac
heart_of_chornobyl/sounds/Events/Anomalies/FireBreath/SFX_FireBreath_13.flac
heart_of_chornobyl/sounds/Events/Anomalies/LightningBall/SFX_LightningBall_06.flac
heart_of_chornobyl/sounds/Events/Anomalies/Razor/SFX_Anomaly_Razor_09.flac
heart_of_chornobyl/sounds/Events/Anomalies/Visual_Anomalies/Safebox/SFX_Anomaly_Safebox_Lightning_05.flac
heart_of_chornobyl/sounds/Events/Anomalies/Vortex/SFX_Vortex_15.flac
heart_of_chornobyl/sounds/Events/Artifacts/SFX_Artifact_Jump_02.flac
heart_of_chornobyl/sounds/Events/Effects/Psy/SFX_Psy_PhantomDeath_02.flac
heart_of_chornobyl/sounds/Events/Emission/SFX_Emission_Active_Stop_11.flac
heart_of_chornobyl/sounds/Events/Emission/SFX_Emission_Pre_Start_16.flac
heart_of_chornobyl/sounds/Events/Explosion/SFX_Explosion_Barrel_19.flac
heart_of_chornobyl/sounds/Events/Explosion/SFX_Explosion_GasCylinder_10.flac
heart_of_chornobyl/sounds/Events/Mutants/Flesh/Flesh_Voice/SFX_Flesh_Hit_Punch_04.flac
heart_of_chornobyl/sounds/Events/Mutants/pseudogiant/Pseudogiant_Locomotion/SFX_Pseudogiant_Hand_Hit_07.flac
heart_of_chornobyl/sounds/Events/Mutants/pseudogiant/Pseudogiant_Locomotion/SFX_Pseudogiant_Step_179.flac
heart_of_chornobyl/sounds/Events/Mutants/pseudogiant/Pseudogiant_Locomotion/SFX_Pseudogiant_Step_411.flac
heart_of_chornobyl/sounds/Events/Mutants/pseudogiant/Pseudogiant_Locomotion/SFX_Pseudogiant_Step_615.flac
heart_of_chornobyl/sounds/Events/Mutants/pseudogiant/Pseudogiant_Locomotion/SFX_Pseudogiant_Step_834.flac
heart_of_chornobyl/sounds/Events/Mutants/snork/Snork_SFX/SFX/SFX_Snork_Kick_Attack_Two_Hand_22.flac
heart_of_chornobyl/sounds/Events/PhysicsInteraction/Collision/DeadBody/SFX_PhysicsInteraction_Collision_DeadBody_Mutant_S_07.flac
heart_of_chornobyl/sounds/Events/PhysicsInteraction/Collision/Items/Ammo/SFX_PhysicsInteraction_Collision_Ammo_Grenade_24.flac
heart_of_chornobyl/sounds/Events/PhysicsInteraction/Collision/Items/Equipment/SFX_PhysicsInteraction_Collision_Bolt_55.flac
heart_of_chornobyl/sounds/Events/PhysicsInteraction/Collision/Items/Equipment/SFX_PhysicsInteraction_Collision_Grenade_20.flac
heart_of_chornobyl/sounds/Events/PhysicsInteraction/Collision/Items/Equipment/SFX_PhysicsInteraction_Collision_Grenade_21.flac
heart_of_chornobyl/sounds/Events/PhysicsInteraction/Collision/Items/Magazines/SFX_PhysicsInteraction_Collision_Magazine_Plastic_Medium_29.flac
heart_of_chornobyl/sounds/Events/PhysicsInteraction/Collision/Items/SFX_PhysicsInteraction_Collision_Device_02.flac
heart_of_chornobyl/sounds/Events/PhysicsInteraction/Collision/PhysicalObjects/SFX_PhysicsInteraction_Collision_MetalKitchenUtensils_04.flac
heart_of_chornobyl/sounds/Events/PhysicsInteraction/Collision/PhysicalObjects/SFX_PhysicsInteraction_Collision_Phone_07.flac
heart_of_chornobyl/sounds/Events/PhysicsInteraction/Destruct/SFX_PhysicsInteraction_Destruct_BigBox_22.flac
heart_of_chornobyl/sounds/Events/UI/InteractTrigger/Weapon_Unload/ar/SFX_WeaponUnload_Auto_AK74_02.flac
heart_of_chornobyl/sounds/Events/UI/Inventory/SFX_Inventory_Pickup_Item_56.flac
heart_of_chornobyl/sounds/Events/Voiceover/Effects/HitReactions/VO_PC_HitReaction_FallDamage_05.flac
heart_of_chornobyl/sounds/Events/Voiceover/Effects/knockdown/VO_PC_Knockdown_Front_Fall_22.flac
heart_of_chornobyl/sounds/Events/Weapons/HG/apb/APB_Fire/SFX_APB_Oneshot_114.flac
heart_of_chornobyl/sounds/Events/Weapons/HG/m10/M10_Fire/SFX_M10_Oneshot_100.flac
heart_of_chornobyl/sounds/Events/Weapons/HG/rhino/Rhino_Fire/SFX_Rhino_Oneshot_104.flac
heart_of_chornobyl/sounds/Events/Weapons/SG/d12/D12_Fire/SFX_D12_Oneshot_113.flac
heart_of_chornobyl/sounds/Events/Weapons/SG/m860/M860_Fire/SFX_M860_Oneshot_13.flac
heart_of_chornobyl/sounds/Events/Weapons/SG/spsa/SPSA_Fire/SFX_SPSA_Oneshot_121.flac
heart_of_chornobyl/sounds/Events/Weapons/SG/toz34/TOZ34_Fire/SFX_TOZ34_Oneshot_06.flac
heart_of_chornobyl/sounds/Events/Weapons/SG/toz34/TOZ34_Reload/SFX_TOZ34_Mag_Out_Tactical_02.flac
heart_of_chornobyl/sounds/Events/Weapons/Weapon_Related/Bullets/SFX_Shell_Collision_130.flac
heart_of_chornobyl/sounds/Events/Weapons/Weapon_Related/Bullets/SFX_Shell_Collision_139.flac
heart_of_chornobyl/sounds/Events/Weapons/Weapon_Related/Bullets/SFX_Shell_Collision_229.flac
heart_of_chornobyl/sounds/Events/Weapons/Weapon_Related/Bullets/SFX_Shell_Collision_359.flac
heart_of_chornobyl/sounds/Events/Weapons/Weapon_Related/Bullets/SFX_Shell_Collision_479.flac
heart_of_chornobyl/sounds/Events/Weapons/ar/dnipro/Dnipro_FIre/SFX_Dnipro_Oneshot_43.flac
heart_of_chornobyl/sounds/Events/Weapons/ar/gp37/GP37_Fire/SFX_GP37_Oneshot_148.flac
heart_of_chornobyl/sounds/Events/Weapons/ar/grim/Grim_Fire/SFX_Grim_Oneshot_145.flac
heart_of_chornobyl/sounds/Events/Weapons/smg/bucket/Bucket_Fire/SFX_Bucket_Oneshot_34.flac
heart_of_chornobyl/sounds/Events/Weapons/smg/integral/Integral_Fire/SFX_Integral_Oneshot_117.flac
heart_of_chornobyl/sounds/Events/Weapons/smg/viper/Viper_Fire/SFX_Viper_Oneshot_57.flac
heart_of_chornobyl/sounds/Events/Weapons/smg/zubr/Zubr_Fire/SFX_Zubr_Oneshot_174.flac
heart_of_chornobyl/sounds/Events/Weapons/sr/ThreeLine/ThreeLine_Fire/SFX_ThreeLine_Oneshot_10.flac
heart_of_chornobyl/sounds/Events/Weapons/sr/m701/M701_Fire/SFX_M701_Oneshot_100.flac
heart_of_chornobyl/sounds/Events/Weapons/sr/svdm/SVDM_Fire/SFX_SVDM_Oneshot_06.flac
heart_of_chornobyl/sounds/Events/Weapons/sr/svu/SVU_Fire/SFX_SVU_Oneshot_126.flac
heart_of_chornobyl/sounds/Events/detectors/DetectorBear/SFX_DetectorBear_Signal_01.flac
heart_of_chornobyl/sounds/Events/detectors/DetectorVeles/SFX_DetectorVeles_Signal_01.flac
heart_of_chornobyl/sounds/Events/human/GettingDamage/SFX_Human_GettingDamage_55.flac
heart_of_chornobyl/sounds/Events/human/GettingDamage/SFX_Human_GettingDamage_62.flac
heart_of_chornobyl/sounds/Events/human/Movement/SFX_Human_Landing_616.flac
heart_of_chornobyl/sounds/Events/human/Movement/SFX_Human_Landing_659.flac
shadow_of_chornobyl/sounds/anomaly/electra_hit.ogg
shadow_of_chornobyl/sounds/detectors/contact_1.ogg
shadow_of_chornobyl/sounds/detectors/geiger_1.ogg
shadow_of_chornobyl/sounds/detectors/geiger_3.ogg
shadow_of_chornobyl/sounds/localization/rus/characters_voice/human_01/bandit/threat/drop_weapon/dropweapon_6.ogg
shadow_of_chornobyl/sounds/localization/rus/characters_voice/human_01/ecolog/threat/back_off/backoff_2.ogg
shadow_of_chornobyl/sounds/localization/rus/characters_voice/human_01/killer/threat/stop/stop_3.ogg
shadow_of_chornobyl/sounds/localization/rus/characters_voice/human_01/monolith/threat/drop_weapon/dropweapon_4.ogg
shadow_of_chornobyl/sounds/localization/rus/characters_voice/human_01/zombied/fight/attack/attack_1.ogg
shadow_of_chornobyl/sounds/monsters/controller/controller_final_hit.ogg
shadow_of_chornobyl/sounds/monsters/zombie/zombie_attack_hit_0.ogg
shadow_of_chornobyl/sounds/weapons/ak74_shot_0.ogg
shadow_of_chornobyl/sounds/weapons/f1_explode2.ogg
shadow_of_chornobyl/sounds/weapons/fort_shoot.ogg
```

## PACK: FL Studio's legacy `stalker sounds` pack (11 files)

Relative to `Data\Patches\Packs\Legacy\stalker sounds`.

```
anomaly/anomaly_body_tear_1.ogg
detectors/da-2_beep1.ogg
material/actor/step/n_metal_plate_1.ogg
material/actor/step/n_wood_3.ogg
material/bullet/collide/water01gr.ogg
material/dead-body/collide/hithard05hl.ogg
material/hoof/hoof_hard4.ogg
material/human/step/n_default_2.ogg
material/large_weapon/collide/bounce03hl.ogg
material/medium/paw_earth4.ogg
soundtrack/static/static_01.ogg
```
