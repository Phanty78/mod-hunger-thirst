/*
 * Hunger & Thirst - command script.
 *
 * Exposes the `.hungerthirst` command:
 *   .hungerthirst / .hungerthirst status       show the player's current state
 *   .hungerthirst help                         short help
 *   .hungerthirst set <player> <satiety> <hydration> [cold]  game master only
 *   .hungerthirst reset <player>               game master only
 *
 * The command owns no state: every read and write goes through the
 * HungerThirst contract declared in HungerThirst.h.
 */

#include "Chat.h"
#include "ChatCommandTags.h"
#include "CommandScript.h"
#include "Common.h"
#include "HungerThirst.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "StringConvert.h"
#include "Tokenize.h"
#include "Util.h"
#include "WorldSession.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace HungerThirst
{
namespace
{
    bool IsGameMaster(ChatHandler* handler)
    {
        WorldSession* session = handler->GetSession();
        return session && session->GetSecurity() >= SEC_GAMEMASTER;
    }

    bool RejectWithoutGameMaster(ChatHandler* handler)
    {
        handler->PSendSysMessage("This command is restricted to game masters.");
        handler->SetSentErrorMessage(true);
        return false;
    }

    bool EnsureModuleEnabled(ChatHandler* handler)
    {
        if (GetConfig().Enable)
            return true;

        handler->PSendSysMessage("The Hunger & Thirst module is disabled on this server.");
        handler->SetSentErrorMessage(true);
        return false;
    }

    Player* FindOnlineTarget(ChatHandler* handler, std::string_view name)
    {
        Player* target = ObjectAccessor::FindPlayerByName(std::string(name));
        if (!target)
        {
            handler->PSendSysMessage("Player '{}' could not be found or is offline.", std::string(name));
            handler->SetSentErrorMessage(true);
        }

        return target;
    }

    bool SendHelp(ChatHandler* handler)
    {
        Config const& config = GetConfig();

        handler->PSendSysMessage("Hunger & Thirst - commands:");
        handler->PSendSysMessage("  .hungerthirst, .hungerthirst status: show satiety, hydration and cold.");
        handler->PSendSysMessage("  .hungerthirst help: show this help.");
        handler->PSendSysMessage(
            "  .hungerthirst set <player> <satiety> <hydration> [cold]: set the values, "
            "clamped from 0 to 100 (game master). Omit cold to keep the current cold gauge.");
        handler->PSendSysMessage("  .hungerthirst reset <player>: reset a player's state (game master).");
        handler->PSendSysMessage("Thresholds: warning at {} %, critical at {} %. Active mode: {}.",
            config.WarningThreshold, config.CriticalThreshold, ModeName(config.CurrentMode));
        return true;
    }

    bool HandleSetCommand(ChatHandler* handler, std::vector<std::string_view> const& tokens)
    {
        if (!IsGameMaster(handler))
            return RejectWithoutGameMaster(handler);

        if (!EnsureModuleEnabled(handler))
            return false;

        if (tokens.size() != 4 && tokens.size() != 5)
        {
            handler->PSendSysMessage("Syntax: .hungerthirst set <player> <satiety> <hydration> [cold]");
            handler->SetSentErrorMessage(true);
            return false;
        }

        Player* target = FindOnlineTarget(handler, tokens[1]);
        if (!target)
            return false;

        Optional<double> satiety = Acore::StringTo<double>(tokens[2]);
        Optional<double> hydration = Acore::StringTo<double>(tokens[3]);
        if (!satiety || !hydration || !std::isfinite(*satiety) || !std::isfinite(*hydration))
        {
            handler->PSendSysMessage("Satiety and hydration values must be numbers.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        double const clampedSatiety = std::clamp(*satiety, 0.0, 100.0);
        double const clampedHydration = std::clamp(*hydration, 0.0, 100.0);

        double clampedCold = 0.0;
        if (tokens.size() == 5)
        {
            Optional<double> cold = Acore::StringTo<double>(tokens[4]);
            if (!cold || !std::isfinite(*cold))
            {
                handler->PSendSysMessage("The cold value must be a number.");
                handler->SetSentErrorMessage(true);
                return false;
            }

            clampedCold = std::clamp(*cold, 0.0, 100.0);
            SetState(target, clampedSatiety, clampedHydration, clampedCold);
        }
        else
            SetState(target, clampedSatiety, clampedHydration);

        SaveState(target, true);
        SendAddonState(target);

        char const* persistenceNote = IsPersistenceReady() ? "" : " (not persisted: database unavailable)";
        if (tokens.size() == 5)
            handler->PSendSysMessage(
                "State of {} set: satiety {:.1f}/100, hydration {:.1f}/100, cold {:.1f}/100{}.",
                target->GetName(), clampedSatiety, clampedHydration, clampedCold, persistenceNote);
        else
            handler->PSendSysMessage(
                "State of {} set: satiety {:.1f}/100, hydration {:.1f}/100, cold unchanged{}.",
                target->GetName(), clampedSatiety, clampedHydration, persistenceNote);
        return true;
    }

    bool HandleResetCommand(ChatHandler* handler, std::vector<std::string_view> const& tokens)
    {
        if (!IsGameMaster(handler))
            return RejectWithoutGameMaster(handler);

        if (!EnsureModuleEnabled(handler))
            return false;

        if (tokens.size() != 2)
        {
            handler->PSendSysMessage("Syntax: .hungerthirst reset <player>");
            handler->SetSentErrorMessage(true);
            return false;
        }

        Player* target = FindOnlineTarget(handler, tokens[1]);
        if (!target)
            return false;

        ResetState(target);
        SaveState(target, true);
        SendAddonState(target);

        char const* persistenceNote = IsPersistenceReady() ? "" : " (not persisted: database unavailable)";
        handler->PSendSysMessage("Hunger/thirst/cold state of {} reset{}.", target->GetName(), persistenceNote);
        return true;
    }
}

class HungerThirst_CommandScript : public CommandScript
{
public:
    HungerThirst_CommandScript() : CommandScript("HungerThirst_CommandScript") { }

    [[nodiscard]] Acore::ChatCommands::ChatCommandTable GetCommands() const override
    {
        static Acore::ChatCommands::ChatCommandTable commandTable =
        {
            { "hungerthirst", HandleHungerThirstCommand, SEC_PLAYER, Acore::ChatCommands::Console::No }
        };

        return commandTable;
    }

    static bool HandleHungerThirstCommand(ChatHandler* handler, Acore::ChatCommands::Tail args)
    {
        Player* player = handler->GetPlayer();
        if (!player)
        {
            handler->PSendSysMessage("This command must be used in game.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::string_view const input = args;
        std::vector<std::string_view> const tokens = Acore::Tokenize(input, ' ', false);

        if (tokens.empty() || StringEqualI(tokens[0], "status"))
        {
            SendStatus(handler, player);
            return true;
        }

        if (StringEqualI(tokens[0], "help"))
            return SendHelp(handler);

        if (StringEqualI(tokens[0], "set"))
            return HandleSetCommand(handler, tokens);

        if (StringEqualI(tokens[0], "reset"))
            return HandleResetCommand(handler, tokens);

        handler->PSendSysMessage("Unknown subcommand: '{}'. Use .hungerthirst help.", tokens[0]);
        handler->SetSentErrorMessage(true);
        return false;
    }
};

void AddHungerThirstCommandScript()
{
    new HungerThirst_CommandScript();
}
}
