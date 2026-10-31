# Online play

This package is CTR-NitroFueled-PC (same runtime, renderer, upscalers and settings) with the
online play of [OCTR-NitroFueled-PC](https://github.com/Ryu7w7/OCTR-NitroFueled-PC) on the
RyuNET community server, and a launcher in the "Nitro Cinema" style.

## How to play online

1. **SETTINGS → ONLINE & MULTI**: turn on **Multiplayer Online** and choose your own nickname
   (3–16 letters, digits, `-` or `_`; shared names such as Player or Guest are refused). The
   launcher checks it on the server and registers it for this PC. The server is
   `octr.ryu7w7.xyz` unless you enter another RyuNET server.
2. **FRIENDS** (top right): type a friend's nickname and **SEND FRIEND REQUEST**; accept or
   decline the requests you receive there too.
3. **PLAY**. From the game's own menus: public matches, private rooms, your friends list
   (invite or join them) and the Pit Stop.

## What it is

- The game's Demonware, PSN and friends traffic goes to the RyuNET server (a Demonware server
  written for this game); Sony and Activision servers are never contacted. The server's key
  replaces Demonware's in the executable for online starts only
  (`patches/CrashTeamRacingOnline*.xml`, in memory; the game files are not changed).
- Races connect the players' games directly (UDP 3074), as on the PS4, with the server's relay
  for players who cannot reach each other.
- With Multiplayer Online off, nothing of this is used: the game runs exactly as
  CTR-NitroFueled-PC.

## Your nickname

The first installation that plays with a nickname keeps it, through the key in
`ryunet_online.key` (next to the launcher, with a copy in `%APPDATA%\CTR-NitroFueled-PC`).
**Never share that file.** Requests to the server are signed with a hash of it; the key itself
never leaves your PC. Unzipping a new version over the folder, or into a new folder on the same
PC, keeps the nickname.

## Pit Stop

Online, the Pit Stop uses the items bought on the server (tied to your nickname). *Pit Stop: my
save's items* (ONLINE & MULTI) uses the ones unlocked in your own save instead.

## Connection problems

Some home connections (strict NAT, mobile data, carrier-grade NAT) cannot reach some other
players directly. If a match keeps failing with the same people, enabling UPnP on your router or
forwarding UDP 3074 to your PC helps.

## Launcher language

Arabic or English (ADVANCED → Launcher language; Arabic by default on an Arabic Windows).
Technical terms stay in English. Arabic is shaped and laid out right to left by
`launcher/windows/arabic.cpp` through an optional hook in Dear ImGui
(`gpu/patches/imgui`, unused by the game's own overlay); titles use Lemonada (OFL-1.1, its
Arabic presentation forms mapped by `tools/arabic_presentation_forms.py`).

## Sources

The online runtime is `src/runtime_net*.c`, `runtime_http.c`, `runtime_np_online.c`,
`runtime_friends.c` and `runtime_ryunet.c` (its services), with `run.py setup_online` and
`launcher/windows/online.cpp`, from OCTR-NitroFueled-PC (GPL-2.0) by Ryu7w7 and its
contributors.
