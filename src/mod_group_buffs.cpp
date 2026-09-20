/*
 * mod-group-buffs
 *
 * "The more players, the stronger": every member of a group / raid gets bonuses that grow with the
 * number of counted members N: damage done, XP gained, regeneration, run speed and attack speed.
 *
 *     bonus = min(PerMember * (N - 1), Max)
 *
 * How it works
 *  - XP is scaled directly in PlayerScript::OnPlayerGiveXP (stateless, exact float percent).
 *  - Everything else is carried by three passive, hidden, server-side spells (200210-200212, created by
 *    data/sql/db-world/updates/mod_group_buffs_*.sql). Every UpdateIntervalMs each player's bonus is
 *    re-evaluated in PlayerScript::OnPlayerUpdate; the self-aura is created on demand and its effect amounts
 *    are rewritten with AuraEffect::ChangeAmount, or the aura is removed when there is no bonus.
 *    Using real auras means the core keeps every derived stat right by itself (mounts, shapeshifts, other
 *    speed auras, attack timers, regen, character sheet) and nothing can leak: the auras live on the Player
 *    object, are passive (never saved to the DB, never sent to the client) and vanish with it on logout.
 *  - Because the evaluation is periodic and recomputed from scratch (idempotent), group join / leave /
 *    disband, map change, death and resurrection are all handled without any bookkeeping and bonuses can
 *    never stack on repeated recalculation.
 *  - Per-player state (interval timer, announce state) lives in Player::CustomData.
 *  - Bots are ordinary Players; a bot is recognised through its session (WorldSession::IsBot()).
 */

#include "Chat.h"
#include "Config.h"
#include "Group.h"
#include "GroupReference.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "Timer.h"
#include "WorldSession.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace
{
    // Server-side passive carrier spells, see data/sql/db-world/updates/mod_group_buffs_2026_09_20_00.sql
    constexpr uint32 SPELL_BONUS_SWIFTNESS = 200210; // eff0 run speed, eff1 mounted speed, eff2 melee+ranged haste
    constexpr uint32 SPELL_BONUS_VIGOR     = 200211; // eff0 spell haste, eff1 health regen, eff2 mana regen
    constexpr uint32 SPELL_BONUS_MIGHT     = 200212; // eff0 damage done, eff1 energy regen

    constexpr std::array<uint32, 3> BONUS_SPELLS = { SPELL_BONUS_SWIFTNESS, SPELL_BONUS_VIGOR, SPELL_BONUS_MIGHT };

    // Hard sanity clamp for aura percentages (a casting speed amount >= 1000 would mean "instant cast").
    constexpr int32 MAX_AURA_AMOUNT = 300;

    // Do not spam a player whose group size flaps (someone entering / leaving range, dying, ...).
    constexpr uint32 ANNOUNCE_COOLDOWN_MS = 3000;

    struct EffectConfig
    {
        bool enable = true;
        float perMember = 0.0f;
        float max = 0.0f;

        // Bonus in percent for the given number of extra (other) members.
        [[nodiscard]] float Bonus(uint32 extraMembers) const
        {
            if (!enable || perMember <= 0.0f || max <= 0.0f)
                return 0.0f;

            return std::min(perMember * static_cast<float>(extraMembers), max);
        }
    };

    struct GroupBuffsConfig
    {
        bool enable = true;
        bool includeBots = true;
        bool sameMapOnly = true;
        float maxDistance = 0.0f;
        bool countDead = false;
        uint32 maxGroupSize = 40;
        bool disableInPvP = true;
        bool announce = true;
        uint32 intervalMs = 1000;

        EffectConfig damage;
        EffectConfig xp;
        EffectConfig regen;
        EffectConfig speed;
        EffectConfig haste;
        bool speedMounted = true;
        bool hasteSpells = true;
    };

    GroupBuffsConfig sCfg;

    // False when the carrier spells are missing from spell_dbc (SQL not applied): only XP works then.
    bool sAuraSpellsAvailable = true;

    // Per-player state, lives in Player::CustomData (map updates run in several threads, each player is only
    // ever touched by the thread updating its own map).
    struct GroupBuffsData : public DataMap::Base
    {
        uint32 elapsedMs = 0;       // time since the last evaluation
        bool active = false;        // our auras may currently be on the player
        uint32 lastSize = 1;        // group size the player was last told about
        uint32 lastAnnounceMs = 0;  // getMSTime() of the last announcement (0 = never)
    };

    std::string const DATA_KEY = "GroupBuffs";

    // The computed bonuses for one player. Aura based values are whole percents (the aura amounts).
    struct Bonuses
    {
        uint32 size = 1;
        float xp = 0.0f;
        int32 damage = 0;
        int32 speed = 0;
        int32 haste = 0;
        int32 regen = 0;

        [[nodiscard]] bool HasAuraBonus() const
        {
            return damage > 0 || speed > 0 || haste > 0 || regen > 0;
        }
    };

    void LoadEffectConfig(EffectConfig& effect, std::string const& prefix, float defaultPerMember, float defaultMax)
    {
        effect.enable = sConfigMgr->GetOption<bool>(prefix + ".Enable", true);
        effect.perMember = std::max(0.0f, sConfigMgr->GetOption<float>(prefix + ".PerMember", defaultPerMember));
        effect.max = std::max(0.0f, sConfigMgr->GetOption<float>(prefix + ".Max", defaultMax));
    }

    void LoadConfig()
    {
        sCfg.enable = sConfigMgr->GetOption<bool>("GroupBuffs.Enable", true);
        sCfg.includeBots = sConfigMgr->GetOption<bool>("GroupBuffs.IncludeBots", true);
        sCfg.sameMapOnly = sConfigMgr->GetOption<bool>("GroupBuffs.SameMapOnly", true);
        sCfg.maxDistance = std::max(0.0f, sConfigMgr->GetOption<float>("GroupBuffs.MaxDistance", 0.0f));
        sCfg.countDead = sConfigMgr->GetOption<bool>("GroupBuffs.CountDeadMembers", false);
        sCfg.maxGroupSize = std::clamp<uint32>(sConfigMgr->GetOption<uint32>("GroupBuffs.MaxGroupSize", 40), 2, 40);
        sCfg.disableInPvP = sConfigMgr->GetOption<bool>("GroupBuffs.DisableInPvPInstances", true);
        sCfg.announce = sConfigMgr->GetOption<bool>("GroupBuffs.AnnounceOnChange", true);
        sCfg.intervalMs = std::max<uint32>(sConfigMgr->GetOption<uint32>("GroupBuffs.UpdateIntervalMs", 1000), 250);

        LoadEffectConfig(sCfg.damage, "GroupBuffs.Damage", 1.5f, 25.0f);
        LoadEffectConfig(sCfg.xp, "GroupBuffs.Xp", 2.0f, 50.0f);
        LoadEffectConfig(sCfg.regen, "GroupBuffs.Regen", 3.0f, 60.0f);
        LoadEffectConfig(sCfg.speed, "GroupBuffs.Speed", 1.0f, 20.0f);
        LoadEffectConfig(sCfg.haste, "GroupBuffs.Haste", 1.0f, 25.0f);

        sCfg.speedMounted = sConfigMgr->GetOption<bool>("GroupBuffs.Speed.Mounted", true);
        sCfg.hasteSpells = sConfigMgr->GetOption<bool>("GroupBuffs.Haste.IncludeSpells", true);
    }

    bool IsBotPlayer(Player* player)
    {
        WorldSession* session = player->GetSession();
        return session && session->IsBot();
    }

    // Does `member` (another member of player's group) count towards player's group size?
    bool IsMemberCounted(Player* player, Player* member)
    {
        if (!member->IsInWorld())
            return false;

        if (!sCfg.includeBots && IsBotPlayer(member))
            return false;

        if (!sCfg.countDead && !member->IsAlive())
            return false;

        if ((sCfg.sameMapOnly || sCfg.maxDistance > 0.0f) && member->FindMap() != player->FindMap())
            return false;

        if (sCfg.maxDistance > 0.0f && !member->IsWithinDist(player, sCfg.maxDistance))
            return false;

        return true;
    }

    // Number of members (including the player) that count for the player's bonus. 1 = no bonus:
    // not grouped, dead, or in a PvP instance while those are disabled.
    uint32 GetGroupSize(Player* player)
    {
        if (!player->IsInWorld() || !player->IsAlive())
            return 1;

        if (sCfg.disableInPvP && (player->InBattleground() || player->InArena()))
            return 1;

        Group* group = player->GetGroup();
        if (!group)
            return 1;

        uint32 count = 0;
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member)
                continue;

            if (member != player && !IsMemberCounted(player, member))
                continue;

            ++count;
            if (count >= sCfg.maxGroupSize)
                break;
        }

        return std::max<uint32>(count, 1);
    }

    int32 ToAuraAmount(float percent)
    {
        return std::clamp(static_cast<int32>(std::lround(percent)), 0, MAX_AURA_AMOUNT);
    }

    Bonuses BuildBonuses(uint32 size)
    {
        Bonuses bonuses;
        bonuses.size = size;
        if (size < 2)
            return bonuses;

        uint32 const extraMembers = size - 1;
        bonuses.xp = sCfg.xp.Bonus(extraMembers);
        bonuses.damage = ToAuraAmount(sCfg.damage.Bonus(extraMembers));
        bonuses.speed = ToAuraAmount(sCfg.speed.Bonus(extraMembers));
        bonuses.haste = ToAuraAmount(sCfg.haste.Bonus(extraMembers));
        bonuses.regen = ToAuraAmount(sCfg.regen.Bonus(extraMembers));
        return bonuses;
    }

    // Make sure the carrier aura exists with exactly these effect amounts, or is gone when all are 0.
    void SetBonusAura(Player* player, uint32 spellId, std::array<int32, 3> const& amounts)
    {
        bool const wanted = amounts[0] > 0 || amounts[1] > 0 || amounts[2] > 0;
        Aura* aura = player->GetAura(spellId);

        if (!wanted)
        {
            if (aura)
                player->RemoveAurasDueToSpell(spellId);
            return;
        }

        if (!aura)
            aura = player->AddAura(spellId, player);

        if (!aura || aura->IsRemoved())
            return;

        for (uint8 i = 0; i < amounts.size(); ++i)
        {
            AuraEffect* effect = aura->GetEffect(i);
            if (effect && effect->GetAmount() != amounts[i])
                effect->ChangeAmount(amounts[i]);
        }
    }

    void ApplyBonusAuras(Player* player, Bonuses const& bonuses)
    {
        SetBonusAura(player, SPELL_BONUS_SWIFTNESS, { bonuses.speed, sCfg.speedMounted ? bonuses.speed : 0, bonuses.haste });
        SetBonusAura(player, SPELL_BONUS_VIGOR, { sCfg.hasteSpells ? bonuses.haste : 0, bonuses.regen, bonuses.regen });
        SetBonusAura(player, SPELL_BONUS_MIGHT, { bonuses.damage, bonuses.regen, 0 });
    }

    void RemoveBonusAuras(Player* player)
    {
        for (uint32 spellId : BONUS_SPELLS)
            player->RemoveAurasDueToSpell(spellId);
    }

    std::string FormatPercent(float value)
    {
        if (std::fabs(value - std::round(value)) < 0.05f)
            return Acore::StringFormat("{}", static_cast<int32>(std::lround(value)));

        return Acore::StringFormat("{:.1f}", value);
    }

    std::string BuildAnnouncement(Bonuses const& bonuses)
    {
        if (bonuses.size < 2)
            return "|cff4CFF00[Group Bonus]|r no group bonus active.";

        std::string parts;
        auto add = [&parts](std::string const& text)
        {
            if (!parts.empty())
                parts += ", ";
            parts += text;
        };

        if (bonuses.damage > 0)
            add(Acore::StringFormat("+{}% damage", bonuses.damage));
        if (bonuses.xp > 0.0f)
            add(Acore::StringFormat("+{}% XP", FormatPercent(bonuses.xp)));
        if (bonuses.regen > 0)
            add(Acore::StringFormat("+{}% regeneration", bonuses.regen));
        if (bonuses.speed > 0)
            add(Acore::StringFormat("+{}% run speed", bonuses.speed));
        if (bonuses.haste > 0)
            add(Acore::StringFormat("+{}% attack speed", bonuses.haste));

        if (parts.empty())
            return std::string();

        return Acore::StringFormat("|cff4CFF00[Group Bonus]|r {} in group: {}", bonuses.size, parts);
    }

    void AnnounceIfChanged(Player* player, GroupBuffsData* data, Bonuses const& bonuses)
    {
        // No messages for bots, and none while dead (a dead player has no bonus; the state is announced
        // again only if it differs from what the player was last told once they are back).
        if (!sCfg.announce || bonuses.size == data->lastSize || !player->IsAlive())
            return;

        // GetSession() can be null while a player is being torn down; IsBotPlayer() is false for a null session.
        if (!player->GetSession() || IsBotPlayer(player))
            return;

        uint32 const now = getMSTime();
        if (data->lastAnnounceMs != 0 && getMSTimeDiff(data->lastAnnounceMs, now) < ANNOUNCE_COOLDOWN_MS)
            return; // stay silent for now, lastSize is unchanged so we will tell the player later

        std::string const message = BuildAnnouncement(bonuses);
        if (!message.empty())
            ChatHandler(player->GetSession()).SendSysMessage(message);

        data->lastSize = bonuses.size;
        data->lastAnnounceMs = now;
    }

    // Recompute the player's bonus from scratch and bring the auras in line with it.
    void RefreshPlayer(Player* player, GroupBuffsData* data)
    {
        Bonuses bonuses;
        if (sCfg.enable)
            bonuses = BuildBonuses(GetGroupSize(player));

        if (sAuraSpellsAvailable)
        {
            if (bonuses.HasAuraBonus())
            {
                ApplyBonusAuras(player, bonuses);
                data->active = true;
            }
            else if (data->active)
            {
                RemoveBonusAuras(player);
                data->active = false;
            }
        }

        AnnounceIfChanged(player, data, bonuses);
    }
}

class GroupBuffsPlayerScript : public PlayerScript
{
public:
    GroupBuffsPlayerScript() : PlayerScript("GroupBuffsPlayerScript",
        { PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_GIVE_EXP, PLAYERHOOK_ON_PLAYER_JUST_DIED }) { }

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        GroupBuffsData* data = player->CustomData.GetDefault<GroupBuffsData>(DATA_KEY);

        data->elapsedMs += diff;
        if (data->elapsedMs < sCfg.intervalMs)
            return;

        data->elapsedMs = 0;
        RefreshPlayer(player, data);
    }

    // Evaluate again on the very next tick: a dead player loses the bonuses immediately.
    void OnPlayerJustDied(Player* player) override
    {
        if (GroupBuffsData* data = player->CustomData.Get<GroupBuffsData>(DATA_KEY))
            data->elapsedMs = sCfg.intervalMs;
    }

    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* /*victim*/, uint8 /*xpSource*/) override
    {
        if (!sCfg.enable || !sCfg.xp.enable || amount == 0)
            return;

        float const percent = sCfg.xp.Bonus(GetGroupSize(player) - 1);
        if (percent <= 0.0f)
            return;

        uint32 const extra = static_cast<uint32>(std::lround(static_cast<float>(amount) * percent / 100.0f));
        amount += extra;
    }
};

class GroupBuffsWorldScript : public WorldScript
{
public:
    GroupBuffsWorldScript() : WorldScript("GroupBuffsWorldScript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadConfig();
    }

    // The spell store is loaded by now: verify the carrier spells created by the module's SQL exist.
    void OnStartup() override
    {
        uint32 missing = 0;
        for (uint32 spellId : BONUS_SPELLS)
        {
            if (!sSpellMgr->GetSpellInfo(spellId))
            {
                ++missing;
                LOG_ERROR("server.loading", "mod-group-buffs: server-side spell {} is missing from spell_dbc "
                    "(data/sql/db-world/updates/mod_group_buffs_*.sql not applied?). Damage, speed, haste and regeneration "
                    "bonuses are disabled, only the XP bonus works.", spellId);
            }
        }

        sAuraSpellsAvailable = (missing == 0);

        if (sAuraSpellsAvailable)
            LOG_INFO("server.loading", "mod-group-buffs: loaded (enabled: {}, bots count: {}).", sCfg.enable, sCfg.includeBots);
    }
};

void AddGroupBuffsScripts()
{
    LoadConfig();

    new GroupBuffsWorldScript();
    new GroupBuffsPlayerScript();
}
