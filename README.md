# Hunger & Thirst (mod-hunger-thirst)

[![core-build](https://github.com/Phanty78/mod-hunger-thirst/actions/workflows/core-build.yml/badge.svg?branch=main)](https://github.com/Phanty78/mod-hunger-thirst/actions/workflows/core-build.yml)
[![Codestyle Checks](https://github.com/Phanty78/mod-hunger-thirst/actions/workflows/core_codestyle.yml/badge.svg?branch=main)](https://github.com/Phanty78/mod-hunger-thirst/actions/workflows/core_codestyle.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

<!-- TODO: capture d'écran réelle des jauges de satiété/hydratation (addon client).
     Aucune capture vérifiée n'est disponible à ce jour ; ne pas illustrer avec une image générique. -->

## Description

Hunger & Thirst is a standalone [AzerothCore](https://www.azerothcore.org/) module for World of Warcraft: Wrath of the Lich King 3.3.5a. It gives every character a satiety gauge and a hydration gauge, both running from 0 to 100. The gauges fall over time and are restored by eating and drinking. In `survival` mode, low satiety shows a visible hunger state and low hydration a visible thirst state, two debuffs that weaken the character. Neither gauge removes health or kills the character.

An optional cold and heat exposure layer adds a third gauge: cold rises from 0 (comfortable) to 100 (freezing) while the character is outdoors in a cold zone, away from a fire, and falls back under shelter. In `survival` mode it also shows the stock `Chilled` aura and, at 100, drains health down to a 1% floor, so even cold exposure cannot kill a character on its own. Hot zones have no gauge: they only make hydration drain faster.

No client patch is required: the states reuse four standard Blizzard spells, including their icons, as server-side auras, and the server cannot rewrite their client tooltips. The cold state reuses the stock `Chilled` spell (6136) unchanged, applied and refreshed by the server. A graphical addon is included but **optional**: it also replaces the four state-aura tooltips with the real effects and the remedy, while without it the original Blizzard tooltips remain and chat warnings, the login summary and the `.hungerthirst` command stay available.

This repository is a module repository, not a fork of the core. It is not bundled with AzerothCore and must be installed into the `modules/` directory of an existing WotLK 3.3.5a source tree.

## Features

- Per-character satiety, hydration and cold gauges persisted in the characters database.
- Decay based on active play time, with separate rest-area and dungeon/raid multipliers.
- Configurable pauses for taxi flights, battlegrounds, arenas, duels and game masters.
- Warning and critical thresholds with hysteresis, chat warnings, an optional login summary and state entry thresholds at the warning band.
- Three simulation modes: `off`, `advisory` and `survival`.
- Two visible hunger states in `survival` mode, built on two otherwise unused Blizzard spells: a weakened state at or below the warning threshold (movement speed and damage dealt reduced, damage taken increased) and a stronger starving state at 0.
- Two visible thirst states in `survival` mode, built on two otherwise unused Blizzard spells: a thirsty state at or below the warning threshold that reduces mana regeneration, magical damage done and healing done by 10% by default, and a dehydrated state at 0 that applies the same three penalties at 30% by default.
- A periodic drinking blur for the thirst states: a 2-second client blur every 10 seconds at intensity 35 (thirsty) or 45 (dehydrated), applied through a player update field. Both intensities stay below the client's drunk threshold (50), so the effect only tints the screen and never switches the character into the drunk stance or its walk animation; there is no aura and no gameplay effect. The blur is not what caused the sliding walk reported on a real client: the old dehydrated spell 14870 (now retired) has a stun in the stock client data, so the client played the stunned stand animation while the server let the character move. The dehydrated spell is now 5909, which keeps the same icon with a stat aura on the client.
- Hunger and thirst apply penalties only: neither gauge removes health, kills or logs a death.
- Cold and heat exposure (optional, `ExposureEnable`): a cold gauge from 0 (comfortable) to 100 (freezing) that rises outdoors in configured cold zones and falls under shelter or near a campfire, with `Chilly`, `Cold` and `Freezing` bands and chat warnings in both `advisory` and `survival`.
- In `survival` mode, cold at or above 75 keeps the stock `Chilled` aura (6136) on the player with a 15-second duration refreshed on every tick, and cold at 100 drains `ColdHealthDrainPct` of maximum health per `ColdDrainIntervalMs` down to a floor of 1% of maximum health. That drain cannot kill a character on its own, and it never runs in `advisory`, in instances or with `ExposureEnable=0`.
- Hot zones have no gauge: while the character is outdoors in a configured hot zone, is not sheltered and is not drinking, hydration loss is multiplied by `HotHydrationMultiplier`.
- Food and drink credit from items, feasts, game objects and player-cast auras, with anti-double-credit windows.
- Optional client addon using the `HT` addon prefix and the built-in 3.3.5a textures only.
- No SQL file to import: the characters table is created automatically at startup, and an existing table is upgraded with `ALTER TABLE` to add the `cold` column.

## Requirements

- An AzerothCore WotLK 3.3.5a server source tree that builds `worldserver`.
- A writable `acore_characters` database. The module creates and uses the table `hunger_thirst_state`.
- CMake and the normal AzerothCore build toolchain. The module adds no external dependency.
- A 3.3.5a client (build 12340) only if you want to install the optional addon.

## Installation

This is a standalone module repository. The module is **not** shipped with the core, and the core does not know about it until its files are present in the `modules/` directory and CMake is re-run.

1. Clone (or copy) this repository into the `modules/` directory of your AzerothCore source tree, using the directory name `mod-hunger-thirst`:

   ```bash
   cd /path/to/azerothcore-wotlk
   git clone https://github.com/Phanty78/mod-hunger-thirst.git modules/mod-hunger-thirst
   ```

2. Re-run CMake so the module is discovered, then rebuild `worldserver`:

   ```bash
   cmake -S . -B build
   cmake --build build --target worldserver -j"$(nproc)"
   ```

   With the AzerothCore Docker setup, rebuild the worldserver image and recreate the containers instead:

   ```bash
   docker compose build ac-worldserver
   docker compose up -d
   ```

3. Copy the configuration file to the module configuration directory and edit it. For Docker, the supported path is `env/dist/etc/modules/mod_hunger_thirst.conf`:

   ```bash
   cp modules/mod-hunger-thirst/conf/mod_hunger_thirst.conf.dist \
      env/dist/etc/modules/mod_hunger_thirst.conf
   ```

   On a native install, use the module configuration directory read by your `worldserver` (`etc/modules/` in a default Linux install). The module is deliberately shipped with `HungerThirst.Mode = off`; set the mode you want in this file. When an existing installation is updated, the live `.conf` file must be edited by hand: the Docker entrypoint only copies the file when it is missing (`cp -n`), so new keys and removed keys are not applied automatically.

4. Restart `worldserver`. The module does not start tracking until the rebuilt binary and the `.conf` file are both in place. Later configuration changes can be applied with `.reload config` instead of a restart, except for the `Enable` transition and the four state penalty percentages described under [Configuration](#configuration).

5. Optional: install the client addon from `client/Interface/AddOns/HungerThirst` as described under [Addon](#addon-optional).

## Configuration

The configuration file is `conf/mod_hunger_thirst.conf.dist` in this repository (section `[worldserver]`). It is deployed as `env/dist/etc/modules/mod_hunger_thirst.conf` with Docker, or as the equivalent module configuration file on a native install.

Changes can be applied with `.reload config`; they take effect at the next tick. Exception: changing `HungerThirst.Enable` from `0` to `1` does not load state for players who are already online, because their state was dropped while the module was disabled. Those players must reconnect, or a game master can initialize them with `.hungerthirst set`; new logins are unaffected.

Another exception: `HungerThirst.HungerStatePenaltyPct`, `HungerThirst.HungerStarvingPenaltyPct`, `HungerThirst.ThirstStatePenaltyPct` and `HungerThirst.ThirstDehydratedPenaltyPct` are baked into the four state spell auras when `worldserver` starts. Changing any of them requires a restart of `worldserver`. A configuration reload alone leaves the aura magnitudes at their startup values, and an entry chat line sent after the reload quotes the new value while the aura keeps the old one. The four spells are patched at startup even when `HungerStates=0` or `ThirstStates=0`, so enabling the states later with a reload never applies the original Blizzard spells. The thirst blur interval and duration, by contrast, are read on every tick and can be reloaded.

| Key | Default | Description |
|---|---|---|
| `HungerThirst.Enable` | `1` | Master switch. With `0` the module loads and saves nothing: no decay, warnings, state updates, blur, consumable credit or periodic addon updates, and the status command reports the module as disabled. If `AddonEnable=1`, one synthetic disabled snapshot is still sent at login so the addon can display `Disabled`. Commands remain registered, but `set` and `reset` refuse to run. |
| `HungerThirst.Mode` | `off` | Simulation mode: `off`, `advisory` or `survival` (case-insensitive). An unknown value falls back to `off` and logs an error. See [Modes](#modes). |
| `HungerThirst.SatietyDecayPerMinute` | `0.50` | Satiety points lost per minute of active play, before the rest and instance multipliers. `0` disables satiety decay. Clamped to 0-100. |
| `HungerThirst.HydrationDecayPerMinute` | `0.75` | Hydration points lost per minute of active play, before multipliers. Hydration drains slightly faster than satiety at the default values. Clamped to 0-100. |
| `HungerThirst.RestMultiplier` | `0.50` | Decay multiplier while resting (`REST_FLAG_IN_TAVERN`, `REST_FLAG_IN_CITY` or `REST_FLAG_IN_FACTION_AREA`). Multiplies the per-tick decay. Clamped to 0-10. |
| `HungerThirst.InstanceMultiplier` | `1.00` | Decay multiplier inside a dungeon or raid map (`Map::IsDungeon`). Stacks multiplicatively with `RestMultiplier` if a rest area is also an instance. Clamped to 0-10. |
| `HungerThirst.PauseOnTaxi` | `1` | Suspend decay, warnings, state updates, blur and periodic addon updates while the player is on a taxi flight. Any active blur is cleared for the duration of the pause. |
| `HungerThirst.PauseInBattleground` | `1` | Suspend decay, warnings, state updates, blur and periodic addon updates while the player is in a battleground or arena (the arena switch below is an additional guard). A pause clears any active blur and freezes the state auras on the player; with both switches at `0`, decay, warnings, states and blur keep running. |
| `HungerThirst.PauseInArena` | `1` | Suspend decay, warnings, state updates, blur and periodic addon updates while the player is in an arena match. A pause clears any active blur and freezes the state auras on the player; with both switches at `0`, decay, warnings, states and blur keep running. |
| `HungerThirst.ExemptGMs` | `1` | Skip the module entirely for accounts with game master security: no decay, warnings, state updates, blur or addon packets are produced for them. |
| `HungerThirst.WarningThreshold` | `30` | Upper bound of the warning band, in gauge points. A gauge above this value is normal; at or below it the player is hungry or thirsty. In `survival` mode with `HungerStates=1` it is also the entry threshold of the weakened hunger state; with `ThirstStates=1` it is the entry threshold of the thirsty thirst state. Must be `<= 100` and greater than `CriticalThreshold`. |
| `HungerThirst.CriticalThreshold` | `10` | Upper bound of the critical band, in gauge points. At or below it the player is very hungry or very thirsty; the critical band only sharpens the band label and the advisory warnings. At `0` the gauge is empty: satiety switches the hunger state to starving and hydration switches the thirst state to dehydrated. Must be lower than `WarningThreshold`. |
| `HungerThirst.HungerStates` | `1` | `survival` only. `1` enables the two visible hunger states: weakened at or below `WarningThreshold` (and above 0), starving at 0. They reduce movement speed and damage dealt and increase damage taken by a fixed percentage. `0` removes the states, so hunger has no gameplay effect; satiety band chat warnings are then not sent in `survival` (they remain available in `advisory`). See [Hunger states](#hunger-states). |
| `HungerThirst.HungerStatePenaltyPct` | `10.00` | Penalty of the weakened state, in percent: movement speed and damage dealt are reduced by this value, damage taken is increased by the same value. Clamped to 0-90. Baked into the spell aura at startup, so changing it requires a restart. |
| `HungerThirst.HungerStarvingPenaltyPct` | `30.00` | Penalty of the starving state, in percent, applied to the same three effects: movement speed and damage dealt reduced, damage taken increased. Clamped to 0-90. Baked into the spell aura at startup, so changing it requires a restart. |
| `HungerThirst.ThirstStates` | `1` | `survival` only. `1` enables the two visible thirst states: thirsty at or below `WarningThreshold` (and above 0), dehydrated at 0. Both reduce mana regeneration, magical damage done and healing done by their configured percentage. `0` removes the states and their blur, so hydration has no gameplay effect; hydration band chat warnings are then not sent in `survival` (they remain available in `advisory`). See [Thirst states](#thirst-states). |
| `HungerThirst.ThirstStatePenaltyPct` | `10.00` | Penalty of the thirsty state, in percent, applied to mana regeneration, magical damage done and healing done. Clamped to 0-90. Baked into the spell aura at startup, so changing it requires a restart. |
| `HungerThirst.ThirstDehydratedPenaltyPct` | `30.00` | Penalty of the dehydrated state, in percent, applied to the same three effects. Clamped to 0-90. Baked into the spell aura at startup, so changing it requires a restart. |
| `HungerThirst.ThirstBlurIntervalMs` | `10000` | Interval between two drinking blur pulses while a thirst state is active, in milliseconds. Values below 1000 are clamped to 1000. Reloadable. |
| `HungerThirst.ThirstBlurDurationMs` | `2000` | Duration of one drinking blur pulse, in milliseconds. Values below 100 are clamped to 100. Reloadable. |
| `HungerThirst.ExposureEnable` | `1` | Master switch for cold and heat exposure. With `0` the cold gauge is frozen at its current value (no gain, no warming), the `Chilled` aura is removed at the next tick, the health drain stops and hot zones have no effect; the gauge is still persisted and reported. Reloadable. |
| `HungerThirst.StartingCold` | `0.00` | Cold assigned to a character with no stored row, and the value restored by `.hungerthirst reset`. Clamped to 0-100. |
| `HungerThirst.ColdZones` | `1,65,67,210,618` | Comma-separated zone IDs where the cold gauge rises while the player is exposed. An empty value disables cold gain. See [Cold and heat](#cold-and-heat). |
| `HungerThirst.HotZones` | `3,46,51,440,3483` | Comma-separated zone IDs counted as hot. Heat has no gauge: hydration loss is multiplied by `HotHydrationMultiplier` when the player is outdoors, not sheltered and not drinking. |
| `HungerThirst.ColdCampfireEntries` | `2061` | Extra `gameobject_template` entries treated as a warm fire, in addition to every `SPELL_FOCUS` game object with focus id 4. Entry 2061 (`Campfire`) is listed explicitly so the stock campfire works even though its `spellFocus.focusId` is `0`. |
| `HungerThirst.ColdSeconds` | `600` | Seconds needed to go from 0 to 100 cold while exposed; swimming doubles the rate. Values below 30 are clamped to 30. |
| `HungerThirst.WarmSeconds` | `120` | Seconds needed to go from 100 back to 0 cold while warm or sheltered, that is whenever the player is not exposed. Values below 30 are clamped to 30. |
| `HungerThirst.ColdDrainIntervalMs` | `5000` | `survival` only. Interval between two cold health drain ticks at 100 cold while the player is still exposed. Values below 1000 are clamped to 1000. |
| `HungerThirst.ColdHealthDrainPct` | `1.00` | `survival` only. Percent of maximum health removed per drain interval at 100 cold, with a floor of 1% of maximum health, so it cannot kill alone. `0` disables the drain while keeping the aura and the warnings. Clamped to 0-100. |
| `HungerThirst.HotHydrationMultiplier` | `2.00` | Hydration loss multiplier in a hot zone when the player is outdoors, not sheltered and not drinking. Clamped to 1-10; `1` disables the effect. |
| `HungerThirst.FoodSatietyGain` | `30.00` | Satiety restored when a food aura is credited (`SPELL_SPECIFIC_FOOD` or `FOOD_AND_DRINK`). The gain is clamped to 0-100 and the gauge itself to 100. |
| `HungerThirst.DrinkHydrationGain` | `35.00` | Hydration restored when a drink aura is credited (`SPELL_SPECIFIC_DRINK` or `FOOD_AND_DRINK`). The gain is clamped to 0-100 and the gauge itself to 100. |
| `HungerThirst.RestoreCooldownMs` | `1000` | Anti-double-credit window for item-based food and drink only: the same cast item entry is credited at most once per window, so refresh callbacks for one bite are ignored. Auras without a cast item use `AuraCreditCooldownMs` instead. Values below 1000 are clamped to 1000. |
| `HungerThirst.AuraCreditCooldownMs` | `30000` | Anti-double-credit window for every food or drink aura without a cast item (feast, game object, player-cast aura). The credit is keyed by spell ID and caster GUID, and the actual window is `max(this value, aura duration)`, so one feast tick credits a player once rather than once per server tick. Values below 1000 are clamped to 1000. |
| `HungerThirst.StartingSatiety` | `100.00` | Satiety assigned to a character with no stored row (a new character, or the first login after installing the module). Clamped to 0-100. |
| `HungerThirst.StartingHydration` | `100.00` | Hydration assigned to a character with no stored row. Clamped to 0-100. |
| `HungerThirst.Hysteresis` | `2.00` | Points a gauge must gain strictly above the threshold it is leaving before its band improves. Bands always worsen immediately. The same margin governs the exit of the hunger and thirst states. `0` disables hysteresis. Clamped to 0-`(100 - WarningThreshold)` so a full gauge can always leave the warning band. |
| `HungerThirst.ShowLoginSummary` | `1` | Send a two-line chat summary on login when the module is enabled and the mode is not `off`: mode, satiety and hydration with their bands, then the cold value with its band and a note when the player is in a hot zone. The cold band warning is restated at login when exposure is enabled and the gauge is at 25 or more, so a reconnecting player is not left without guidance. In `survival` mode, the state chat lines replace the corresponding band warnings when the matching states are enabled. |
| `HungerThirst.DebugLog` | `0` | Emit `LOG_DEBUG` lines for state load and save, and consumable credits. |
| `HungerThirst.AddonEnable` | `1` | Send addon state packets when `Enable=1`, `AddonEnable=1` and the player's state is loaded. A login snapshot is sent even in mode `off` (`mode=0`), and one synthetic disabled snapshot is sent when the module is disabled, so the addon can display `Disabled`. Periodic packets run only while the tick is active (`advisory` or `survival`, not paused). The packet is a `CHAT_MSG_WHISPER` / `LANG_ADDON` message sent directly to the player. |
| `HungerThirst.AddonUpdateIntervalMs` | `1000` | Periodic addon packet interval in milliseconds while the tick is active. The login snapshot is sent immediately. Values below 1000 are clamped to 1000. |
| `HungerThirst.AddonPrefix` | `HT` | Prefix written as the first tab-separated field of the addon packet. It must match the addon's prefix. An empty value falls back to `HT`. The default is the string `HT`, written as `HungerThirst.AddonPrefix = "HT"` in the file. |

Validation performed while loading:

- An unknown `Mode` becomes `off` and logs an error.
- `CriticalThreshold` must be lower than `WarningThreshold`; otherwise the critical threshold is adjusted and an error is logged. `WarningThreshold` above 100 is clamped to 100.
- Gauges (satiety, hydration, cold), percentages, gains and decay rates are clamped to 0-100.
- The two hunger penalty percentages and the two thirst penalty percentages are clamped to 0-90 rather than 0-100.
- `RestMultiplier` and `InstanceMultiplier` are clamped to 0-10. `HotHydrationMultiplier` is clamped to 1-10.
- `ColdSeconds` and `WarmSeconds` are clamped to a minimum of 30 seconds.
- `ThirstBlurIntervalMs` is clamped to a minimum of 1000 ms and `ThirstBlurDurationMs` to a minimum of 100 ms.
- Other intervals and cooldowns are clamped to a minimum of 1000 ms.
- `Hysteresis` is clamped to 0-`(100 - WarningThreshold)`.

## Commands

The command is available in game only, not from the server console. Subcommands are case-insensitive.

| Command | Access | Effect |
|---|---|---|
| `.hungerthirst` or `.hungerthirst status` | Player | Show the active mode, state loaded or not loaded, persistence ready or unavailable, the three gauges with their bands (satiety, hydration, cold), whether the player is in a hot zone, and a one-line explanation of the mode. |
| `.hungerthirst help` | Player | Show the command list, the warning and critical thresholds, and the active mode. |
| `.hungerthirst set <player> <satiety> <hydration> [cold]` | Game master (`SEC_GAMEMASTER`) | Set the gauges, clamped to 0-100, and force a save. `cold` is optional: omitted, the current cold gauge is preserved. The target must be online. The hunger, thirst and cold states are reconciled on the next active tick. |
| `.hungerthirst reset <player>` | Game master (`SEC_GAMEMASTER`) | Restore the target to `StartingSatiety`, `StartingHydration` and `StartingCold`, and force a save. The target must be online. Any active state, the cold aura and the blur are cleared or reconciled on the next active tick. |

`status` and `help` still answer when the module is disabled. `set` and `reset` fail with a message when the module is disabled, when the caller lacks game master security, or when the named player is offline.

## Modes

| `HungerThirst.Mode` | Decay | Warnings and login summary | Hunger states | Thirst states | Blur | Cold gauge | Chilled aura and cold drain | Periodic addon updates |
|---|---|---|---|---|---|---|---|---|
| `off` (default) | No | No | No | No | No | Frozen at its stored value | No | No |
| `advisory` | Yes | Yes | No | No | No | Simulated with `ExposureEnable=1`, with cold band warnings | No | Yes |
| `survival` | Yes | Band warnings replaced by the state chat lines for the gauges whose states are enabled; the login summary still reports gauges and bands; cold band warnings are always sent | Yes with `HungerStates=1` | Yes with `ThirstStates=1` | Yes with `ThirstStates=1` | Simulated, with cold band warnings | Yes with `ExposureEnable=1`, outside instances | Yes |

`off` keeps the module loaded: the state is persisted and can be queried, and the login addon snapshot is still sent with `mode=0`, but nothing is simulated and the cold gauge does not move. `Enable=0` is stronger than `Mode=off`: state is not loaded or saved at all and the module reports itself as disabled.

Hunger and thirst never apply damage in any mode. `survival` combines the two visible state families with their chat lines and, for thirst, the blur. Cold exposure, in `survival` mode only, adds the stock `Chilled` aura and the only health drain in the module; that drain stops at a 1% floor and therefore cannot kill a character on its own.

Hunger and thirst states exist only in `survival` mode. In `off` and `advisory`, any active state aura is removed at the next tick, and any active blur is restored. With `HungerStates=0`, hunger has no gameplay effect and its band warnings are not sent in `survival`; with `ThirstStates=0`, hydration has no gameplay effect, no state and no blur, and its band warnings are not sent in `survival`. `advisory` still shows all band warnings and never applies a state or a blur. The cold gauge is the exception: it is simulated in `advisory` as well, and only its `Chilled` aura and health drain are restricted to `survival`.

### Bands

| Band | Satiety label | Hydration label |
|---|---|---|
| Normal | Satiated | Hydrated |
| Warning | Hungry | Thirsty |
| Critical | Very hungry | Very thirsty |
| Empty | Starving | Dehydrated |

A gauge is empty at 0, critical at or below `CriticalThreshold`, in the warning band at or below `WarningThreshold`, and normal above it. Warning messages fire when a band worsens. Hysteresis prevents messages from flapping around a threshold: a band only improves once the gauge is strictly above the threshold it is leaving plus `Hysteresis`. The cold gauge uses its own fixed thresholds and labels; see [Cold and heat](#cold-and-heat). In `survival` mode the satiety band selects the hunger state and the hydration band selects the thirst state; with the corresponding states enabled, the band warnings are replaced by the state chat line. Advisory mode never applies states, so it keeps every band warning.

The status command uses the words `loaded` or `not loaded` for the state and `ready` or `unavailable` for persistence.

### Hunger states

In `survival` mode with `HungerStates=1`, the satiety band drives a visible debuff. The module reuses two Blizzard spells that ship with the 3.3.5a client and are otherwise unused, and patches their server-side spell data in memory at startup. No client patch, MPQ or DBC change is involved.

| State | Satiety | Spell ID | Name (enUS) | Icon | Effects |
|---|---|---|---|---|---|
| Weakened | above 0 and at or below `WarningThreshold` (warning or critical band, 30 by default) | 52749 | Voracious Appetite | `ability_racial_cannibalize` | Movement speed -`HungerStatePenaltyPct`%, damage dealt -`HungerStatePenaltyPct`%, damage taken +`HungerStatePenaltyPct`% (10% by default) |
| Starving | 0 | 55316 | Ravenous Hunger | `spell_holy_stoicism` | Movement speed -`HungerStarvingPenaltyPct`%, damage dealt -`HungerStarvingPenaltyPct`%, damage taken +`HungerStarvingPenaltyPct`% (30% by default) |

- The state uses the same band and hysteresis logic as the gauges. The weakened state starts as soon as satiety enters the warning band and ends when the band returns to normal; a gauge leaving the warning band must rise strictly above `WarningThreshold + Hysteresis` (32 by default). The starving state starts at 0 and ends as soon as satiety rises above 0. Eating one bite from 0 to 30 by default therefore replaces starving with weakened, not with no state.
- Entering a state applies an infinite-duration aura and sends exactly one English chat line with the real percentages and the remedy. The weakened line ends with `Eat something.`, the starving line with `Eat something!`. The line is sent on entry only, never per tick. Leaving a state removes its aura; switching between the two states removes the old aura before applying the new one.
- The three effects are speed reduction, damage-dealt reduction and damage-taken increase. Damage effects cover every spell school. The percentage is the same for all three effects of a state, but differs between the weakened and starving states.
- The aura is a negative debuff (red border) and cannot be dispelled, cancelled by right-click or removed by immunity effects: the patched spells have no dispel type and no mechanic. Only the module removes them, following the band, when the states are disabled or when the mode leaves `survival`.
- The state is reconciled on every active tick, which covers login, resurrection, `.reload config` and `.hungerthirst set/reset`. Death removes all auras by core behavior; the next active tick after resurrection re-applies the state if the band still calls for it.
- Any pause (taxi, battleground, arena, duel, GM exemption) stops the tick: gauges are frozen and the state is left exactly as it was, with no chat line and no update.
- The client displays the original Blizzard spell name and icon. Its tooltip keeps the original Blizzard description, which describes unrelated effects, and does not show the configured percentages; the chat line, the configuration and this README are the authoritative source. The optional addon replaces that tooltip with the real effects and the remedy. Without the addon the Blizzard text stays, because no server-side change can alter a client tooltip without a client patch.
- If either spell cannot be found at startup, the module logs an error and disables the hunger states instead of applying an unpatched spell.

### Thirst states

In `survival` mode with `ThirstStates=1`, the hydration band drives a second visible debuff family. Like the hunger states, it reuses two client-visible Blizzard spells that ship with the 3.3.5a client and are otherwise unused, and patches their server-side spell data in memory at startup. No client patch, MPQ or DBC change is involved.

| State | Hydration | Spell ID | Name (enUS) | Icon | Effects |
|---|---|---|---|---|---|
| Thirsty | above 0 and at or below `WarningThreshold` (warning or critical band, 30 by default) | 25696 | Drink | `INV_Drink_07` | Mana regeneration -`ThirstStatePenaltyPct`%, magical damage done -`ThirstStatePenaltyPct`%, healing done -`ThirstStatePenaltyPct`% (10% by default) |
| Dehydrated | 0 | 5909 | Watered-down Beer | `INV_Drink_04` | Mana regeneration -`ThirstDehydratedPenaltyPct`%, magical damage done -`ThirstDehydratedPenaltyPct`%, healing done -`ThirstDehydratedPenaltyPct`% (30% by default) |

The dehydrated state previously reused 14870 ("Drunken Stupor"). It carries the same icon in the stock client, but its stock client effect is `SPELL_AURA_MOD_STUN`: the client played the stunned stand animation while the server let the character move, which is the "sliding walk" reported on a real client. Server-side patching cannot change what the client renders for that spell, so 14870 was retired in favour of 5909 ("Watered-down Beer"), whose stock client effect is `SPELL_AURA_MOD_STAT`: same icon, no animation, no stance.

- Earlier module versions used 43155 ("Drink"), 46583 ("Cosmetic - Drink") and 14869 ("Drunken Stupor") for these states. The stock client hides all of them: 43155 carries `SPELL_ATTR0_DO_NOT_DISPLAY`, 46583 carries `SPELL_ATTR1_NO_AURA_ICON` and 14869 is passive, so no thirst icon appeared. 14870 ("Drunken Stupor") is retired for the reason above. Those legacy IDs are no longer applied; their auras are removed automatically from a character that still carries them, before the current state aura is reconciled, and whenever the thirst states are removed.
- A single thirst state is active at a time. The state is selected from the hydration band with the same band and hysteresis logic as the gauges: thirsty starts as soon as hydration enters the warning band and ends when the band returns to normal (strictly above `WarningThreshold + Hysteresis`, 32 by default); dehydrated starts at 0 and replaces thirsty, and ends as soon as hydration rises above 0. At the default values, one standard drink restores 35 hydration, which is above 32, so a single drink can clear the state outright.
- Thirst states exist only in `survival` with `ThirstStates=1`. `advisory` and `off` never apply a state or a blur, and `ThirstStates=0` removes both at the next tick; in those cases hydration keeps its band labels but has no gameplay effect. `advisory` keeps the hydration band warnings that `survival` replaces with the state chat line.
- Entering a state applies an infinite-duration aura and sends exactly one English chat line with the real percentages and the remedy, never once per tick:
  - thirsty: `Your thirst weakens you: mana regeneration and spell effectiveness reduced by 10%. Drink something.`
  - dehydrated: `You are dehydrated: mana regeneration and spell effectiveness reduced by 30%. Drink something!`
- The percentages in those lines are the configured `ThirstStatePenaltyPct` and `ThirstDehydratedPenaltyPct`; the wording "spell effectiveness" covers the two spell effects below.
- The three effects are:
  - **Mana regeneration**: the canonical `SPELL_AURA_MOD_POWER_REGEN_PERCENT` (110) aura on `POWER_MANA`. In this core that aura scales only the spirit-based regeneration portion (`power_regen`); it does not reduce the flat mana-per-5-seconds contribution (`power_regen_mp5`). Reducing both would require a core change in `Player::UpdateManaRegen`, which this module deliberately does not make.
  - **Magical damage done**: `SPELL_AURA_MOD_DAMAGE_PERCENT_DONE` (79) limited to `SPELL_SCHOOL_MASK_MAGIC`. Physical, ranged and pet damage are not affected, and neither is damage taken.
  - **Healing done**: `SPELL_AURA_MOD_HEALING_DONE_PERCENT` (136).
- The aura is a negative debuff (red border) and cannot be dispelled, cancelled by right-click or removed by immunity effects: the patched spells have no dispel type and no mechanic. Only the module removes them, following the band, when the states are disabled or when the mode leaves `survival`.
- Thirst has no damage component: hydration never removes health. There is no damage-per-hit setting, no lethal switch and no damage ramp for the thirst states; the only health drain in the module is the cold one, which stops at a 1% floor and has no death cause or death log. The only consequences of thirst are the three percentage penalties and the blur.
- The state is reconciled on every active tick, which covers login, resurrection, `.reload config` and `.hungerthirst set/reset`. Death removes all auras by core behavior; the next active tick after resurrection re-applies the state if the band still calls for it.
- Any pause (taxi, battleground, arena, duel, GM exemption) stops the tick: gauges are frozen, the state aura is left exactly as it was with no chat line and no update, and any active blur is cleared (restoring the previous client value) for the duration of the pause.
- The client displays the original Blizzard spell name and icon, localized by the client. Its tooltip keeps the original Blizzard description, which describes unrelated effects, and does not show the configured percentages or the spirit-only mana limitation; the chat line, the configuration and this README are the authoritative source. The optional addon replaces that tooltip with the real effects and the remedy. Without the addon the Blizzard text stays, because no server-side change can alter a client tooltip without a client patch.
- If either spell cannot be found at startup, the module logs an error and disables the thirst states at startup instead of applying an unpatched spell.

#### Blur

While a thirst state is active, the module pulses a drinking blur on the player through the public `PLAYER_FAKE_INEBRIATION` player update field, the field the client already uses for the visual effect of intoxication. The field is written directly: no aura is applied, no buff or debuff icon appears, no combat statistic changes and the character's real intoxication state is untouched.

- A pulse lasts `ThirstBlurDurationMs` (2 seconds by default) and repeats every `ThirstBlurIntervalMs` (10 seconds by default) as long as the state lasts.
- The pulse intensity is 35 for the thirsty state and 45 for the dehydrated state, both below the client's drunk threshold (50), where `PLAYER_FAKE_INEBRIATION` only tints the screen: the effect is visual only and never puts the character into the drunk stance or its walk animation. The field was not the cause of the sliding walk reported on a real client: that came from the retired dehydrated state aura 14870, whose stock client effect is a stun. The dehydrated intensity stays at 45. The module stores the current field value, writes the greater of the current value and the pulse intensity, then restores the stored value when the pulse ends. A pre-existing value from another source is therefore preserved, including a value that already places the character in the drunk state.
- The field is not persisted in the database. It is restored when the state ends, when the tick pauses (taxi, battleground, arena, duel, GM exemption, dead), when the mode leaves `survival`, when `ThirstStates=0`, on reset and on logout.
- No blur is produced in `off` or `advisory` mode, with `ThirstStates=0`, or while the module is disabled.
- Caveat: whether the stock 3.3.5a client renders `PLAYER_FAKE_INEBRIATION` as a screen effect outside a real intoxication aura must be validated on a real client. If the client ignores the field, the documented fallback is a short aura using `SPELL_AURA_MOD_FAKE_INEBRIATE` (304), or `SPELL_AURA_SCREEN_EFFECT` (260), applied through an unused spell; such an aura briefly shows a debuff icon. Either way there is no client patch and no MPQ change.

### Pauses and exemptions

- Dead characters, characters not yet in the world and characters whose state is still loading or failed to load are never decayed, warned or updated. Death removes the state auras by core behavior; the state is re-applied by the next active tick after resurrection if the band still calls for it. An active blur is cleared and the previous field value restored.
- A player in a duel is always paused. There is no configuration key for this.
- A taxi flight pauses the module when `PauseOnTaxi=1`.
- A battleground or arena pauses the module when the corresponding switch is `1`. With both switches at `0`, decay, warnings, states and blur keep updating.
- GM accounts are skipped when `ExemptGMs=1`.

While paused, decay, warnings, state updates, blur and periodic addon updates stop. The cold gauge is frozen, its aura stays in place and the cold health drain stops. The login snapshot is unaffected. An existing state aura stays in place for the duration of the pause, so a paused player keeps the debuff without any further chat line; if the state aura was removed (for example by death) while the pause applies, it is restored by the first active tick once no pause applies. Any active blur is cleared at the start of the pause and is not resumed until the next pulse after the pause ends.

## Cold and heat

Cold and heat are an optional exposure layer on top of hunger and thirst. The layer is controlled by `HungerThirst.ExposureEnable` (on by default) and adds a third gauge, `Cold`, stored next to the other two and reported by the status command, the login summary and the addon.

### Cold bands

| Cold | Band | Effect |
|---|---|---|
| 0-24 | Comfortable | No gameplay effect. |
| 25-49 | Chilly | Band warning `You are starting to feel cold. Seek shelter or a fire.` |
| 50-74 | Cold | Band warning `You are cold. Seek shelter or a fire.` |
| 75-99 | Freezing | Band warning `The cold is slowing you down. Seek shelter or a cooking fire.`; the `Chilled` aura in `survival` mode. |
| 100 | Freezing | Same band warning; the `Chilled` aura and the health drain in `survival` mode, while the player stays exposed. |

Cold runs from 0 (comfortable) to 100 (freezing). Its band thresholds are fixed and independent of `WarningThreshold` and `CriticalThreshold`: Chilly from 25, Cold from 50, Freezing from 75, which is also the aura threshold, and 100 is the drain threshold. Bands worsen immediately and improve only once the gauge falls strictly below the band's exit threshold minus `Hysteresis`, the same margin the other gauges use. A cold band warning is sent in both `advisory` and `survival`.

### Exposure

A player is *exposed* to cold when every one of these holds:

- the zone is listed in `HungerThirst.ColdZones` (zone IDs, `1,65,67,210,618` by default) and the map is not an instance;
- the player is outdoors: `Player::IsOutdoors()`, so any indoor area counts as shelter;
- the player is not resting: `REST_FLAG_IN_TAVERN`, `REST_FLAG_IN_CITY` and `REST_FLAG_IN_FACTION_AREA` all count as shelter;
- no warm fire is within 8 yards and in line of sight.

A warm fire is a game object of type `GAMEOBJECT_TYPE_SPELL_FOCUS` whose `spellFocus.focusId` is 4, or any game object entry listed in `HungerThirst.ColdCampfireEntries`. The default list contains entry `2061` (`Campfire`), whose stock `spellFocus.focusId` is `0`, so the explicit entry is what makes the common campfire work; the focus-id check covers maps or custom data that use focus id 4. The fire probe visits the grid, so it is throttled to one check every 5 seconds while the player is outdoors in a cold zone: a fire reached, lit or put out is noticed within that window, and the first check after entering a cold zone can take up to 5 seconds.

While exposed, the gauge gains `100 / ColdSeconds` points per second (600 seconds from 0 to 100 by default). Swimming doubles the rate (`Player::IsInWater`), so a river, lake or sea halves the effective exposure time. At any other moment - sheltered, near a fire, in a warm zone, in an instance - the gauge loses `100 / WarmSeconds` points per second (120 seconds from 100 back to 0 by default). Instances are excluded from exposure: inside a dungeon, raid or any other instanceable map the gauge warms at the normal rate, the `Chilled` aura is removed and the drain never runs.

### Cold in survival mode

In `survival` mode, with `ExposureEnable=1` and outside instances:

- At 75 cold or more, the module applies the stock `Chilled` aura (spell 6136) and refreshes its 15-second duration on every active tick, so it never expires while the gauge stays at 75 or more. The module only manages its own copy: a `Chilled` aura cast by a creature is left untouched and a second copy is never stacked. The aura is removed when the gauge drops below 75, when the player leaves `survival`, when `ExposureEnable` becomes `0`, inside an instance, when the character is dead or out of the world, and on `reset` or character deletion.
- At 100 cold, while the player is still exposed, health is drained every `ColdDrainIntervalMs` (5 seconds by default) by `ColdHealthDrainPct` of maximum health (1% by default). The drain stops at a floor of `ceil(maxHealth / 100)` health points (at least 1), so it can never kill the character on its own: there is no death cause, no death record and no death log. Time spent at 100 cold accumulates, so a long server tick applies every interval it covers up to the module's 10-second tick cap. The player is told once, when the drain starts, with `You are freezing: the cold is draining your health. Seek shelter or a fire immediately.`; the notice re-arms once the gauge drops below 100.
- `ColdHealthDrainPct = 0` disables the drain and keeps the aura and the warnings.

`advisory` mode simulates the gauge, sends the cold band warnings and reports the value to the command, the login summary and the addon, but never applies the `Chilled` aura and never drains health. In `off` mode the gauge is frozen and the aura is removed at the next tick. With `Enable=0` the state is not loaded at all.

### Heat

Hot zones have no gauge and no aura. When the zone is listed in `HungerThirst.HotZones` (`3,46,51,440,3483` by default), the map is not an instance, and the player is outdoors, not sheltered and not drinking, hydration loss is multiplied by `HotHydrationMultiplier` (2 by default); `1` disables the effect. *Drinking* means a drink or food-and-drink aura cast from an item (`SPELL_SPECIFIC_DRINK` or `SPELL_SPECIFIC_FOOD_AND_DRINK` with a cast item), so a normal drink stops the extra loss while it lasts.

The hot flag is reported by the status command (`Hot zone: hydration drains faster.`), by the login summary and as field 11 of the addon protocol. The multiplier is applied to the same tick as the decay rates, after the rest and instance multipliers, and is skipped while a pause applies.

### Disabling exposure

`ExposureEnable = 0` freezes the cold gauge at its current value (no gain, no warming), removes the module's `Chilled` aura at the next tick, stops the health drain and disables the hot-zone multiplier. The stored value is kept and still reported. The switch is reloadable with `.reload config`.

## Persistence

State is stored in the `acore_characters` database, in the table `hunger_thirst_state`. The module creates the table at startup with `CREATE TABLE IF NOT EXISTS`; there is no SQL file to import.

| Column | Type | Description |
|---|---|---|
| `guid` | `INT UNSIGNED` (primary key) | Character low GUID. |
| `satiety` | `DECIMAL(5,2)` | Satiety from 0 to 100, default `100.00`. |
| `hydration` | `DECIMAL(5,2)` | Hydration from 0 to 100, default `100.00`. |
| `cold` | `DECIMAL(5,2)` | Cold from 0 to 100, default `0.00`. |
| `updated_at` | `BIGINT UNSIGNED` | Timestamp of the last save, in epoch milliseconds. |

A table created by an earlier module version has no `cold` column. The module checks `information_schema` at startup and migrates it automatically with `ALTER TABLE hunger_thirst_state ADD COLUMN cold DECIMAL(5,2) NOT NULL DEFAULT 0.00 AFTER hydration`, so no manual SQL is required. The column is then verified with `SELECT COUNT(cold)`, which is also the persistence readiness check.

Writes are conditional on `updated_at`, so an older save never overwrites a newer one. State is loaded before the character enters the world, saved periodically with the character, saved synchronously on logout, and written synchronously by the `set` and `reset` commands. The row is deleted together with the character.

If the table cannot be created or verified, the simulation is suspended for the affected players: no tick, decay, warning, state update, blur, save, consumable credit or periodic addon update. Login is never blocked. With an active mode and `ShowLoginSummary=1`, the player receives a persistence-unavailable chat notice, and with `AddonEnable=1` the addon displays the same status. The state stays unloaded for the whole session, so a reconnect is required once the table is available again.

Gauges do not decay while the character is offline; only active play time advances the simulation.

## Addon (optional)

The addon is shipped in `client/Interface/AddOns/HungerThirst`. Copy that folder into the client's addon directory:

```bash
cp -r modules/mod-hunger-thirst/client/Interface/AddOns/HungerThirst \
      "/path/to/World of Warcraft/Interface/AddOns/"
```

The addon displays the satiety, hydration and cold gauges and their band labels. Apart from the aura tooltip override described below, it only reads the state sent by the server. It uses the `HT` addon prefix; `HungerThirst.AddonPrefix` must match the prefix configured in the addon. Slash commands:

- `/hungerthirst` or `/ht` shows or hides the gauge window.
- `/hungerthirst reset` moves the window and the status icons back to their default positions and shows the window.
- `/hungerthirst icons` shows or hides the status icons; the choice is saved.

The window position, icon position and icon visibility are saved between sessions. The cold gauge and the hot flag are carried by the appended protocol fields described under [Protocol](#protocol); an older server that sends only eight fields leaves them at `0` (Comfortable) and off. The addon adds one client-side element: it replaces the tooltips of the four hunger and thirst state auras, as described below.

### Aura tooltips

The server patches the four state spells only on its side, so the stock client tooltips keep the original Blizzard names and descriptions, which describe unrelated effects. The addon corrects them: it hooks `GameTooltip:SetUnitAura` for the player and replaces the Blizzard title and description of spells 52749, 55316, 25696 and 5909 with a state name, the three effects of that state and how to remove it.

- The effects and remedies are the ones documented under [Hunger states](#hunger-states) and [Thirst states](#thirst-states): movement speed, damage dealt and damage taken for hunger; mana regeneration, magical damage done and healing done for thirst.
- The remedy states how to leave the state: for hunger, eat until satiety is strictly above `WarningThreshold + Hysteresis`; from 0, the first bite replaces starving with weakened and the second clears the state. For thirst, drink until hydration is above the same margin; with the default 35-point drink, one drink suffices.
- The percentages shown are the module defaults (10% for the warning-band state, 30% at 0) and are labelled as such, because the addon protocol does not carry the configured values. The actual percentages are in the entry chat line.
- The replacement titles and lines are written in French.
- The hook only covers the player's own auras and leaves every other aura and tooltip untouched. It is a client-side display change: other players, and clients without the addon, still see the Blizzard tooltip.

Without the addon the Blizzard tooltip remains and the entry chat line is the authoritative description of the state, its real percentages and its remedy. The addon can only override the text on the local client; changing the underlying tooltip data shipped with the game would require a client patch or a DBC edit, which the module does not use.

### Protocol

The server sends a `CHAT_MSG_WHISPER` / `LANG_ADDON` message directly to the player. The payload is the prefix followed by exactly eleven tab-separated fields:

```text
HT<TAB>satiety<TAB>hydration<TAB>mode<TAB>lethal<TAB>loaded<TAB>persistence<TAB>satBand<TAB>hydBand<TAB>cold<TAB>coldBand<TAB>hot
```

- Satiety, hydration and cold use one decimal.
- Mode is `0` for `off`, `1` for `advisory` and `2` for `survival`.
- Hunger and thirst bands are `0` for normal, `1` for warning, `2` for critical and `3` for empty.
- Cold band (`coldBand`) is `0` for Comfortable, `1` for Chilly, `2` for Cold and `3` for Freezing.
- `hot` is `0` or `1`; it is `1` while the player is in a hot zone and takes the increased hydration loss.
- `loaded` and `persistence` are `0` or `1`.
- `lethal` is always `0`. The module has no lethal model, but the field is kept in the payload, documented as reserved, so addons keep parsing the same field positions. It must not be interpreted as a gameplay flag.

The first eight fields keep their historical positions and meanings; `cold`, `coldBand` and `hot` are appended. An addon that only reads the first eight fields keeps working unchanged, and a message with eight fields leaves cold at `0` (Comfortable) and hot off in the current addon.

There are no color codes.

A login snapshot is sent immediately. In mode `off` it carries `mode=0` and no periodic updates follow. When the module is disabled, one synthetic snapshot (`0.0<TAB>0.0<TAB>0<TAB>0<TAB>1<TAB>0<TAB>0<TAB>0<TAB>0.0<TAB>0<TAB>0` after the prefix) tells the addon to display `Disabled`. When the state failed to load because persistence is unavailable, the snapshot reports `loaded=0` and `persistence=0`. Until any packet has been received, the addon stays on `Loading...`.

### Status icons

Three small status icons, one for satiety, one for hydration and one for cold, are shown by default even when the gauge window is hidden. They sit at the bottom right of the screen, can be moved by dragging, and their position is saved. The border color indicates the band: gray for normal (Comfortable for cold), yellow for warning (Chilly), orange for critical (Cold) and red for empty (Freezing). The tooltip shows the value out of 100, the band and the active mode. A left click on any icon shows or hides the gauge window. The icons use only built-in 3.3.5a textures (`INV_Misc_Food_15`, `INV_Drink_05` and `Spell_Frost_ChillingArmor`); no custom texture or client patch is needed. The status line under the gauges reports the mode, `Not persisted` when the save is unavailable and `Hot` in a hot zone; the reserved lethal flag is never set by the server, so it never appears.

Without the addon, everything remains functional through chat warnings, the hunger, thirst and cold debuffs, the login summary and the `.hungerthirst` command; the state tooltips then keep their original Blizzard text, and the entry chat line remains authoritative.

## Compatibility

- The module is server-side. It needs no client patch, no custom MPQ, no custom texture and no SQL import.
- The hunger states reuse two spells shipped with the stock 3.3.5a client, 52749 ("Voracious Appetite") and 55316 ("Ravenous Hunger"), and the thirst states reuse 25696 ("Drink") and 5909 ("Watered-down Beer"). No core content uses them. Their server-side spell data is patched in memory at `worldserver` startup; no DBC file, MPQ, client file or core source file is modified. The thirst spells used by earlier versions, 43155, 46583, 14869 and the retired 14870 ("Drunken Stupor"), are no longer applied; their auras are removed automatically from characters that still carry them.
- The mana-regeneration penalty relies on the canonical `SPELL_AURA_MOD_POWER_REGEN_PERCENT` aura only. No core patch is applied, so only the spirit-based regeneration portion is reduced; flat mana-per-5-seconds regeneration keeps its full value.
- It uses public AzerothCore script hooks (world configuration and startup, player load/login/update/save/logout/delete, and aura apply) and its own `hunger_thirst_state` table. No core file is modified.
- The configuration namespace (`HungerThirst.*`), the command (`.hungerthirst`) and the addon prefix (`HT`) are module-specific, so the module can be installed alongside other standalone modules.
- There is no direct coupling with `mod-hardcore`. The module never resurrects a character, never writes a death table and never kills: hunger and thirst apply percentage penalties only, and cold exposure drains health only down to a 1% floor with no death cause, so no module-caused death can trigger a hardcore policy.
- Cold exposure applies the stock `Chilled` spell (6136) unchanged, without patching its data: no client patch, DBC edit or MPQ change is involved. Warm fires are detected from `gameobject_template` entries and the spell-focus id, so no new game object or area trigger is needed.
- The module writes no death record and sends no global announcement. Its `LOG_INFO` and `LOG_DEBUG` output covers configuration, state persistence, consumable credits, state application and blur pulses only; there is no death line and no death history in the database.

## Limitations

- The 3.3.5a client exposes no native hunger, thirst or cold gauge. The visual display therefore depends on the optional addon; without it, only the state debuffs, chat messages and commands are available.
- The state debuffs show a Blizzard spell name and icon, localized by the client. Their tooltips keep the original Blizzard descriptions, which describe unrelated effects and not the configured penalties; the real values and the remedy appear in the entry chat line, the configuration and this README. The optional addon overrides those four tooltips with the effects and the remedy, using the default percentages; without it the Blizzard text stays. The `Chilled` aura uses the stock spell and tooltip as-is. No native server-side tooltip change is possible without a client patch. On the weakened hunger spell the client may briefly show the original short duration countdown, but the server keeps the aura as long as the state lasts.
- The drinking blur depends on the client rendering the `PLAYER_FAKE_INEBRIATION` field without an aura. Its intensities (35 and 45) are deliberately kept below the client's drunk threshold (50) so the blur stays a screen tint and never triggers the drunk stance or its walk animation. The reported sliding walk was not caused by this field but by the stock client stun carried by the now-retired dehydrated spell 14870, which is why that spell was replaced by 5909; the dehydrated blur intensity stays at 45. If the field were ignored by a client, the fallback would be a short aura (`SPELL_AURA_MOD_FAKE_INEBRIATE` or `SPELL_AURA_SCREEN_EFFECT`), which briefly shows a debuff icon. No client patch or MPQ is used in either case.
- The mana-regeneration penalty applies to the spirit-based regeneration only. The flat mana-per-5-seconds regeneration is unaffected in this core; reducing it would require a core change the module deliberately avoids.
- The module ships with `Mode = off`. Without explicit configuration it has no gameplay effect.
- The states exist only in `survival` mode. `HungerStates=0` leaves hunger with no gameplay effect; `ThirstStates=0` leaves hydration with no gameplay effect, no state and no blur. In both cases the corresponding band warnings are then not sent in `survival` (they remain available in `advisory`).
- Hunger and thirst never damage or kill: there is no damage-per-hit setting and no lethal mode. Hydration at 0 only applies the dehydrated state, and satiety at 0 the starving state. Cold exposure is the only health drain: it removes `ColdHealthDrainPct` of maximum health per `ColdDrainIntervalMs` at 100 cold, stops at a floor of 1% of maximum health (at least 1 health point) and cannot kill the character on its own.
- Cold and heat use static zone-ID lists (`ColdZones`, `HotZones`) and a game object probe. There is no weather, time-of-day or climate model; a zone is either cold, hot or neither, and a warm fire is a `SPELL_FOCUS` game object with focus id 4 or an entry listed in `ColdCampfireEntries`. Warm fires are only searched while the player is exposed in a cold zone, and the result is cached for 5 seconds.
- The cold health drain only runs in `survival` mode and only while the player is still exposed at 100 cold: stepping indoors, resting or reaching a fire stops it immediately and starts warming. Any pause (taxi, battleground, arena, duel, dead, GM exemption) also stops the tick, so no drain occurs during it.
- Well Fed buffs and similar auras do not restore the gauges. Only Food and Drink effects are credited, including food and drink items, feast or game object auras, and player-cast auras.
- Gauges do not decay while the character is offline.
- Commands work in game only, not from the server console.
- If persistence is unavailable, the affected session stays suspended even after the database recovers; a reconnect is required.

## Credits

- The [AzerothCore](https://www.azerothcore.org/) project and community, for the WotLK 3.3.5a core, the module skeleton and the script API.
- This module follows the AzerothCore skeleton-module repository layout and documentation structure.

See [LICENSE](LICENSE) for licensing information.
