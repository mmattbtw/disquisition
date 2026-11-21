#include "console/net.h"

#include <mbedtls/ecp.h>
#include <mbedtls/md.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/oid.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "console/json.h"

namespace console {
namespace {
// 1.3.6.1.4.1.55555.1, the desktop's identity binding extension.
constexpr char kBindingOid[] = "\x2B\x06\x01\x04\x01\x83\xB2\x03\x01";
constexpr std::size_t kBindingOidSize = sizeof(kBindingOid) - 1;
constexpr char kBindingDomain[] = "disquisition/tls-identity/v1\n";

bool wouldBlock() {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}
bool connectPending(int code) {
#ifdef __3DS__
    // SO_ERROR's value is not translated by libctru. These are the SOC
    // service's raw EAGAIN, EALREADY and EINPROGRESS values, not POSIX errno.
    if (code == -6 || code == -7 || code == -26)
        return true;
#endif
#ifdef __WIIU__
    // wut translates syscall errno, but SO_ERROR can contain nsysnet error
    // numbers: EWOULDBLOCK=6, EALREADY=10, EINPROGRESS=22.
    if (code == 6 || code == 10 || code == 22 || code == 10006 || code == 10010 || code == 10022)
        return true;
#endif
    return code == EINPROGRESS || code == EALREADY || code == EWOULDBLOCK || code == EAGAIN;
}
std::string systemError(const char* operation, int code) {
    // Raw service error values cannot be passed to newlib's strerror safely.
    if (code < 0 || code >= 10000)
        return std::string(operation) + ": socket error [" + std::to_string(code) + "]";
    return std::string(operation) + ": " + std::strerror(code) + " [" + std::to_string(code) + "]";
}
std::string tlsError(const char* operation, int code) {
    char hexCode[16];
    std::snprintf(hexCode, sizeof(hexCode), "-0x%04X", unsigned(-code));
    return std::string(operation) + " failed (" + hexCode + ")";
}

int acceptAnyChain(void*, mbedtls_x509_crt*, int, std::uint32_t* flags) {
    // Peers use self-signed certificates. certificatePeer() checks the
    // self-signature and identity binding after the handshake instead.
    *flags = 0;
    return 0;
}

struct BindingSearch {
    std::string value;
    int found = 0;
};
int findBinding(void* context, const mbedtls_x509_crt*, const mbedtls_x509_buf* oid, int,
                const unsigned char* begin, const unsigned char* end) {
    auto* search = static_cast<BindingSearch*>(context);
    if (oid->len == kBindingOidSize && std::memcmp(oid->p, kBindingOid, kBindingOidSize) == 0) {
        search->value.assign(reinterpret_cast<const char*>(begin), std::size_t(end - begin));
        ++search->found;
    }
    return 0; // other unknown, non-critical extensions are ignored
}

int random(void*, unsigned char* out, std::size_t size) {
    try {
        randomBytes(out, size);
        return 0;
    }
    catch (const std::exception&) {
        return -1;
    }
}

void require(int result, const char* operation) {
    if (result != 0)
        throw std::runtime_error(tlsError(operation, result));
}
} // namespace

struct Credentials::Context {
    mbedtls_pk_context key;
    mbedtls_x509_crt certificate;
    mbedtls_ssl_config client;
    mbedtls_ssl_config server;
    Context() {
        mbedtls_pk_init(&key);
        mbedtls_x509_crt_init(&certificate);
        mbedtls_ssl_config_init(&client);
        mbedtls_ssl_config_init(&server);
    }
    ~Context() {
        mbedtls_ssl_config_free(&server);
        mbedtls_ssl_config_free(&client);
        mbedtls_x509_crt_free(&certificate);
        mbedtls_pk_free(&key);
    }
    void configure(mbedtls_ssl_config& config, int endpoint) {
        require(mbedtls_ssl_config_defaults(&config, endpoint, MBEDTLS_SSL_TRANSPORT_STREAM,
                                            MBEDTLS_SSL_PRESET_DEFAULT),
                "TLS configuration");
        mbedtls_ssl_conf_rng(&config, random, nullptr);
        mbedtls_ssl_conf_min_version(&config, MBEDTLS_SSL_MAJOR_VERSION_3,
                                     MBEDTLS_SSL_MINOR_VERSION_3);
        // Like the desktop's VerifyPeer mode, both sides present certificates.
        mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_OPTIONAL);
        mbedtls_ssl_conf_verify(&config, acceptAnyChain, nullptr);
        mbedtls_ssl_conf_renegotiation(&config, MBEDTLS_SSL_RENEGOTIATION_DISABLED);
        require(mbedtls_ssl_conf_own_cert(&config, &certificate, &key), "TLS certificate");
    }
};

Credentials::Credentials(const Identity& identity) : context_(std::make_unique<Context>()) {
    auto& context = *context_;
    // An ECDSA key is generated quickly even on an original 3DS. The
    // desktop accepts any key type that the identity binding signs.
    require(mbedtls_pk_setup(&context.key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)),
            "TLS key setup");
    require(mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(context.key), random,
                                nullptr),
            "TLS key generation");
    unsigned char publicKey[256];
    const int publicSize = mbedtls_pk_write_pubkey_der(&context.key, publicKey, sizeof(publicKey));
    if (publicSize <= 0)
        throw std::runtime_error(tlsError("TLS public key", publicSize));
    const std::string publicDer(
        reinterpret_cast<const char*>(publicKey + sizeof(publicKey) - std::size_t(publicSize)),
        std::size_t(publicSize));
    const auto binding = json::canonical(json::Object{
        {"peer", identity.id()}, {"signature", identity.sign(kBindingDomain + publicDer)}});

    mbedtls_x509write_cert writer;
    mbedtls_x509write_crt_init(&writer);
    mbedtls_mpi serial;
    mbedtls_mpi_init(&serial);
    try {
        const auto subject = "CN=" + identity.id();
        mbedtls_x509write_crt_set_version(&writer, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&writer, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&writer, &context.key);
        mbedtls_x509write_crt_set_issuer_key(&writer, &context.key);
        require(mbedtls_x509write_crt_set_subject_name(&writer, subject.c_str()), "TLS subject");
        require(mbedtls_x509write_crt_set_issuer_name(&writer, subject.c_str()), "TLS issuer");
        require(mbedtls_mpi_lset(&serial, 1), "TLS serial");
        require(mbedtls_x509write_crt_set_serial(&writer, &serial), "TLS serial");
        // Console clocks are often unset. A fixed, wide validity period keeps
        // peers' certificate checks independent of this device's time.
        require(mbedtls_x509write_crt_set_validity(&writer, "20000101000000", "20491231235959"),
                "TLS validity");
        require(mbedtls_x509write_crt_set_extension(
                    &writer, kBindingOid, kBindingOidSize, 0,
                    reinterpret_cast<const unsigned char*>(binding.data()), binding.size()),
                "TLS identity binding");
        unsigned char der[4096];
        const int size = mbedtls_x509write_crt_der(&writer, der, sizeof(der), random, nullptr);
        if (size <= 0)
            throw std::runtime_error(tlsError("TLS certificate", size));
        require(mbedtls_x509_crt_parse_der(&context.certificate,
                                           der + sizeof(der) - std::size_t(size), std::size_t(size)),
                "TLS certificate parse");
    }
    catch (...) {
        mbedtls_mpi_free(&serial);
        mbedtls_x509write_crt_free(&writer);
        throw;
    }
    mbedtls_mpi_free(&serial);
    mbedtls_x509write_crt_free(&writer);
    context.configure(context.client, MBEDTLS_SSL_IS_CLIENT);
    context.configure(context.server, MBEDTLS_SSL_IS_SERVER);
}

Credentials::~Credentials() = default;

std::string certificatePeer(const unsigned char* der, std::size_t size) {
    mbedtls_x509_crt certificate;
    mbedtls_x509_crt_init(&certificate);
    BindingSearch search;
    std::string result;
    if (mbedtls_x509_crt_parse_der_with_ext_cb(&certificate, der, size, 1, findBinding, &search) ==
            0 &&
        search.found == 1) {
        const auto* md = mbedtls_md_info_from_type(certificate.sig_md);
        unsigned char hash[MBEDTLS_MD_MAX_SIZE];
        const bool selfSigned =
            md && mbedtls_md(md, certificate.tbs.p, certificate.tbs.len, hash) == 0 &&
            mbedtls_pk_verify_ext(certificate.sig_pk, certificate.sig_opts, &certificate.pk,
                                  certificate.sig_md, hash, mbedtls_md_get_size(md),
                                  certificate.sig.p, certificate.sig.len) == 0;
        if (selfSigned) {
            try {
                const auto binding = json::parse(search.value);
                const auto& peer = binding["peer"].toString();
                const std::string publicDer(reinterpret_cast<const char*>(certificate.pk_raw.p),
                                            certificate.pk_raw.len);
                if (Identity::verify(peer, kBindingDomain + publicDer,
                                     binding["signature"].toString()))
                    result = peer;
            }
            catch (const std::exception&) {
            }
        }
    }
    mbedtls_x509_crt_free(&certificate);
    return result;
}

struct Stream::Tls {
    mbedtls_ssl_context ssl;
    Tls() { mbedtls_ssl_init(&ssl); }
    ~Tls() { mbedtls_ssl_free(&ssl); }
};

Stream::Stream() = default;

Stream::~Stream() {
    close();
}

void Stream::close(const std::string& reason) {
    closing_ = false;
    closeAt_ = 0;
    if (socket_ >= 0)
        ::close(socket_);
    socket_ = -1;
    tls_.reset();
    if (state_ != State::Closed && !reason.empty())
        error_ = reason;
    state_ = State::Closed;
    raw_.clear();
}

bool Stream::connect(const std::string& host, unsigned port, std::uint64_t now) {
    close();
    error_.clear();
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        // The console SDKs supply only blocking IPv4 gethostbyname. Every
        // tunnel reconnects to the same relay, so results are cached.
        struct Resolved {
            in_addr address{};
            bool ok = false;
            std::uint64_t until = 0;
        };
        static std::map<std::string, Resolved> cache;
        auto found = cache.find(host);
        if (found == cache.end() || now >= found->second.until) {
            if (cache.size() >= 32)
                cache.clear();
            Resolved entry;
            hostent* resolved = gethostbyname(host.c_str());
            entry.ok = resolved && resolved->h_addrtype == AF_INET && resolved->h_length == 4 &&
                       resolved->h_addr_list[0];
            if (entry.ok)
                std::memcpy(&entry.address, resolved->h_addr_list[0], 4);
            entry.until = now + (entry.ok ? 300000 : 15000);
            found = cache.insert_or_assign(host, entry).first;
        }
        if (!found->second.ok) {
            close("Cannot resolve " + host + ". Check Wi-Fi and the relay address.");
            return false;
        }
        address.sin_addr = found->second.address;
    }
    socket_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ < 0) {
        close(systemError("Socket", errno));
        return false;
    }
    if (socket_ >= FD_SETSIZE) {
        close("Socket descriptor exceeds select limit.");
        return false;
    }
    if (fcntl(socket_, F_SETFL, O_NONBLOCK) < 0) {
        close(systemError("Nonblocking socket", errno));
        return false;
    }
    int yes = 1;
    setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
#ifdef SO_NOSIGPIPE
    setsockopt(socket_, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
    const int result = ::connect(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    if (result < 0 && !connectPending(errno)) {
        close(systemError("TCP connect", errno));
        return false;
    }
    state_ = result == 0 ? State::Plain : State::Connecting;
    deadline_ = now + 10000;
    return true;
}

void Stream::writePlain(const std::string& bytes) {
    if (state_ == State::Connecting || state_ == State::Plain)
        raw_ += bytes;
}

void Stream::startTls(Credentials& credentials, bool client) {
    if (state_ != State::Plain)
        return;
    tls_ = std::make_unique<Tls>();
    auto& context = credentials.context();
    const int result =
        mbedtls_ssl_setup(&tls_->ssl, client ? &context.client : &context.server);
    if (result != 0) {
        close(tlsError("TLS setup", result));
        return;
    }
    mbedtls_ssl_set_bio(&tls_->ssl, this, sendCallback, receiveCallback, nullptr);
    state_ = State::Handshaking;
}

int Stream::sendCallback(void* self, const unsigned char* bytes, std::size_t size) {
    // Ciphertext is buffered and flushed by poll(); the client bounds it.
    static_cast<Stream*>(self)->raw_.append(reinterpret_cast<const char*>(bytes), size);
    return int(size);
}

int Stream::receiveCallback(void* self, unsigned char* bytes, std::size_t size) {
    auto* stream = static_cast<Stream*>(self);
    // A relay can deliver the peer's first TLS bytes with its "ready" frame.
    if (!stream->plain_.empty()) {
        const auto count = std::min(size, stream->plain_.size());
        std::memcpy(bytes, stream->plain_.data(), count);
        stream->plain_.erase(0, count);
        return int(count);
    }
    const auto got = ::recv(stream->socket_, bytes, size, 0);
    if (got < 0 && wouldBlock())
        return MBEDTLS_ERR_SSL_WANT_READ;
    if (got < 0) {
        stream->error_ = systemError("TCP receive", errno);
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return int(got); // 0 is end of stream
}

void Stream::flush() {
    for (int index = 0; index < 32 && !raw_.empty() && socket_ >= 0; ++index) {
        const auto sent = ::send(socket_, raw_.data(), raw_.size(),
#ifdef MSG_NOSIGNAL
                                 MSG_NOSIGNAL
#else
                                 0
#endif
        );
        if (sent < 0 && wouldBlock())
            return;
        if (sent <= 0) {
            close(sent < 0 ? systemError("TCP send", errno) : "Connection closed.");
            return;
        }
        raw_.erase(0, std::size_t(sent));
    }
}

bool Stream::fill(std::size_t limit) {
    // Plaintext before TLS: relay frames only, so a small bound is enough.
    char buffer[4096];
    while (plain_.size() < limit) {
        const auto got = ::recv(socket_, buffer, sizeof(buffer), 0);
        if (got < 0 && wouldBlock())
            return true;
        if (got <= 0) {
            close(got < 0 ? systemError("TCP receive", errno) : "Connection closed.");
            return false;
        }
        plain_.append(buffer, std::size_t(got));
    }
    return true;
}

void Stream::secure() {
    const auto* certificate = mbedtls_ssl_get_peer_cert(&tls_->ssl);
    if (!certificate) {
        close("Peer did not present an identity certificate.");
        return;
    }
    peer_ = certificatePeer(certificate->raw.p, certificate->raw.len);
    if (peer_.empty()) {
        close("Peer identity certificate is missing or invalid.");
        return;
    }
    state_ = State::Secure;
}

bool Stream::write(const std::string& bytes) {
    if (state_ != State::Secure)
        return false;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const int result = mbedtls_ssl_write(
            &tls_->ssl, reinterpret_cast<const unsigned char*>(bytes.data()) + offset,
            bytes.size() - offset);
        if (result < 0) {
            // The send callback never blocks, so a short write is an error.
            close(tlsError("TLS write", result));
            return false;
        }
        offset += std::size_t(result);
    }
    return true;
}

bool Stream::poll(std::uint64_t now) {
    if (state_ == State::Closed || state_ == State::Idle)
        return state_ != State::Closed;
    if (state_ == State::Connecting) {
        fd_set writes, errors;
        FD_ZERO(&writes);
        FD_ZERO(&errors);
        FD_SET(socket_, &writes);
        FD_SET(socket_, &errors);
        timeval timeout{};
        const int ready = select(socket_ + 1, nullptr, &writes, &errors, &timeout);
        if (ready > 0) {
            int error = 0;
            socklen_t length = sizeof(error);
            if (getsockopt(socket_, SOL_SOCKET, SO_ERROR, &error, &length) < 0)
                close(systemError("Socket status", errno));
            else if (error != 0 && !connectPending(error))
                close(systemError("TCP connect", error));
            else {
                // Horizon may retain raw EINPROGRESS in SO_ERROR after the
                // connection completes. Verify a peer rather than declaring
                // either failure or success from that stale value alone.
                sockaddr_in peer{};
                socklen_t peerLength = sizeof(peer);
                if (getpeername(socket_, reinterpret_cast<sockaddr*>(&peer), &peerLength) == 0)
                    state_ = State::Plain;
                else if (errno != ENOTCONN && !connectPending(errno) && errno != EINTR)
                    close(systemError("TCP peer", errno));
            }
        }
        else if (ready < 0 && errno != EINTR)
            close(systemError("Socket select", errno));
        if (state_ == State::Connecting && now >= deadline_)
            close("Connection timed out.");
        if (state_ != State::Plain)
            return state_ != State::Closed;
    }
    flush();
    if (state_ == State::Plain) {
        fill(64 * 1024);
        return state_ != State::Closed;
    }
    if (state_ == State::Handshaking) {
        const int result = mbedtls_ssl_handshake(&tls_->ssl);
        if (result == 0)
            secure();
        else if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE)
            close(error_.empty() ? tlsError("Secure peer connection", result) : error_);
        flush();
        if (state_ != State::Secure)
            return state_ != State::Closed;
    }
    // Bound each pass so one busy peer cannot starve the others or the UI.
    unsigned char buffer[4096];
    for (std::size_t total = 0; total < 256 * 1024 && state_ == State::Secure;) {
        const int result = mbedtls_ssl_read(&tls_->ssl, buffer, sizeof(buffer));
        if (result > 0) {
            received_.append(reinterpret_cast<const char*>(buffer), std::size_t(result));
            total += std::size_t(result);
            continue;
        }
        if (result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE)
            break;
        close(result == 0 || result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY
                  ? std::string("Connection closed.")
                  : (error_.empty() ? tlsError("TLS read", result) : error_));
    }
    flush();
    if (closing_ && state_ != State::Closed && raw_.empty()) {
        if (closeAt_ == 0)
            closeAt_ = now + 2000; // the grace period starts once all bytes are sent
        else if (now >= closeAt_)
            close("Connection closed.");
    }
    return state_ != State::Closed;
}

} // namespace console
