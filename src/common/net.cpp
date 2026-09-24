#include "common/net.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstring>

#include "common/protocol.h"

namespace chat {
namespace {

constexpr int kAcceptPollMs = 200;

volatile std::sig_atomic_t gShutdownRequested = 0;

void handleShutdownSignal(int) {
    gShutdownRequested = 1;
}

} // namespace

int listenTcp(std::uint16_t port, std::string& error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    const std::string service = std::to_string(port);
    addrinfo* results = nullptr;
    const int rc = getaddrinfo(nullptr, service.c_str(), &hints, &results);
    if (rc != 0) {
        error = gai_strerror(rc);
        return -1;
    }

    error = "cannot bind port " + service;
    int listenFd = -1;
    for (addrinfo* entry = results; entry != nullptr && listenFd < 0; entry = entry->ai_next) {
        const int fd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (fd < 0) {
            error = std::strerror(errno);
            continue;
        }

        const int enable = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
        if (entry->ai_family == AF_INET6) {
            // Dual-stack is the default on Linux and macOS but not on Windows.
            const int disable = 0;
            setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &disable, sizeof(disable));
        }

        if (bind(fd, entry->ai_addr, entry->ai_addrlen) == 0 && ::listen(fd, SOMAXCONN) == 0) {
            listenFd = fd;
        }
        else {
            error = std::strerror(errno);
            ::close(fd);
        }
    }
    freeaddrinfo(results);

    if (listenFd >= 0) {
        error.clear();
    }
    return listenFd;
}

std::uint16_t localPort(int fd) {
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return 0;
    }
    if (address.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<sockaddr_in6*>(&address)->sin6_port);
    }
    if (address.ss_family == AF_INET) {
        return ntohs(reinterpret_cast<sockaddr_in*>(&address)->sin_port);
    }
    return 0;
}

std::string localIpAddress(const std::string& serverHost, std::uint16_t serverPort) {
    // A UDP connect selects a route without sending a packet or depending on
    // the server being up. The resulting socket has the interface's address.
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* results = nullptr;
    if (getaddrinfo(serverHost.c_str(), std::to_string(serverPort).c_str(), &hints,
                    &results) == 0) {
        for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
            const int fd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
            if (fd < 0) {
                continue;
            }
            sockaddr_in local{};
            socklen_t length = sizeof(local);
            const bool found = connect(fd, entry->ai_addr, entry->ai_addrlen) == 0 &&
                               getsockname(fd, reinterpret_cast<sockaddr*>(&local), &length) == 0 &&
                               local.sin_family == AF_INET &&
                               (ntohl(local.sin_addr.s_addr) >> 24) != 127;
            ::close(fd);
            if (found) {
                char address[INET_ADDRSTRLEN];
                if (inet_ntop(AF_INET, &local.sin_addr, address, sizeof(address)) != nullptr) {
                    freeaddrinfo(results);
                    return address;
                }
            }
        }
        freeaddrinfo(results);
    }

    // Loopback servers and unavailable DNS cannot identify a LAN route.
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0) {
        return {};
    }
    std::string address;
    // Prefer a LAN interface over a point-to-point tunnel when the server
    // route cannot help us choose.
    for (int pass = 0; pass < 2 && address.empty(); ++pass) {
        for (ifaddrs* entry = interfaces; entry != nullptr; entry = entry->ifa_next) {
            if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET ||
                (entry->ifa_flags & IFF_UP) == 0 || (entry->ifa_flags & IFF_LOOPBACK) != 0 ||
                (pass == 0 && (entry->ifa_flags & IFF_BROADCAST) == 0)) {
                continue;
            }
            const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(entry->ifa_addr);
            const std::uint32_t ip = ntohl(ipv4->sin_addr.s_addr);
            if (ip == 0 || (ip >> 24) == 127 || (ip >> 16) == 0xa9fe) {
                continue;
            }
            char text[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &ipv4->sin_addr, text, sizeof(text)) != nullptr) {
                address = text;
                break;
            }
        }
    }
    freeifaddrs(interfaces);
    return address;
}

void acceptConnections(int listenFd, const std::atomic<bool>& running,
                       const std::function<void(int fd)>& onAccept) {
    while (running.load()) {
        pollfd watched{listenFd, POLLIN, 0};
        if (::poll(&watched, 1, kAcceptPollMs) <= 0) {
            continue;
        }
        if ((watched.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return;
        }
        if ((watched.revents & POLLIN) == 0) {
            continue;
        }
        const int fd = accept(listenFd, nullptr, nullptr);
        if (fd >= 0) {
            onAccept(fd);
        }
    }
}

bool setBlocking(int fd, bool blocking) {
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    const int updated = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
    return fcntl(fd, F_SETFL, updated) == 0;
}

bool parseHostPort(const std::string& text, std::string& host, std::uint16_t& port,
                   std::uint16_t defaultPort) {
    if (text.empty()) {
        return false;
    }
    if (text.front() == '[') {
        const auto close = text.find(']');
        if (close == std::string::npos || close == 1) {
            return false;
        }
        host = text.substr(1, close - 1);
        if (close + 1 == text.size()) {
            port = defaultPort;
            return true;
        }
        return text[close + 1] == ':' && parsePort(text.substr(close + 2), port, false);
    }

    const auto colon = text.rfind(':');
    if (colon == std::string::npos) {
        host = text;
        port = defaultPort;
        return true;
    }
    if (text.find(':') != colon) {
        return false; // a bare IPv6 address needs brackets
    }
    host = text.substr(0, colon);
    return !host.empty() && parsePort(text.substr(colon + 1), port, false);
}

void ignoreSigpipe() {
    std::signal(SIGPIPE, SIG_IGN);
}

void installShutdownHandlers() {
    struct sigaction action{};
    action.sa_handler = handleShutdownSignal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
}

bool shutdownRequested() {
    return gShutdownRequested != 0;
}

} // namespace chat
