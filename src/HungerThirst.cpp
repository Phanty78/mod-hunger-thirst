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

#include "Chat.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
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
            bool   Loaded          = false;
            bool   Dirty           = false;
            uint64 UpdatedAtMs     = 0;
            Band   SatietyBand     = Band::Normal;
            Band   HydrationBand   = Band::Normal;
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
        cfg.GroundMountMultiplier = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.GroundMountMultiplier", 2.00),
            0.00, 10.00, "GroundMountMultiplier");
        cfg.FlyingMountMultiplier = ClampRange(
            sConfigMgr->GetOption<float>("HungerThirst.FlyingMountMultiplier", 3.00),
            0.00, 10.00, "FlyingMountMultiplier");
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

        LOG_INFO("module.hunger-thirst", "Configuration {}: enable {}, mode {}, addon {}.",
            reload ? "reloaded" : "loaded", cfg.Enable, ModeName(cfg.CurrentMode), cfg.AddonEnable);
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
            "`updated_at` BIGINT UNSIGNED NOT NULL DEFAULT 0, "
            "PRIMARY KEY (`guid`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");

        // An empty table makes the probe return no row even though the table
        // exists, so COUNT(*) is used to distinguish "empty" from
        // "missing/DB error" (it always returns exactly one row on success).
        bool ready = static_cast<bool>(CharacterDatabase.Query(
            "SELECT 1 FROM `hunger_thirst_state` LIMIT 1"));

        if (!ready)
        {
            ready = static_cast<bool>(CharacterDatabase.Query(
                "SELECT COUNT(*) FROM `hunger_thirst_state`"));
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
        state.SatietyBand = RawBand(state.Satiety, cfg);
        state.HydrationBand = RawBand(state.Hydration, cfg);
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
            "SELECT `satiety`, `hydration`, `updated_at` FROM `hunger_thirst_state` WHERE `guid` = {}", guid);

        if (row)
        {
            Field* fields = row->Fetch();
            state.Satiety = ClampGauge(fields[0].Get<double>());
            state.Hydration = ClampGauge(fields[1].Get<double>());
            state.UpdatedAtMs = fields[2].Get<uint64>();
            state.SatietyBand = RawBand(state.Satiety, cfg);
            state.HydrationBand = RawBand(state.Hydration, cfg);
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
                "Loaded state for {} ({}): satiety {:.2f}, hydration {:.2f}, updated_at {}.",
                player->GetName(), guid, state.Satiety, state.Hydration, state.UpdatedAtMs);
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
            updatedAt = std::max<uint64>(NowMs(), state.UpdatedAtMs + 1);
            state.UpdatedAtMs = updatedAt;
            state.Dirty = false;
        }

        // Conditional upsert: a stale writer (older timestamp) is ignored, so
        // concurrent saves cannot roll the gauges back. Forced saves (logout,
        // GM commands) run synchronously so an immediate reconnect cannot
        // read the stale row; periodic saves stay asynchronous.
        char const* const query =
            "INSERT INTO `hunger_thirst_state` (`guid`, `satiety`, `hydration`, `updated_at`) "
            "VALUES ({}, {}, {}, {}) "
            "ON DUPLICATE KEY UPDATE "
            "`satiety` = IF(VALUES(`updated_at`) >= `updated_at`, VALUES(`satiety`), `satiety`), "
            "`hydration` = IF(VALUES(`updated_at`) >= `updated_at`, VALUES(`hydration`), `hydration`), "
            "`updated_at` = GREATEST(`updated_at`, VALUES(`updated_at`))";

        if (force)
            CharacterDatabase.DirectExecute(query, guid, satiety, hydration, updatedAt);
        else
            CharacterDatabase.Execute(query, guid, satiety, hydration, updatedAt);

        if (cfg.DebugLog)
        {
            LOG_DEBUG("module.hunger-thirst",
                "Saved state for guid {}: satiety {:.2f}, hydration {:.2f}, updated_at {}.",
                guid, satiety, hydration, updatedAt);
        }
    }

    void RemoveState(Player* player)
    {
        if (!player)
            return;

        // The client field is not persisted: restore it before the state row
        // is dropped so a reconnect starts clean.
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

        if (!cfg.Enable || cfg.CurrentMode == Mode::Off)
            return;

        if (!player->IsInWorld() || !player->IsAlive())
        {
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
        bool const resting = player->HasRestFlag(REST_FLAG_IN_TAVERN) ||
                             player->HasRestFlag(REST_FLAG_IN_CITY) ||
                             player->HasRestFlag(REST_FLAG_IN_FACTION_AREA);
        bool const OnMount = player->IsMounted();
        bool const OnFlyingMount = player->IsFlying();
        double multiplier = 1.00;
        if (resting)
            multiplier *= cfg.RestMultiplier;
        if (inDungeon)
            multiplier *= cfg.InstanceMultiplier;
        if(OnMount && !OnFlyingMount){
            multiplier *= cfg.GroundMountMultiplier;
        }
        if(OnFlyingMount){
            multiplier *= cfg.FlyingMountMultiplier;
        }
        

        uint32 const guid = player->GetGUID().GetCounter();

        char const* satietyMessage = nullptr;
        char const* hydrationMessage = nullptr;
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
            double const hydrationLoss = std::max(0.00, cfg.HydrationDecayPerMinute * minutes * multiplier);

            double const oldSatiety = state.Satiety;
            double const oldHydration = state.Hydration;
            Band const oldSatietyBand = state.SatietyBand;
            Band const oldHydrationBand = state.HydrationBand;

            state.Satiety = ClampGauge(state.Satiety - satietyLoss);
            state.Hydration = ClampGauge(state.Hydration - hydrationLoss);

            if (state.Satiety != oldSatiety || state.Hydration != oldHydration)
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
        out.CurrentMode = cfg.CurrentMode;
        out.Loaded = state.Loaded;
        out.PersistenceReady = IsPersistenceReady();
        out.SatietyBand = state.SatietyBand;
        out.HydrationBand = state.HydrationBand;
        return true;
    }

    void SetState(Player* player, double satiety, double hydration)
    {
        if (!player)
            return;

        Config const& cfg = GetConfig();
        if (!cfg.Enable)
            return;

        uint32 const guid = player->GetGUID().GetCounter();
        double const newSatiety = ClampGauge(satiety);
        double const newHydration = ClampGauge(hydration);
        Band satietyBand = Band::Normal;
        Band hydrationBand = Band::Normal;

        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            State& state = g_states[guid];
            state.Satiety = newSatiety;
            state.Hydration = newHydration;
            state.Loaded = true;
            state.Dirty = true;
            state.SatietyBand = RawBand(newSatiety, cfg);
            state.HydrationBand = RawBand(newHydration, cfg);
            satietyBand = state.SatietyBand;
            hydrationBand = state.HydrationBand;
        }

        // A GM jump can land directly inside a warning band without Tick
        // observing the crossing, so emit the corresponding warning once.
        // Band warnings are advisory-only: in survival the state message of
        // the next Tick informs the player.
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

        // Drop any running pulse: the next Tick restarts from the new state.
        ClearThirstBlur(player);
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
            state.Loaded = true;
            state.Dirty = true;
            state.AddonTimerMs = 0;
            state.SatietyBand = RawBand(state.Satiety, cfg);
            state.HydrationBand = RawBand(state.Hydration, cfg);
        }

        // Reset restores the gauges only; any running blur is rolled back so
        // the next Tick starts from the new state.
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
                "{}\t0.0\t0.0\t0\t0\t1\t0\t0\t0", cfg.AddonPrefix);

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
        std::string const payload = Acore::StringFormat(
            "{}\t{:.1f}\t{:.1f}\t{}\t{}\t{}\t{}\t{}\t{}",
            cfg.AddonPrefix,
            snapshot.Satiety,
            snapshot.Hydration,
            uint32(snapshot.CurrentMode),
            0,
            snapshot.Loaded ? 1 : 0,
            (snapshot.Loaded && snapshot.PersistenceReady) ? 1 : 0,
            uint32(snapshot.SatietyBand),
            uint32(snapshot.HydrationBand));

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

        if (cfg.CurrentMode == Mode::Off)
            handler->SendSysMessage("Mode off: gauges are not simulated.");
        else if (cfg.CurrentMode == Mode::Advisory)
            handler->SendSysMessage("Advisory: warnings only, no states, no damage.");
        else
            handler->SendSysMessage("Survival: hunger and thirst states, no exhaustion damage.");
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
