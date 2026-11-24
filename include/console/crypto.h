#pragma once

#include <string>
#include <string_view>

#include "console/json.h"

// Byte strings are std::string. Peer IDs are lowercase hex Ed25519 public keys,
// matching community::Identity on the desktop.
namespace console {

std::string hex(std::string_view bytes);
bool fromHex(std::string_view text, std::string& bytes);
std::string base64(std::string_view bytes);
std::string base64Url(std::string_view bytes); // without padding
// QByteArray::fromBase64Encoding(AbortOnBase64DecodingErrors).
bool fromBase64Strict(std::string_view text, std::string& bytes, bool url = false);
// QByteArray::fromBase64(): invalid characters are skipped.
std::string fromBase64(std::string_view text);

std::string sha256(std::string_view bytes);
std::string digest(std::string_view bytes); // hex SHA-256
std::string hmacSha256(std::string_view key, std::string_view message);
bool constantTimeEqual(std::string_view a, std::string_view b);

// One process-wide CTR-DRBG seeded from the platform entropy source. All
// callers are on the community worker thread.
void randomBytes(void* out, std::size_t size);
std::string randomCode(); // 24 random bytes, base64url: 32 characters
std::string freshId();    // lowercase RFC 4122 version 4 UUID
bool isPeerId(std::string_view value);
bool isUuid(std::string_view value);

class Identity {
public:
    static Identity generate();
    static Identity fromSecret(std::string_view seed); // throws unless 32 bytes
    const std::string& secret() const { return seed_; }
    const std::string& id() const { return id_; }
    std::string sign(std::string_view bytes) const; // hex signature
    static bool verify(std::string_view peer, std::string_view bytes, std::string_view signature);

private:
    std::string seed_;
    std::string key_; // Monocypher's 64-byte secret key: seed || public key
    std::string id_;
};

json::Object signedObject(json::Object object, const Identity& identity, std::string_view domain);
bool verifyObject(json::Object object, std::string_view peer, std::string_view domain);

} // namespace console
