/*
 * Hunger & Thirst - module contract.
 *
 * This header is the shared interface between the manager, the player/unit
 * hooks and the command script. The implementation lives in HungerThirst.cpp;
 * hooks and commands must not mutate the state cache directly.
 */

#ifndef MOD_HUNGER_THIRST_H
#define MOD_HUNGER_THIRST_H

#include "Define.h"
#include <string>
#include <unordered_set>

class Aura;
class ChatHandler;
class Player;

namespace HungerThirst
{
enum class Mode : uint8
{
    Off      = 0,
    Advisory = 1,
    Survival = 2
};

enum class Band : uint8
{
    Normal   = 0,
    Warning  = 1,
    Critical = 2,
    Empty    = 3
};

struct Config
{
    bool   Enable                  = true;
    Mode   CurrentMode             = Mode::Off;
    double SatietyDecayPerMinute   = 0.50;
    double HydrationDecayPerMinute = 0.75;
    double RestMultiplier          = 0.50;
    double InstanceMultiplier      = 1.00;
    bool   PauseOnTaxi             = true;
    bool   PauseInBattleground     = true;
    bool   PauseInArena            = true;
    bool   ExemptGMs               = true;
    bool   HungerStates            = true;
    double HungerStatePenaltyPct   = 10.00;
    double HungerStarvingPenaltyPct = 30.00;
    bool   ThirstStates               = true;
    double ThirstStatePenaltyPct      = 10.00;
    double ThirstDehydratedPenaltyPct = 30.00;
    uint32 ThirstBlurIntervalMs       = 10000;
    uint32 ThirstBlurDurationMs       = 2000;
    uint32 WarningThreshold        = 30;
    uint32 CriticalThreshold       = 10;
    double FoodSatietyGain         = 30.00;
    double DrinkHydrationGain      = 35.00;
    uint32 RestoreCooldownMs       = 1000;
    uint32 AuraCreditCooldownMs    = 30000;
    double StartingSatiety         = 100.00;
    double StartingHydration       = 100.00;
    double StartingCold            = 0.00;
    double Hysteresis              = 2.00;

    // Cold and heat exposure. Cold is a third gauge (0 = comfortable,
    // 100 = freezing); heat has no gauge and only multiplies hydration loss.
    bool   ExposureEnable              = true;
    std::unordered_set<uint32> ColdZones;
    std::unordered_set<uint32> HotZones;
    std::unordered_set<uint32> ColdFires;
    uint32 ColdSeconds                 = 600;
    uint32 WarmSeconds                 = 120;
    uint32 ColdDrainIntervalMs         = 5000;
    double ColdHealthDrainPct          = 1.00;
    double HotHydrationMultiplier      = 2.00;
    bool   ShowLoginSummary        = true;
    bool   DebugLog                = false;
    bool   AddonEnable             = true;
    uint32 AddonUpdateIntervalMs   = 1000;
    std::string AddonPrefix        = "HT";
};

struct Snapshot
{
    double Satiety           = 100.00;
    double Hydration         = 100.00;
    double Cold              = 0.00;
    Mode   CurrentMode       = Mode::Off;
    bool   Loaded            = false;
    bool   PersistenceReady  = false;
    bool   Hot               = false;
    Band   SatietyBand       = Band::Normal;
    Band   HydrationBand     = Band::Normal;
    Band   ColdBand          = Band::Normal;
};

Config& GetConfig();

void LoadConfig(bool reload);
void EnsureSchema();
bool IsPersistenceReady();

bool LoadState(Player* player);
void SaveState(Player* player, bool force);
void RemoveState(Player* player);
void RemoveStateByGuid(uint32 guid);

void Tick(Player* player, uint32 diff);
void CreditConsumable(Player* player, Aura* aura);

bool GetSnapshot(Player* player, Snapshot& out);
void SetState(Player* player, double satiety, double hydration);
void SetState(Player* player, double satiety, double hydration, double cold);
void ResetState(Player* player);

void SendAddonState(Player* player);
void SendLoginSummary(Player* player);
void SendStatus(ChatHandler* handler, Player* player);

char const* ModeName(Mode mode);
char const* BandName(Band band, bool hydration);
char const* ColdBandName(Band band);

void AddHungerThirstPlayerScripts();
void AddHungerThirstCommandScript();
}

#endif
