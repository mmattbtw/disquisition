#include "community/node.h"

#include <QDateTime>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <QHostAddress>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <algorithm>
#include <functional>
#include <stdexcept>

#include "common/media_limits.h"
#include "community/wire.h"

namespace community {
namespace {
qint64 now() {
    return QDateTime::currentMSecsSinceEpoch();
}
class Listener final : public QTcpServer {
public:
    std::function<void(qintptr)> accepted;
    void incomingConnection(qintptr descriptor) override { accepted(descriptor); }
};
QString lanHost() {
    for (const auto& interface : QNetworkInterface::allInterfaces()) {
        const auto flags = interface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::CanBroadcast) ||
            (flags & QNetworkInterface::IsLoopBack))
            continue;
        for (const auto& entry : interface.addressEntries())
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !entry.ip().isLinkLocal())
                return entry.ip().toString();
    }
    return "127.0.0.1";
}
bool readPlainFrame(QTcpSocket* socket, QJsonObject& object) {
    if (socket->bytesAvailable() < 4)
        return false;
    const auto header = socket->peek(4);
    const auto size = qFromBigEndian<quint32>(header.constData());
    if (size == 0 || size > 64 * 1024)
        throw std::runtime_error("invalid relay frame");
    if (socket->bytesAvailable() < size + 4)
        return false;
    auto bytes = socket->read(size + 4);
    return takeFrame(bytes, object);
}
QJsonObject presenceObject(const QString& cid, const Presence& presence) {
    return {{"type", "presence"},
            {"community", cid},
            {"channel", presence.voiceChannel},
            {"muted", presence.muted},
            {"deafened", presence.deafened},
            {"sharing", presence.sharing}};
}
} // namespace

struct Node::Link {
    QSslSocket* socket = nullptr;
    QString peer;
    QString expected;
    QString discover;
    QString route;
    QString relayAction;
    QString ticket;
    QString nonce = randomCode();
    QString remoteNonce;
    bool outgoing = false;
    bool tls = false;
    bool awaitingTls = false;
    bool hello = false;
    bool merging = false;
    qint64 started = now();
    qint64 touched = now();
    QByteArray buffer;
    // Framed video waiting for the socket; see pumpVideo().
    QList<QByteArray> video;
    qint64 videoBytes = 0;
    QSet<QString> claimed;
    QSet<QString> allowed;
    QSet<QString> ready;
    QMap<QString, qint64> synchronizing;
    void beginSynchronization(const QString& cid) { synchronizing.insert(cid, now()); }
    QMap<QString, QJsonObject> admissions;
    struct Snapshot {
        QString owner;
        QJsonArray operations;
        int count = 0;
        qsizetype bytes = 0;
        qint64 started = now();
        bool complete = false;
    };
    QMap<QString, Snapshot> snapshots;
};

Node::Node(const QString& directory, QObject* parent) :
    QObject(parent), store_(directory), profile_(store_.profile()), models_(store_.load()) {
    const auto credentials = store_.identity().tlsCredentials();
    certificate_ = credentials.first;
    privateKey_ = credentials.second;
    for (const auto& value : store_.pending()) {
        auto invite = Invite::fromLink(value.toString());
        if (!models_.contains(invite.community))
            pending_[invite.community] = std::move(invite);
    }
    auto listener = std::make_unique<Listener>();
    listener->accepted = [this](qintptr descriptor) { accept(descriptor); };
    listener_ = std::move(listener);
    timer_.setInterval(500);
    connect(&timer_, &QTimer::timeout, this, &Node::tick);
}

Node::~Node() {
    stop();
}

bool Node::start(const QString& displayName, const QString& advertisedHost, quint16 listenPort,
                 const QString& relayEndpoint) {
    if (running_)
        return true;
    name_ = displayName.trimmed();
    if (name_.isEmpty() || name_.size() > 64 ||
        std::any_of(name_.begin(), name_.end(),
                    [](QChar ch) { return ch.category() == QChar::Other_Control; })) {
        emit errorOccurred("Enter a name of at most 64 characters without control characters.");
        return false;
    }
    if (!QSslSocket::supportsSsl()) {
        emit errorOccurred("TLS support is unavailable.");
        return false;
    }
    try {
        relayEndpoint_ = normalizeRelayEndpoint(relayEndpoint);
    }
    catch (const std::exception& error) {
        emit errorOccurred(QString::fromUtf8(error.what()));
        return false;
    }
    connectionErrors_.clear();
    // Relay mode never advertises a direct address or opens an inbound port.
    if (relayEndpoint_.isEmpty()) {
        if (listenPort == 0) {
            for (const auto& model : models_) {
                const QUrl previous(model.members.value(peerId()).endpoint);
                if (previous.scheme() == "tcp" && previous.port() > 0) {
                    listenPort = static_cast<quint16>(previous.port());
                    break;
                }
            }
        }
        if (!listener_->listen(QHostAddress::Any, listenPort)) {
            emit errorOccurred(listener_->errorString());
            return false;
        }
        QUrl address;
        address.setScheme("tcp");
        address.setHost(advertisedHost.isEmpty() ? lanHost() : advertisedHost);
        address.setPort(listener_->serverPort());
        endpoint_ = address.toString();
    }
    else
        endpoint_ = relayEndpoint_;
    if (!validEndpoint(endpoint_)) {
        listener_->close();
        emit errorOccurred("Invalid advertised address.");
        return false;
    }
    running_ = true;
    recoveringUntil_ = now() + 2000;
    try {
        profile_.name = name_;
        store_.saveProfile(profile_);
        for (const auto& cid : models_.keys())
            publishProfile(cid);
    }
    catch (const std::exception& error) {
        running_ = false;
        listener_->close();
        emit errorOccurred(QString::fromUtf8(error.what()));
        return false;
    }
    timer_.start();
    tick();
    emit changed();
    return true;
}

void Node::stop() {
    running_ = false;
    timer_.stop();
    listener_->close();
    if (relay_) {
        relay_->disconnect(this);
        relay_->abort();
        relay_->deleteLater();
        relay_ = nullptr;
    }
    relayNonce_.clear();
    relayRegistered_ = false;
    relayDue_ = 0;
    while (!links_.isEmpty()) {
        auto* link = links_.takeLast();
        link->socket->disconnect(this);
        link->socket->abort();
        link->socket->deleteLater();
        delete link;
    }
    peers_.clear();
    joining_.clear();
    joinStarted_.clear();
    presence_.clear();
    mine_.clear();
}

quint16 Node::port() const {
    return listener_->serverPort();
}

void Node::save(Model model) {
    store_.save(model); // Never advertise an update we haven't durably saved.
    const auto id = model.id;
    models_.insert(id, std::move(model));
    if (!models_[id].isMember(peerId())) {
        mine_.remove(id);
        presence_.remove(id);
    }
    registerRelay();
    emit changed();
}

QString Node::createCommunity(const QString& name) {
    if (!running_)
        throw std::runtime_error("connect before creating a community");
    if (models_.size() >= 64)
        throw std::runtime_error("too many communities");
    auto model = Model::create(store_.identity(), name.trimmed(), name_, endpoint_);
    const auto id = model.id;
    save(std::move(model));
    publishProfile(id);
    emit joined(id);
    return id;
}

void Node::persistPending() {
    QJsonArray array;
    for (const auto& invite : pending_)
        array.append(invite.toLink());
    store_.savePending(array);
}

void Node::joinCommunity(const QString& link) {
    if (!running_)
        throw std::runtime_error("connect before joining a community");
    const auto invite = Invite::fromLink(link.trimmed());
    if (models_.contains(invite.community) && models_[invite.community].isMember(peerId())) {
        emit joined(invite.community);
        return;
    }
    if (pending_.size() >= 16)
        throw std::runtime_error("too many pending invitations");
    pending_[invite.community] = invite;
    rejected_.remove(invite.community);
    joining_.remove(invite.community);
    joinStarted_.remove(invite.community);
    persistPending();
    retries_.clear();
    tick();
}

QString Node::invite(const QString& cid) {
    if (!models_.contains(cid))
        throw std::runtime_error("unknown community");
    const auto preferred = models_[cid].host(online(cid));
    auto model = models_.value(cid);
    const auto code = model.issueInvite(store_.identity());
    save(std::move(model));
    broadcastState(cid);
    Invite invite{cid, models_[cid].owner, code, {}};
    // A relay route can discover another member of this community after host
    // migration. A join proof is bound to that peer's TLS identity and nonce.
    // Start at the issuer, which already has this new invite operation. It
    // redirects only once the preferred host has acknowledged the revision.
    invite.seeds.append(QJsonObject{
        {"peer", peerId()},
        {"endpoint", endpoint_.isEmpty() ? models_[cid].members.value(peerId()).endpoint : endpoint_}});
    if (!preferred.isEmpty() && preferred != peerId())
        invite.seeds.append(QJsonObject{{"peer", preferred},
                                        {"endpoint", models_[cid].members[preferred].endpoint}});
    for (const auto& member : models_[cid].members) {
        if (member.id == peerId() || member.id == preferred || invite.seeds.size() >= 8)
            continue;
        invite.seeds.append(QJsonObject{{"peer", member.id}, {"endpoint", member.endpoint}});
    }
    return invite.toLink();
}

void Node::edit(const QString& cid, const QString& kind, const QJsonObject& data) {
    if (!models_.contains(cid))
        throw std::runtime_error("unknown community");
    // Authority comes from signed community state. An unresponsive peer must
    // not prevent local edits or a member leaving; concurrent state merges later.
    auto model = models_.value(cid);
    model.change(store_.identity(), kind, data);
    save(std::move(model));
    broadcastState(cid);
}

void Node::publishProfile(const QString& cid) {
    if (!models_.contains(cid) || !models_[cid].isMember(peerId()))
        return;
    const auto member = models_[cid].members.value(peerId());
    if (member.name == name_ && member.endpoint == endpoint_ && member.bio == profile_.bio &&
        member.avatar == profile_.avatar)
        return;
    auto model = models_.value(cid);
    auto data = profile_.toJson();
    data["name"] = name_;
    data["endpoint"] = endpoint_;
    model.change(store_.identity(), "profile", data);
    save(std::move(model));
    broadcastState(cid);
}

void Node::setProfile(const Profile& profile) {
    const auto next = Profile::fromJson(profile.toJson());
    QMap<QString, Model> updates;
    for (const auto& current : models_) {
        const auto member = current.members.value(peerId());
        if (!current.isMember(peerId()) ||
            (member.name == next.name && member.bio == next.bio && member.avatar == next.avatar))
            continue;
        auto model = current;
        auto data = next.toJson();
        data["endpoint"] = running_ ? endpoint_ : member.endpoint;
        model.change(store_.identity(), "profile", data);
        updates.insert(model.id, std::move(model));
    }
    store_.saveProfile(next);
    profile_ = next;
    name_ = next.name;
    for (const auto& cid : updates.keys()) {
        save(updates[cid]);
        if (running_)
            broadcastState(cid);
    }
    emit changed();
}

Node::Link* Node::makeLink(bool outgoing, const QString& expected) {
    auto* link = new Link;
    link->socket = new QSslSocket(this);
    link->outgoing = outgoing;
    link->expected = expected;
    auto* socket = link->socket;
    socket->setReadBufferSize(512 * 1024);
    QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
    // Native macOS/older Windows TLS backends may only support TLS 1.2.
    // Negotiate TLS 1.3 where available, while retaining mutual identity checks.
    configuration.setProtocol(QSsl::TlsV1_2OrLater);
    configuration.setLocalCertificate(QSslCertificate(certificate_));
    configuration.setPrivateKey(QSslKey(privateKey_, QSsl::Rsa));
    // Schannel requests a client certificate only in VerifyPeer mode. CA and
    // hostname exceptions below are allowed only after the identity binding
    // and certificate self-signature have been verified independently.
    configuration.setPeerVerifyMode(QSslSocket::VerifyPeer);
    socket->setSslConfiguration(configuration);
    links_.append(link);
    connect(socket, &QSslSocket::encryptedBytesWritten, this, [this, link] { pumpVideo(link); });
    connect(socket, &QSslSocket::sslErrors, this, [socket](const QList<QSslError>& errors) {
        if (Identity::certificatePeer(socket->peerCertificate().toDer()).isEmpty()) {
            socket->abort();
            return;
        }
        for (const auto& error : errors) {
            if (error.error() != QSslError::SelfSignedCertificate &&
                error.error() != QSslError::HostNameMismatch &&
                error.error() != QSslError::CertificateUntrusted) {
                socket->abort();
                return;
            }
        }
        socket->ignoreSslErrors(errors);
    });
    connect(socket, &QSslSocket::connected, this, [link] {
        if (!link->relayAction.isEmpty())
            link->socket->write(kRelayPrefix, sizeof(kRelayPrefix) - 1);
    });
    connect(socket, &QSslSocket::encrypted, this, [this, link] { encrypted(link); });
    connect(socket, &QSslSocket::readyRead, this, [this, link] { read(link); });
    connect(socket, &QSslSocket::errorOccurred, this,
            [this, link](QAbstractSocket::SocketError error) {
                if ((error == QAbstractSocket::SslHandshakeFailedError ||
                     error == QAbstractSocket::SslInternalError ||
                     error == QAbstractSocket::SslInvalidUserDataError) &&
                    links_.contains(link) && link->outgoing)
                    connectionError(link->route,
                                    "Secure peer connection failed. Check the invite address and "
                                    "that both clients and the relay are up to date.");
                if (links_.contains(link) &&
                    link->socket->state() == QAbstractSocket::UnconnectedState) {
                    // Failed connection attempts don't always emit disconnected().
                    QMetaObject::invokeMethod(
                        link->socket,
                        [this, link] {
                            if (links_.contains(link) &&
                                link->socket->state() == QAbstractSocket::UnconnectedState)
                                emit link->socket->disconnected();
                        },
                        Qt::QueuedConnection);
                }
            });
    connect(socket, &QSslSocket::disconnected, this, [this, link] {
        if (!links_.removeOne(link))
            return;
        if (peers_.value(link->peer) == link) {
            peers_.remove(link->peer);
            for (auto& group : presence_)
                group.remove(link->peer);
        }
        for (auto it = joining_.begin(); it != joining_.end();) {
            if (it.value() == link) {
                joinStarted_.remove(it.key());
                it = joining_.erase(it);
            }
            else
                ++it;
        }
        link->socket->disconnect(this);
        link->socket->deleteLater();
        delete link;
        emit changed();
    });
    return link;
}

void Node::accept(qintptr descriptor) {
    auto* link = makeLink(false);
    if (!link->socket->setSocketDescriptor(descriptor)) {
        link->socket->abort();
        return;
    }
    if (links_.size() > 256) {
        link->socket->abort();
        return;
    }
    // Relay handshakes and accidental non-TLS traffic must not enter the TLS
    // decoder. In particular, a desktop listener is not a relay endpoint.
    link->awaitingTls = true;
    read(link);
}

void Node::dial(const QString& peer, const QString& endpoint, const QString& discover) {
    if (!validEndpoint(endpoint) || peer == peerId() || links_.size() >= 256)
        return;
    const auto key = peer + ':' + endpoint + ':' + discover;
    if (peers_.contains(peer))
        return;
    for (auto* link : links_)
        if (link->route == key)
            return;
    auto& retry = retries_[key];
    if (retry.due > now())
        return;
    retry.failures = std::min(6, retry.failures + 1);
    retry.due = now() + std::min(30000, 500 * (1 << retry.failures)) +
                QRandomGenerator::global()->bounded(300);
    auto* link = makeLink(true, peer);
    link->route = key;
    link->discover = discover;
    const QUrl address(endpoint);
    if (address.scheme() == "relay") {
        link->relayAction = "dial";
        link->socket->connectToHost(address.host(), address.port());
    }
    else
        link->socket->connectToHostEncrypted(address.host(), address.port());
}

void Node::encrypted(Link* link) {
    const auto peer = Identity::certificatePeer(link->socket->peerCertificate().toDer());
    if (peer.isEmpty() || peer == peerId() ||
        (!link->expected.isEmpty() && peer != link->expected)) {
        emit errorOccurred(
            "Peer identity certificate is missing, invalid, or does not match the requested peer.");
        link->socket->abort();
        return;
    }
    link->tls = true;
    connectionErrors_.remove(link->route);
    link->peer = peer;
    if (auto* existing = peers_.value(peer, nullptr)) {
        // Both sides select the connection initiated by the smaller peer ID.
        const bool preferOutgoing = peerId() < peer;
        if (existing->outgoing == preferOutgoing || link->outgoing != preferOutgoing) {
            link->socket->abort();
            return;
        }
        peers_[peer] = link;
        existing->socket->abort();
    }
    else
        peers_[peer] = link;
    retries_.remove(link->route);
    QJsonArray communities;
    for (const auto& model : models_)
        if (model.isMember(peerId()) && model.isMember(peer))
            communities.append(QJsonObject{{"id", model.id}, {"revision", model.revision()}});
    send(link, {{"type", "hello"},
                {"protocol", "disquisition-community"},
                {"v", 1},
                {"peer", peerId()},
                {"nonce", link->nonce},
                {"communities", communities}});
}

qint64 Node::writeBacklog(const Link* link) {
    // Written data waits unencrypted only until the next event loop pass; most
    // of the backlog is already encrypted.
    return link->socket->bytesToWrite() + link->socket->encryptedBytesToWrite();
}

void Node::queueVideo(Link* link, const QJsonObject& message) {
    if (!links_.contains(link) || !link->tls)
        return;
    link->video.append(frame(message));
    link->videoBytes += link->video.last().size();
    pumpVideo(link);
}

void Node::pumpVideo(Link* link) {
    // A whole frame written at once would hold back the sound and voice sent
    // after it until it had all gone. Feeding it in a chunk at a time lets them
    // go out in between.
    constexpr qint64 kVideoPaceBytes = 16 * 1024;
    if (!links_.contains(link))
        return;
    while (!link->video.isEmpty() && writeBacklog(link) < kVideoPaceBytes) {
        const auto bytes = link->video.takeFirst();
        link->videoBytes -= bytes.size();
        link->socket->write(bytes);
    }
}

void Node::send(Link* link, QJsonObject message) {
    if (!links_.contains(link) || !link->tls)
        return;
    if (writeBacklog(link) > 2 * kMaxMetadataBytes)
        throw std::runtime_error("community connection is backlogged");
    link->socket->write(frame(message));
}

void Node::read(Link* link) {
    try {
        link->touched = now();
        if (!link->tls) {
            if (link->awaitingTls) {
                if (link->socket->bytesAvailable() < 5)
                    return;
                const auto prefix = link->socket->peek(5);
                link->awaitingTls = false;
                if (prefix == QByteArray(kRelayPrefix, sizeof(kRelayPrefix) - 1)) {
                    link->socket->read(5);
                    link->socket->write(frame(
                        {{"type", "error"},
                         {"message", "This is a desktop client's direct address. Enter a relay "
                                     "hostname, or leave Relay blank for direct connections."}}));
                    link->socket->disconnectFromHost();
                }
                else if (prefix[0] == '\x16' && prefix[1] == '\x03')
                    link->socket->startServerEncryption();
                else
                    link->socket->abort();
                return;
            }
            if (link->relayAction.isEmpty())
                return; // TLS handshake owns these bytes.
            QJsonObject response;
            while (readPlainFrame(link->socket, response)) {
                if (response.value("type") == "challenge") {
                    QJsonObject request{
                        {"type", link->relayAction},        {"peer", peerId()},
                        {"nonce", response.value("nonce")}, {"target", link->expected},
                        {"community", link->discover},      {"ticket", link->ticket}};
                    link->socket->write(
                        frame(signedObject(request, store_.identity(), "disquisition/relay/v1\n")));
                }
                else if (response.value("type") == "ready") {
                    if (link->relayAction == "dial" && !link->discover.isEmpty())
                        link->expected = response.value("peer").toString();
                    const bool client = link->relayAction == "dial";
                    link->relayAction.clear();
                    if (client)
                        link->socket->startClientEncryption();
                    else
                        link->socket->startServerEncryption();
                    return;
                }
                else {
                    link->socket->abort();
                    return;
                }
            }
            return;
        }
        if (link->merging)
            return; // Leave further frames in the socket until validation finishes.
        QJsonObject message;
        // Take only what fits the receive bound. A merge pause can leave more
        // decrypted data queued than the socket's read buffer size.
        do {
            link->buffer += link->socket->read(3 * kMaxPacketBytes - link->buffer.size());
            while (takeFrame(link->buffer, message)) {
                handle(link, message);
                if (!links_.contains(link) || link->merging)
                    return;
            }
        } while (link->socket->bytesAvailable() > 0);
    }
    catch (const std::exception& error) {
        emit errorOccurred(QString::fromUtf8(error.what()));
        if (links_.contains(link))
            link->socket->abort();
    }
}

void Node::mergeSnapshot(Link* link, const QString& cid, const QJsonObject& object) {
    // Only value copies cross threads. Socket, store and UI callbacks stay on this event loop.
    const auto base = models_.value(cid);
    const auto revision = base.id.isEmpty() ? QString{} : base.revision();
    struct Result {
        Model candidate;
        bool hasChanges = true;
        QString error;
    };
    auto* watcher = new QFutureWatcher<Result>(link->socket);
    link->merging = true;
    connect(watcher, &QFutureWatcher<Result>::finished, this,
            [this, link, socket = QPointer<QSslSocket>(link->socket), cid, object, revision, watcher] {
        if (!socket || !links_.contains(link) || link->socket != socket)
            return;
        auto result = watcher->result();
        watcher->deleteLater();
        try {
            if (!result.error.isEmpty())
                throw std::runtime_error(result.error.toStdString());
            // Local edits and other peers may have advanced the model during validation.
            const auto current = models_.contains(cid) ? models_[cid].revision() : QString{};
            if (current != revision) {
                mergeSnapshot(link, cid, object);
                return;
            }
            link->snapshots.remove(cid);
            auto candidate = std::move(result.candidate);
            const bool hasChanges = result.hasChanges;
            if (!candidate.isMember(link->peer) && !link->allowed.contains(cid))
                throw std::runtime_error("snapshot sender is not a member");
            const bool joining = pending_.contains(cid);
            const bool admitted = candidate.isMember(peerId());
            // A retained community can receive an older snapshot while rejoining.
            // Merge its history, but keep the join pending until membership is proven.
            if (joining && !admitted && !models_.contains(cid))
                throw std::runtime_error("admission proof was not accepted");
            if (hasChanges)
                save(std::move(candidate));
            if (joining && admitted) {
                pending_.remove(cid);
                joining_.remove(cid);
                joinStarted_.remove(cid);
                persistPending();
                emit joined(cid);
            }
            link->claimed.insert(cid);
            if (writable(link, cid)) {
                link->allowed.insert(cid);
                link->ready.insert(cid);
                link->synchronizing.remove(cid);
                send(link,
                     {{"type", "synced"}, {"community", cid}, {"revision", models_[cid].revision()}});
                send(link, presenceObject(cid, mine_.value(cid)));
            }
            else {
                link->allowed.remove(cid);
                link->ready.remove(cid);
                link->synchronizing.remove(cid);
            }
            if (hasChanges)
                broadcastState(cid);
            publishProfile(cid);
            emit changed();
            link->merging = false;
            // Resume buffered frames even if no further readyRead signal arrives.
            QTimer::singleShot(0, this, [this, link, socket] {
                if (socket && links_.contains(link) && link->socket == socket)
                    read(link);
            });
        }
        catch (const std::exception& error) {
            emit errorOccurred(QString::fromUtf8(error.what()));
            if (links_.contains(link))
                link->socket->abort();
        }
    });
    watcher->setFuture(QtConcurrent::run([base, object, cid]() mutable {
        Result result;
        try {
            result.candidate = base;
            if (base.id.isEmpty())
                result.candidate = Model::fromJson(object, cid, object.value("owner").toString());
            else
                result.hasChanges = result.candidate.merge(object);
        }
        catch (const std::exception& error) {
            result.error = QString::fromUtf8(error.what());
        }
        return result;
    }));
}

bool Node::writable(Link* link, const QString& cid) const {
    return models_.contains(cid) && models_[cid].isMember(peerId()) &&
           models_[cid].isMember(link->peer);
}

void Node::authorize(Link* link, const QString& cid) {
    if (!writable(link, cid))
        return;
    link->allowed.insert(cid);
    if (!link->ready.contains(cid))
        link->beginSynchronization(cid);
    send(link, {{"type", "offer"}, {"community", cid}, {"revision", models_[cid].revision()}});
}

void Node::offer(Link* link) {
    if (!link->hello)
        return;
    for (const auto& model : models_)
        authorize(link, model.id);
    for (const auto& invite : pending_) {
        if (joining_.value(invite.community) == link)
            send(link, {{"type", "join"},
                        {"community", invite.community},
                        {"intent", joinIntent(store_.identity(), invite, name_, endpoint_,
                                              link->peer, link->remoteNonce)}});
    }
}

void Node::sendState(Link* link, const QString& cid) {
    if (!link->hello || !link->allowed.contains(cid))
        return;
    const auto snapshot = models_[cid].toJson();
    const auto operations = snapshot.value("operations").toArray();
    // Established members stay online while exchanging newer metadata.
    link->beginSynchronization(cid);
    send(link, {{"type", "state-begin"},
                {"community", cid},
                {"owner", models_[cid].owner},
                {"count", operations.size()}});
    for (const auto& operation : operations) {
        link->beginSynchronization(cid);
        send(link, {{"type", "state-operation"}, {"community", cid}, {"operation", operation}});
    }
    link->beginSynchronization(cid);
    send(link, {{"type", "state-end"}, {"community", cid}});
}

void Node::broadcastState(const QString& cid) {
    const auto connections = peers_.values();
    for (auto* link : connections) {
        if (writable(link, cid)) {
            link->allowed.insert(cid);
            sendState(link, cid);
        }
        else {
            // Deliver the signed removal/tombstone before withdrawing access.
            if (link->allowed.contains(cid))
                sendState(link, cid);
            link->allowed.remove(cid);
            link->ready.remove(cid);
            link->synchronizing.remove(cid);
            presence_[cid].remove(link->peer);
        }
    }
}

void Node::handle(Link* link, const QJsonObject& message) {
    const auto type = message.value("type").toString();
    const auto cid = message.value("community").toString();
    if (type == "hello") {
        if (link->hello || message.value("protocol") != "disquisition-community" ||
            message.value("v").toInt() != 1 || message.value("peer").toString() != link->peer ||
            message.value("nonce").toString().size() != 32 ||
            message.value("communities").toArray().size() > 64)
            throw std::runtime_error("invalid community handshake");
        link->hello = true;
        link->remoteNonce = message.value("nonce").toString();
        for (const auto& value : message.value("communities").toArray())
            link->claimed.insert(value.toObject().value("id").toString());
        offer(link);
        emit changed();
        return;
    }
    if (!link->hello)
        throw std::runtime_error("community handshake required");
    if (type == "ping") {
        send(link, {{"type", "pong"}});
        return;
    }
    if (type == "pong")
        return;
    if (type == "join") {
        const auto intent = message.value("intent").toObject();
        if (intent.value("peer").toString() != link->peer ||
            intent.value("nonce").toString() != link->nonce ||
            intent.value("host").toString() != peerId() ||
            intent.value("community").toString() != cid || link->admissions.size() >= 16 ||
            canonical(intent).size() > 2048)
            throw std::runtime_error("invalid admission request");
        link->admissions[cid] = intent;
        processAdmissions();
        return;
    }
    if (type == "rejected") {
        if (joining_.value(cid) == link && pending_.remove(cid)) {
            joining_.remove(cid);
            joinStarted_.remove(cid);
            persistPending();
            rejected_[cid] = "Invite rejected";
            emit joinRejected(cid, "Invite rejected");
        }
        return;
    }
    if (type == "redirect") {
        if (!pending_.contains(cid))
            return;
        const auto peer = message.value("peer").toString(),
                   endpoint = message.value("endpoint").toString();
        if (!isPeerId(peer) || peer == peerId() || !validEndpoint(endpoint))
            throw std::runtime_error("invalid host redirect");
        auto& seeds = pending_[cid].seeds;
        const QJsonObject route{{"peer", peer}, {"endpoint", endpoint}};
        if (!seeds.contains(route)) {
            if (seeds.size() >= 8)
                seeds.removeLast();
            seeds.prepend(route);
            persistPending();
        }
        else {
            for (int index = 0; index < seeds.size(); ++index)
                if (seeds[index].toObject() == route) {
                    seeds.removeAt(index);
                    break;
                }
            seeds.prepend(route);
        }
        joining_.remove(cid);
        joinStarted_.remove(cid);
        QTimer::singleShot(0, this, &Node::tick);
        return;
    }
    if (type == "offer") {
        if (!writable(link, cid)) {
            // A returning member may have missed a departure or deletion.
            // Send the signed final state to members of the retained history.
            if (models_.contains(cid) && !models_[cid].isMember(peerId()) &&
                models_[cid].members.contains(link->peer)) {
                link->allowed.insert(cid);
                sendState(link, cid);
                link->allowed.remove(cid);
                link->synchronizing.remove(cid);
            }
            return;
        }
        link->allowed.insert(cid);
        link->claimed.insert(cid);
        if (message.value("revision").toString() == models_[cid].revision()) {
            link->ready.insert(cid);
            link->synchronizing.remove(cid);
            send(link,
                 {{"type", "synced"}, {"community", cid}, {"revision", models_[cid].revision()}});
            send(link, presenceObject(cid, mine_.value(cid)));
        }
        else
            sendState(link, cid);
        emit changed();
        return;
    }
    if (type == "state-begin") {
        if ((!models_.contains(cid) && !pending_.contains(cid)) || link->snapshots.contains(cid) ||
            link->snapshots.size() >= 64)
            throw std::runtime_error("unexpected community snapshot");
        const int count = message.value("count").toInt();
        if (count < 1 || count > kMaxOperations)
            throw std::runtime_error("invalid snapshot count");
        const auto owner = models_.contains(cid) ? models_[cid].owner : pending_[cid].owner;
        if (owner != message.value("owner").toString())
            throw std::runtime_error("community owner mismatch");
        link->beginSynchronization(cid);
        link->snapshots[cid] = {owner, {}, count, 0};
        return;
    }
    if (type == "state-operation") {
        if (!link->snapshots.contains(cid))
            throw std::runtime_error("snapshot was not started");
        auto& snapshot = link->snapshots[cid];
        snapshot.bytes += canonical(message.value("operation")).size();
        qsizetype buffered = 0;
        for (const auto* connection : links_)
            for (const auto& partial : connection->snapshots)
                buffered += partial.bytes;
        if (snapshot.operations.size() >= snapshot.count || snapshot.bytes > kMaxMetadataBytes)
            throw std::runtime_error("snapshot exceeds its limits");
        if (buffered > 2 * kMaxMetadataBytes)
            throw std::runtime_error("too many metadata transfers in progress");
        snapshot.operations.append(message.value("operation"));
        link->beginSynchronization(cid);
        return;
    }
    if (type == "state-end") {
        if (!link->snapshots.contains(cid))
            throw std::runtime_error("snapshot was not started");
        const auto snapshot = link->snapshots.value(cid);
        if (snapshot.operations.size() != snapshot.count)
            throw std::runtime_error("incomplete community snapshot");
        const QJsonObject object{
            {"v", 1}, {"id", cid}, {"owner", snapshot.owner}, {"operations", snapshot.operations}};
        link->snapshots[cid].complete = true;
        mergeSnapshot(link, cid, object);
        return;
    }
    if (type == "synced") {
        if (!writable(link, cid))
            return;
        if (message.value("revision").toString() != models_[cid].revision())
            return;
        link->allowed.insert(cid);
        link->ready.insert(cid);
        link->synchronizing.remove(cid);
        send(link, presenceObject(cid, mine_.value(cid)));
        emit changed();
        return;
    }
    if (!writable(link, cid) || !link->allowed.contains(cid) || !link->ready.contains(cid))
        return;
    if (type == "move-voice") {
        const auto channel = message.value("channel").toString();
        if (!models_[cid].canAdmin(link->peer) || mine_.value(cid).voiceChannel.isEmpty() ||
            !models_[cid].channels.contains(channel) || models_[cid].channels[channel].type != "voice")
            return;
        const auto previous = mine_.value(cid);
        setVoice(cid, channel, previous.muted, previous.deafened, false);
        emit voiceMoved(cid, channel);
        return;
    }
    if (type == "presence") {
        const auto channel = message.value("channel").toString();
        if (!channel.isEmpty() && (!models_[cid].channels.contains(channel) ||
                                   models_[cid].channels[channel].type != "voice"))
            return;
        presence_[cid][link->peer] = {channel, message.value("muted").toBool(),
                                      message.value("deafened").toBool(),
                                      !channel.isEmpty() && message.value("sharing").toBool()};
        emit changed();
        return;
    }
    if (type == "packet") {
        const auto channel = message.value("channel").toString();
        const auto kind = message.value("kind").toString();
        const auto data = message.value("data").toObject();
        const auto id = data.value("id").toString();
        const auto key = link->peer + ':' + cid + ':' + id;
        if (id.size() > 64 || id.isEmpty() || seenMessages_.contains(key) ||
            !validPacket(cid, channel, kind, data, link->peer))
            return;
        seenMessages_.insert(key);
        messageOrder_.append(key);
        if (messageOrder_.size() > 4096)
            seenMessages_.remove(messageOrder_.takeFirst());
        emit packetReceived(cid, channel, link->peer, kind, data);
    }
}

bool Node::synchronizing(const QString& cid) const {
    if (!running_)
        return false;
    if (now() < recoveringUntil_)
        return true;
    for (auto* link : links_) {
        if (!link->hello || !models_.contains(cid) || !models_[cid].isMember(link->peer))
            continue;
        const auto started = link->synchronizing.constFind(cid);
        if (!link->ready.contains(cid) || started != link->synchronizing.cend()) {
            // A stalled transfer still leaves the link and its voice connection
            // alive, but it must not suspend every other admission.
            if (started != link->synchronizing.cend() && now() - *started > 20000)
                continue;
            return true;
        }
    }
    return false;
}

void Node::processAdmissions() {
    const auto connections = peers_.values();
    for (auto* link : connections) {
        const auto communities = link->admissions.keys();
        for (const auto& cid : communities) {
            if (synchronizing(cid))
                continue;
            const auto intent = link->admissions.take(cid);
            try {
                if (!models_.contains(cid) || !models_[cid].isMember(peerId()))
                    throw std::runtime_error("join rejected");
                auto model = models_[cid];
                if (!model.acceptsIntent(peerId(), intent))
                    throw std::runtime_error("join rejected");
                const auto preferred = model.host(online(cid));
                if (!preferred.isEmpty() && preferred != peerId()) {
                    // A bootstrap peer validates the invite before revealing a
                    // routing hint, then lets the preferred host admit the peer.
                    send(link, {{"type", "redirect"},
                                {"community", cid},
                                {"peer", preferred},
                                {"endpoint", model.members[preferred].endpoint}});
                    if (link->allowed.isEmpty() && link->admissions.isEmpty()) {
                        link->socket->disconnectFromHost();
                        break;
                    }
                    continue;
                }
                if (!model.isMember(link->peer))
                    model.admit(store_.identity(), intent);
                save(std::move(model));
                link->claimed.insert(cid);
                link->allowed.insert(cid);
                broadcastState(cid);
            }
            catch (const std::exception&) {
                send(link, {{"type", "rejected"}, {"community", cid}});
                if (link->allowed.isEmpty() && link->admissions.isEmpty()) {
                    link->socket->disconnectFromHost();
                    break;
                }
            }
        }
    }
}

QSet<QString> Node::online(const QString& cid) const {
    QSet<QString> result;
    if (!running_ || !models_.contains(cid) || !models_[cid].isMember(peerId()))
        return result;
    result.insert(peerId());
    for (auto* link : peers_)
        if (writable(link, cid) && link->ready.contains(cid))
            result.insert(link->peer);
    return result;
}
QString Node::host(const QString& cid) const {
    return models_.contains(cid) ? models_[cid].host(online(cid)) : QString{};
}
Presence Node::presence(const QString& cid, const QString& peer) const {
    return peer == peerId() ? mine_.value(cid) : presence_.value(cid).value(peer);
}
qsizetype Node::connectionCount() const {
    return peers_.size();
}
qint64 Node::mediaBacklog() const {
    qint64 result = 0;
    for (auto* link : peers_)
        result = std::max(result, writeBacklog(link) + link->videoBytes);
    return result;
}

void Node::setVoice(const QString& cid, const QString& channel, bool muted, bool deafened,
                    bool sharing) {
    if (!channel.isEmpty() && (!models_.contains(cid) || !models_[cid].isMember(peerId()) ||
                               !models_[cid].channels.contains(channel) ||
                               models_[cid].channels[channel].type != "voice"))
        throw std::runtime_error("unknown voice channel");
    for (auto it = mine_.begin(); it != mine_.end(); ++it) {
        if (it.key() == cid || it->voiceChannel.isEmpty())
            continue;
        it.value() = {};
        for (auto* link : peers_)
            if (writable(link, it.key()) && link->ready.contains(it.key()))
                send(link, presenceObject(it.key(), {}));
    }
    mine_[cid] = {channel, muted, deafened, sharing};
    for (auto* link : peers_)
        if (writable(link, cid) && link->ready.contains(cid))
            send(link, presenceObject(cid, mine_[cid]));
    emit changed();
}

void Node::moveVoice(const QString& cid, const QString& peer, const QString& channel) {
    if (!running_ || !models_.contains(cid) || !models_[cid].canAdmin(peerId()))
        throw std::runtime_error("only the owner or an admin can move members");
    if (!models_[cid].channels.contains(channel) || models_[cid].channels[channel].type != "voice")
        throw std::runtime_error("unknown voice channel");
    if (!online(cid).contains(peer) || presence(cid, peer).voiceChannel.isEmpty())
        throw std::runtime_error("member is not connected to voice");
    if (peer == peerId()) {
        const auto previous = mine_.value(cid);
        setVoice(cid, channel, previous.muted, previous.deafened, false);
        emit voiceMoved(cid, channel);
    }
    else
        send(peers_[peer], {{"type", "move-voice"}, {"community", cid}, {"channel", channel}});
}

bool Node::validPacket(const QString& cid, const QString& channel, const QString& type,
                       const QJsonObject& data, const QString& peer) const {
    if (!models_.contains(cid) || !models_[cid].isMember(peerId()) ||
        !models_[cid].isMember(peer))
        return false;
    const auto timestamp = data.value("timestamp").toDouble(-1);
    if (timestamp < 1 || timestamp > 9007199254740991.0)
        return false;
    const auto target = data.value("target").toString();
    if (!target.isEmpty() && target != peerId())
        return false;
    if (type == "sprite") {
        if (!channel.isEmpty() || canonical(data).size() > 24 * 1024)
            return false;
        const int count = data.value("count").toInt(-1);
        if (count == 0) return !data.contains("bytes");
        const int index = data.value("index").toInt(-1);
        const double transfer = data.value("transfer").toDouble(-1);
        const auto bytes = QByteArray::fromBase64(data.value("bytes").toString().toLatin1());
        return count > 0 && count <= 22 && index >= 0 && index < count &&
               transfer >= 0 && transfer <= UINT32_MAX && transfer == quint32(transfer) &&
               !bytes.isEmpty() && bytes.size() <= qsizetype(chat::kImageChunkSize);
    }
    if (!models_[cid].channels.contains(channel)) return false;
    if (type == "chat")
        return models_[cid].channels[channel].type == "text" &&
               !data.value("body").toString().trimmed().isEmpty() &&
               data.value("body").toString().toUtf8().size() <= 2000;
    if (type == "image")
        return models_[cid].channels[channel].type == "text" && canonical(data).size() <= 24 * 1024;
    const auto local = mine_.value(cid);
    const auto remote = presence_.value(cid).value(peer);
    if (models_[cid].channels[channel].type != "voice" || local.voiceChannel != channel ||
        remote.voiceChannel != channel)
        return false;
    if (type == "audio")
        return !local.deafened && !remote.muted &&
               QByteArray::fromBase64(data.value("bytes").toString().toLatin1()).size() == 640;
    if (type == "watch")
        return data.value("enabled").isBool() && target == peerId();
    if (type == "drawing")
        return target == peerId() && canonical(data).size() <= 24 * 1024;
    if (type == "screen")
        return remote.sharing && canonical(data).size() <= 24 * 1024;
    // The sharer's list of who is watching, shown to everyone in the call.
    if (type == "viewers") {
        if (!remote.sharing || !data.value("peers").isArray())
            return false;
        const auto peers = data.value("peers").toArray();
        return peers.size() <= kMaxMembers &&
               std::all_of(peers.begin(), peers.end(),
                           [](const QJsonValue& value) { return value.isString(); }) &&
               canonical(data).size() <= 24 * 1024;
    }
    if (type == "screen-audio") {
        const auto bytes = QByteArray::fromBase64(data.value("bytes").toString().toLatin1());
        return remote.sharing && !local.deafened && target == peerId() && !bytes.isEmpty() &&
               bytes.size() <= qsizetype(chat::kMaxScreenAudioBytes);
    }
    return false;
}

int Node::sendPacket(const QString& cid, const QString& channel, const QString& type,
                     QJsonObject data) {
    return sendPacketImpl(cid, channel, type, std::move(data), true);
}

bool Node::sendScreenFrame(const QString& cid, const QString& channel, const QString& peer,
                           quint32 frameId, const QList<QByteArray>& chunks, bool keyframe) {
    if (!isPeerId(peer) || chunks.isEmpty() || chunks.size() > chat::kMaxScreenChunks ||
        std::any_of(chunks.begin(), chunks.end(), [](const auto& chunk) {
            return chunk.isEmpty() || chunk.size() > qsizetype(chat::kScreenChunkBytes);
        }))
        throw std::runtime_error("invalid screen frame");
    for (int index = 0; index < chunks.size(); ++index) {
        if (!sendPacketImpl(cid, channel, "screen",
                            {{"target", peer},
                             {"frame", double(frameId)},
                             {"index", index},
                             {"count", chunks.size()},
                             {"keyframe", keyframe},
                             {"bytes", QString::fromLatin1(chunks[index].toBase64())}},
                            index == 0))
            return false;
    }
    return true;
}

int Node::sendPacketImpl(const QString& cid, const QString& channel, const QString& type,
                         QJsonObject data, bool checkScreenBacklog) {
    if (!running_ || !models_.contains(cid) || !models_[cid].isMember(peerId()) ||
        (type != "sprite" && !models_[cid].channels.contains(channel)))
        throw std::runtime_error("unknown channel");
    const auto channelType = models_[cid].channels.value(channel).type;
    if (type == "sprite" ? !channel.isEmpty() :
        ((type == "chat" || type == "image") ? channelType != "text" : channelType != "voice"))
        throw std::runtime_error("message does not belong in this channel");
    if (type == "chat" && (data.value("body").toString().trimmed().isEmpty() ||
                           data.value("body").toString().toUtf8().size() > 2000))
        throw std::runtime_error("messages must contain 1 to 2000 bytes");
    data["id"] = data.value("id").toString().isEmpty() ? freshId() : data.value("id");
    data["timestamp"] = data.value("timestamp").isDouble() ? data.value("timestamp")
                                                           : QJsonValue(double(now()));
    const QJsonObject packet{{"type", "packet"},
                             {"community", cid},
                             {"channel", channel},
                             {"kind", type},
                             {"data", data}};
    int recipients = 0;
    for (auto* link : peers_) {
        if (!writable(link, cid) || !link->ready.contains(cid))
            continue;
        if (!data.value("target").toString().isEmpty() &&
            data.value("target").toString() != link->peer)
            continue;
        if (channelType == "voice" && (mine_.value(cid).voiceChannel != channel ||
                                       presence_[cid].value(link->peer).voiceChannel != channel))
            continue;
        if ((type == "audio" || type == "screen-audio") && writeBacklog(link) > 64 * 1024)
            continue;
        if (type == "screen") {
            if (checkScreenBacklog && writeBacklog(link) + link->videoBytes > 64 * 1024)
                continue;
            queueVideo(link, packet);
            ++recipients;
            continue;
        }
        send(link, packet);
        ++recipients;
    }
    return recipients;
}

void Node::connectRelay() {
    if (relayEndpoint_.isEmpty() || relay_ || relayDue_ > now())
        return;
    relayDue_ = now() + 3000;
    auto* socket = new QTcpSocket(this);
    relay_ = socket;
    relayLastRead_ = now();
    socket->setReadBufferSize(64 * 1024);
    connect(socket, &QTcpSocket::connected, this,
            [socket] { socket->write(kRelayPrefix, sizeof(kRelayPrefix) - 1); });
    connect(socket, &QTcpSocket::readyRead, this, &Node::readRelay);
    const auto cleanup = [this, socket = QPointer<QTcpSocket>(socket)] {
        if (!socket || relay_ != socket)
            return;
        relay_ = nullptr;
        relayNonce_.clear();
        relayRegistered_ = false;
        relayDue_ = now() + 3000;
        socket->deleteLater();
        emit relayConnectionChanged(false);
    };
    connect(socket, &QTcpSocket::disconnected, this, cleanup);
    // Aborting before a connection is established need not emit disconnected.
    // Defer cleanup so errorOccurred can still report the failure first.
    connect(socket, &QTcpSocket::stateChanged, this,
            [this, cleanup](QAbstractSocket::SocketState state) {
                if (state == QAbstractSocket::UnconnectedState)
                    QTimer::singleShot(0, this, cleanup);
            });
    connect(socket, &QTcpSocket::errorOccurred, this,
            [this, cleanup, socket](QAbstractSocket::SocketError error) {
                if (error == QAbstractSocket::RemoteHostClosedError && relayNonce_.isEmpty())
                    connectionError(relayEndpoint_,
                                    "Relay " + QUrl(relayEndpoint_).authority() +
                                        " closed the community handshake. Check the address and "
                                        "that the relay is up to date.");
                else if (error != QAbstractSocket::RemoteHostClosedError)
                    connectionError(relayEndpoint_, "Cannot reach relay " +
                                                        QUrl(relayEndpoint_).authority() + ". " +
                                                        socket->errorString());
                cleanup();
            });
    const QUrl address(relayEndpoint_);
    socket->connectToHost(address.host(), address.port());
}

void Node::registerRelay() {
    if (!relay_ || relayNonce_.isEmpty() || relay_->state() != QAbstractSocket::ConnectedState)
        return;
    QJsonArray communities;
    for (const auto& model : models_)
        if (model.isMember(peerId()))
            communities.append(model.id);
    relay_->write(frame(signedObject({{"type", "register"},
                                      {"peer", peerId()},
                                      {"nonce", relayNonce_},
                                      {"communities", communities}},
                                     store_.identity(), "disquisition/relay/v1\n")));
}

void Node::readRelay() {
    try {
        relayLastRead_ = now();
        QJsonObject message;
        while (relay_ && readPlainFrame(relay_, message)) {
            if (message.value("type") == "challenge") {
                relayNonce_ = message.value("nonce").toString();
                registerRelay();
            }
            else if (message.value("type") == "registered") {
                connectionErrors_.remove(relayEndpoint_);
                if (!relayRegistered_) {
                    relayRegistered_ = true;
                    retries_.clear();
                    emit relayConnectionChanged(true);
                }
            }
            else if (message.value("type") == "error") {
                connectionError(relayEndpoint_, message.value("message").toString());
                relay_->abort();
            }
            else if (message.value("type") == "incoming") {
                if (links_.size() >= 256)
                    continue;
                auto* link = makeLink(false, message.value("peer").toString());
                link->relayAction = "accept";
                link->ticket = message.value("ticket").toString();
                const QUrl address(relayEndpoint_);
                link->socket->connectToHost(address.host(), address.port());
            }
        }
    }
    catch (const std::exception&) {
        if (relay_)
            relay_->abort();
    }
}

void Node::connectionError(const QString& route, const QString& message) {
    if (connectionErrors_.value(route) == message)
        return;
    connectionErrors_[route] = message;
    emit errorOccurred(message);
}

void Node::tick() {
    if (!running_)
        return;
    try {
        if (recoveringUntil_ != 0 && now() >= recoveringUntil_) {
            recoveringUntil_ = 0;
            emit changed();
        }
        connectRelay();
        if (relay_ && now() - relayLastRead_ > (relayRegistered_ ? 35000 : 10000))
            relay_->abort();
        if (relay_ && !relayNonce_.isEmpty() && now() / 10000 != (now() - 500) / 10000)
            relay_->write(frame({{"type", "ping"}}));
        const auto connections = links_;
        for (auto* link : connections) {
            if (!links_.contains(link))
                continue;
            bool stalled = false;
            for (const auto& snapshot : link->snapshots)
                if (!snapshot.complete && now() - snapshot.started > 20000)
                    stalled = true;
            if ((!link->hello && now() - link->started > 10000) ||
                (link->hello && now() - link->touched > 35000) || stalled) {
                link->socket->abort();
                continue;
            }
            if (link->hello && link->allowed.isEmpty() && link->admissions.isEmpty() &&
                now() - link->started > 30000) {
                link->socket->abort();
                continue;
            }
            if (link->hello && now() - link->touched > 10000)
                send(link, {{"type", "ping"}});
        }
        for (const auto& model : models_) {
            if (!model.isMember(peerId()))
                continue;
            for (const auto& member : model.members) {
                if (member.id == peerId() || peers_.contains(member.id))
                    continue;
                // Either side may dial after a restart; duplicate selection is
                // deterministic and the registry still retains one connection.
                dial(member.id, member.endpoint);
            }
        }
        for (auto& invite : pending_) {
            if (joining_.contains(invite.community)) {
                if (now() - joinStarted_.value(invite.community) <= 20000)
                    continue;
                joining_.remove(invite.community);
                joinStarted_.remove(invite.community);
                if (invite.seeds.size() > 1) {
                    const auto seed = invite.seeds.takeAt(0);
                    invite.seeds.append(seed);
                }
            }
            for (const auto& value : invite.seeds) {
                const auto seed = value.toObject();
                const auto peer = seed.value("peer").toString(),
                           endpoint = seed.value("endpoint").toString();
                Link* selected = peers_.value(peer, nullptr);
                if (!selected) {
                    dial(peer, endpoint, invite.community);
                    const auto route = peer + ':' + endpoint + ':' + invite.community;
                    for (auto* connection : links_)
                        if (connection->route == route) {
                            selected = connection;
                            break;
                        }
                }
                if (selected) {
                    joining_[invite.community] = selected;
                    joinStarted_[invite.community] = now();
                    if (selected->hello)
                        offer(selected);
                    break;
                }
            }
        }
        processAdmissions();
    }
    catch (const std::exception& error) {
        emit errorOccurred(QString::fromUtf8(error.what()));
    }
}
} // namespace community
