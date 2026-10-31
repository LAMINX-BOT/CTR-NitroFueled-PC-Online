// The launcher's RyuNET client (launcher/windows/online.cpp) against a fake server on this PC: the
// nickname is checked and claimed with the installation's key (never sent), the friends list and
// the requests are read, an action goes out signed, and a nickname owned by someone else stops
// the online start. Nothing leaves 127.0.0.1.
#include "../launcher/windows/online.h"

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

static int failures;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static std::mutex g_seen_mutex;
static std::vector<std::string> g_seen;  // "METHOD path proof keyhash"
static std::atomic<bool> g_taken{false};

static std::string Header(const std::string& request, const char* name) {
    const std::string key = std::string("\r\n") + name + ": ";
    const auto at = request.find(key);
    if (at == std::string::npos) return "-";
    return request.substr(at + key.size(), request.find("\r\n", at + key.size()) - at - key.size());
}

static void Serve(SOCKET listener) {
    for (;;) {
        SOCKET c = accept(listener, nullptr, nullptr);
        if (c == INVALID_SOCKET) return;
        std::string request;
        char buf[4096];
        while (request.find("\r\n\r\n") == std::string::npos) {
            const int n = recv(c, buf, sizeof buf, 0);
            if (n <= 0) break;
            request.append(buf, size_t(n));
        }
        const std::string method = request.substr(0, request.find(' '));
        const std::string target = request.substr(method.size() + 1, request.find(' ', method.size() + 1) - method.size() - 1);
        const std::string path = target.substr(0, target.find('?'));
        {
            std::scoped_lock lock{g_seen_mutex};
            g_seen.push_back(method + " " + target + " " + Header(request, "X-RyuNET-Proof") + " " +
                             Header(request, "X-RyuNET-Key-Hash"));
        }
        std::string body, type = "application/json";
        if (path == "/ryunet/nick") body = g_taken ? R"({"status":"taken"})" : R"({"status":"free"})";
        else if (path == "/ryunet/nick/claim") body = R"({"status":"claimed"})";
        else if (path == "/ryunet/friends") {
            type = "text/plain";
            body = "friend 0 0 0 Sleepy\nfriend 1 3 8 Racer_7\nincoming Newbie\noutgoing Waiting1\n";
        } else if (path == "/ryunet/friends/request") {
            type = "text/plain";
            body = "status sent\n";
        }
        const std::string reply = "HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\nContent-Length: " +
                                  std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
        send(c, reply.data(), int(reply.size()), 0);
        closesocket(c);
    }
}

template <class F> static bool WaitFor(F done) {
    for (int i = 0; i < 200; ++i) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    int len = sizeof addr;
    getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len);
    listen(listener, 16);
    std::thread(Serve, listener).detach();
    const std::string server = "127.0.0.1:" + std::to_string(ntohs(addr.sin_port));

    // The key goes next to the launcher and to %APPDATA%: both in a folder of this test.
    const fs::path root = fs::current_path() / (L"ryunet-launcher-test-" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(root);
    SetEnvironmentVariableW(L"APPDATA", (root / L"appdata").c_str());
    _wputenv_s(L"APPDATA", (root / L"appdata").c_str());

    CHECK(launcher_online::ValidPlayerName("Racer_7"));
    CHECK(!launcher_online::ValidPlayerName("Player"));
    CHECK(!launcher_online::ValidPlayerName("guest"));
    CHECK(!launcher_online::ValidPlayerName("ab"));
    CHECK(!launcher_online::ValidPlayerName("bad name"));

    launcher_online::Start(root, server, "Tester");
    CHECK(WaitFor([] { return launcher_online::GetStatus().running; }));
    CHECK(launcher_online::PrepareLaunch().empty());
    std::string key;
    std::ifstream(root / L"ryunet_online.key") >> key;
    CHECK(key.size() == 64);
    std::string backup;
    std::ifstream(root / L"appdata" / L"CTR-NitroFueled-PC" / L"ryunet_online.key") >> backup;
    CHECK(backup == key);
    CHECK(WaitFor([] { return launcher_online::Friends().size() == 4; }));
    const auto friends = launcher_online::Friends();
    if (friends.size() == 4) {
        // A request first, then online friends in a room, online, offline, and the asked ones.
        CHECK(friends[0].name == "Newbie" && friends[0].incoming);
        CHECK(friends[1].name == "Racer_7" && friends[1].online && friends[1].in_room);
        CHECK(friends[2].name == "Sleepy" && !friends[2].online);
        CHECK(friends[3].name == "Waiting1" && friends[3].outgoing);
    }
    launcher_online::FriendAction("request", "Racer_8");
    launcher_online::FriendAction("request", "bad name");  // never sent
    std::string who;
    CHECK(WaitFor([&] { return launcher_online::LastResult(&who) == "sent"; }));
    CHECK(who == "Racer_8");
    {
        std::scoped_lock lock{g_seen_mutex};
        bool claimed = false, requested = false;
        for (const auto& line : g_seen) {
            CHECK(line.find(key) == std::string::npos);  // the key never crosses the network
            const bool signed_ = line.find(" 1.") != std::string::npos;
            if (line.rfind("POST /ryunet/nick/claim?name=Tester ", 0) == 0) claimed = signed_ && line.back() != '-';  // with the key's hash
            if (line.rfind("POST /ryunet/friends/request?name=Tester&to=Racer_8&format=text ", 0) == 0) requested = signed_;
            CHECK(line.find("to=bad") == std::string::npos);  // an invalid nickname is never sent
        }
        CHECK(claimed);
        CHECK(requested);
    }

    // Someone else's nickname: no online start, and the reason.
    g_taken = true;
    launcher_online::Restart(root, server, "Taken_1");
    CHECK(WaitFor([] { const auto s = launcher_online::GetStatus(); return !s.busy && !s.error.empty(); }));
    CHECK(!launcher_online::GetStatus().running);
    CHECK(launcher_online::PrepareLaunch() == "This nickname belongs to another player: choose another.");

    // No server: the reason, not a hang.
    closesocket(listener);
    launcher_online::Restart(root, "127.0.0.1:9", "Tester");
    CHECK(WaitFor([] { const auto s = launcher_online::GetStatus(); return !s.busy && !s.error.empty(); }));
    CHECK(launcher_online::PrepareLaunch() == "Could not reach the RyuNET server.");

    launcher_online::Shutdown();
    std::error_code ec;
    fs::remove_all(root, ec);
    if (failures) return 1;
    std::puts("launcher online: ok");
    return 0;
}
