// SPDX-License-Identifier: GPL-2.0-or-later
// Online play in the launcher: this PC's online service (online/online_server, from
// guzuold/CTRNativePorted) runs inside the launcher while Multiplayer Online is on, as in
// CTRNativePorted's launcher. Its files are <port>\runtime\user\{identity,online}, the same
// layout, so an identity and friends list copied from CTRNativePorted keep their friend code.
// A separate translation unit: the service needs Winsock 2 before <windows.h>.
#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace launcher_online {

struct Friend {
    std::string code;
    std::string label;      // the online ID the friend's service reported, else the nickname
    bool online = false;    // the friend's service answered
    bool in_game = false;   // and its game is connected to it
    bool named = false;     // the friend's service ever answered (they added this PC's code)
};

struct Status {
    bool running = false;   // the service is up
    bool busy = false;      // starting or stopping in the background
    std::string error;      // why it could not start
    std::string friend_code;
    std::string public_ip, address_source;
    bool upnp_router = false;
};

// Starts or stops the service in the background (UPnP discovery takes a few seconds).
void Start(const std::filesystem::path& root, const std::string& player_name);
void Stop();
// Stop, then start again (the player name changed).
void Restart(const std::filesystem::path& root, const std::string& player_name);
// Stops the service and waits (launcher exit).
void Shutdown();
Status GetStatus();
// The friends list with live presence, in-game first.
std::vector<Friend> Friends(const std::filesystem::path& root);
// This PC's friend code (the identity is created on first use).
std::string OwnCode(const std::filesystem::path& root);
// "" when added, else the reason (in English; the caller translates the known ones).
std::string AddFriend(const std::filesystem::path& root, const std::string& code);
void RemoveFriend(const std::filesystem::path& root, const std::string& code);
// The game's environment for an online start (CTR_ONLINE=1 and the rest), after verifying
// this PC's service and saving its traffic key. False with the reason when it cannot start.
bool PrepareLaunch(const std::filesystem::path& root, const std::string& player_name,
                   std::map<std::wstring, std::wstring>& env, std::string& error);
// 3-16 letters, digits, '-' or '_' (the game's online ID rules).
bool ValidPlayerName(const std::string& name);

} // namespace launcher_online
