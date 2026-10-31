// SPDX-License-Identifier: GPL-2.0-or-later
// Online play in the launcher: a client of the RyuNET community server (from
// Ryu7w7/OCTR-NitroFueled-PC). While Multiplayer Online is on, a background thread checks that
// the server answers and that the nickname belongs to this installation (claiming it when it is
// free), and keeps the RyuNET friends list. The game itself is set up by run.py (setup_online)
// from bbport.ini's online, online_name and online_server.
// This installation's key is <port>\ryunet_online.key (with a copy in
// %APPDATA%\CTR-NitroFueled-PC), shared with run.py: requests are signed with its hash for the
// server and the key never leaves the PC.
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace launcher_online {

constexpr const char* kDefaultServer = "octr.ryu7w7.xyz";

struct Friend {
    std::string name;
    bool online = false;    // signed in to the server
    bool in_room = false;   // in a room with other players
    bool incoming = false;  // asked to be friends: accept or decline
    bool outgoing = false;  // this player asked them; waiting for their answer
};

struct Status {
    bool running = false;   // the server answered and the nickname is this installation's
    bool busy = false;      // asking the server
    std::string error;      // why not (English; the launcher translates the known ones)
};

// Starts or stops the background checks (Restart: another nickname or server).
void Start(const std::filesystem::path& root, const std::string& server, const std::string& nickname);
void Stop();
void Restart(const std::filesystem::path& root, const std::string& server, const std::string& nickname);
// Stops and waits (launcher exit).
void Shutdown();
Status GetStatus();
// The friends and the requests, from the last answer: online first.
std::vector<Friend> Friends();
// Sent in the background: request (to who), accept and decline (from who), remove.
void FriendAction(const std::string& action, const std::string& who);
// The server's word for the last action ("sent", "friends", "removed", "unknown", ...), and who.
std::string LastResult(std::string* who = nullptr);
// "" when the game can start online, else the reason (English).
std::string PrepareLaunch();
// 3-16 letters, digits, '-' or '_', and not a name many players share (Player, Guest, ...).
bool ValidPlayerName(const std::string& name);

} // namespace launcher_online
