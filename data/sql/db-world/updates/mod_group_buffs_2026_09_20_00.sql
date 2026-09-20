-- mod-group-buffs: three server-side, passive, hidden "carrier" spells.
--
-- The module applies them to a player as self-auras and rewrites the effect amounts at runtime
-- (AuraEffect::ChangeAmount) according to the group size. Everything about them is server-side:
--   * Attributes = PASSIVE (0x40) | DO_NOT_DISPLAY (0x80) | NO_IMMUNITIES (0x20000000)
--     -> never sent to the client (no buff icon), never saved to the character DB, not dispellable.
--   * The client does not need them in Spell.dbc (client-side patch NOT required).
--   * EquippedItemClass = -1 -> no weapon/item requirement (needed for the damage-done aura to count).
--   * DurationIndex 21 = infinite, RangeIndex 1 = self, CastingTimeIndex 1 = instant.
--   * Effect_n = 6 (SPELL_EFFECT_APPLY_AURA), ImplicitTargetA_n = 1 (TARGET_UNIT_CASTER), base points 0.
--
-- 200210 Swiftness: eff1 129 MOD_SPEED_ALWAYS, eff2 130 MOD_MOUNTED_SPEED_ALWAYS, eff3 192 MOD_MELEE_RANGED_HASTE
-- 200211 Vigor    : eff1 216 HASTE_SPELLS,     eff2  88 MOD_HEALTH_REGEN_PERCENT, eff3 110 MOD_POWER_REGEN_PERCENT (mana)
-- 200212 Might    : eff1  79 MOD_DAMAGE_PERCENT_DONE (all schools, misc 127), eff2 110 MOD_POWER_REGEN_PERCENT (energy, misc 3)
--
-- Idempotent (DELETE + INSERT), only touches spell ids 200210-200212.

DELETE FROM `spell_dbc` WHERE `ID` IN (200210, 200211, 200212);

INSERT INTO `spell_dbc`
(`ID`, `Attributes`, `EquippedItemClass`, `CastingTimeIndex`, `DurationIndex`, `RangeIndex`, `SchoolMask`,
 `Effect_1`, `Effect_2`, `Effect_3`,
 `ImplicitTargetA_1`, `ImplicitTargetA_2`, `ImplicitTargetA_3`,
 `EffectAura_1`, `EffectAura_2`, `EffectAura_3`,
 `EffectMiscValue_1`, `EffectMiscValue_2`, `EffectMiscValue_3`,
 `Name_Lang_enUS`, `Name_Lang_Mask`)
VALUES
(200210, 536871104, -1, 1, 21, 1, 1, 6, 6, 6, 1, 1, 1, 129, 130, 192, 0, 0, 0,   'Group Bonus: Swiftness', 16712190),
(200211, 536871104, -1, 1, 21, 1, 1, 6, 6, 6, 1, 1, 1, 216,  88, 110, 0, 0, 0,   'Group Bonus: Vigor',     16712190),
(200212, 536871104, -1, 1, 21, 1, 1, 6, 6, 0, 1, 1, 0,  79, 110,   0, 127, 3, 0, 'Group Bonus: Might',     16712190);
