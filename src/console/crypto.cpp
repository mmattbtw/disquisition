#include "console/crypto.h"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

#include <cstring>
#include <stdexcept>

extern "C" {
#include "third_party/monocypher/monocypher-ed25519.h"
#include "third_party/monocypher/monocypher.h"
}

namespace console {
namespace {
constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr char kBase64Url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string encode64(std::string_view bytes, const char* alphabet, bool pad) {
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t at = 0;
    for (; at + 3 <= bytes.size(); at += 3) {
        const unsigned value = unsigned(static_cast<unsigned char>(bytes[at])) << 16 |
                               unsigned(static_cast<unsigned char>(bytes[at + 1])) << 8 |
                               unsigned(static_cast<unsigned char>(bytes[at + 2]));
        out += alphabet[(value >> 18) & 63];
        out += alphabet[(value >> 12) & 63];
        out += alphabet[(value >> 6) & 63];
        out += alphabet[value & 63];
    }
    const auto rest = bytes.size() - at;
    if (rest == 1) {
        const unsigned value = unsigned(static_cast<unsigned char>(bytes[at])) << 16;
        out += alphabet[(value >> 18) & 63];
        out += alphabet[(value >> 12) & 63];
        if (pad)
            out += "==";
    }
    else if (rest == 2) {
        const unsigned value = unsigned(static_cast<unsigned char>(bytes[at])) << 16 |
                               unsigned(static_cast<unsigned char>(bytes[at + 1])) << 8;
        out += alphabet[(value >> 18) & 63];
        out += alphabet[(value >> 12) & 63];
        out += alphabet[(value >> 6) & 63];
        if (pad)
            out += '=';
    }
    return out;
}

int digit64(char ch, bool url) {
    if (ch >= 'A' && ch <= 'Z')
        return ch - 'A';
    if (ch >= 'a' && ch <= 'z')
        return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9')
        return ch - '0' + 52;
    if (ch == (url ? '-' : '+'))
        return 62;
    if (ch == (url ? '_' : '/'))
        return 63;
    return -1;
}

struct Random {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    Random() {
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&drbg);
        static const unsigned char personal[] = "disquisition-console";
        if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, personal,
                                  sizeof(personal) - 1) != 0)
            throw std::runtime_error("random number generator unavailable");
    }
};

Random& random() {
    static Random instance;
    return instance;
}
} // namespace

std::string hex(std::string_view bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        out += digits[static_cast<unsigned char>(byte) >> 4];
        out += digits[static_cast<unsigned char>(byte) & 15];
    }
    return out;
}

bool fromHex(std::string_view text, std::string& bytes) {
    if (text.size() % 2)
        return false;
    bytes.clear();
    bytes.reserve(text.size() / 2);
    const auto nibble = [](char ch) {
        if (ch >= '0' && ch <= '9')
            return ch - '0';
        if (ch >= 'a' && ch <= 'f')
            return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F')
            return ch - 'A' + 10;
        return -1;
    };
    for (std::size_t at = 0; at < text.size(); at += 2) {
        const int high = nibble(text[at]), low = nibble(text[at + 1]);
        if (high < 0 || low < 0)
            return false;
        bytes += char(high << 4 | low);
    }
    return true;
}

std::string base64(std::string_view bytes) {
    return encode64(bytes, kBase64, true);
}
std::string base64Url(std::string_view bytes) {
    return encode64(bytes, kBase64Url, false);
}

bool fromBase64Strict(std::string_view text, std::string& bytes, bool url) {
    bytes.clear();
    unsigned buffer = 0;
    int bits = 0;
    for (std::size_t at = 0; at < text.size(); ++at) {
        const int value = digit64(text[at], url);
        if (value < 0) {
            if (text[at] != '=' || text.size() % 4 != 0)
                return false;
            // One or two trailing '=' characters may pad to a multiple of four.
            if (at == text.size() - 1 || (at == text.size() - 2 && text[at + 1] == '='))
                break;
            return false;
        }
        buffer = (buffer << 6) | unsigned(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes += char(buffer >> bits);
            buffer &= (1u << bits) - 1;
        }
    }
    return true;
}

std::string fromBase64(std::string_view text) {
    std::string bytes;
    unsigned buffer = 0;
    int bits = 0;
    for (const auto ch : text) {
        const int value = digit64(ch, false);
        if (value < 0)
            continue;
        buffer = (buffer << 6) | unsigned(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes += char(buffer >> bits);
            buffer &= (1u << bits) - 1;
        }
    }
    return bytes;
}

std::string sha256(std::string_view bytes) {
    unsigned char out[32];
    if (mbedtls_sha256_ret(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(),
                           out, 0) != 0)
        throw std::runtime_error("SHA-256 failed");
    return std::string(reinterpret_cast<const char*>(out), sizeof(out));
}

std::string digest(std::string_view bytes) {
    return hex(sha256(bytes));
}

std::string hmacSha256(std::string_view key, std::string_view message) {
    unsigned char out[32];
    if (mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                        reinterpret_cast<const unsigned char*>(key.data()), key.size(),
                        reinterpret_cast<const unsigned char*>(message.data()), message.size(),
                        out) != 0)
        throw std::runtime_error("HMAC failed");
    return std::string(reinterpret_cast<const char*>(out), sizeof(out));
}

bool constantTimeEqual(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    unsigned char difference = 0;
    for (std::size_t index = 0; index < a.size(); ++index)
        difference |= static_cast<unsigned char>(a[index] ^ b[index]);
    return difference == 0;
}

void randomBytes(void* out, std::size_t size) {
    auto* bytes = static_cast<unsigned char*>(out);
    while (size > 0) {
        const auto chunk = size > MBEDTLS_CTR_DRBG_MAX_REQUEST ? MBEDTLS_CTR_DRBG_MAX_REQUEST : size;
        if (mbedtls_ctr_drbg_random(&random().drbg, bytes, chunk) != 0)
            throw std::runtime_error("random number generator failed");
        bytes += chunk;
        size -= chunk;
    }
}

std::string randomCode() {
    std::string bytes(24, '\0');
    randomBytes(bytes.data(), bytes.size());
    return base64Url(bytes);
}

std::string freshId() {
    unsigned char bytes[16];
    randomBytes(bytes, sizeof(bytes));
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    const auto text = hex(std::string(reinterpret_cast<const char*>(bytes), sizeof(bytes)));
    return text.substr(0, 8) + '-' + text.substr(8, 4) + '-' + text.substr(12, 4) + '-' +
           text.substr(16, 4) + '-' + text.substr(20);
}

bool isPeerId(std::string_view value) {
    if (value.size() != 64)
        return false;
    for (const auto ch : value)
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return false;
    return true;
}

bool isUuid(std::string_view value) {
    // QUuid(text).toString(WithoutBraces) == text, and not the null UUID.
    if (value.size() != 36)
        return false;
    bool nonzero = false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char ch = value[index];
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (ch != '-')
                return false;
            continue;
        }
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return false;
        nonzero = nonzero || ch != '0';
    }
    return nonzero;
}

Identity Identity::generate() {
    std::string seed(32, '\0');
    randomBytes(seed.data(), seed.size());
    return fromSecret(seed);
}

Identity Identity::fromSecret(std::string_view seed) {
    if (seed.size() != 32)
        throw std::runtime_error("invalid identity key");
    uint8_t copy[32], secret[64], publicKey[32];
    std::memcpy(copy, seed.data(), sizeof(copy));
    crypto_ed25519_key_pair(secret, publicKey, copy); // wipes copy
    Identity identity;
    identity.seed_ = std::string(seed);
    identity.key_ = std::string(reinterpret_cast<const char*>(secret), sizeof(secret));
    identity.id_ = hex(std::string(reinterpret_cast<const char*>(publicKey), sizeof(publicKey)));
    crypto_wipe(secret, sizeof(secret));
    return identity;
}

std::string Identity::sign(std::string_view bytes) const {
    uint8_t signature[64];
    crypto_ed25519_sign(signature, reinterpret_cast<const uint8_t*>(key_.data()),
                        reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    return hex(std::string(reinterpret_cast<const char*>(signature), sizeof(signature)));
}

bool Identity::verify(std::string_view peer, std::string_view bytes, std::string_view signature) {
    if (!isPeerId(peer) || signature.size() != 128)
        return false;
    std::string publicKey, signatureBytes;
    // Like the desktop, accept only the lowercase hex it produces.
    if (!fromHex(peer, publicKey) || !fromHex(signature, signatureBytes) ||
        hex(signatureBytes) != signature)
        return false;
    return crypto_ed25519_check(reinterpret_cast<const uint8_t*>(signatureBytes.data()),
                                reinterpret_cast<const uint8_t*>(publicKey.data()),
                                reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()) == 0;
}

json::Object signedObject(json::Object object, const Identity& identity, std::string_view domain) {
    object.erase("signature");
    const auto bytes = std::string(domain) + json::canonical(json::Value(object));
    object["signature"] = identity.sign(bytes);
    return object;
}

bool verifyObject(json::Object object, std::string_view peer, std::string_view domain) {
    const auto found = object.find("signature");
    if (found == object.end())
        return false;
    const auto signature = found->second.toString();
    object.erase(found);
    return Identity::verify(peer, std::string(domain) + json::canonical(json::Value(object)),
                            signature);
}

} // namespace console
