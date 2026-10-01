#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace chat {

int listenTcp(std::uint16_t port, std::string& error);

std::uint16_t localPort(int fd);

// The non-loopback IPv4 address peers can use to reach this machine.
// Prefers the interface used for the given server, then any active interface.
std::string localIpAddress(const std::string& serverHost, std::uint16_t serverPort);

void acceptConnections(int listenFd, const std::atomic<bool>& running,
                       const std::function<void(int fd)>& onAccept);

bool setBlocking(int fd, bool blocking);

bool parseHostPort(const std::string& text, std::string& host, std::uint16_t& port,
                   std::uint16_t defaultPort);

void ignoreSigpipe();
void installShutdownHandlers();
bool shutdownRequested();

} // namespace chat
