/*
 * Hunger & Thirst - implementation.
 *
 * The module keeps one hunger/thirst state per character in memory, keyed by
 * the low guid, and persists a single row per character in
 * `acore_characters`.`hunger_thirst_state`. All player-facing entry points are
 * declared in HungerThirst.h; player/unit hooks and commands only go through
 * that interface.
 *
 * Design notes:
 *   - The cache is touched from map update threads (Tick) and from the world
 *     thread (commands). Every access is protected by a mutex; no re-entrant
 *     player API (auras, chat, script hooks) is called while that mutex is
 *     held, because those calls can run script hooks that re-enter this
 *     module.
 *   - Writes are conditional on `updated_at` so a stale save (for example a
 *     logout racing a periodic save) can never overwrite newer gauges.
 *   - A DB error is not treated as "new character": the state stays
 *     Loaded=false and is neither ticked nor saved until a successful load.
 *
 * Released under the MIT license; see LICENSE.
 */

#include "HungerThirst.h"

#include "CellImpl.h"
#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Log.h"
#include "Map.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "UpdateFields.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace HungerThirst
{
    Config& GetConfig()
    {
        static Config config;
        return config;
    }

    namespace
    {
        constexpr uint32 MaxTickDiffMs      = 10000;
        constexpr uint32 MinIntervalMs      = 1000;
        constexpr double MinGaugeValue      = 0.00;
        constexpr double MaxGaugeValue      = 100.00;
        constexpr std::size_t MaxCachedAuras = 64;

        // Thirst blur: pulse the public client drunk field for a short while
        // every interval; no aura and no gameplay effect. The intensities stay
        // below the client's drunk threshold (50), where the field only tints
        // the screen (tipsy): at 50 and above the client switches to its drunk
        // stance and walk animation. The blur is visual only and was never the
        // cause of the "sliding walk" reported on a real client: the character
        // slid because of the old dehydrated state spell 14870 (now retired),
        // whose stock client data is a stun, so the client played the stunned
        // stand animation while the server let the character move (see the
        // spell list below).
        constexpr uint32 ThirstBlurIntervalDefaultMs = 10000;
        constexpr uint32 ThirstBlurDurationDefaultMs = 2000;
        constexpr uint32 MinBlurDurationMs           = 100;
        constexpr int32  ThirstBlurValueThirsty      = 35;
        constexpr int32  ThirstBlurValueDehydrated   = 45;

        // Cold exposure. The stock "Chilled" spell is applied to the player
        // and only the aura whose caster is the player is managed here, so a
        // chill cast by a creature is never removed. Its stock slow is used
        // as-is; the spell data is not patched.
        constexpr uint32 SpellColdId               = 6136; // "Chilled"
        constexpr uint32 ColdAuraDurationMs        = 15000;
        constexpr uint32 ColdFireCheckIntervalMs   = 5000;
        constexpr uint32 ColdDrainIntervalDefaultMs = 5000;
        constexpr uint32 ColdFireFocusId           = 4;    // SPELL_FOCUS cooking fire
        constexpr float  ColdFireRange             = 8.0f;
        constexpr double ColdAuraThreshold         = 75.00;
        constexpr double ColdDrainThreshold        = 100.00;

        // Blizzard spells that no content references any more, repurposed as
        // the hunger and thirst state debuffs. They are patched on the server
        // only; no client data is touched. A repurposed spell must be visible
        // in the aura bar of a stock 3.3.5a client: no SPELL_ATTR0_PASSIVE,
        // no SPELL_ATTR0_DO_NOT_DISPLAY and no SPELL_ATTR1_NO_AURA_ICON (the
        // legacy spells 43155 and 46583 each carry one of these, so real
        // clients showed no thirst icon for them).
        // The client also keeps rendering the stock effect of a repurposed
        // spell even though its server data is patched, so that effect must
        // not play an animation. 5909 carries Effect 6 + SPELL_AURA_MOD_STAT
        // (no animation, no stance); 14870, the retired spell used before it,
        // carries Effect 6 + SPELL_AURA_MOD_STUN, which froze the client into
        // the stunned stand animation while the server let the character
        // move: that mismatch is the "sliding walk" seen on a real client.
        constexpr uint32 SpellHungerWeakenedId = 52749; // "Voracious Appetite" / frFR "Appétit vorace"
        constexpr uint32 SpellHungerStarvingId = 55316; // "Ravenous Hunger" / frFR "Faim dévorante"
        constexpr uint32 SpellThirstyId        = 25696; // "Drink" / frFR "Boisson", icon INV_Drink_07
        constexpr uint32 SpellDehydratedId     = 5909;  // "Watered-down Beer", icon INV_Drink_04

        // Thirst spells used by earlier module versions. They are no longer
        // patched or applied; these constants exist only so the auras they
        // left on characters can be cleaned up (legacy aura cleanup). 14870
        // is retired because of its client-side stun (see above).
        constexpr uint32 SpellThirstyLegacyId     = 43155; // old "Drink", client-hidden
        constexpr uint32 SpellThirstyLegacyId2    = 46583; // "Cosmetic - Drink", SPELL_ATTR1_NO_AURA_ICON
        constexpr uint32 SpellDehydratedLegacyId  = 14869; // old "Drunken Stupor", passive
        constexpr uint32 SpellDehydratedLegacyId2 = 14870; // old "Drunken Stupor", client-side stun (sliding walk)

        constexpr uint32 InfiniteDurationIndex = 21;    // SpellDuration.dbc: Duration = -1

        // Reproduces the core ApplySpellFix pattern
        // (src/server/game/Spells/SpellInfoCorrections.cpp). Spell infos are
        // fully loaded when OnStartup runs, so patching them in place is the
        // last write of the loading sequence.
        bool PatchHungerStateSpell(uint32 spellId, double penaltyPct)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info)
            {
                LOG_ERROR("module.hunger-thirst", "Spell {} not found; hunger states disabled.", spellId);
                return false;
            }

            SpellInfo* spellInfo = const_cast<SpellInfo*>(info);
            int32 const pct = int32(std::lround(std::clamp(penaltyPct, 0.0, 90.0)));

            // Effect 0: movement speed -pct%.
            spellInfo->Effects[0].Effect        = SPELL_EFFECT_APPLY_AURA;
            spellInfo->Effects[0].ApplyAuraName = SPELL_AURA_MOD_DECREASE_SPEED;
            spellInfo->Effects[0].BasePoints    = -pct;
            spellInfo->Effects[0].MiscValue     = 0;
            // Effect 1: damage done -pct%, all schools.
            spellInfo->Effects[1].Effect        = SPELL_EFFECT_APPLY_AURA;
            spellInfo->Effects[1].ApplyAuraName = SPELL_AURA_MOD_DAMAGE_PERCENT_DONE;
            spellInfo->Effects[1].BasePoints    = -pct;
            spellInfo->Effects[1].MiscValue     = SPELL_SCHOOL_MASK_ALL;
            // Effect 2: damage taken +pct%, all schools ("global defense -pct%").
            spellInfo->Effects[2].Effect        = SPELL_EFFECT_APPLY_AURA;
            spellInfo->Effects[2].ApplyAuraName = SPELL_AURA_MOD_DAMAGE_PERCENT_TAKEN;
            spellInfo->Effects[2].BasePoints    = pct;
            spellInfo->Effects[2].MiscValue     = SPELL_SCHOOL_MASK_ALL;

            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            {
                SpellEffectInfo& effect = spellInfo->Effects[i];
                effect.DieSides            = 0;
                effect.RealPointsPerLevel  = 0.0f;
                effect.PointsPerComboPoint = 0.0f;
                effect.ValueMultiplier     = 1.0f;
                effect.DamageMultiplier    = 1.0f;
                effect.BonusMultiplier     = 0.0f;
                effect.Amplitude           = 0;
                effect.Mechanic            = MECHANIC_NONE; // freedom and immunities do not interact
                effect.TargetA             = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
                effect.TargetB             = SpellImplicitTargetInfo(0);
                effect.TriggerSpell        = 0;
                effect.ItemType            = 0;
                effect.SpellClassMask      = flag96();
            }

            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(InfiniteDurationIndex);
            spellInfo->AttributesCu  = SPELL_ATTR0_CU_NEGATIVE; // applied as a debuff
            spellInfo->Attributes   &= ~SPELL_ATTR0_PASSIVE;    // passive auras are not sent to the client

            // The repurposed spells may carry Blizzard interrupt flags; the
            // state aura must stay until the module removes it.
            spellInfo->InterruptFlags        = 0;
            spellInfo->AuraInterruptFlags    = 0;
            spellInfo->ChannelInterruptFlags = 0;

            LOG_INFO("module.hunger-thirst",
                "Hunger state spell {} patched (-{}% speed, -{}% damage done, +{}% damage taken).",
                spellId, pct, pct, pct);
            return true;
        }

        // Same server-side patch as the hunger states, for the thirst states:
        // effect 0 reduces spirit-based mana regeneration, effect 1 magical
        // damage done and effect 2 healing done, all by the same percentage.
        bool PatchThirstStateSpell(uint32 spellId, double penaltyPct)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!info)
            {
                LOG_ERROR("module.hunger-thirst", "Spell {} not found; thirst states disabled.", spellId);
                return false;
            }

            SpellInfo* spellInfo = const_cast<SpellInfo*>(info);
            int32 const pct = int32(std::lround(std::clamp(penaltyPct, 0.0, 90.0)));

            // Effect 0: mana regeneration -pct% (the spirit-based part only;
            // the flat mp5 regeneration is not affected by this aura).
            spellInfo->Effects[0].Effect        = SPELL_EFFECT_APPLY_AURA;
            spellInfo->Effects[0].ApplyAuraName = SPELL_AURA_MOD_POWER_REGEN_PERCENT;
            spellInfo->Effects[0].BasePoints    = -pct;
            spellInfo->Effects[0].MiscValue     = POWER_MANA;
            // Effect 1: magical damage done -pct%, physical and ranged excluded.
            spellInfo->Effects[1].Effect        = SPELL_EFFECT_APPLY_AURA;
            spellInfo->Effects[1].ApplyAuraName = SPELL_AURA_MOD_DAMAGE_PERCENT_DONE;
            spellInfo->Effects[1].BasePoints    = -pct;
            spellInfo->Effects[1].MiscValue     = SPELL_SCHOOL_MASK_MAGIC;
            // Effect 2: healing done -pct%.
            spellInfo->Effects[2].Effect        = SPELL_EFFECT_APPLY_AURA;
            spellInfo->Effects[2].ApplyAuraName = SPELL_AURA_MOD_HEALING_DONE_PERCENT;
            spellInfo->Effects[2].BasePoints    = -pct;
            spellInfo->Effects[2].MiscValue     = 0;

            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            {
                SpellEffectInfo& effect = spellInfo->Effects[i];
                effect.DieSides            = 0;
                effect.RealPointsPerLevel  = 0.0f;
                effect.PointsPerComboPoint = 0.0f;
                effect.ValueMultiplier     = 1.0f;
                effect.DamageMultiplier    = 1.0f;
                effect.BonusMultiplier     = 0.0f;
                effect.Amplitude           = 0;
                effect.Mechanic            = MECHANIC_NONE; // freedom and immunities do not interact
                effect.TargetA             = SpellImplicitTargetInfo(TARGET_UNIT_CASTER);
                effect.TargetB             = SpellImplicitTargetInfo(0);
                effect.TriggerSpell        = 0;
                effect.ItemType            = 0;
                effect.SpellClassMask      = flag96();
            }

            spellInfo->DurationEntry = sSpellDurationStore.LookupEntry(InfiniteDurationIndex);
            spellInfo->AttributesCu  = SPELL_ATTR0_CU_NEGATIVE; // applied as a debuff
            spellInfo->Attributes   &= ~SPELL_ATTR0_PASSIVE;    // passive auras are not sent to the client

            // Repurposed spells may carry Blizzard interrupt flags; the state
            // aura must stay until the module removes it.
            spellInfo->InterruptFlags        = 0;
            spellInfo->AuraInterruptFlags    = 0;
            spellInfo->ChannelInterruptFlags = 0;

            LOG_INFO("module.hunger-thirst",
                "Thirst state spell {} patched (-{}% mana regeneration, -{}% magic damage, -{}% healing).",
                spellId, pct, pct, pct);
            return true;
        }

        struct State
        {
            double Satiety         = MaxGaugeValue;
            double Hydration       = MaxGaugeValue;
            double Cold            = MinGaugeValue;
            bool   Loaded          = false;
            bool   Dirty           = false;
            uint64 UpdatedAtMs     = 0;
            Band   SatietyBand     = Band::Normal;
            Band   HydrationBand   = Band::Normal;
            Band   ColdBand        = Band::Normal;
            bool   HotActive       = false;
            uint32 ColdDrainTimerMs = 0;
            uint32 FireCheckTimerMs = 0;
            bool   AtFire          = false;
            bool   ColdDrainNotified = false;
            uint64 AddonTimerMs    = 0;
            uint64 BlurTimerMs     = 0;
            uint32 BlurRemainingMs = 0;
            bool   BlurActive      = false;
            int32  BlurPreviousValue = 0;

            // Anti-double-credit bookkeeping.
            std::map<uint32, uint64> ItemCredits; // item entry -> expiry epoch ms
            std::map<std::pair<uint32, uint64>, uint64> AuraCredits; // (spellId, caster guid) -> expiry epoch ms
        };

        std::unordered_map<uint32, State> g_states;
        std::mutex g_stateMutex;

        std::mutex g_schemaMutex;
        std::atomic<bool> g_persistenceReady{ false };

        uint64 NowMs()
        {
            return static_cast<uint64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        }

        double ClampGauge(double value)
        {
            if (!std::isfinite(value))
                value = MinGaugeValue;

            return std::clamp(value, MinGaugeValue, MaxGaugeValue);
        }

        double ClampRange(double value, double minValue, double maxValue, char const* key)
        {
            if (!std::isfinite(value))
            {
                LOG_ERROR("module.hunger-thirst", "HungerThirst.{} is not a finite number; using {}.", key, minValue);
                return minValue;
            }

            if (value < minValue)
            {
                LOG_ERROR("module.hunger-thirst", "HungerThirst.{} = {} is below {}; clamping.", key, value, minValue);
                return minValue;
            }

            if (value > maxValue)
            {
                LOG_ERROR("module.hunger-thirst", "HungerThirst.{} = {} is above {}; clamping.", key, value, maxValue);
                return maxValue;
            }

            return value;
        }

        uint32 ClampInterval(uint32 value, char const* key)
        {
            if (value < MinIntervalMs)
            {
                LOG_ERROR("module.hunger-thirst", "HungerThirst.{} = {} ms is below the minimum of {} ms; clamping.",
                    key, value, MinIntervalMs);
                return MinIntervalMs;
            }

            return value;
        }

        uint32 ClampSeconds(uint32 value, char const* key)
        {
            if (value < 30)
            {
                LOG_ERROR("module.hunger-thirst",
                    "HungerThirst.{} = {} s is below the minimum of 30 s; clamping.", key, value);
                return 30;
            }

            if (value > 86400)
            {
                LOG_ERROR("module.hunger-thirst",
                    "HungerThirst.{} = {} s is above the maximum of 86400 s; clamping.", key, value);
                return 86400;
            }

            return value;
        }

        // The blur duration has its own floor: it is not a module interval and
        // may legitimately be shorter than MinIntervalMs.
        uint32 ClampBlurDuration(uint32 value)
        {
            if (value < MinBlurDurationMs)
            {
                LOG_ERROR("module.hunger-thirst",
                    "HungerThirst.ThirstBlurDurationMs = {} ms is below the minimum of {} ms; clamping.",
                    value, MinBlurDurationMs);
                return MinBlurDurationMs;
            }

            return value;
        }

        Band RawBand(double value, Config const& cfg)
        {
            if (value <= MinGaugeValue)
                return Band::Empty;

            if (value <= double(cfg.CriticalThreshold))
                return Band::Critical;

            if (value <= double(cfg.WarningThreshold))
                return Band::Warning;

            return Band::Normal;
        }

        // Bands worsen immediately. They only improve once the gauge is
        // strictly above the current band's exit threshold plus Hysteresis.
        Band ResolveBand(double value, Band current, Config const& cfg)
        {
            Band const raw = RawBand(value, cfg);

            if (raw > current)
                return raw; // worse band: apply immediately

            if (raw == current)
                return current;

            // raw < current: improvement candidate. Stay in the current band
            // until the gauge is strictly above threshold + Hysteresis.
            double exitAt = 0.00;
            switch (current)
            {
                case Band::Empty:
                    exitAt = MinGaugeValue;
                    break;
                case Band::Critical:
                    exitAt = double(cfg.CriticalThreshold) + cfg.Hysteresis;
                    break;
                case Band::Warning:
                    exitAt = double(cfg.WarningThreshold) + cfg.Hysteresis;
                    break;
                case Band::Normal:
                    return raw;
            }

            // A full gauge always recovers, even with the maximum allowed
            // hysteresis.
            if (value >= MaxGaugeValue)
                return raw;

            return value > exitAt ? raw : current;
        }

        char const* BandMessage(bool hydration, Band band)
        {
            switch (band)
            {
                case Band::Warning:
                    return hydration
                        ? "You are starting to get thirsty."
                        : "You are starting to get hungry.";
                case Band::Critical:
                    return hydration
                        ? "You are very thirsty. Drink something soon."
                        : "You are very hungry. Eat something soon.";
                case Band::Empty:
                    return hydration
                        ? "You are dehydrated."
                        : "You are starving.";
                case Band::Normal:
                    break;
            }

            return nullptr;
        }

        // Cold uses fixed thresholds (25/50/75 %) and its own band labels.
        // Reusing Band keeps the addon protocol numeric and stable.
        Band RawColdBand(double value)
        {
            if (value >= 75.00)
                return Band::Empty;

            if (value >= 50.00)
                return Band::Critical;

            if (value >= 25.00)
                return Band::Warning;

            return Band::Normal;
        }

        // Cold worsens upward. The band only improves once the gauge drops
        // below the current band's exit threshold, minus Hysteresis.
        Band ResolveColdBand(double value, Band current, Config const& cfg)
        {
            Band const raw = RawColdBand(value);

            if (raw > current)
                return raw;

            if (raw == current)
                return current;

            double exitAt = 0.00;
            switch (current)
            {
                case Band::Empty:
                    exitAt = 75.00;
                    break;
                case Band::Critical:
                    exitAt = 50.00;
                    break;
                case Band::Warning:
                    exitAt = 25.00;
                    break;
                case Band::Normal:
                    return raw;
            }

            return value < (exitAt - cfg.Hysteresis) ? raw : current;
        }

        char const* ColdBandMessage(Band band)
        {
            switch (band)
            {
                case Band::Warning:
                    return "You are starting to feel cold. Seek shelter or a fire.";
                case Band::Critical:
                    return "You are cold. Seek shelter or a fire.";
                case Band::Empty:
                    return "The cold is slowing you down. Seek shelter or a cooking fire.";
                case Band::Normal:
                    break;
            }

            return nullptr;
        }

        bool IsInstanceMap(Player* player)
        {
            return player->GetMap() && player->GetMap()->Instanceable();
        }

        // Remove only the "Chilled" aura applied by the player. A creature's
        // chill has a different caster GUID and must be left untouched.
        void RemoveColdAura(Player* player)
        {
            if (!player)
                return;

            if (Aura* aura = player->GetAura(SpellColdId, player->GetGUID()))
                aura->Remove();
        }

        void ApplyColdAura(Player* player, bool desired)
        {
            if (!player)
                return;

            Aura* own = player->GetAura(SpellColdId, player->GetGUID());

            if (!desired)
            {
                if (own)
                    own->Remove();
                return;
            }

            if (own)
            {
                own->SetMaxDuration(ColdAuraDurationMs);
                own->SetDuration(ColdAuraDurationMs);
                return;
            }

            // A chill from another caster is already on the player; do not
            // stack a second copy or override its remaining duration.
            if (player->HasAura(SpellColdId))
                return;

            if (Aura* aura = player->AddAura(SpellColdId, player))
            {
                aura->SetMaxDuration(ColdAuraDurationMs);
                aura->SetDuration(ColdAuraDurationMs);
            }
        }

        // Applies one or more 5-second cold drain intervals at once, with a
        // 1 % max-health floor. The drain can never kill on its own.
        void ApplyColdHealthDrain(Player* player, uint32 intervals, double pct)
        {
            if (!player || !intervals || pct <= 0.00)
                return;

            uint32 const maxHealth = player->GetMaxHealth();
            if (!maxHealth)
                return;

            uint32 const floor = std::max<uint32>(1, (maxHealth + 99) / 100);
            uint32 const health = player->GetHealth();
            if (health <= floor)
                return;

            uint64 const perInterval = std::max<uint64>(1, uint64(maxHealth) * pct / 100.0);
            uint64 const total = perInterval * intervals;
            uint32 const drained = health > total ? uint32(health - total) : 0;
            uint32 const newHealth = std::max(floor, drained);

            if (newHealth < health)
                player->SetHealth(newHealth);
        }

        bool IsDrinking(Player* player)
        {
            for (auto const& entry : player->GetAppliedAuras())
            {
                Aura* aura = entry.second->GetBase();
                if (!aura || aura->GetCastItemGUID().IsEmpty())
                    continue;

                SpellSpecificType const specific = aura->GetSpellInfo()->GetSpellSpecific();
                if (specific == SPELL_SPECIFIC_DRINK || specific == SPELL_SPECIFIC_FOOD_AND_DRINK)
                    return true;
            }

            return false;
        }

        bool IsNearWarmFire(Player* player, Config const& cfg)
        {
            GameObject* fire = nullptr;

            auto isWarmFire = [player, &cfg](GameObject* object)
            {
                if (!object || !object->isSpawned() || !object->GetGOInfo())
                    return false;

                bool const cookingFire = object->GetGoType() == GAMEOBJECT_TYPE_SPELL_FOCUS
                    && object->GetGOInfo()->spellFocus.focusId == ColdFireFocusId;
                bool const listed = cfg.ColdFires.count(object->GetEntry()) != 0;

                return (cookingFire || listed)
                    && player->IsWithinDistInMap(object, ColdFireRange)
                    && player->IsWithinLOSInMap(object);
            };

            Acore::GameObjectLastSearcher<decltype(isWarmFire)> searcher(player, fire, isWarmFire);
            Cell::VisitObjects(player, searcher, ColdFireRange);
            return fire != nullptr;
        }

        std::unordered_set<uint32> ParseIdSet(std::string text)
        {
            std::replace(text.begin(), text.end(), ',', ' ');

            std::istringstream input(text);
            std::unordered_set<uint32> ids;
            uint32 value = 0;
            while (input >> value)
            {
                if (value)
                    ids.insert(value);
            }

            return ids;
        }

        void SendChatLine(Player* player, std::string_view message)
        {
            if (!player || message.empty() || !player->GetSession())
                return;

            ChatHandler(player->GetSession())
                .SendSysMessage(Acore::StringFormat("|cffffcc00[Hunger & Thirst]|r {}", message));
        }

        enum class HungerState : uint8
        {
            None     = 0,
            Weakened = 1,
            Starving = 2
        };

        // The existing bands provide the hysteresis for free: entering the
        // warning band applies the weakened state, an empty gauge the starving
        // one.
        HungerState HungerStateFromBand(Band band)
        {
            switch (band)
            {
                case Band::Warning:
                case Band::Critical:
                    return HungerState::Weakened;
                case Band::Empty:
                    return HungerState::Starving;
                case Band::Normal:
                    break;
            }

            return HungerState::None;
        }

        enum class ThirstState : uint8
        {
            None       = 0,
            Thirsty    = 1,
            Dehydrated = 2
        };

        // Warning and critical share the first state; dehydration only starts
        // with an empty gauge. Hysteresis is handled by the band resolution.
        ThirstState ThirstStateFromBand(Band band)
        {
            switch (band)
            {
                case Band::Warning:
                case Band::Critical:
                    return ThirstState::Thirsty;
                case Band::Empty:
                    return ThirstState::Dehydrated;
                case Band::Normal:
                    break;
            }

            return ThirstState::None;
        }

        // Restores the client drunk field and forgets any pending pulse. The
        // field is written outside the state mutex: SetInt32Value updates the
        // player and must never run under g_stateMutex.
        void ClearThirstBlur(Player* player)
        {
            if (!player)
                return;

            uint32 const guid = player->GetGUID().GetCounter();
            bool restore = false;
            int32 restoreValue = 0;

            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                auto const it = g_states.find(guid);
                if (it == g_states.end())
                    return;

                State& state = it->second;
                if (state.BlurActive)
                {
                    restore = true;
                    restoreValue = state.BlurPreviousValue;
                }

                state.BlurActive = false;
                state.BlurRemainingMs = 0;
                state.BlurTimerMs = 0;
            }

            if (restore)
                player->SetInt32Value(PLAYER_FAKE_INEBRIATION, restoreValue);
        }

        // Pulses PLAYER_FAKE_INEBRIATION for ThirstBlurDurationMs every
        // ThirstBlurIntervalMs while a thirst state is active. The field value
        // found on the first pulse is saved and restored when the pulse ends
        // or the state disappears, so other drunk sources are preserved.
        void UpdateThirstBlur(Player* player, ThirstState desired, Config const& cfg, uint32 diff)
        {
            if (!player)
                return;

            uint32 const guid = player->GetGUID().GetCounter();
            uint32 const interval = std::max<uint32>(1, cfg.ThirstBlurIntervalMs);
            uint32 const duration = std::min(interval, std::max<uint32>(1, cfg.ThirstBlurDurationMs));
            int32 const intensity = desired == ThirstState::Dehydrated
                ? ThirstBlurValueDehydrated
                : ThirstBlurValueThirsty;

            bool restore = false;
            bool startPulse = false;
            int32 restoreValue = 0;

            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                auto const it = g_states.find(guid);
                if (it == g_states.end() || !it->second.Loaded)
                    return;

                State& state = it->second;

                if (desired == ThirstState::None)
                {
                    if (state.BlurActive)
                    {
                        restore = true;
                        restoreValue = state.BlurPreviousValue;
                    }

                    state.BlurActive = false;
                    state.BlurRemainingMs = 0;
                    state.BlurTimerMs = 0;
                }
                else
                {
                    // A zero timer with no pulse pending marks a fresh entry
                    // (None resets the timer), so the first pulse is shown at
                    // once; later pulses follow the interval.
                    if (!state.BlurActive && state.BlurTimerMs == 0 && state.BlurRemainingMs == 0)
                        state.BlurTimerMs = interval;
                    else
                        state.BlurTimerMs += diff;

                    if (state.BlurActive)
                    {
                        if (state.BlurRemainingMs <= diff)
                        {
                            restore = true;
                            restoreValue = state.BlurPreviousValue;
                            state.BlurActive = false;
                            state.BlurRemainingMs = 0;
                        }
                        else
                            state.BlurRemainingMs -= diff;
                    }
                    else if (state.BlurTimerMs >= interval)
                    {
                        startPulse = true;
                        state.BlurTimerMs = 0;
                    }
                }
            }

            if (restore)
                player->SetInt32Value(PLAYER_FAKE_INEBRIATION, restoreValue);

            if (!startPulse)
                return;

            int32 const previous = player->GetInt32Value(PLAYER_FAKE_INEBRIATION);
            player->SetInt32Value(PLAYER_FAKE_INEBRIATION, std::max(previous, intensity));

            std::lock_guard<std::mutex> lock(g_stateMutex);
            auto const it = g_states.find(guid);
            if (it == g_states.end())
                return;

            State& state = it->second;
            state.BlurPreviousValue = previous;
            state.BlurActive = true;
            state.BlurRemainingMs = duration;
        }

        void RemoveHungerStates(Player* player)
        {
            if (player->HasAura(SpellHungerWeakenedId))
                player->RemoveAurasDueToSpell(SpellHungerWeakenedId);
            if (player->HasAura(SpellHungerStarvingId))
                player->RemoveAurasDueToSpell(SpellHungerStarvingId);
        }

        // Reconciles the auras with the desired state. Called from Tick only,
        // so login, death, resurrection, `.reload config` and the commands are
        // repaired on the next update (at most one tick later).
        void ApplyHungerState(Player* player, HungerState desired, Config const& cfg)
        {
            bool const weakened = player->HasAura(SpellHungerWeakenedId);
            bool const starving = player->HasAura(SpellHungerStarvingId);

            switch (desired)
            {
                case HungerState::None:
                    if (weakened)
                        player->RemoveAurasDueToSpell(SpellHungerWeakenedId);
                    if (starving)
                        player->RemoveAurasDueToSpell(SpellHungerStarvingId);
                    break;
                case HungerState::Weakened:
                    if (starving)
                        player->RemoveAurasDueToSpell(SpellHungerStarvingId);
                    if (!weakened)
                    {
                        player->AddAura(SpellHungerWeakenedId, player);
                        SendChatLine(player, Acore::StringFormat(
                            "Your hunger weakens you: movement speed and damage dealt reduced by {:.0f}%, "
                            "damage taken increased by {:.0f}%. Eat something.",
                            cfg.HungerStatePenaltyPct, cfg.HungerStatePenaltyPct));
                    }
                    break;
                case HungerState::Starving:
                    if (weakened)
                        player->RemoveAurasDueToSpell(SpellHungerWeakenedId);
                    if (!starving)
                    {
                        player->AddAura(SpellHungerStarvingId, player);
                        SendChatLine(player, Acore::StringFormat(
                            "You are starving: movement speed and damage dealt reduced by {:.0f}%, "
                            "damage taken increased by {:.0f}%. Eat something!",
                            cfg.HungerStarvingPenaltyPct, cfg.HungerStarvingPenaltyPct));
                    }
                    break;
            }
        }

        // Removes the thirst auras applied by earlier module versions. The
        // current state spells are 25696 ("Drink", thirsty) and 5909
        // ("Watered-down Beer", dehydrated); the legacy spells 43155, 46583,
        // 14869 and 14870 are never applied again (43155 and 46583 are hidden
        // by the client, 14869 is passive, 14870 is a client-side stun).
        // Called before the state is reconciled and when it is removed, so a
        // character that still carries a legacy aura is cleaned up on the
        // next active tick.
        void RemoveLegacyThirstAuras(Player* player)
        {
            if (player->HasAura(SpellThirstyLegacyId))
                player->RemoveAurasDueToSpell(SpellThirstyLegacyId);
            if (player->HasAura(SpellThirstyLegacyId2))
                player->RemoveAurasDueToSpell(SpellThirstyLegacyId2);
            if (player->HasAura(SpellDehydratedLegacyId))
                player->RemoveAurasDueToSpell(SpellDehydratedLegacyId);
            if (player->HasAura(SpellDehydratedLegacyId2))
                player->RemoveAurasDueToSpell(SpellDehydratedLegacyId2);
        }

        void RemoveThirstStates(Player* player)
        {
            if (!player)
                return;

            RemoveLegacyThirstAuras(player);

            if (player->HasAura(SpellThirstyId))
                player->RemoveAurasDueToSpell(SpellThirstyId);
            if (player->HasAura(SpellDehydratedId))
                player->RemoveAurasDueToSpell(SpellDehydratedId);

            ClearThirstBlur(player);
        }

        // Same reconciliation as the hunger states, with a single entry line
        // carrying the configured penalty. Called from Tick only.
        void ApplyThirstState(Player* player, ThirstState desired, Config const& cfg)
        {
            // Characters updated from an earlier module version may still
            // carry one of the legacy auras 43155/46583/14869/14870; drop it
            // before adding a current aura.
            RemoveLegacyThirstAuras(player);

            bool const thirsty = player->HasAura(SpellThirstyId);
            bool const dehydrated = player->HasAura(SpellDehydratedId);

            switch (desired)
            {
                case ThirstState::None:
                    if (thirsty)
                        player->RemoveAurasDueToSpell(SpellThirstyId);
                    if (dehydrated)
                        player->RemoveAurasDueToSpell(SpellDehydratedId);
                    break;
                case ThirstState::Thirsty:
                    if (dehydrated)
                        player->RemoveAurasDueToSpell(SpellDehydratedId);
                    if (!thirsty)
                    {
                        Aura* const applied = player->AddAura(SpellThirstyId, player);
                        if (!applied)
                            LOG_ERROR("module.hunger-thirst",
                                "Failed to apply thirst state aura {} to {}.", SpellThirstyId, player->GetName());
                        else
                            SendChatLine(player, Acore::StringFormat(
                                "Your thirst weakens you: mana regeneration and spell effectiveness "
                                "reduced by {:.0f}%. Drink something.",
                                cfg.ThirstStatePenaltyPct));
                    }
                    break;
                case ThirstState::Dehydrated:
                    if (thirsty)
                        player->RemoveAurasDueToSpell(SpellThirstyId);
                    if (!dehydrated)
                    {
                        Aura* const applied = player->AddAura(SpellDehydratedId, player);
                        if (!applied)
                            LOG_ERROR("module.hunger-thirst",
                                "Failed to apply thirst state aura {} to {}.", SpellDehydratedId, player->GetName());
                        else
                            SendChatLine(player, Acore::StringFormat(
                                "You are dehydrated: mana regeneration and spell effectiveness reduced "
                                "by {:.0f}%. Drink something!",
                                cfg.ThirstDehydratedPenaltyPct));
                    }
                    break;
            }
        }

        template <typename Key>
        void PruneExpiredMap(std::map<Key, uint64>& credits, uint64 now)
        {
            for (auto it = credits.begin(); it != credits.end();)
            {
                if (it->second <= now)
                    it = credits.erase(it);
                else
                    ++it;
            }
        }

        void PruneExpiredCredits(State& state, uint64 now)
        {
            PruneExpiredMap(state.ItemCredits, now);
            PruneExpiredMap(state.AuraCredits, now);
        }
    } // namespace

    // Public contract implementation (declared in HungerThirst.h).

    void LoadConfig(bool reload)
    {
        Config& cfg = GetConfig();

        cfg.Enable = sConfigMgr->GetOption<bool>("HungerThirst.Enable", true);

        std::string mode = sConfigMgr->GetOption<std::string>("HungerThirst.Mode", "off");
        std::transform(mode.begin(), mode.end(), mode.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (mode == "off")
            cfg.CurrentMode = Mode::Off;
        else if (mode == "advisory")
            cfg.CurrentMode = Mode::Advisory;
        else if (mode == "survival")
            cfg.CurrentMode = Mode::Survival;
        else
        {
            LOG_ERROR("module.hunger-thirst", "Unknown HungerThirst.Mode '{}'; falling back to off.", mode);
            cfg.CurrentMode = Mode::Off;
        }

        cfg.SatietyDecayPerMinute = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.SatietyDecayPerMinute", 0.50),
            0.00, 100.00, "SatietyDecayPerMinute");
        cfg.HydrationDecayPerMinute = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.HydrationDecayPerMinute", 0.75),
            0.00, 100.00, "HydrationDecayPerMinute");

        cfg.RestMultiplier = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.RestMultiplier", 0.50),
            0.00, 10.00, "RestMultiplier");
        cfg.InstanceMultiplier = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.InstanceMultiplier", 1.00),
            0.00, 10.00, "InstanceMultiplier");

        cfg.PauseOnTaxi = sConfigMgr->GetOption<bool>("HungerThirst.PauseOnTaxi", true);
        cfg.PauseInBattleground = sConfigMgr->GetOption<bool>("HungerThirst.PauseInBattleground", true);
        cfg.PauseInArena = sConfigMgr->GetOption<bool>("HungerThirst.PauseInArena", true);
        cfg.ExemptGMs = sConfigMgr->GetOption<bool>("HungerThirst.ExemptGMs", true);

        uint32 warningThreshold = sConfigMgr->GetOption<uint32>("HungerThirst.WarningThreshold", 30);
        if (warningThreshold > 100)
        {
            LOG_ERROR("module.hunger-thirst",
                "HungerThirst.WarningThreshold = {} is above 100; clamping.", warningThreshold);
            warningThreshold = 100;
        }

        uint32 criticalThreshold = sConfigMgr->GetOption<uint32>("HungerThirst.CriticalThreshold", 10);
        if (criticalThreshold >= warningThreshold)
        {
            uint32 const adjusted = warningThreshold > 0 ? warningThreshold - 1 : 0;
            LOG_ERROR("module.hunger-thirst",
                "HungerThirst.CriticalThreshold ({}) must be lower than HungerThirst.WarningThreshold ({}); using {}.",
                criticalThreshold, warningThreshold, adjusted);
            criticalThreshold = adjusted;
        }

        cfg.WarningThreshold = warningThreshold;
        cfg.CriticalThreshold = criticalThreshold;

        cfg.HungerStates = sConfigMgr->GetOption<bool>("HungerThirst.HungerStates", true);
        cfg.HungerStatePenaltyPct = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.HungerStatePenaltyPct", 10.00),
            0.00, 90.00, "HungerStatePenaltyPct");
        cfg.HungerStarvingPenaltyPct = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.HungerStarvingPenaltyPct", 30.00),
            0.00, 90.00, "HungerStarvingPenaltyPct");

        cfg.ThirstStates = sConfigMgr->GetOption<bool>("HungerThirst.ThirstStates", true);
        cfg.ThirstStatePenaltyPct = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.ThirstStatePenaltyPct", 10.00),
            0.00, 90.00, "ThirstStatePenaltyPct");
        cfg.ThirstDehydratedPenaltyPct = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.ThirstDehydratedPenaltyPct", 30.00),
            0.00, 90.00, "ThirstDehydratedPenaltyPct");
        cfg.ThirstBlurIntervalMs = ClampInterval(
            sConfigMgr->GetOption<uint32>("HungerThirst.ThirstBlurIntervalMs", ThirstBlurIntervalDefaultMs),
            "ThirstBlurIntervalMs");
        cfg.ThirstBlurDurationMs = ClampBlurDuration(
            sConfigMgr->GetOption<uint32>("HungerThirst.ThirstBlurDurationMs", ThirstBlurDurationDefaultMs));
        if (cfg.ThirstBlurDurationMs > cfg.ThirstBlurIntervalMs)
        {
            LOG_ERROR("module.hunger-thirst",
                "HungerThirst.ThirstBlurDurationMs ({}) cannot exceed HungerThirst.ThirstBlurIntervalMs ({}); "
                "using {} ms.",
                cfg.ThirstBlurDurationMs, cfg.ThirstBlurIntervalMs, cfg.ThirstBlurIntervalMs);
            cfg.ThirstBlurDurationMs = cfg.ThirstBlurIntervalMs;
        }

        cfg.FoodSatietyGain = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.FoodSatietyGain", 30.00),
            0.00, 100.00, "FoodSatietyGain");
        cfg.DrinkHydrationGain = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.DrinkHydrationGain", 35.00),
            0.00, 100.00, "DrinkHydrationGain");

        cfg.RestoreCooldownMs = ClampInterval(
            sConfigMgr->GetOption<uint32>("HungerThirst.RestoreCooldownMs", 1000), "RestoreCooldownMs");
        cfg.AuraCreditCooldownMs = ClampInterval(
            sConfigMgr->GetOption<uint32>("HungerThirst.AuraCreditCooldownMs", 30000), "AuraCreditCooldownMs");

        cfg.StartingSatiety = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.StartingSatiety", 100.00),
            0.00, 100.00, "StartingSatiety");
        cfg.StartingHydration = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.StartingHydration", 100.00),
            0.00, 100.00, "StartingHydration");
        cfg.StartingCold = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.StartingCold", 0.00),
            0.00, 100.00, "StartingCold");

        cfg.ExposureEnable = sConfigMgr->GetOption<bool>("HungerThirst.ExposureEnable", true);
        cfg.ColdZones = ParseIdSet(sConfigMgr->GetOption<std::string>(
            "HungerThirst.ColdZones", "1,65,67,210,618"));
        cfg.HotZones = ParseIdSet(sConfigMgr->GetOption<std::string>(
            "HungerThirst.HotZones", "3,46,51,440,3483"));
        cfg.ColdFires = ParseIdSet(sConfigMgr->GetOption<std::string>(
            "HungerThirst.ColdCampfireEntries", "2061"));
        cfg.ColdSeconds = ClampSeconds(
            sConfigMgr->GetOption<uint32>("HungerThirst.ColdSeconds", 600), "ColdSeconds");
        cfg.WarmSeconds = ClampSeconds(
            sConfigMgr->GetOption<uint32>("HungerThirst.WarmSeconds", 120), "WarmSeconds");
        cfg.ColdDrainIntervalMs = ClampInterval(
            sConfigMgr->GetOption<uint32>("HungerThirst.ColdDrainIntervalMs", ColdDrainIntervalDefaultMs),
            "ColdDrainIntervalMs");
        cfg.ColdHealthDrainPct = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.ColdHealthDrainPct", 1.00),
            0.00, 100.00, "ColdHealthDrainPct");
        cfg.HotHydrationMultiplier = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.HotHydrationMultiplier", 2.00),
            1.00, 10.00, "HotHydrationMultiplier");

        double const maxHysteresis = std::max(0.00, MaxGaugeValue - double(cfg.WarningThreshold));
        cfg.Hysteresis = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.Hysteresis", 2.00),
            0.00, maxHysteresis, "Hysteresis");

        cfg.ShowLoginSummary = sConfigMgr->GetOption<bool>("HungerThirst.ShowLoginSummary", true);
        cfg.DebugLog = sConfigMgr->GetOption<bool>("HungerThirst.DebugLog", false);

        cfg.AddonEnable = sConfigMgr->GetOption<bool>("HungerThirst.AddonEnable", true);
        cfg.AddonUpdateIntervalMs = ClampInterval(
            sConfigMgr->GetOption<uint32>("HungerThirst.AddonUpdateIntervalMs", 1000), "AddonUpdateIntervalMs");
        cfg.AddonPrefix = sConfigMgr->GetOption<std::string>("HungerThirst.AddonPrefix", "HT");
        if (cfg.AddonPrefix.empty())
        {
            LOG_ERROR("module.hunger-thirst", "HungerThirst.AddonPrefix is empty; using \"HT\".");
            cfg.AddonPrefix = "HT";
        }

        LOG_INFO("module.hunger-thirst",
            "Configuration {}: enable {}, mode {}, addon {}, exposure {} ({} cold zones, {} hot zones).",
            reload ? "reloaded" : "loaded", cfg.Enable, ModeName(cfg.CurrentMode), cfg.AddonEnable,
            cfg.ExposureEnable, cfg.ColdZones.size(), cfg.HotZones.size());
    }

    void EnsureSchema()
    {
        std::lock_guard<std::mutex> lock(g_schemaMutex);
        if (g_persistenceReady.load())
            return;

        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS `hunger_thirst_state` ("
            "`guid` INT UNSIGNED NOT NULL, "
            "`satiety` DECIMAL(5,2) NOT NULL DEFAULT 100.00, "
            "`hydration` DECIMAL(5,2) NOT NULL DEFAULT 100.00, "
            "`cold` DECIMAL(5,2) NOT NULL DEFAULT 0.00, "
            "`updated_at` BIGINT UNSIGNED NOT NULL DEFAULT 0, "
            "PRIMARY KEY (`guid`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");

        // Existing installations predate the cold column. CREATE TABLE IF NOT
        // EXISTS does not alter them, so add the column explicitly when it is
        // missing. information_schema is used instead of relying on a
        // vendor-specific ADD COLUMN IF NOT EXISTS.
        QueryResult column = CharacterDatabase.Query(
            "SELECT COUNT(*) FROM `information_schema`.`COLUMNS` "
            "WHERE `TABLE_SCHEMA` = DATABASE() "
            "AND `TABLE_NAME` = 'hunger_thirst_state' "
            "AND `COLUMN_NAME` = 'cold'");

        bool schemaOk = static_cast<bool>(column);
        if (schemaOk && column->Fetch()->Get<uint64>() == 0)
        {
            CharacterDatabase.DirectExecute(
                "ALTER TABLE `hunger_thirst_state` "
                "ADD COLUMN `cold` DECIMAL(5,2) NOT NULL DEFAULT 0.00 AFTER `hydration`");
        }

        // COUNT(cold) both proves the table exists and proves the column is
        // readable; an empty table still returns one row.
        bool ready = false;
        if (schemaOk)
        {
            ready = static_cast<bool>(CharacterDatabase.Query(
                "SELECT COUNT(`cold`) FROM `hunger_thirst_state`"));
        }

        g_persistenceReady.store(ready);

        if (ready)
            LOG_INFO("module.hunger-thirst", "Persistence ready (table `hunger_thirst_state`).");
        else
            LOG_ERROR("module.hunger-thirst",
                "Persistence unavailable: `hunger_thirst_state` could not be created or verified.");
    }

    bool IsPersistenceReady()
    {
        return g_persistenceReady.load();
    }

    bool LoadState(Player* player)
    {
        if (!player)
            return false;

        Config const& cfg = GetConfig();
        uint32 const guid = player->GetGUID().GetCounter();

        if (!cfg.Enable)
        {
            RemoveStateByGuid(guid);
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            auto const it = g_states.find(guid);
            if (it != g_states.end() && it->second.Loaded)
                return true;
        }

        if (!IsPersistenceReady())
            EnsureSchema();

        State state;
        state.Satiety = ClampGauge(cfg.StartingSatiety);
        state.Hydration = ClampGauge(cfg.StartingHydration);
        state.Cold = ClampGauge(cfg.StartingCold);
        state.SatietyBand = RawBand(state.Satiety, cfg);
        state.HydrationBand = RawBand(state.Hydration, cfg);
        state.ColdBand = RawColdBand(state.Cold);
        state.Loaded = false;
        state.Dirty = false;

        if (!IsPersistenceReady())
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_states[guid] = state;
            LOG_ERROR("module.hunger-thirst", "Cannot load state for {} ({}): persistence is unavailable.",
                player->GetName(), guid);
            return false;
        }

        QueryResult row = CharacterDatabase.Query(
            "SELECT `satiety`, `hydration`, `cold`, `updated_at` "
            "FROM `hunger_thirst_state` WHERE `guid` = {}", guid);

        if (row)
        {
            Field* fields = row->Fetch();
            state.Satiety = ClampGauge(fields[0].Get<double>());
            state.Hydration = ClampGauge(fields[1].Get<double>());
            state.Cold = ClampGauge(fields[2].Get<double>());
            state.UpdatedAtMs = fields[3].Get<uint64>();
            state.SatietyBand = RawBand(state.Satiety, cfg);
            state.HydrationBand = RawBand(state.Hydration, cfg);
            state.ColdBand = RawColdBand(state.Cold);
            state.Loaded = true;
            state.Dirty = false;
        }
        else
        {
            // Query() returns null both for "no row" and for a DB error;
            // COUNT(*) returns exactly one row on success, so it tells the
            // two cases apart.
            QueryResult countResult = CharacterDatabase.Query(
                "SELECT COUNT(*) FROM `hunger_thirst_state` WHERE `guid` = {}", guid);

            if (!countResult)
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                g_states[guid] = state;
                LOG_ERROR("module.hunger-thirst", "Database error while loading state for {} ({}).",
                    player->GetName(), guid);
                return false;
            }

            uint64 const rowCount = countResult->Fetch()->Get<uint64>();
            if (rowCount == 0)
            {
                state.Loaded = true;
                state.Dirty = true;
            }
            else
            {
                std::lock_guard<std::mutex> lock(g_stateMutex);
                g_states[guid] = state;
                LOG_ERROR("module.hunger-thirst", "State row exists but could not be read for {} ({}).",
                    player->GetName(), guid);
                return false;
            }
        }

        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            g_states[guid] = state;
        }

        if (cfg.DebugLog)
        {
            LOG_DEBUG("module.hunger-thirst",
                "Loaded state for {} ({}): satiety {:.2f}, hydration {:.2f}, cold {:.2f}, updated_at {}.",
                player->GetName(), guid, state.Satiety, state.Hydration, state.Cold, state.UpdatedAtMs);
        }

        return true;
    }

    void SaveState(Player* player, bool force)
    {
        if (!player)
            return;

        Config const& cfg = GetConfig();
        if (!cfg.Enable || !IsPersistenceReady())
            return;

        uint32 const guid = player->GetGUID().GetCounter();
        double satiety = 0.00;
        double hydration = 0.00;
        double cold = 0.00;
        uint64 updatedAt = 0;

        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            auto const it = g_states.find(guid);
            if (it == g_states.end() || !it->second.Loaded)
                return;

            State& state = it->second;
            if (!state.Dirty && !force)
                return;

            satiety = state.Satiety;
            hydration = state.Hydration;
            cold = state.Cold;
            updatedAt = std::max<uint64>(NowMs(), state.UpdatedAtMs + 1);
            state.UpdatedAtMs = updatedAt;
            state.Dirty = false;
        }

        // Conditional upsert: a stale writer (older timestamp) is ignored, so
        // concurrent saves cannot roll the gauges back. Forced saves (logout,
        // GM commands) run synchronously so an immediate reconnect cannot
        // read the stale row; periodic saves stay asynchronous.
        char const* const query =
            "INSERT INTO `hunger_thirst_state` (`guid`, `satiety`, `hydration`, `cold`, `updated_at`) "
            "VALUES ({}, {}, {}, {}, {}) "
            "ON DUPLICATE KEY UPDATE "
            "`satiety` = IF(VALUES(`updated_at`) >= `updated_at`, VALUES(`satiety`), `satiety`), "
            "`hydration` = IF(VALUES(`updated_at`) >= `updated_at`, VALUES(`hydration`), `hydration`), "
            "`cold` = IF(VALUES(`updated_at`) >= `updated_at`, VALUES(`cold`), `cold`), "
            "`updated_at` = GREATEST(`updated_at`, VALUES(`updated_at`))";

        if (force)
            CharacterDatabase.DirectExecute(query, guid, satiety, hydration, cold, updatedAt);
        else
            CharacterDatabase.Execute(query, guid, satiety, hydration, cold, updatedAt);

        if (cfg.DebugLog)
        {
            LOG_DEBUG("module.hunger-thirst",
                "Saved state for guid {}: satiety {:.2f}, hydration {:.2f}, cold {:.2f}, updated_at {}.",
                guid, satiety, hydration, cold, updatedAt);
        }
    }

    void RemoveState(Player* player)
    {
        if (!player)
            return;

        // The client field is not persisted: restore it before the state row
        // is dropped so a reconnect starts clean. The cold aura is ours, not a
        // creature's chill, so remove only the copy cast by the player.
        RemoveColdAura(player);
        ClearThirstBlur(player);
        RemoveStateByGuid(player->GetGUID().GetCounter());
    }

    void RemoveStateByGuid(uint32 guid)
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_states.erase(guid);
    }

    void Tick(Player* player, uint32 diff)
    {
        Config const& cfg = GetConfig();
        if (!player)
            return;

        // Outside survival, or with the states disabled, the module keeps
        // decaying and warning as before; only the state auras are removed.
        if (!cfg.Enable || cfg.CurrentMode != Mode::Survival || !cfg.HungerStates)
            RemoveHungerStates(player);

        if (!cfg.Enable || cfg.CurrentMode != Mode::Survival || !cfg.ThirstStates)
            RemoveThirstStates(player);

        // Cold is visible only in survival. It still warms/cools in advisory
        // (so the gauge and warnings work), but no aura is applied there.
        if (!cfg.Enable || cfg.CurrentMode != Mode::Survival || !cfg.ExposureEnable)
            RemoveColdAura(player);

        if (!cfg.Enable || cfg.CurrentMode == Mode::Off)
            return;

        if (!player->IsInWorld() || !player->IsAlive())
        {
            RemoveColdAura(player);
            ClearThirstBlur(player);
            return;
        }

        bool const inBattleground = player->GetBattlegroundId() != 0;
        bool const inArena = player->InArena();

        if (cfg.ExemptGMs && player->GetSession() && player->GetSession()->IsGMAccount())
        {
            ClearThirstBlur(player);
            return;
        }

        if (cfg.PauseOnTaxi && player->IsInFlight())
        {
            ClearThirstBlur(player);
            return;
        }

        if (cfg.PauseInBattleground && inBattleground)
        {
            ClearThirstBlur(player);
            return;
        }

        if (cfg.PauseInArena && inArena)
        {
            ClearThirstBlur(player);
            return;
        }

        if (player->duel)
        {
            ClearThirstBlur(player);
            return;
        }

        uint32 const effectiveDiff = std::min<uint32>(diff, MaxTickDiffMs);
        if (!effectiveDiff)
            return;

        bool const inDungeon = player->GetMap() && player->GetMap()->IsDungeon();
        bool const inInstance = IsInstanceMap(player);
        bool const resting = player->HasRestFlag(REST_FLAG_IN_TAVERN) ||
                             player->HasRestFlag(REST_FLAG_IN_CITY) ||
                             player->HasRestFlag(REST_FLAG_IN_FACTION_AREA);
        bool const sheltered = !player->IsOutdoors() || resting;
        bool const wet = player->IsInWater();

        double multiplier = 1.00;
        if (resting)
            multiplier *= cfg.RestMultiplier;
        if (inDungeon)
            multiplier *= cfg.InstanceMultiplier;

        uint32 const guid = player->GetGUID().GetCounter();
        uint32 const zoneId = player->GetZoneId();
        bool const coldZone = cfg.ExposureEnable && !inInstance && cfg.ColdZones.count(zoneId) != 0;
        bool const hotZone = cfg.ExposureEnable && !inInstance && cfg.HotZones.count(zoneId) != 0;
        bool const drinking = hotZone && !sheltered && IsDrinking(player);

        // The campfire probe is a grid visit; throttle it instead of walking
        // the grid on every player update. AtFire is cached in the state and
        // refreshed every ColdFireCheckIntervalMs while outdoors in a cold
        // zone.
        bool needFireCheck = false;
        bool atFire = false;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            auto const it = g_states.find(guid);
            if (it == g_states.end() || !it->second.Loaded)
                return;

            State& state = it->second;
            if (coldZone && !sheltered)
            {
                state.FireCheckTimerMs += effectiveDiff;
                if (state.FireCheckTimerMs >= ColdFireCheckIntervalMs)
                {
                    state.FireCheckTimerMs %= ColdFireCheckIntervalMs;
                    needFireCheck = true;
                }
                atFire = state.AtFire;
            }
            else
            {
                state.FireCheckTimerMs = 0;
                state.AtFire = false;
            }
        }

        if (needFireCheck && coldZone && !sheltered)
        {
            atFire = IsNearWarmFire(player, cfg);

            std::lock_guard<std::mutex> lock(g_stateMutex);
            auto const it = g_states.find(guid);
            if (it != g_states.end())
                it->second.AtFire = atFire;
        }

        bool const chilling = coldZone && !sheltered && !atFire;
        bool const hotEffect = hotZone && !sheltered && !drinking;

        char const* satietyMessage = nullptr;
        char const* hydrationMessage = nullptr;
        char const* coldMessage = nullptr;
        bool coldDrainWarning = false;
        bool drainDue = false;
        uint32 drainIntervals = 0;
        bool applyColdAuraDesired = false;
        HungerState desiredHungerState = HungerState::None;
        ThirstState desiredThirstState = ThirstState::None;
        bool sendAddon = false;

        {
            std::lock_guard<std::mutex> lock(g_stateMutex);

            auto const it = g_states.find(guid);
            if (it == g_states.end() || !it->second.Loaded)
                return;

            State& state = it->second;

            double const minutes = double(effectiveDiff) / 60000.0;
            double const satietyLoss = std::max(0.00, cfg.SatietyDecayPerMinute * minutes * multiplier);
            double hydrationLoss = std::max(0.00, cfg.HydrationDecayPerMinute * minutes * multiplier);
            if (hotEffect)
                hydrationLoss *= cfg.HotHydrationMultiplier;

            double const oldSatiety = state.Satiety;
            double const oldHydration = state.Hydration;
            double const oldCold = state.Cold;
            Band const oldSatietyBand = state.SatietyBand;
            Band const oldHydrationBand = state.HydrationBand;
            Band const oldColdBand = state.ColdBand;

            state.Satiety = ClampGauge(state.Satiety - satietyLoss);
            state.Hydration = ClampGauge(state.Hydration - hydrationLoss);

            if (cfg.ExposureEnable)
            {
                double const seconds = double(effectiveDiff) / 1000.0;
                if (chilling)
                {
                    double gain = MaxGaugeValue * seconds / double(cfg.ColdSeconds);
                    if (wet)
                        gain *= 2.00;
                    state.Cold = ClampGauge(state.Cold + gain);
                }
                else
                    state.Cold = ClampGauge(state.Cold - MaxGaugeValue * seconds / double(cfg.WarmSeconds));

                state.ColdBand = ResolveColdBand(state.Cold, state.ColdBand, cfg);
            }

            state.HotActive = hotEffect;

            if (state.Satiety != oldSatiety || state.Hydration != oldHydration || state.Cold != oldCold)
                state.Dirty = true;

            state.SatietyBand = ResolveBand(state.Satiety, state.SatietyBand, cfg);
            state.HydrationBand = ResolveBand(state.Hydration, state.HydrationBand, cfg);

            desiredHungerState = HungerStateFromBand(state.SatietyBand);
            desiredThirstState = ThirstStateFromBand(state.HydrationBand);

            // In survival the state messages replace the band warnings (no
            // contradictory duplicate); advisory keeps them unchanged.
            if (state.SatietyBand > oldSatietyBand && cfg.CurrentMode == Mode::Advisory)
                satietyMessage = BandMessage(false, state.SatietyBand);
            if (state.HydrationBand > oldHydrationBand && cfg.CurrentMode == Mode::Advisory)
                hydrationMessage = BandMessage(true, state.HydrationBand);

            // Cold warnings are independent of the hunger/thirst state
            // messages and are sent in both advisory and survival.
            if (state.ColdBand > oldColdBand)
                coldMessage = ColdBandMessage(state.ColdBand);

            if (cfg.CurrentMode == Mode::Survival && cfg.ExposureEnable && !inInstance)
            {
                applyColdAuraDesired = state.Cold >= ColdAuraThreshold;

                if (state.Cold >= ColdDrainThreshold && chilling && cfg.ColdHealthDrainPct > 0.00)
                {
                    state.ColdDrainTimerMs += effectiveDiff;
                    drainIntervals = state.ColdDrainTimerMs / cfg.ColdDrainIntervalMs;
                    if (drainIntervals > 0)
                    {
                        state.ColdDrainTimerMs %= cfg.ColdDrainIntervalMs;
                        drainDue = true;
                        if (!state.ColdDrainNotified)
                        {
                            state.ColdDrainNotified = true;
                            coldDrainWarning = true;
                        }
                    }
                }
                else
                {
                    state.ColdDrainTimerMs = 0;
                    if (state.Cold < ColdDrainThreshold)
                        state.ColdDrainNotified = false;
                }
            }
            else
            {
                state.ColdDrainTimerMs = 0;
                state.ColdDrainNotified = false;
            }

            if (cfg.AddonEnable && cfg.AddonUpdateIntervalMs > 0)
            {
                state.AddonTimerMs += effectiveDiff;
                if (state.AddonTimerMs >= cfg.AddonUpdateIntervalMs)
                {
                    state.AddonTimerMs %= cfg.AddonUpdateIntervalMs;
                    sendAddon = true;
                }
            }
            else
                state.AddonTimerMs = 0;
        }

        if (satietyMessage)
            SendChatLine(player, satietyMessage);

        if (hydrationMessage)
            SendChatLine(player, hydrationMessage);

        if (coldMessage)
            SendChatLine(player, coldMessage);

        if (coldDrainWarning)
            SendChatLine(player,
                "You are freezing: the cold is draining your health. "
                "Seek shelter or a fire immediately.");

        // Only reconcile while the states are actually active: otherwise the
        // aura would be re-applied right after the entry guard removed it.
        if (cfg.CurrentMode == Mode::Survival && cfg.HungerStates)
            ApplyHungerState(player, desiredHungerState, cfg);

        // Thirst no longer deals damage in phase 2: only the state auras and
        // the periodic blur remain.
        if (cfg.CurrentMode == Mode::Survival && cfg.ThirstStates)
        {
            ApplyThirstState(player, desiredThirstState, cfg);
            UpdateThirstBlur(player, desiredThirstState, cfg, effectiveDiff);
        }

        if (cfg.CurrentMode == Mode::Survival && cfg.ExposureEnable && !inInstance)
            ApplyColdAura(player, applyColdAuraDesired);
        else if (!cfg.ExposureEnable || inInstance)
            RemoveColdAura(player);

        if (drainDue && cfg.CurrentMode == Mode::Survival && cfg.ExposureEnable)
            ApplyColdHealthDrain(player, drainIntervals, cfg.ColdHealthDrainPct);

        if (sendAddon)
            SendAddonState(player);
    }

    void CreditConsumable(Player* player, Aura* aura)
    {
        if (!player || !aura)
            return;

        // Auras restored from the database are re-applied while the session is
        // still loading; they are not a new consumption.
        if (!player->GetSession() || player->GetSession()->PlayerLoading())
            return;

        Config const& cfg = GetConfig();
        if (!cfg.Enable || cfg.CurrentMode == Mode::Off)
            return;

        SpellInfo const* spellInfo = aura->GetSpellInfo();
        if (!spellInfo || !spellInfo->IsPositive())
            return;

        SpellSpecificType const specific = spellInfo->GetSpellSpecific();
        bool const food = specific == SPELL_SPECIFIC_FOOD || specific == SPELL_SPECIFIC_FOOD_AND_DRINK;
        bool const drink = specific == SPELL_SPECIFIC_DRINK || specific == SPELL_SPECIFIC_FOOD_AND_DRINK;
        if (!food && !drink)
            return;

        uint32 const guid = player->GetGUID().GetCounter();
        uint32 const spellId = spellInfo->Id;
        uint32 const itemEntry = aura->GetCastItemEntry();
        ObjectGuid const casterGuid = aura->GetCasterGUID();
        uint64 const now = NowMs();
        double creditedSatiety = 0.00;
        double creditedHydration = 0.00;

        {
            std::lock_guard<std::mutex> lock(g_stateMutex);

            auto const it = g_states.find(guid);
            if (it == g_states.end() || !it->second.Loaded)
                return;

            State& state = it->second;

            if (itemEntry != 0)
            {
                // Item path: refresh callbacks fire several times for the same
                // bite, so the same cast item entry is credited once per
                // RestoreCooldownMs.
                auto const itemCredit = state.ItemCredits.find(itemEntry);
                if (itemCredit != state.ItemCredits.end() && now < itemCredit->second)
                    return;

                state.ItemCredits[itemEntry] = now + cfg.RestoreCooldownMs;
                if (state.ItemCredits.size() > MaxCachedAuras)
                    PruneExpiredCredits(state, now);
            }
            else
            {
                // No cast item (feast, gameobject, player-cast aura): one
                // credit per (spellId, casterGuid) for at least the aura's
                // full/remaining duration or AuraCreditCooldownMs.
                std::pair<uint32, uint64> const key(spellId, casterGuid.GetRawValue());
                uint64 const auraDurationMs = aura->GetDuration() > 0 ? uint64(aura->GetDuration()) : 0;
                uint64 const cooldown = std::max<uint64>(cfg.AuraCreditCooldownMs, auraDurationMs);

                auto const credit = state.AuraCredits.find(key);
                if (credit != state.AuraCredits.end() && now < credit->second)
                    return;

                state.AuraCredits[key] = now + cooldown;
                if (state.AuraCredits.size() > MaxCachedAuras)
                    PruneExpiredCredits(state, now);
            }

            if (food)
                state.Satiety = ClampGauge(state.Satiety + cfg.FoodSatietyGain);
            if (drink)
                state.Hydration = ClampGauge(state.Hydration + cfg.DrinkHydrationGain);

            state.SatietyBand = ResolveBand(state.Satiety, state.SatietyBand, cfg);
            state.HydrationBand = ResolveBand(state.Hydration, state.HydrationBand, cfg);
            state.Dirty = true;

            creditedSatiety = state.Satiety;
            creditedHydration = state.Hydration;
        }

        if (cfg.DebugLog)
        {
            LOG_DEBUG("module.hunger-thirst",
                "Credited {} (spell {}, item {}, caster {}) for {}: satiety {:.2f}, hydration {:.2f}.",
                food && drink ? "food and drink" : food ? "food" : "drink",
                spellId, itemEntry, casterGuid.ToString(), player->GetName(),
                creditedSatiety, creditedHydration);
        }

        // Refresh the client immediately. Tick is the periodic sender, but it
        // is deliberately skipped for exempt GMs and paused players; without
        // this push, eating would update the server state but leave the bars
        // frozen until the next command or login.
        SendAddonState(player);
    }

    bool GetSnapshot(Player* player, Snapshot& out)
    {
        out = Snapshot();

        if (!player)
            return false;

        Config const& cfg = GetConfig();
        uint32 const guid = player->GetGUID().GetCounter();

        std::lock_guard<std::mutex> lock(g_stateMutex);

        auto const it = g_states.find(guid);
        if (it == g_states.end())
        {
            out.CurrentMode = cfg.CurrentMode;
            out.PersistenceReady = IsPersistenceReady();
            return false;
        }

        State const& state = it->second;
        out.Satiety = state.Satiety;
        out.Hydration = state.Hydration;
        out.Cold = state.Cold;
        out.CurrentMode = cfg.CurrentMode;
        out.Loaded = state.Loaded;
        out.PersistenceReady = IsPersistenceReady();
        out.Hot = state.HotActive;
        out.SatietyBand = state.SatietyBand;
        out.HydrationBand = state.HydrationBand;
        out.ColdBand = state.ColdBand;
        return true;
    }

    void SetState(Player* player, double satiety, double hydration, double cold)
    {
        if (!player)
            return;

        Config const& cfg = GetConfig();
        if (!cfg.Enable)
            return;

        uint32 const guid = player->GetGUID().GetCounter();
        double const newSatiety = ClampGauge(satiety);
        double const newHydration = ClampGauge(hydration);
        double const newCold = ClampGauge(cold);
        Band satietyBand = Band::Normal;
        Band hydrationBand = Band::Normal;
        Band coldBand = Band::Normal;

        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            State& state = g_states[guid];
            state.Satiety = newSatiety;
            state.Hydration = newHydration;
            state.Cold = newCold;
            state.Loaded = true;
            state.Dirty = true;
            state.SatietyBand = RawBand(newSatiety, cfg);
            state.HydrationBand = RawBand(newHydration, cfg);
            state.ColdBand = RawColdBand(newCold);
            state.ColdDrainTimerMs = 0;
            state.FireCheckTimerMs = 0;
            state.AtFire = false;
            state.ColdDrainNotified = false;
            state.HotActive = false;
            satietyBand = state.SatietyBand;
            hydrationBand = state.HydrationBand;
            coldBand = state.ColdBand;
        }

        // A GM jump can land directly inside a warning band without Tick
        // observing the crossing, so emit the corresponding warning once.
        // Hunger/thirst band warnings are advisory-only: in survival the
        // state message of the next Tick informs the player. Cold has no
        // state entry message, so its warning is sent in both modes.
        if (cfg.CurrentMode == Mode::Advisory)
        {
            if (satietyBand >= Band::Warning)
            {
                if (char const* message = BandMessage(false, satietyBand))
                    SendChatLine(player, message);
            }

            if (hydrationBand >= Band::Warning)
            {
                if (char const* message = BandMessage(true, hydrationBand))
                    SendChatLine(player, message);
            }
        }

        if (cfg.ExposureEnable && cfg.CurrentMode != Mode::Off && coldBand >= Band::Warning)
        {
            if (char const* message = ColdBandMessage(coldBand))
                SendChatLine(player, message);
        }

        // Drop any running pulse: the next Tick restarts from the new state.
        ClearThirstBlur(player);
    }

    void SetState(Player* player, double satiety, double hydration)
    {
        if (!player)
            return;

        Config const& cfg = GetConfig();
        double cold = cfg.StartingCold;

        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            auto const it = g_states.find(player->GetGUID().GetCounter());
            if (it != g_states.end())
                cold = it->second.Cold;
        }

        SetState(player, satiety, hydration, cold);
    }

    void ResetState(Player* player)
    {
        if (!player)
            return;

        Config const& cfg = GetConfig();
        if (!cfg.Enable)
            return;

        uint32 const guid = player->GetGUID().GetCounter();

        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            State& state = g_states[guid];
            state.Satiety = ClampGauge(cfg.StartingSatiety);
            state.Hydration = ClampGauge(cfg.StartingHydration);
            state.Cold = ClampGauge(cfg.StartingCold);
            state.Loaded = true;
            state.Dirty = true;
            state.AddonTimerMs = 0;
            state.SatietyBand = RawBand(state.Satiety, cfg);
            state.HydrationBand = RawBand(state.Hydration, cfg);
            state.ColdBand = RawColdBand(state.Cold);
            state.ColdDrainTimerMs = 0;
            state.FireCheckTimerMs = 0;
            state.AtFire = false;
            state.ColdDrainNotified = false;
            state.HotActive = false;
        }

        // Reset restores the gauges only; any running blur is rolled back so
        // the next Tick starts from the new state. The cold aura is removed
        // immediately; the next Tick re-applies it if the gauge calls for it.
        RemoveColdAura(player);
        ClearThirstBlur(player);
    }

    void SendAddonState(Player* player)
    {
        if (!player || !player->GetSession())
            return;

        Config const& cfg = GetConfig();
        if (!cfg.AddonEnable)
            return;

        if (!cfg.Enable)
        {
            // Master switch off: send a synthetic disabled state so the client
            // leaves "Loading..." (loaded=1, mode=0, persistence=0).
            std::string const payload = Acore::StringFormat(
                "{}\t0.0\t0.0\t0\t0\t1\t0\t0\t0\t0.0\t0\t0", cfg.AddonPrefix);

            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, payload);
            player->SendDirectMessage(&data);
            return;
        }

        Snapshot snapshot;
        if (!GetSnapshot(player, snapshot))
            return;

        // An unloaded state (persistence down) is still advertised so the
        // client leaves "Loading..." and shows the placeholder gauges;
        // loaded and persistence both report 0 in that case. The lethal field
        // is kept for addon compatibility but is always 0 now.
        // The first eight payload fields keep their historical positions and
        // meanings. cold, coldBand and hot are appended so an older addon
        // (which ignores extra fields) keeps working.
        std::string const payload = Acore::StringFormat(
            "{}\t{:.1f}\t{:.1f}\t{}\t{}\t{}\t{}\t{}\t{}\t{:.1f}\t{}\t{}",
            cfg.AddonPrefix,
            snapshot.Satiety,
            snapshot.Hydration,
            uint32(snapshot.CurrentMode),
            0,
            snapshot.Loaded ? 1 : 0,
            (snapshot.Loaded && snapshot.PersistenceReady) ? 1 : 0,
            uint32(snapshot.SatietyBand),
            uint32(snapshot.HydrationBand),
            snapshot.Cold,
            uint32(snapshot.ColdBand),
            snapshot.Hot ? 1 : 0);

        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player, player, payload);
        player->SendDirectMessage(&data);
    }

    void SendLoginSummary(Player* player)
    {
        if (!player || !player->GetSession())
            return;

        Config const& cfg = GetConfig();
        if (!cfg.Enable || cfg.CurrentMode == Mode::Off || !cfg.ShowLoginSummary)
            return;

        Snapshot snapshot;
        if (!GetSnapshot(player, snapshot))
            return;

        if (!snapshot.Loaded)
        {
            // Persistence was unavailable when the state was loaded: the
            // gauges stay frozen until the database is back and the player
            // logs in again.
            SendChatLine(player,
                "Persistence unavailable: hunger and thirst are suspended "
                "until the database is restored and you log in again.");
            return;
        }

        ChatHandler handler(player->GetSession());
        handler.PSendSysMessage(
            "|cffffcc00[Hunger & Thirst]|r Mode: {} | Satiety: {:.1f}/100 ({}) | Hydration: {:.1f}/100 ({}).",
            ModeName(snapshot.CurrentMode),
            snapshot.Satiety, BandName(snapshot.SatietyBand, false),
            snapshot.Hydration, BandName(snapshot.HydrationBand, true));
        handler.PSendSysMessage(
            "|cffffcc00[Hunger & Thirst]|r Cold: {:.1f}/100 ({}).{}",
            snapshot.Cold, ColdBandName(snapshot.ColdBand),
            snapshot.Hot ? " Hot zone: hydration drains faster." : "");

        // Cold has no state message, so restate its warning at login: the
        // player may reconnect already cold and otherwise get no guidance
        // until the band changes.
        if (cfg.ExposureEnable && snapshot.ColdBand >= Band::Warning)
        {
            if (char const* message = ColdBandMessage(snapshot.ColdBand))
                SendChatLine(player, message);
        }

        // In survival the state messages of the next Tick inform the player.
        if (cfg.CurrentMode == Mode::Advisory)
        {
            if (snapshot.SatietyBand >= Band::Critical)
            {
                if (char const* message = BandMessage(false, snapshot.SatietyBand))
                    SendChatLine(player, message);
            }

            if (snapshot.HydrationBand >= Band::Critical)
            {
                if (char const* message = BandMessage(true, snapshot.HydrationBand))
                    SendChatLine(player, message);
            }
        }

        if (!snapshot.PersistenceReady)
        {
            handler.SendSysMessage(
                "|cffff0000[Hunger & Thirst]|r Persistence unavailable: progress will not be saved.");
        }
    }

    void SendStatus(ChatHandler* handler, Player* player)
    {
        if (!handler)
            return;

        Config const& cfg = GetConfig();
        if (!cfg.Enable)
        {
            handler->SendSysMessage("Hunger & Thirst: disabled by configuration.");
            return;
        }

        if (!player)
        {
            handler->SendSysMessage("Hunger & Thirst: no player selected.");
            return;
        }

        Snapshot snapshot;
        if (!GetSnapshot(player, snapshot))
        {
            handler->PSendSysMessage("Hunger & Thirst: no state loaded for {}.", player->GetName());
            return;
        }

        if (!snapshot.Loaded)
        {
            handler->PSendSysMessage(
                "Hunger & Thirst: the state of {} is not loaded (persistence {}).",
                player->GetName(), snapshot.PersistenceReady ? "ready" : "unavailable");
            return;
        }

        handler->PSendSysMessage(
            "Hunger & Thirst: mode {}, state {}, persistence {}.",
            ModeName(snapshot.CurrentMode),
            snapshot.Loaded ? "loaded" : "not loaded",
            snapshot.PersistenceReady ? "ready" : "unavailable");

        handler->PSendSysMessage(
            "Satiety {:.1f}/100 ({}) | Hydration {:.1f}/100 ({}).",
            snapshot.Satiety, BandName(snapshot.SatietyBand, false),
            snapshot.Hydration, BandName(snapshot.HydrationBand, true));

        handler->PSendSysMessage(
            "Cold {:.1f}/100 ({}).{}",
            snapshot.Cold, ColdBandName(snapshot.ColdBand),
            snapshot.Hot ? " Hot zone: hydration drains faster." : "");

        if (cfg.CurrentMode == Mode::Off)
            handler->SendSysMessage("Mode off: gauges are not simulated.");
        else if (cfg.CurrentMode == Mode::Advisory)
            handler->SendSysMessage("Advisory: warnings only, no states, no damage.");
        else
            handler->SendSysMessage(
                "Survival: hunger/thirst states and cold exposure; cold drains health to a 1 % floor.");
    }

    char const* ModeName(Mode mode)
    {
        switch (mode)
        {
            case Mode::Off:
                return "off";
            case Mode::Advisory:
                return "advisory";
            case Mode::Survival:
                return "survival";
        }

        return "unknown";
    }

    char const* BandName(Band band, bool hydration)
    {
        switch (band)
        {
            case Band::Normal:
                return hydration ? "Hydrated" : "Satiated";
            case Band::Warning:
                return hydration ? "Thirsty" : "Hungry";
            case Band::Critical:
                return hydration ? "Very thirsty" : "Very hungry";
            case Band::Empty:
                return hydration ? "Dehydrated" : "Starving";
        }

        return "unknown";
    }

    char const* ColdBandName(Band band)
    {
        switch (band)
        {
            case Band::Normal:
                return "Comfortable";
            case Band::Warning:
                return "Chilly";
            case Band::Critical:
                return "Cold";
            case Band::Empty:
                return "Freezing";
        }

        return "unknown";
    }
} // namespace HungerThirst

// =====================================================================
//  WorldScript: configuration and schema bootstrap.
// =====================================================================
class HungerThirstWorldScript : public WorldScript
{
public:
    HungerThirstWorldScript() : WorldScript("HungerThirst_WorldScript",
        {
            WORLDHOOK_ON_AFTER_CONFIG_LOAD,
            WORLDHOOK_ON_STARTUP
        })
    {
    }

    void OnAfterConfigLoad(bool reload) override
    {
        HungerThirst::LoadConfig(reload);
    }

    void OnStartup() override
    {
        HungerThirst::EnsureSchema();

        // Patch the state spells unconditionally: a later `.reload config`
        // enabling the states must never apply the original client spells.
        // The percentages are baked into the auras here, so changing them
        // requires a restart.
        HungerThirst::Config& cfg = HungerThirst::GetConfig();
        if (!HungerThirst::PatchHungerStateSpell(HungerThirst::SpellHungerWeakenedId, cfg.HungerStatePenaltyPct)
            || !HungerThirst::PatchHungerStateSpell(HungerThirst::SpellHungerStarvingId,
                cfg.HungerStarvingPenaltyPct))
            cfg.HungerStates = false; // fail safe: no states rather than wrong states

        if (!HungerThirst::PatchThirstStateSpell(HungerThirst::SpellThirstyId, cfg.ThirstStatePenaltyPct)
            || !HungerThirst::PatchThirstStateSpell(HungerThirst::SpellDehydratedId,
                cfg.ThirstDehydratedPenaltyPct))
            cfg.ThirstStates = false; // fail safe: no states rather than wrong states
    }
};

// =====================================================================
//  Registration
// =====================================================================
void AddHungerThirstScripts()
{
    new HungerThirstWorldScript();

    HungerThirst::AddHungerThirstPlayerScripts();
    HungerThirst::AddHungerThirstCommandScript();
}
