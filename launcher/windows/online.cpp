// SPDX-License-Identifier: GPL-2.0-or-later
// Online play in the launcher (online.h). The key, the signatures and the requests follow
// Ryu7w7/OCTR-NitroFueled-PC's launcher (OnlineKey, ServerKeyHash, ProofHeader, HttpRequest,
// AskNickStatus) and its run.py (claim_nick); the friends follow its src/runtime_friends.c.
#include "online.h"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <utility>

namespace fs = std::filesystem;

namespace launcher_online {
namespace {

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string Trim(std::string s) {
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
    return s;
}

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Hex(const unsigned char* data, size_t size) {
    static const char kHex[] = "0123456789abcdef";
    std::string hex;
    for (size_t i = 0; i < size; ++i) {
        hex += kHex[data[i] >> 4];
        hex += kHex[data[i] & 15];
    }
    return hex;
}

// ---- This installation's key (run.py online_key) ----

/// A second copy of the key outside the game folder, so that deleting, moving or re-downloading
/// the folder on the same PC keeps the nicknames.
fs::path BackupKeyPath() {
    const wchar_t* appdata = _wgetenv(L"APPDATA");
    return appdata && *appdata ? fs::path(appdata) / L"CTR-NitroFueled-PC" / L"ryunet_online.key" : fs::path();
}

std::string ReadKeyFile(const fs::path& path) {
    std::string key;
    if (path.empty()) return key;
    std::ifstream(path, std::ios::binary) >> key;
    key = Lower(key);
    if (key.size() != 64 || key.find_first_not_of("0123456789abcdef") != std::string::npos) key.clear();
    return key;
}

bool WriteKeyFile(const fs::path& path, const std::string& key) {
    if (path.empty()) return false;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << key << "\n";
    return bool(out);
}

/// Every key used on this PC (run.py known_keys): a nickname the current key does not own may
/// belong to another folder's, and the nickname check switches to it.
fs::path KeyringPath() {
    const fs::path backup = BackupKeyPath();
    return backup.empty() ? fs::path() : backup.parent_path() / L"ryunet_online_keys.txt";
}

std::vector<std::string> KnownKeys() {
    std::vector<std::string> keys;
    const fs::path path = KeyringPath();
    if (path.empty()) return keys;
    std::ifstream in(path, std::ios::binary);
    for (std::string line; std::getline(in, line);) {
        line = Lower(Trim(line));
        if (line.size() == 64 && line.find_first_not_of("0123456789abcdef") == std::string::npos &&
            std::find(keys.begin(), keys.end(), line) == keys.end())
            keys.push_back(line);
    }
    return keys;
}

void RememberKey(const std::string& key) {
    const fs::path path = KeyringPath();
    if (path.empty() || key.size() != 64) return;
    const std::vector<std::string> keys = KnownKeys();
    if (std::find(keys.begin(), keys.end(), key) != keys.end()) return;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream(path, std::ios::binary | std::ios::app) << key << "\n";
}

std::string OnlineKey(const fs::path& root) {
    const fs::path path = root / L"ryunet_online.key";
    const fs::path backup = BackupKeyPath();
    std::string key = ReadKeyFile(path);
    if (key.empty()) key = ReadKeyFile(backup);  // a new folder on a PC that played before
    if (key.empty()) {
        unsigned char raw[32];
        if (BCryptGenRandom(nullptr, raw, sizeof raw, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return {};
        key = Hex(raw, sizeof raw);
    }
    if (ReadKeyFile(path) != key && !WriteKeyFile(path, key)) return {};
    if (ReadKeyFile(backup).empty()) WriteKeyFile(backup, key);  // never overwrites another folder's
    RememberKey(ReadKeyFile(backup));
    RememberKey(key);
    return key;
}

// ---- Signed requests (the server's nick_proof.go) ----

/// SHA-256 of msg (HMAC-SHA256 with key when there is one), in lower-case hex; "" on failure.
std::string Sha256Hex(const std::string& msg, const std::string& key = {}) {
    unsigned char out[32];
    bool ok = false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    key.empty() ? 0 : BCRYPT_ALG_HANDLE_HMAC_FLAG) == 0) {
        BCRYPT_HASH_HANDLE hash = nullptr;
        // No secret at all for a plain hash: a non-null pointer, even with length 0, makes
        // BCryptCreateHash fail.
        PUCHAR secret = key.empty() ? nullptr : reinterpret_cast<PUCHAR>(const_cast<char*>(key.data()));
        if (BCryptCreateHash(alg, &hash, nullptr, 0, secret, ULONG(key.size()), 0) == 0) {
            ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(msg.data())), ULONG(msg.size()), 0) == 0 &&
                 BCryptFinishHash(hash, out, sizeof out, 0) == 0;
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return ok ? Hex(out, sizeof out) : std::string();
}

/// What one server sees of the key: HMAC-SHA256(key, host), as run.py server_key. Every server
/// gets a different value, so none can reuse it on another to take a nickname.
std::string ServerKey(const std::string& host, const std::string& key) {
    if (key.size() != 64) return {};
    std::string raw(32, '\0');
    for (int i = 0; i < 32; ++i) raw[size_t(i)] = static_cast<char>(std::stoi(key.substr(i * 2, 2), nullptr, 16));
    return Sha256Hex(Lower(Trim(host)), raw);
}

/// What the server stores of the server key (its keyHash): the key requests are signed with.
std::string ServerKeyHash(const std::string& host, const std::string& key) {
    const std::string server_key = ServerKey(host, key);
    return server_key.empty() ? std::string() : Sha256Hex(server_key);
}

/// Seconds to add to this PC's clock to get the server's, from the Date of its answers: it takes
/// signatures only within 5 minutes of its own clock.
std::atomic<long long> g_server_clock_offset{0};

void NoteServerClock(HINTERNET req) {
    SYSTEMTIME date{};
    DWORD size = sizeof date;
    FILETIME file{};
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_DATE | WINHTTP_QUERY_FLAG_SYSTEMTIME, WINHTTP_HEADER_NAME_BY_INDEX,
                             &date, &size, WINHTTP_NO_HEADER_INDEX) ||
        !SystemTimeToFileTime(&date, &file))
        return;
    const long long ticks = (static_cast<long long>(file.dwHighDateTime) << 32) | file.dwLowDateTime;
    const long long server = (ticks - 116444736000000000LL) / 10000000LL;  // 100 ns since 1601 -> Unix
    const long long offset = server - static_cast<long long>(std::time(nullptr));
    g_server_clock_offset = offset > 5 || offset < -5 ? offset : 0;
}

/// X-RyuNET-Proof for one request: good once, for a few minutes of the server's time.
std::string ProofHeader(const std::string& key_hash, const std::string& method, const std::string& target) {
    unsigned char raw[16];
    if (key_hash.empty() || BCryptGenRandom(nullptr, raw, sizeof raw, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return {};
    const std::string nonce = Hex(raw, sizeof raw);
    const std::string ts = std::to_string(static_cast<long long>(std::time(nullptr)) + g_server_clock_offset.load());
    const std::string mac = Sha256Hex(ts + "\n" + nonce + "\n" + method + "\n" + target, key_hash);
    return mac.empty() ? std::string() : "1." + ts + "." + nonce + "." + mac;
}

/// The server setting's host and HTTP port, split as run.py does ("host" or "host:port").
void SplitServer(const std::string& server, std::string& host, INTERNET_PORT& port) {
    host = server;
    port = 8080;  // run.py's online_http_port default
    const auto colon = server.find(':');
    if (colon == std::string::npos || server.front() == '[') return;
    const std::string digits = server.substr(colon + 1);
    if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) return;
    host = server.substr(0, colon);
    port = INTERNET_PORT(std::atoi(digits.c_str()));
}

/// A request to the server's HTTP port, signed with key for that server; register_key also sends
/// the key's hash (a nickname claim). The body ("" when the server could not be reached) and the
/// HTTP status. A request signed with a clock found off by minutes goes again once.
std::string HttpRequest(const std::string& server, const std::string& key, const wchar_t* method,
                        const std::wstring& path, int* status_out = nullptr, bool register_key = false) {
    std::string host;
    INTERNET_PORT port = 0;
    SplitServer(server, host, port);
    const std::string key_hash = ServerKeyHash(host, key);
    std::string body;
    DWORD status = 0;
    HINTERNET session = WinHttpOpen(L"CTR-NitroFueled-PC launcher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return body;
    WinHttpSetTimeouts(session, 4000, 4000, 4000, 4000);
    HINTERNET conn = WinHttpConnect(session, Widen(host).c_str(), port, 0);
    for (int attempt = 1; conn && attempt <= 2; ++attempt) {
        const long long offset = g_server_clock_offset.load();
        const std::string proof = ProofHeader(key_hash, Narrow(method), Narrow(path));
        HINTERNET req = WinHttpOpenRequest(conn, method, path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        std::wstring headers;
        if (!proof.empty()) headers += L"X-RyuNET-Proof: " + Widen(proof) + L"\r\n";
        if (register_key && !key_hash.empty()) headers += L"X-RyuNET-Key-Hash: " + Widen(key_hash) + L"\r\n";
        body.clear();
        status = 0;
        bool answered = false;
        if (req && WinHttpSendRequest(req, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                      headers.empty() ? 0 : DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            answered = true;
            NoteServerClock(req);
            DWORD size = sizeof status;
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &status, &size, WINHTTP_NO_HEADER_INDEX);
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(req, &avail) && avail && body.size() < 65536) {
                std::string chunk(avail, '\0');
                DWORD got = 0;
                if (!WinHttpReadData(req, chunk.data(), avail, &got) || !got) break;
                body.append(chunk.data(), got);
            }
        }
        if (req) WinHttpCloseHandle(req);
        const long long moved = g_server_clock_offset.load() - offset;
        if (!answered || proof.empty() || (moved <= 240 && moved >= -240)) break;
    }
    if (conn) WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    if (status_out) *status_out = int(status);
    return body;
}

/// A string field of a small flat JSON answer ("" when missing).
std::string JsonField(const std::string& body, const char* key) {
    const std::string field = std::string("\"") + key + "\":\"";
    const auto at = body.find(field);
    if (at == std::string::npos) return {};
    const auto end = body.find('"', at + field.size());
    return end == std::string::npos ? std::string() : body.substr(at + field.size(), end - at - field.size());
}

// ---- The background thread ----

std::mutex g_mutex;
std::condition_variable g_wake;
std::thread g_thread;
bool g_quit = false;
unsigned g_generation = 0;  // a new Start/Stop/Restart
fs::path g_root;
std::string g_server, g_nick;
bool g_started = false;
Status g_status;
std::vector<Friend> g_friends;
std::deque<std::pair<std::string, std::string>> g_actions;
std::string g_result, g_result_who;
bool g_refresh_now = false;

/// The nickname's status for this installation, claiming it when it is free; "" when running.
std::string CheckNickname(const fs::path& root, const std::string& server, const std::string& nick, std::string& key) {
    key = OnlineKey(root);
    if (key.empty()) return "Could not save the online key next to the launcher.";
    const std::wstring path = L"/ryunet/nick?name=" + Widen(nick);  // [A-Za-z0-9_-]
    std::string body = HttpRequest(server, key, L"GET", path);
    if (body.empty()) return "Could not reach the RyuNET server.";
    std::string status = JsonField(body, "status");
    if (status == "taken") {
        // Another installation on this PC may own it: this folder takes its key.
        for (const std::string& other : KnownKeys()) {
            if (other == key) continue;
            if (JsonField(HttpRequest(server, other, L"GET", path), "status") == "yours" &&
                WriteKeyFile(root / L"ryunet_online.key", other)) {
                key = other;
                status = "yours";
                break;
            }
        }
    }
    if (const std::string banned = JsonField(body, "banned"); !banned.empty()) return "Banned from the server: " + banned;
    if (status == "taken") return "This nickname belongs to another player: choose another.";
    if (status == "reserved") return "That nickname is reserved: choose another.";
    if (status == "invalid") return "Invalid nickname.";
    if (status == "free") {
        // Registered for this installation now (run.py claim_nick does the same before a start).
        const std::string claimed = HttpRequest(server, key, L"POST", L"/ryunet/nick/claim?name=" + Widen(nick), nullptr, true);
        const std::string word = JsonField(claimed, "status");
        if (word == "taken") return "This nickname belongs to another player: choose another.";
    }
    return {};  // "yours", just claimed, or an older server without the check
}

/// The friends list: "friend <online> <players> <max> <name>", "incoming <name>", "outgoing <name>".
bool RefreshFriends(const std::string& server, const std::string& key, const std::string& nick, std::vector<Friend>& out) {
    int status = 0;
    const std::string body = HttpRequest(server, key, L"GET", L"/ryunet/friends?name=" + Widen(nick) + L"&format=text", &status);
    if (status != 200) return false;
    out.clear();
    size_t at = 0;
    while (at < body.size()) {
        size_t end = body.find('\n', at);
        if (end == std::string::npos) end = body.size();
        const std::string line = Trim(body.substr(at, end - at));
        at = end + 1;
        char name[64] = {};
        int online = 0, players = 0, max = 0;
        Friend f;
        if (std::sscanf(line.c_str(), "friend %d %d %d %63s", &online, &players, &max, name) == 4) {
            f.online = online != 0;
            f.in_room = players > 1;
        } else if (std::sscanf(line.c_str(), "incoming %63s", name) == 1) {
            f.incoming = true;
        } else if (std::sscanf(line.c_str(), "outgoing %63s", name) == 1) {
            f.outgoing = true;
        } else {
            continue;
        }
        f.name = name;
        if (!f.name.empty()) out.push_back(std::move(f));
    }
    std::stable_sort(out.begin(), out.end(), [](const Friend& a, const Friend& b) {
        const auto rank = [](const Friend& f) { return f.incoming ? 0 : f.in_room ? 1 : f.online ? 2 : f.outgoing ? 4 : 3; };
        return rank(a) < rank(b);
    });
    return true;
}

void Worker() {
    std::unique_lock lock{g_mutex};
    unsigned checked = ~0u;
    ULONGLONG next_check = 0, next_friends = 0;
    std::string key;
    while (!g_quit) {
        const unsigned generation = g_generation;
        if (!g_started) {
            g_status = {};
            g_friends.clear();
            g_wake.wait(lock, [&] { return g_quit || g_generation != generation; });
            continue;
        }
        const fs::path root = g_root;
        const std::string server = g_server, nick = g_nick;
        const ULONGLONG now = GetTickCount64();
        if (checked != generation || (!g_status.running && now >= next_check)) {
            g_status.busy = true;
            lock.unlock();
            const std::string error = CheckNickname(root, server, nick, key);
            lock.lock();
            if (g_generation != generation) continue;
            checked = generation;
            g_status.busy = false;
            g_status.running = error.empty();
            g_status.error = error;
            next_check = GetTickCount64() + 30000;  // asked again while it fails
            g_refresh_now = g_status.running;
        }
        if (g_status.running && !g_actions.empty()) {
            const auto [action, who] = g_actions.front();
            g_actions.pop_front();
            const char* arg = action == "accept" || action == "decline" ? "from" : action == "remove" ? "who" : "to";
            lock.unlock();
            int status = 0;
            const std::string body = HttpRequest(server, key, L"POST",
                                                 L"/ryunet/friends/" + Widen(action) + L"?name=" + Widen(nick) + L"&" +
                                                     Widen(arg) + L"=" + Widen(who) + L"&format=text",
                                                 &status);
            char word[24] = "error";
            if (status == 200) std::sscanf(body.c_str(), "status %23s", word);
            else if (status == 403) std::snprintf(word, sizeof word, "refused");
            lock.lock();
            g_result = word;
            g_result_who = who;
            g_refresh_now = true;
            continue;
        }
        if (g_status.running && (g_refresh_now || GetTickCount64() >= next_friends)) {
            g_refresh_now = false;
            next_friends = GetTickCount64() + 8000;
            lock.unlock();
            std::vector<Friend> friends;
            const bool ok = RefreshFriends(server, key, nick, friends);
            lock.lock();
            if (g_generation == generation && ok) g_friends = std::move(friends);
            continue;
        }
        // Requests and refreshes wait for a running check (no spinning while it fails).
        g_wake.wait_for(lock, std::chrono::seconds(1), [&] {
            return g_quit || g_generation != generation || (g_status.running && (g_refresh_now || !g_actions.empty()));
        });
    }
}

void Set(bool started, const fs::path& root, const std::string& server, const std::string& nick) {
    std::scoped_lock lock{g_mutex};
    if (!g_thread.joinable()) g_thread = std::thread(Worker);
    g_started = started;
    g_root = root;
    g_server = Trim(server).empty() ? kDefaultServer : Trim(server);
    g_nick = nick;
    g_status = {};
    g_status.busy = started;
    g_friends.clear();
    g_actions.clear();
    g_result.clear();
    ++g_generation;
    g_wake.notify_all();
}

} // namespace

bool ValidPlayerName(const std::string& name) {
    if (name.size() < 3 || name.size() > 16) return false;
    if (!std::all_of(name.begin(), name.end(), [](unsigned char c) { return c < 128 && (std::isalnum(c) || c == '_' || c == '-'); }))
        return false;
    // The server derives the player's ID from the nickname: names many players share are refused
    // (run.py RESERVED_NICKS).
    static const char* const kReserved[] = {"ryupc", "ryunet", "player", "jugador", "guest"};
    const std::string lower = Lower(name);
    return std::none_of(std::begin(kReserved), std::end(kReserved), [&](const char* r) { return lower == r; });
}

void Start(const fs::path& root, const std::string& server, const std::string& nickname) { Set(true, root, server, nickname); }
void Restart(const fs::path& root, const std::string& server, const std::string& nickname) { Set(true, root, server, nickname); }
void Stop() { Set(false, {}, {}, {}); }

void Shutdown() {
    {
        std::scoped_lock lock{g_mutex};
        g_quit = true;
        g_wake.notify_all();
    }
    if (g_thread.joinable()) g_thread.join();
}

Status GetStatus() {
    std::scoped_lock lock{g_mutex};
    return g_status;
}

std::vector<Friend> Friends() {
    std::scoped_lock lock{g_mutex};
    return g_friends;
}

void FriendAction(const std::string& action, const std::string& who) {
    std::scoped_lock lock{g_mutex};
    if (!g_started || who.empty() || who.size() > 16 ||
        !std::all_of(who.begin(), who.end(), [](unsigned char c) { return c < 128 && (std::isalnum(c) || c == '_' || c == '-'); }))
        return;
    g_actions.emplace_back(action, who);
    g_wake.notify_all();
}

std::string LastResult(std::string* who) {
    std::scoped_lock lock{g_mutex};
    if (who) *who = g_result_who;
    return g_result;
}

std::string PrepareLaunch() {
    const Status status = GetStatus();
    if (status.busy) return "Wait for Online to connect.";
    if (!status.running) return status.error.empty() ? "Not connected." : status.error;
    return {};
}

} // namespace launcher_online
