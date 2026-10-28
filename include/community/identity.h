#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace community {

// Public keys are stable peer IDs. Names and network addresses are attributes.
class Identity {
public:
    static Identity generate();
    static Identity fromSecret(const QByteArray& secret);
    QByteArray secret() const { return secret_; }
    QString id() const { return public_; }
    QString sign(const QByteArray& bytes) const;
    QPair<QByteArray, QByteArray> tlsCredentials() const; // certificate, private key
    static bool verify(const QString& peer, const QByteArray& bytes, const QString& signature);
    static QString certificatePeer(const QByteArray& certificateDer);

private:
    QByteArray secret_;
    QString public_;
};

QByteArray canonical(const QJsonValue& value);
QString digest(const QByteArray& bytes);
QString randomCode();
bool isPeerId(const QString& value);
QJsonObject signedObject(QJsonObject object, const Identity& identity, const QByteArray& domain);
bool verifyObject(QJsonObject object, const QString& peer, const QByteArray& domain);

} // namespace community
