#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "console/crypto.h"

// Nonblocking TCP and mbedTLS streams for the console community client.
// Everything here runs on the community worker thread; mbedTLS is built
// without threading support on the consoles.
namespace console {

// This peer's disposable TLS certificate. Its public key is bound to the
// persistent Ed25519 identity by a signed certificate extension.
class Credentials {
public:
    explicit Credentials(const Identity& identity);
    ~Credentials();
    Credentials(const Credentials&) = delete;
    Credentials& operator=(const Credentials&) = delete;
    struct Context;
    Context& context() { return *context_; }

private:
    std::unique_ptr<Context> context_;
};

// The Ed25519 identity bound to a DER certificate, or empty if the binding or
// the certificate's self-signature is invalid. Matches Identity::certificatePeer().
std::string certificatePeer(const unsigned char* der, std::size_t size);

class Stream {
public:
    enum class State { Idle, Connecting, Plain, Handshaking, Secure, Closed };
    Stream();
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    // DNS can block; call only from the worker.
    bool connect(const std::string& host, unsigned port, std::uint64_t now);
    void startTls(Credentials& credentials, bool client);
    // Advances connection and handshake state and moves bytes in both
    // directions. Returns false once the stream has closed.
    bool poll(std::uint64_t now);
    void close(const std::string& reason = {});
    // Closes once buffered bytes are written and a relay has had time to
    // forward them: a relay drops a tunnel's unsent bytes when one side closes.
    void finish() { closing_ = true; }

    State state() const { return state_; }
    const std::string& error() const { return error_; }
    // Bytes received before TLS starts (relay frames). Anything left here
    // when TLS starts is fed to the handshake.
    std::string& plain() { return plain_; }
    void writePlain(const std::string& bytes);
    // Decrypted application data.
    std::string& received() { return received_; }
    bool write(const std::string& bytes); // false if the stream is not secure
    std::size_t backlog() const { return raw_.size(); }
    const std::string& peer() const { return peer_; } // verified identity

private:
    int socket_ = -1;
    State state_ = State::Idle;
    std::uint64_t deadline_ = 0;
    std::string error_;
    std::string plain_;
    std::string received_;
    std::string raw_; // ciphertext or plain bytes waiting for the socket
    std::string peer_;
    bool closing_ = false;
    std::uint64_t closeAt_ = 0;
    struct Tls;
    std::unique_ptr<Tls> tls_;
    void flush();
    bool fill(std::size_t limit);
    void secure();
    static int sendCallback(void* self, const unsigned char* bytes, std::size_t size);
    static int receiveCallback(void* self, unsigned char* bytes, std::size_t size);
};

} // namespace console
