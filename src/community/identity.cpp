#include "community/identity.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <memory>
#include <stdexcept>

namespace community {
namespace {
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using Context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
Key privateKey(const QByteArray& secret) {
    return Key(EVP_PKEY_new_raw_private_key(
                   EVP_PKEY_ED25519, nullptr,
                   reinterpret_cast<const unsigned char*>(secret.constData()), secret.size()),
               EVP_PKEY_free);
}
void require(bool ok) {
    if (!ok)
        throw std::runtime_error("community cryptography failed");
}
QByteArray pem(BIO* bio) {
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    return QByteArray(data, static_cast<qsizetype>(size));
}
} // namespace

QByteArray canonical(const QJsonValue& value) {
    if (value.isObject()) {
        const auto object = value.toObject();
        QByteArray result = "{";
        const auto keys = object.keys(); // Qt returns keys in alphabetical order.
        for (const auto& key : keys) {
            if (result.size() > 1)
                result += ',';
            result += canonical(key) + ':' + canonical(object.value(key));
        }
        return result + '}';
    }
    if (value.isArray()) {
        QByteArray result = "[";
        for (const auto& item : value.toArray()) {
            if (result.size() > 1)
                result += ',';
            result += canonical(item);
        }
        return result + ']';
    }
    const auto bytes = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return bytes.mid(1, bytes.size() - 2);
}

QString digest(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString randomCode() {
    QByteArray bytes(24, Qt::Uninitialized);
    require(RAND_bytes(reinterpret_cast<unsigned char*>(bytes.data()), bytes.size()) == 1);
    return QString::fromLatin1(
        bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

bool isPeerId(const QString& value) {
    static const QRegularExpression pattern("^[0-9a-f]{64}$");
    return pattern.match(value).hasMatch();
}

Identity Identity::generate() {
    QByteArray seed(32, Qt::Uninitialized);
    require(RAND_bytes(reinterpret_cast<unsigned char*>(seed.data()), seed.size()) == 1);
    return fromSecret(seed);
}

Identity Identity::fromSecret(const QByteArray& secret) {
    require(secret.size() == 32);
    auto key = privateKey(secret);
    require(key != nullptr);
    unsigned char bytes[32];
    std::size_t size = sizeof(bytes);
    require(EVP_PKEY_get_raw_public_key(key.get(), bytes, &size) == 1 && size == 32);
    Identity identity;
    identity.secret_ = secret;
    identity.public_ = QString::fromLatin1(
        QByteArray(reinterpret_cast<const char*>(bytes), 32).toHex());
    return identity;
}

QString Identity::sign(const QByteArray& bytes) const {
    auto key = privateKey(secret_);
    Context context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    require(key && context &&
            EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key.get()) == 1);
    unsigned char signature[64];
    std::size_t size = sizeof(signature);
    require(EVP_DigestSign(context.get(), signature, &size,
                           reinterpret_cast<const unsigned char*>(bytes.constData()),
                           bytes.size()) == 1);
    return QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(signature), size).toHex());
}

bool Identity::verify(const QString& peer, const QByteArray& bytes, const QString& signature) {
    if (!isPeerId(peer) || signature.size() != 128)
        return false;
    const auto publicBytes = QByteArray::fromHex(peer.toLatin1());
    const auto signatureBytes = QByteArray::fromHex(signature.toLatin1());
    if (signatureBytes.size() != 64 || signatureBytes.toHex() != signature.toLatin1())
        return false;
    Key key(
        EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                    reinterpret_cast<const unsigned char*>(publicBytes.constData()),
                                    publicBytes.size()),
        EVP_PKEY_free);
    Context context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    return key && context &&
           EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, key.get()) == 1 &&
           EVP_DigestVerify(context.get(),
                            reinterpret_cast<const unsigned char*>(signatureBytes.constData()),
                            signatureBytes.size(),
                            reinterpret_cast<const unsigned char*>(bytes.constData()),
                            bytes.size()) == 1;
}

QPair<QByteArray, QByteArray> Identity::tlsCredentials() const {
    // Qt's native backends don't consistently import Ed25519/EC identities.
    // A disposable RSA TLS key works across them; an Ed25519 signature binds
    // its public key to the persistent peer identity.
    Key key(EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", std::size_t(3072)), EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), X509_free);
    require(key && certificate);
    require(X509_set_version(certificate.get(), 2) == 1);
    require(ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) == 1);
    require(X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -3600) != nullptr);
    require(X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 365L * 24 * 3600) != nullptr);
    require(X509_set_pubkey(certificate.get(), key.get()) == 1);
    std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)> subject(X509_NAME_new(), X509_NAME_free);
    auto* name = subject.get();
    const auto commonName = public_.toLatin1();
    require(
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>(commonName.constData()),
                                   commonName.size(), -1, 0) == 1);
    require(X509_set_subject_name(certificate.get(), name) == 1);
    require(X509_set_issuer_name(certificate.get(), name) == 1);
    unsigned char* raw = nullptr;
    const int rawSize = i2d_PUBKEY(key.get(), &raw);
    require(rawSize > 0);
    const QByteArray publicDer(reinterpret_cast<const char*>(raw), rawSize);
    OPENSSL_free(raw);
    const auto binding = canonical(QJsonObject{
        {"peer", id()}, {"signature", sign("disquisition/tls-identity/v1\n" + publicDer)}});
    std::unique_ptr<ASN1_OBJECT, decltype(&ASN1_OBJECT_free)> oid(
        OBJ_txt2obj("1.3.6.1.4.1.55555.1", 1), ASN1_OBJECT_free);
    std::unique_ptr<ASN1_OCTET_STRING, decltype(&ASN1_OCTET_STRING_free)> value(
        ASN1_OCTET_STRING_new(), ASN1_OCTET_STRING_free);
    require(oid && value &&
            ASN1_OCTET_STRING_set(value.get(),
                                  reinterpret_cast<const unsigned char*>(binding.constData()),
                                  binding.size()) == 1);
    std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension(
        X509_EXTENSION_create_by_OBJ(nullptr, oid.get(), 0, value.get()), X509_EXTENSION_free);
    require(extension && X509_add_ext(certificate.get(), extension.get(), -1) == 1);
    require(X509_sign(certificate.get(), key.get(), EVP_sha256()) > 0);
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
    require(bio && PEM_write_bio_X509(bio.get(), certificate.get()) == 1);
    const auto certificateBytes = pem(bio.get());
    BIO_reset(bio.get());
    require(PEM_write_bio_PrivateKey(bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) ==
            1);
    return {certificateBytes, pem(bio.get())};
}

QString Identity::certificatePeer(const QByteArray& der) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(der.constData());
    std::unique_ptr<X509, decltype(&X509_free)> certificate(d2i_X509(nullptr, &bytes, der.size()),
                                                            X509_free);
    if (!certificate)
        return {};
    Key key(X509_get_pubkey(certificate.get()), EVP_PKEY_free);
    if (!key || X509_verify(certificate.get(), key.get()) != 1)
        return {};
    std::unique_ptr<ASN1_OBJECT, decltype(&ASN1_OBJECT_free)> oid(
        OBJ_txt2obj("1.3.6.1.4.1.55555.1", 1), ASN1_OBJECT_free);
    const int index = X509_get_ext_by_OBJ(certificate.get(), oid.get(), -1);
    if (index < 0)
        return {};
    const auto* value = X509_EXTENSION_get_data(X509_get_ext(certificate.get(), index));
    const auto binding = QJsonDocument::fromJson(
                             QByteArray(reinterpret_cast<const char*>(ASN1_STRING_get0_data(value)),
                                        ASN1_STRING_length(value)))
                             .object();
    unsigned char* raw = nullptr;
    const int size = i2d_PUBKEY(key.get(), &raw);
    if (size <= 0)
        return {};
    const QByteArray publicDer(reinterpret_cast<const char*>(raw), size);
    OPENSSL_free(raw);
    const auto peer = binding.value("peer").toString();
    return verify(peer, "disquisition/tls-identity/v1\n" + publicDer,
                  binding.value("signature").toString())
               ? peer
               : QString{};
}

QJsonObject signedObject(QJsonObject object, const Identity& identity, const QByteArray& domain) {
    object.remove("signature");
    object.insert("signature", identity.sign(domain + canonical(object)));
    return object;
}

bool verifyObject(QJsonObject object, const QString& peer, const QByteArray& domain) {
    const auto signature = object.take("signature").toString();
    return Identity::verify(peer, domain + canonical(object), signature);
}
} // namespace community
