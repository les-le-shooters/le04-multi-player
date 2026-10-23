## English | [日本語](README_JP.md) | [中文](README_CN.md)

# le04-multi-player

An experimental modification that lets **two or three people play le04(連縁天影戦記　～ Brilliant pagoda or haze castle) together**, either on one PC or online. The multiplayer design follows
[RUEEE/th06_multi_net](https://github.com/RUEEE/th06_multi_net) and
[sbrik1111/th07_multi_player](https://github.com/sbrik1111/th07_multi_player).

The game itself and its data (`Le04.exe`, `dat.led`, …) are not included. Use your own legitimate copy of 1.20f.

## Current status

Local 2/3 players and online 2/3 players (with lockstep and rollback method) have been tested in story mode and practice mode.

This is still an experimental release, so desyncs and other bugs may remain.

## Known issues

- **A session can still desync in rare cases.** If the red "out of sync" message appears above the playfield, everyone should leave and rematch.

## What it does

* **Two or three player netplay** over UDP. The host is 1P, the guests are 2P and 3P; with three players the two guests also try to connect to each other directly
* **The host must be reachable** on its UDP port (IPv6, a port forward or a virtual LAN)
* **Local two or three player** with a keyboard and gamepads
* Each player picks their **own main, sub and support ship** and uses their **own upgrades**
* Lives, HP, bombs, power and score are per player; sake is shared
* Lives and power can transfer between players; and revival of a player who is out of lives and becomes a ghost
* **Predictive rollback** (on by default): remote input is predicted until it arrives, so input delay is usually 1–4 frames
* UI supports English / Japanese / Chinese / Korean

## Playing

1. Put `le04multi.dll` and `Le04_Multi_Maker.exe` in the game folder, next to `Le04.exe`
2. Double-click `Le04_Multi_Maker.exe` to create `Le04_Multi.exe`. You only do this once, and `Le04.exe` is not changed
3. Run `Le04_Multi.exe`. A settings window opens first
4. Choose a mode: Single player / Local 2 players / Local 3 players / Online: host / Online: guest. Guests enter the host's address in `Host address`
5. Press `Start`, then choose the mode, difficulty and ships in the game menus as usual
6. When online, the host chooses the mode, difficulty and stage (or practice selection) and starts the game; guests choose their own ships once connected and start the game. The game begins for everyone once all players have started

For local play, pick 2P's and 3P's ships in the settings window (they default to 1P's). To play the original game, run `Le04.exe` as usual.

Guests' connection status appears at the bottom left of the menu. 

Guests can pick any mode and stage; it is replaced by the host's choice when the game starts. While waiting for game beginning, press `Esc` to cancel and play alone.

The host has to allow `Le04_Multi.exe` through Windows Firewall (UDP, port 17723 by default). Over the internet, a direct IPv6 connection is recommended: `This PC's IPv6` in the settings window is the host's address, and the router's IPv6 firewall may also need to allow the port. Without IPv6, the host needs a port forward on the router or a virtual LAN (latency gets very high if the virtual LAN relays the traffic).

## Controls

In netplay each PC uses its own keyboard or pad with the original key layout, guests included.

| Action | Key |
| --- | --- |
| Move | Arrow keys |
| Shot | `Z` |
| Bomb | `X` |
| Focus | `Shift` |
| Dialogue | `Z` to advance, hold `X` to skip |
| Pause | `Esc` |

Local multiplayer shares one keyboard, so 2P uses a different set. 3P has no keyboard mapping and needs a pad.

| Action | 2P's key |
| --- | --- |
| Move | `W` / `A` / `S` / `D` |
| Shot | `J` |
| Bomb | `K` |
| Focus | `L` |

In local play, 1P, 2P and 3P can also use pads 1, 2 and 3.

## Launcher settings

The host chooses the player count, `Rollback` and the game rules; guests receive these session settings when they connect. Language and delay are local to each PC; delay 0 adjusts automatically to the network (recommended).

All rules are on by default:

- `Aimed shots target the nearest ship`: When unchecked, enemies aimed shots at 1P only
- `Every ship decides the route direction`: When unchecked, only 1P can move the route gauge
- `Some spells' player-targeted attacks hit all`: When unchecked, some spell cards hit one ship only (see "Some spell cards" below)

When the game starts, the mode, difficulty, stage or practice selection, random seed and the Instant Death mode option in Config follow the host. Ships (main / sub / support), upgrades and the Flashbomb=Bomb option in Config are each player's own.

## What changes in multiplayer

- **Damage to bosses** is ×0.75 with two players in play and ×2/3 with three
- **Enemy aimed shots** target the ship in play nearest to that enemy or boss by default
- **Items** fly to the nearest ship in play. Anyone crossing the auto-collect line triggers it, and the line height follows each player's upgrades
- **Route gauge** adds up everyone's offsets by default: moving to the same side makes it go faster, opposite sides cancel out
- **Life transfer**: overlap two ships within 24 pixels, then release shot and hold focus for 90 frames to give them one life; a player who is out of lives is revived directly
- **Power transfer**: overlap two ships within 24 pixels and tap shot five times to give them 1.00 power (you should keep at least 1.00)
- **Game over** happens only when everyone is out. Continuing revives everyone
- **Spell card bonuses** fail if anyone is hit
- **Stage-wide upgrades** use the best value among the players in play (ghosts don't count).
- **Some spell cards** (rule, on by default) target every ship, The same cards on other difficulties work the same way:
  - 無知「ヒトが恐るるは暴るる己」
  - 霊獣「青蛙神の欲」
  - 怨炎「断魂烈刃」
  - 「二つの煌き」
  - 平穏｢東西廻廊の雀｣
  - 再現と体験「リプレイ」
  - 「運命のルーレット」
  - 「背中に突き立てる思い」
- **HUD**: on each PC the game's own HUD shows that PC's player (local multiplayer: 1P). The others' LV, lives, HP, barrier, power, flashbomb gauge and Bomb are in the bottom-left panels
- **Disconnects**: if a guest drops, the host removes it ("xP left") and the others keep playing. The guest that loses the host continues alone
- **Replays** are not recorded in multiplayer

## Predictive rollback

Predictive rollback continues using recent remote input until the actual input arrives. If the prediction differs, the game rewinds to the affected frame and replays the simulation.If the prediction would go beyond 15 frames, or at moments that can't be undone (pause, menus, loading, scene changes), play waits for everyone's input.

## Building

Requires Windows and Visual Studio 2022 Build Tools (MSVC, x86).

```
build.bat
```

- Produces `build\le04multi.dll` and `build\Le04_Multi_Maker.exe`
- Instead of the Maker you can use Python with `pefile`: `python tools/patch_exe.py <your Le04.exe> <output Le04_Multi.exe>`

## Limitations

- Experimental. There is no NAT traversal or relay, and no encryption or authentication
- Intended for playing with people you trust, not for a public server
- Everyone must run the **same `Le04_Multi.exe`, `le04multi.dll` and the same game data**
- Latency or packet loss beyond the prediction window stalls the game while it waits for the missing input
- No replays in multiplayer

## Data and rights

- Do not redistribute any game files (`Le04.exe`, `dat.led`, …). They are required to run, but each player supplies them from their own copy of the game.
- `Le04_Multi.exe` is created from your own `Le04.exe` and contains the game's program. Do not redistribute it; anyone who needs it creates their own with `Le04_Multi_Maker.exe`
- Len'en Ten'eisenki ~ Brilliant Pagoda or Haze Castle belongs to its original author.

## Credits

- [RUEEE/th06_multi_net](https://github.com/RUEEE/th06_multi_net) for the netplay approach and multiplayer rules
- [sbrik1111/th07_multi_player](https://github.com/sbrik1111/th07_multi_player) for ghosts and revival, aiming at the nearest ship, and rollback
- [Dear ImGui](https://github.com/ocornut/imgui) (MIT) for the in-game panels and status display
