# Online play

This package is CTR-NitroFueled-PC (same runtime, renderer, upscalers and settings) with the
serverless online play of [CTRNativePorted](https://github.com/guzuold/CTRNativePorted) and a
launcher in its "Nitro Cinema" style.

## How to play online

1. **SETTINGS → ONLINE & MULTI**: turn on **Multiplayer Online** and choose your player name
   (3–16 letters, digits, `-` or `_`). The launcher starts this PC's online service.
2. **FRIENDS** (top right): **COPY MY CODE**, send it to your friend, paste theirs and press
   **ADD**. You must add each other.
3. **PLAY**. Invite or JOIN friends from the game's own friends list, or use
   *Online > Matchmaking* for public races.

## What it is

- No central server: each player's launcher runs its own service; the services find each other
  on the BitTorrent DHT and talk over signed requests. Race traffic is the game's own UDP,
  directly between the games.
- Compatible with CTRNativePorted: the same friend codes, friends and public matchmaking.
- Sony and Activision servers are never contacted. The Demonware traffic key is replaced in
  memory with your own service's key (`src/runtime_online.c`); the game files are not changed.
- With Multiplayer Online off, nothing of this loads: the game runs exactly as
  CTR-NitroFueled-PC.

## Launcher language

Arabic or English (ADVANCED → Launcher language; Arabic by default on an Arabic Windows).
Technical terms stay in English. Arabic is shaped and laid out right to left by
`launcher/windows/arabic.cpp` through an optional hook in Dear ImGui
(`gpu/patches/imgui`, unused by the game's own overlay); titles use Lemonada (OFL-1.1, its
Arabic presentation forms mapped by `tools/arabic_presentation_forms.py`).

## Files and limits

- Identity, friends and the Pit Stop wallet: `runtime\user\` (keep it to keep your code; copy
  `runtime\user\identity` and `runtime\user\online` from CTRNativePorted, with both launchers
  closed, to keep the code you have there).
- One online launcher per PC at a time (the service ports are fixed).
- The router is asked to open the ports with UPnP. Public matchmaking needs at least one player
  reachable from the Internet; strict or carrier-grade NAT may prevent direct races.
- Races with more than two players are not verified upstream yet.

## Sources

`online/` holds the online service (`online_server/`), the guest functions (`hle/`,
`ctr_online.dll`) and the DHT (`dht/`, jech/dht, MIT), from CTRNativePorted (GPL-2.0), with the
changes needed to build them with MSYS2 CLANG64. `ctr_online_server.exe` is the same service on
its own.
