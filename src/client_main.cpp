// Client: sends UDP broadcast to discover server, receives TCP challenge,
// responds with number+1.
//
// Build:   cmake -B build && cmake --build build
// Run:     ./build/discovery-client [--tcp-port 8888] [--broadcast-ip 255.255.255.255] [--udp-port 9999]
//
#include <algorithm>
#include <atomic>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <random>
#include <signal.h>
#include <string>
#include <string_view>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// ── Defaults ─────────────────────────────────────────────────────────
constexpr uint16_t kDefaultUdpPort       = 9999;   // server UDP port
constexpr uint16_t kDefaultTcpPort       = 8888;   // client TCP listen port
constexpr const char* kDefaultBroadcastIp = "255.255.255.255";
constexpr int      kMaxPacketSize        = 1024;
constexpr int      kServerResponseTimeout = 10;    // wait for server TCP after broadcast
constexpr int      kTcpTimeoutSec        = 10;    // wait for challenge data

// ── Global interrupt flag ────────────────────────────────────────────
static std::atomic<bool> g_interrupted{false};
extern "C" void handle_sigint(int) {
    g_interrupted.store(true, std::memory_order_release);
}

// ── Utility: send all ─────────────────────────────────────────────────
bool send_all(int fd, const void* data, size_t len) {
    const char* p   = static_cast<const char*>(data);
    size_t      rem = len;
    while (rem > 0) {
        ssize_t n = send(fd, p, rem, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("send");
            return false;
        }
        p += n;
        rem -= static_cast<size_t>(n);
    }
    return true;
}

// ── Utility: receive with timeout ────────────────────────────────────
std::string recv_with_timeout(int fd, int timeout_sec) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);

    timeval tv{};
    tv.tv_sec  = timeout_sec;
    tv.tv_usec = 0;

    int ret = select(fd + 1, &rfds, nullptr, nullptr, &tv);
    if (ret <= 0) return {};

    std::array<char, kMaxPacketSize> buf{};
    ssize_t n = recv(fd, buf.data(), buf.size() - 1, 0);
    if (n <= 0) return {};

    return std::string(buf.data(), static_cast<size_t>(n));
}

// ── Send UDP broadcast ───────────────────────────────────────────────
bool send_udp_broadcast(const std::string& broadcast_ip, uint16_t udp_port,
                        const std::string& message) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket(UDP)");
        return false;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(udp_port);
    if (inet_pton(AF_INET, broadcast_ip.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "[client] invalid broadcast IP: " << broadcast_ip << '\n';
        close(fd);
        return false;
    }

    ssize_t n = sendto(fd, message.data(), message.size(), 0,
                       reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    close(fd);

    if (n < 0) {
        perror("sendto");
        return false;
    }

    std::cout << std::format("[client] sent UDP broadcast to {}:{}\n", broadcast_ip, udp_port);
    return true;
}

// ── Create TCP listen socket ─────────────────────────────────────────
int create_tcp_listen_socket(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket(TCP)");
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind(TCP)");
        close(fd);
        return -1;
    }

    if (listen(fd, 1) < 0) {
        perror("listen(TCP)");
        close(fd);
        return -1;
    }

    std::cout << std::format("[client] TCP listening on 0.0.0.0:{}\n", port);
    return fd;
}

// ── Accept a single TCP connection with timeout ──────────────────────
int accept_with_timeout(int listen_fd, int timeout_sec) {
    while (!g_interrupted.load(std::memory_order_acquire)) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);

        timeval tv{};
        tv.tv_sec  = 1;
        tv.tv_usec = 0;

        int ret = select(listen_fd + 1, &rfds, nullptr, nullptr, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select(accept)");
            return -1;
        }
        if (ret == 0) {
            // 1-second tick: check overall timeout and interrupt
            timeout_sec -= 1;
            if (timeout_sec <= 0) {
                std::cerr << "[client] timeout waiting for server TCP connection\n";
                return -1;
            }
            continue;
        }

        sockaddr_in from{};
        socklen_t   from_len = sizeof(from);
        int client_fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&from), &from_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            return -1;
        }

        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        std::cout << std::format("[client] accepted TCP connection from {}:{}\n",
                                 static_cast<const char*>(ip), ntohs(from.sin_port));
        return client_fd;
    }
    return -1;
}

// ── Main client run ──────────────────────────────────────────────────
int run_client(uint16_t tcp_port, const std::string& broadcast_ip, uint16_t udp_port) {
    // Disable stdout buffering
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    std::cout << "=== Discovery Client ===\n";

    // ── Step 1: create TCP listen socket ────────────────────────────
    int listen_fd = create_tcp_listen_socket(tcp_port);
    if (listen_fd < 0) return 1;

    // ── Step 2: send UDP broadcast ──────────────────────────────────
    std::string discover_msg = std::format("DISCOVER|{}", tcp_port);
    if (!send_udp_broadcast(broadcast_ip, udp_port, discover_msg)) {
        close(listen_fd);
        return 1;
    }

    // ── Step 3: wait for server to connect via TCP ──────────────────
    std::cout << std::format("[client] waiting for server to connect (timeout {}s)...\n",
                             kServerResponseTimeout);
    int server_fd = accept_with_timeout(listen_fd, kServerResponseTimeout);
    if (server_fd < 0) {
        close(listen_fd);
        return 1;
    }
    // We no longer need the listen socket
    close(listen_fd);

    // ── Step 4: receive challenge from server ───────────────────────
    std::cout << std::format("[client] waiting for challenge (timeout {}s)...\n", kTcpTimeoutSec);
    std::string challenge = recv_with_timeout(server_fd, kTcpTimeoutSec);
    if (challenge.empty()) {
        std::cerr << "[client] no challenge received from server\n";
        close(server_fd);
        return 1;
    }

    // Strip trailing whitespace/newlines
    while (!challenge.empty() &&
           (challenge.back() == '\n' || challenge.back() == '\r' || challenge.back() == ' '))
        challenge.pop_back();

    std::cout << std::format("[client] received challenge: \"{}\"\n", challenge);

    // Parse: <random_number>|<server_ip>
    auto sep_pos = challenge.find('|');
    if (sep_pos == std::string::npos) {
        std::cerr << "[client] malformed challenge (no separator)\n";
        close(server_fd);
        return 1;
    }

    std::string num_str = challenge.substr(0, sep_pos);
    std::string server_ip = challenge.substr(sep_pos + 1);

    int random_num = 0;
    auto [ptr, ec] = std::from_chars(num_str.data(), num_str.data() + num_str.size(), random_num);
    if (ec != std::errc{}) {
        std::cerr << "[client] malformed challenge (bad number: " << num_str << ")\n";
        close(server_fd);
        return 1;
    }

    std::cout << std::format("[client] server IP: {}\n", server_ip);
    std::cout << std::format("[client] random number: {}\n", random_num);

    // ── Step 5: compute and send response ───────────────────────────
    int response_num = random_num + 1;
    std::string response = std::format("{}\n", response_num);
    std::cout << std::format("[client] sending response: {}\n", response_num);

    if (!send_all(server_fd, response.data(), response.size())) {
        std::cerr << "[client] failed to send response\n";
        close(server_fd);
        return 1;
    }

    std::cout << "[client] ✓ done - response sent successfully\n";

    close(server_fd);
    return 0;
}

} // namespace

// ── main ─────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    uint16_t    tcp_port     = kDefaultTcpPort;
    uint16_t    udp_port     = kDefaultUdpPort;
    std::string broadcast_ip = kDefaultBroadcastIp;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if ((arg == "--tcp-port" || arg == "-t") && i + 1 < argc) {
            int p = std::atoi(argv[++i]);
            if (p < 1 || p > 65535) { std::cerr << "invalid port: " << argv[i] << '\n'; return 2; }
            tcp_port = static_cast<uint16_t>(p);
        } else if ((arg == "--udp-port" || arg == "-u") && i + 1 < argc) {
            int p = std::atoi(argv[++i]);
            if (p < 1 || p > 65535) { std::cerr << "invalid port: " << argv[i] << '\n'; return 2; }
            udp_port = static_cast<uint16_t>(p);
        } else if ((arg == "--broadcast-ip" || arg == "-b") && i + 1 < argc) {
            broadcast_ip = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << R"(discovery-client: broadcast UDP, receive TCP challenge, respond with number+1.

USAGE
  discovery-client [OPTIONS]

OPTIONS
  -t, --tcp-port <port>     TCP listen port for server connection (default: 8888)
  -u, --udp-port <port>     UDP port to broadcast to (default: 9999)
  -b, --broadcast-ip <ip>   Broadcast IP (default: 255.255.255.255)
  -h, --help                show this help

PROTOCOL
  1. Client sends UDP broadcast: DISCOVER|<tcp_port>
  2. Server connects via TCP to client
  3. Server sends:  <random_number>|<server_ip>
  4. Client responds: <random_number + 1>
)";
            return 0;
        }
    }

    // Install SIGINT handler
    struct sigaction sa{};
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);

    return run_client(tcp_port, broadcast_ip, udp_port);
}
