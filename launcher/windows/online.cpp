// SPDX-License-Identifier: GPL-2.0-or-later
// Online play in the launcher (online.h). The steps follow CTRNativePorted's launcher.cpp
// (host_toggled, friend_action, reload_friends and the online part of its launch).
#include "online.h"

#include "friend_network.hpp"
#include "friend_store.hpp"
#include "friend_verifier.hpp"
#include "json.hpp"
#include "launcher_online.hpp"
#include "protocol.hpp"

#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace launcher_online {
namespace {
std::mutex g_mutex;
ctr_online::Host* g_host = nullptr;        // created on first start, lives until Shutdown
ctr_online::HostStatus g_status;
bool g_busy = false;
std::thread g_worker;

fs::path FriendsPath(const fs::path& root) { return root / "runtime/user/online/friends.json"; }

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

// mode 0 stop, 1 start, 2 restart (a new player name: the service reports it to friends).
void Run(int mode, fs::path root, std::string name) {
    std::scoped_lock lock{g_mutex};
    if (g_busy) return;
    g_busy = true;
    if (g_worker.joinable()) g_worker.join();
    if (!g_host) g_host = new ctr_online::Host();
    g_worker = std::thread([mode, root, name] {
        const bool start = mode != 0;
        ctr_online::HostStatus status;
        try {
            if (mode != 1) g_host->stop();
            if (start) status = g_host->start(root, {}, true, name);
        } catch (const std::exception& e) {
            status = {};
            status.error = e.what();
        }
        std::scoped_lock done{g_mutex};
        g_status = start ? status : ctr_online::HostStatus{};
        g_busy = false;
    });
}
} // namespace

bool ValidPlayerName(const std::string& name) {
    if (name.size() < 3 || name.size() > 16) return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; });
}

void Start(const fs::path& root, const std::string& player_name) { Run(1, root, player_name); }
void Restart(const fs::path& root, const std::string& player_name) { Run(2, root, player_name); }
void Stop() { Run(0, {}, {}); }

void Shutdown() {
    {
        std::scoped_lock lock{g_mutex};
        if (g_worker.joinable()) g_worker.join();
    }
    if (g_host) {
        g_host->stop();
        delete g_host;
        g_host = nullptr;
    }
}

Status GetStatus() {
    std::scoped_lock lock{g_mutex};
    Status s;
    s.busy = g_busy;
    s.running = !g_busy && g_host && g_status.running && g_host->running();
    s.error = g_status.error;
    s.friend_code = g_status.friend_code;
    s.public_ip = g_status.public_ip;
    s.address_source = g_status.address_source;
    s.upnp_router = g_status.upnp_router;
    return s;
}

std::vector<Friend> Friends(const fs::path& root) {
    std::vector<ctr_online::FriendState> live;
    {
        std::scoped_lock lock{g_mutex};
        if (g_host && !g_busy) live = g_host->friends();
    }
    // The last name each friend's service reported (friend_names.json), also while offline.
    ctr_online::Json saved = ctr_online::Json::object();
    try {
        std::ifstream file(FriendsPath(root).parent_path() / "friend_names.json", std::ios::binary);
        std::stringstream text;
        text << file.rdbuf();
        if (file) saved = ctr_online::parse_json(text.str());
    } catch (const std::exception&) {}
    std::vector<Friend> rows;
    for (const auto& entry : ctr_online::FriendStore(FriendsPath(root)).entries()) {
        Friend row;
        row.code = entry.friend_code;
        std::string name;
        for (const auto& state : live)
            if (state.code == entry.friend_code) {
                name = state.name;
                row.online = state.online;
                row.in_game = state.in_game;
            }
        if (name.empty() && saved.is_object())
            if (const auto* n = saved.find(entry.friend_code); n && n->is_string()) name = n->text;
        row.named = !name.empty();
        row.label = name.empty() ? entry.nickname : name;
        if (row.label.empty()) row.label = entry.friend_code.substr(0, 10) + "...";
        rows.push_back(std::move(row));
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Friend& a, const Friend& b) {
        const auto rank = [](const Friend& f) { return f.in_game ? 0 : f.online ? 1 : 2; };
        return rank(a) < rank(b);
    });
    return rows;
}

std::string OwnCode(const fs::path& root) {
    {
        std::scoped_lock lock{g_mutex};
        if (!g_status.friend_code.empty()) return g_status.friend_code;
    }
    try { return ctr_online::local_friend_code(root); } catch (const std::exception&) { return {}; }
}

std::string AddFriend(const fs::path& root, const std::string& typed) {
    std::string code = typed;
    code.erase(0, code.find_first_not_of(" \t\r\n"));
    code.erase(code.find_last_not_of(" \t\r\n") + 1);
    ctr_online::FriendCodeInfo info;
    if (ctr_online::parse_friend_code(code, info) != ctr_online::CodeParseError::Ok) return "Invalid friend code.";
    if (info.code == OwnCode(root)) return "This is your own code.";
    std::string error;
    fs::create_directories(FriendsPath(root).parent_path());
    if (!ctr_online::FriendStore(FriendsPath(root)).add_or_update(info.code, "", &error))
        return error.empty() ? "Could not save the friends list." : error;
    return {};
}

void RemoveFriend(const fs::path& root, const std::string& code) {
    ctr_online::FriendStore(FriendsPath(root)).remove(code);
}

bool PrepareLaunch(const fs::path& root, const std::string& player_name, std::map<std::wstring, std::wstring>& env,
                   std::string& error) {
    const Status status = GetStatus();
    if (status.busy || !status.running) {
        error = "Wait for Online to connect.";
        return false;
    }
    try {
        // The game always talks to this PC's own service, which reaches friends' services and
        // the public matchmaking by itself.
        const std::string server_ip = "127.0.0.1";
        const auto verified = ctr_online::verify_candidate_endpoint(server_ip + ":8780", status.friend_code);
        if (verified.status != ctr_online::VerifyStatus::Verified)
            throw std::runtime_error("This PC's online service could not be verified.");
        const fs::path online_dir = root / "runtime/user/online";
        fs::create_directories(online_dir);
        const fs::path key_path = online_dir / "party_traffic_key.der";
        std::ofstream key(key_path, std::ios::binary | std::ios::trunc);
        key.write(reinterpret_cast<const char*>(verified.traffic_key.data()),
                  static_cast<std::streamsize>(verified.traffic_key.size()));
        key.close();
        if (!key) throw std::runtime_error("Could not save the online service's key.");
        env[L"CTR_ONLINE"] = L"1";
        env[L"CTR_ONLINE_SERVER"] = Widen(server_ip);
        env[L"CTR_ONLINE_TRAFFIC_KEY"] = key_path.wstring();
        env[L"CTR_ONLINE_PORT_OFFSET"] = std::to_wstring(ctr_online::kHostGamePortOffset);
        env[L"CTR_ONLINE_ACCOUNT"] = std::to_wstring(ctr_online::player_account(status.friend_code, player_name));
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}

} // namespace launcher_online
