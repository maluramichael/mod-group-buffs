# mod-group-buffs

An [AzerothCore](https://www.azerothcore.org/) module (WotLK 3.3.5a): **the more players in
a group or raid, the stronger everyone gets.**

## What it does

Every member of a group/raid receives bonuses that scale with the number of counted members
`N`. For each effect:

```
bonus = min(PerMember * (N - 1), Max)      (percent)
```

Effects:

| Effect   | What it boosts                                                        |
|----------|----------------------------------------------------------------------|
| `Damage` | Melee, ranged and spell damage (all schools)                         |
| `Xp`     | Experience from kills, quests and exploration                        |
| `Regen`  | Out-of-combat health, mana (spirit) and energy regeneration          |
| `Speed`  | Run speed (also mounted)                                             |
| `Haste`  | Melee/ranged attack speed (optionally spell haste)                   |

Damage, Speed, Haste and Regen are applied through three passive, hidden, server-side spells
(created automatically by the module's SQL) — no client patch, no visible buff icon. XP is
applied exactly when experience is granted. Bonuses re-evaluate as members join, leave or
disband.

## Configuration

Each effect has its own `Enable`, `PerMember` and `Max`. Global keys include
`GroupBuffs.Enable`, `GroupBuffs.IncludeBots`, `GroupBuffs.SameMapOnly`,
`GroupBuffs.MaxGroupSize`, `GroupBuffs.UpdateIntervalMs`. See
`conf/mod_group_buffs.conf.dist` for the full list.

## Installation

Clone into your AzerothCore `modules/` directory and rebuild the worldserver. The module's
SQL is applied automatically by the DB updater on the next world start.

## License

Released under the GNU GPL v2 (or later).
