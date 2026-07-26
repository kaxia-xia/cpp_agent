// Server: listens for UDP broadcast, establishes TCP connection with client,
// sends random number + server IP, expects number+1 back.
//
// Build:   cmake -B build && cmake --build build
// Run:     ./build/discovery-server [--udp-port 9999]
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
#include <vector>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// ── Defaults ─────────────────────────────────────────────────────────
constexpr uint16_t kDefaultUdpPort   = 9999;   // server UDP listen port
constexpr int      kTcpTimeoutSec    = 5;      // wait for client TCP response
constexpr int      kMaxPacketSize    = 1024;   // max UDP packet size
// (TCP backlog not needed — server connects outbound to client)

// ── Global interrupt flag ────────────────────────────────────────────
static std::atomic<bool> g_interrupted{false};
extern "C" void handle_sigint(int) {
    g_interrupted.store(true, std::memory_order_release);
}

// ── Utility: get a human-readable string from a sockaddr_in ──────────
std::string sockaddr_to_str(const sockaddr_in& addr) {
    char ip[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    // Explicitly construct as C string to avoid trailing null bytes
    return std::format("{}:{}", static_cast<const char*>(ip), ntohs(addr.sin_port));
}

// ── Utility: get server's local IP on a given (or any) interface ────
// Returns the first non-loopback IPv4 address, or 127.0.0.1 as fallback.
std::string get_local_ip() {
    ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0) {
        perror("getifaddrs");
        return "127.0.0.1";
    }

    std::string result = "127.0.0.1";
    for (ifaddrs* p = ifa; p != nullptr; p = p->ifa_next) {
        if (p->ifa_addr == nullptr) continue;
        if (p->ifa_addr->sa_family != AF_INET) continue;
        // Skip loopback
        if (p->ifa_flags & IFF_LOOPBACK) continue;

        auto* sin = reinterpret_cast<sockaddr_in*>(p->ifa_addr);
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
        result = ip;
        break;
    }
    freeifaddrs(ifa);
    return result;
}

// ── Utility: create a UDP socket bound to the given port ─────────────
int create_udp_socket(uint16_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket(UDP)");
        return -1;
    }

    // Allow address reuse (helpful for quick restarts)
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Enable broadcast reception
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("bind(UDP)");
        close(fd);
        return -1;
    }

    std::cout << std::format("[server] UDP listening on 0.0.0.0:{}\n", port);
    return fd;
}

// ── Utility: connect TCP to a client ─────────────────────────────────
int tcp_connect(const std::string& ip, uint16_t port, int timeout_sec) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket(TCP)");
        return -1;
    }

    // Set non-blocking for connect with timeout
    // We use select() to implement the timeout
    // First make the socket non-blocking
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        std::cerr << "[server] invalid IP: " << ip << '\n';
        close(fd);
        return -1;
    }

    int ret = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        perror("connect(TCP)");
        close(fd);
        return -1;
    }

    if (ret == 0) {
        // Connected immediately (unlikely for non-local connections)
        // Restore blocking mode
        fcntl(fd, F_SETFL, flags);
        return fd;
    }

    // Wait for connection to complete
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);

    timeval tv{};
    tv.tv_sec  = timeout_sec;
    tv.tv_usec = 0;

    ret = select(fd + 1, nullptr, &wfds, nullptr, &tv);
    if (ret <= 0) {
        if (ret == 0) {
            std::cerr << "[server] TCP connect timeout to " << ip << ':' << port << '\n';
        } else {
            perror("select(connect)");
        }
        close(fd);
        return -1;
    }

    // Check if connection succeeded
    int       sock_err = 0;
    socklen_t len      = sizeof(sock_err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_err, &len) < 0 || sock_err != 0) {
        std::cerr << "[server] TCP connect failed: " << strerror(sock_err) << '\n';
        close(fd);
        return -1;
    }

    // Restore blocking mode
    fcntl(fd, F_SETFL, flags);
    return fd;
}

// ── Utility: send all bytes on a socket ───────────────────────────────
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
// Returns empty string on timeout/error, otherwise received data.
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

// ── UDP broadcast receive ────────────────────────────────────────────
// Blocks until a UDP packet arrives or interrupted.
// Returns the client address and the message content.
struct UdpPacket {
    std::string  message;
    sockaddr_in  from;
    bool         valid = false;
};

UdpPacket recv_udp(int udp_fd) {
    UdpPacket pkt;
    std::array<char, kMaxPacketSize> buf{};

    sockaddr_in from{};
    socklen_t   from_len = sizeof(from);

    // Use select to allow checking g_interrupted periodically
    while (!g_interrupted.load(std::memory_order_acquire)) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(udp_fd, &rfds);

        timeval tv{};
        tv.tv_sec  = 1;  // poll every second
        tv.tv_usec = 0;

        int ret = select(udp_fd + 1, &rfds, nullptr, nullptr, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select(UDP)");
            return pkt;
        }
        if (ret == 0) continue;  // timeout, check interrupt flag

        ssize_t n = recvfrom(udp_fd, buf.data(), buf.size() - 1, 0,
                             reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recvfrom");
            return pkt;
        }

        pkt.message = std::string(buf.data(), static_cast<size_t>(n));
        pkt.from    = from;
        pkt.valid   = true;
        return pkt;
    }
    return pkt;
}

// ── Parse client TCP port from UDP discovery message ─────────────────
// Expected format: "DISCOVER|<tcp_port>"
// Returns the TCP port, or 0 on parse failure.
uint16_t parse_discover_message(std::string_view msg) {
    // Strip trailing whitespace/newlines
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r' || msg.back() == ' '))
        msg.remove_suffix(1);

    constexpr std::string_view prefix = "DISCOVER|";
    if (!msg.starts_with(prefix)) {
        std::cerr << "[server] unexpected UDP message: " << msg << '\n';
        return 0;
    }

    std::string_view port_str = msg.substr(prefix.size());
    int port = 0;
    auto [ptr, ec] = std::from_chars(port_str.data(), port_str.data() + port_str.size(), port);
    if (ec != std::errc{} || port < 1 || port > 65535) {
        std::cerr << "[server] bad TCP port in message: " << msg << '\n';
        return 0;
    }
    return static_cast<uint16_t>(port);
}

// ── Main server loop ─────────────────────────────────────────────────
int run_server(uint16_t udp_port) {
    // Disable stdout buffering so logs appear immediately (important when
    // running as a background process or redirecting output).
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    std::cout << "=== Discovery Server ===\n";

    // Get server's local IP (will be sent to client)
    std::string server_ip = get_local_ip();
    std::cout << "[server] local IP: " << server_ip << '\n';

    // Create UDP socket
    int udp_fd = create_udp_socket(udp_port);
    if (udp_fd < 0) return 1;

    // Random number generator
    std::random_device              rd;
    std::mt19937                    rng(rd());
    std::uniform_int_distribution<int> dist(1, 999'999'999);

    std::cout << "[server] waiting for client UDP broadcasts...\n";
    std::cout << "[server] press Ctrl+C to stop\n\n";

    while (!g_interrupted.load(std::memory_order_acquire)) {
        // ── Phase 1: wait for UDP broadcast ─────────────────────────
        std::cout << "[server] listening for UDP broadcast on port " << udp_port << "...\n";
        UdpPacket pkt = recv_udp(udp_fd);
        if (!pkt.valid) break;  // interrupted or error

        std::string client_ip = sockaddr_to_str(pkt.from);
        std::cout << std::format("[server] received UDP broadcast from {}\n", client_ip);
        std::cout << std::format("[server]   message: {}\n", pkt.message);

        // Parse the client's TCP port from the discovery message
        uint16_t client_tcp_port = parse_discover_message(pkt.message);
        if (client_tcp_port == 0) {
            std::cerr << "[server] failed to parse client TCP port, ignoring\n";
            continue;
        }

        // Extract client IP (just the IP, not port from UDP)
        char client_ip_str[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &pkt.from.sin_addr, client_ip_str, sizeof(client_ip_str));
        const char* cip = client_ip_str;
        std::cout << std::format("[server] client {} TCP port: {}\n", cip, client_tcp_port);

        // ── Phase 2: connect to client via TCP ──────────────────────
        std::cout << std::format("[server] connecting TCP to {}:{}...\n", cip, client_tcp_port);
        int tcp_fd = tcp_connect(client_ip_str, client_tcp_port, kTcpTimeoutSec);
        if (tcp_fd < 0) {
            std::cerr << "[server] TCP connection failed, going back to UDP listen\n\n";
            continue;
        }
        std::cout << "[server] TCP connection established\n";

        // ── Phase 3: send challenge (random number + server IP) ─────
        int random_num = dist(rng);
        std::string challenge = std::format("{}|{}", random_num, server_ip);
        std::cout << std::format("[server] sending challenge: \"{}\"\n", challenge);

        if (!send_all(tcp_fd, challenge.data(), challenge.size())) {
            std::cerr << "[server] failed to send challenge, closing TCP\n";
            close(tcp_fd);
            continue;
        }
        // Send newline as terminator
        if (!send_all(tcp_fd, "\n", 1)) {
            std::cerr << "[server] failed to send terminator, closing TCP\n";
            close(tcp_fd);
            continue;
        }

        // ── Phase 4: wait for client response ───────────────────────
        std::cout << std::format("[server] waiting for response (timeout {}s)...\n", kTcpTimeoutSec);
        std::string response = recv_with_timeout(tcp_fd, kTcpTimeoutSec);

        if (response.empty()) {
            std::cout << "[server] TIMEOUT - client did not respond, considering disconnected\n";
        } else {
            // Strip trailing whitespace
            while (!response.empty() &&
                   (response.back() == '\n' || response.back() == '\r' || response.back() == ' '))
                response.pop_back();

            std::cout << std::format("[server] received response: \"{}\"\n", response);

            // Parse the response number
            int response_num = 0;
            auto [ptr, ec] = std::from_chars(
                response.data(), response.data() + response.size(), response_num);
            int expected = random_num + 1;

            if (ec == std::errc{} && response_num == expected) {
                std::cout << std::format("[server] ✓ correct response ({} == {} + 1), client is alive\n",
                                         response_num, random_num);
            } else {
                std::cout << std::format("[server] ✗ incorrect response: got {}, expected {}\n",
                                         response_num, expected);
                std::cout << "[server] considering client disconnected\n";
            }
        }

        close(tcp_fd);
        std::cout << '\n';
    }

    close(udp_fd);
    std::cout << "\n[server] stopped.\n";
    return 0;
}

} // namespace

// ── main ─────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    uint16_t udp_port = kDefaultUdpPort;

    // Simple arg parsing
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if ((arg == "--udp-port" || arg == "-p") && i + 1 < argc) {
            int p = std::atoi(argv[++i]);
            if (p < 1 || p > 65535) {
                std::cerr << "invalid port: " << argv[i] << '\n';
                return 2;
            }
            udp_port = static_cast<uint16_t>(p);
        } else if (arg == "--help" || arg == "-h") {
            std::cout << R"(discovery-server: listen for UDP broadcasts, establish TCP, challenge client.

USAGE
  discovery-server [OPTIONS]

OPTIONS
  -p, --udp-port <port>   UDP listen port (default: 9999)
  -h, --help              show this help

PROTOCOL
  1. Server listens on UDP <port> for broadcasts
  2. Client sends:  DISCOVER|<client_tcp_port>
  3. Server connects TCP to client
  4. Server sends:  <random_number>|<server_ip>\n
  5. Client responds: <random_number + 1>\n
  6. If no response within 5s → client disconnected
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

    return run_server(udp_port);
}
