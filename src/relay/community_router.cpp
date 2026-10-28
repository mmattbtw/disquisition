#include "relay/community_router.h"

#include <QDateTime>
#include <QJsonArray>
#include <QTcpSocket>
#include <algorithm>

#include "community/identity.h"
#include "community/wire.h"

namespace chat {
using namespace community;
struct CommunityRouter::Session {
    QTcpSocket* socket = nullptr;
    QByteArray buffer;
    QString nonce;
    QString peer;
    QSet<QString> communities;
    Session* paired = nullptr;
    bool tunnel = false;
    bool control = false;
    bool started = false;
    qint64 touched = QDateTime::currentMSecsSinceEpoch();
};

CommunityRouter::CommunityRouter(QObject* parent) : QObject(parent) {
    timer_.setParent(this);
    timer_.setInterval(1000);
    connect(&timer_, &QTimer::timeout, this, [this] {
        const auto now = QDateTime::currentMSecsSinceEpoch();
        const auto sessions = sessions_;
        for (auto* session : sessions) {
            if (!sessions_.contains(session))
                continue;
            if (!session->tunnel && now - session->touched > (session->control ? 40000 : 15000))
                session->socket->abort();
        }
    });
}

void CommunityRouter::start() {
    timer_.start();
}

void CommunityRouter::adopt(qintptr descriptor) {
    auto* socket = new QTcpSocket(this);
    if (!socket->setSocketDescriptor(descriptor)) {
        socket->deleteLater();
        return;
    }
    if (sessions_.size() >= 512) {
        socket->abort();
        socket->deleteLater();
        return;
    }
    auto* session = new Session;
    session->socket = socket;
    session->nonce = randomCode();
    sessions_.append(session);
    socket->setReadBufferSize(256 * 1024);
    connect(socket, &QTcpSocket::readyRead, this, [this, session] { read(session); });
    connect(socket, &QTcpSocket::bytesWritten, this, [this, session] {
        if (session->paired)
            read(session->paired);
    });
    connect(socket, &QTcpSocket::disconnected, this, [this, session] {
        auto* paired = session->paired;
        if (paired)
            paired->paired = nullptr;
        if (registered_.value(session->peer) == session)
            registered_.remove(session->peer);
        for (auto it = waiting_.begin(); it != waiting_.end();) {
            if (it.value() == session)
                it = waiting_.erase(it);
            else
                ++it;
        }
        sessions_.removeAll(session);
        session->socket->deleteLater();
        delete session;
        if (paired)
            paired->socket->abort();
    });
    read(session);
}

CommunityRouter::~CommunityRouter() {
    close();
}

void CommunityRouter::close() {
    timer_.stop();
    while (!sessions_.isEmpty())
        sessions_.first()->socket->abort();
}

void CommunityRouter::send(Session* session, const QJsonObject& message) {
    session->socket->write(frame(message));
}

void CommunityRouter::read(Session* session) {
    session->touched = QDateTime::currentMSecsSinceEpoch();
    if (!session->started) {
        constexpr auto size = sizeof(kRelayPrefix) - 1;
        const auto prefix = session->socket->peek(size);
        if (!QByteArray(kRelayPrefix, size).startsWith(prefix)) {
            session->socket->abort();
            return;
        }
        if (prefix.size() < static_cast<qsizetype>(size))
            return;
        session->socket->read(size);
        session->started = true;
        send(session, {{"type", "challenge"}, {"nonce", session->nonce}});
    }
    if (session->tunnel) {
        if (!session->paired)
            return;
        auto* destination = session->paired->socket;
        const qint64 budget = 512 * 1024 - destination->bytesToWrite();
        if (budget > 0)
            destination->write(session->socket->read(budget));
        return;
    }
    try {
        session->buffer += session->socket->readAll();
        if (session->buffer.size() > 64 * 1024)
            throw std::runtime_error("relay request too large");
        QJsonObject request;
        while (takeFrame(session->buffer, request)) {
            route(session, request);
            if (!sessions_.contains(session) || session->tunnel)
                break;
        }
    }
    catch (const std::exception&) {
        session->socket->abort();
    }
}

void CommunityRouter::route(Session* session, const QJsonObject& request) {
    const auto type = request.value("type").toString();
    if (session->control && type == "ping") {
        send(session, {{"type", "pong"}});
        return;
    }
    const auto peer = request.value("peer").toString();
    if (request.value("nonce").toString() != session->nonce || !isPeerId(peer) ||
        !verifyObject(request, peer, "disquisition/relay/v1\n")) {
        session->socket->abort();
        return;
    }
    if (type == "register") {
        if (registered_.contains(peer) && registered_.value(peer) != session) {
            session->socket->abort();
            return;
        }
        const auto communities = request.value("communities").toArray();
        if (communities.size() > 64) {
            session->socket->abort();
            return;
        }
        session->control = true;
        session->peer = peer;
        session->communities.clear();
        for (const auto& value : communities)
            session->communities.insert(value.toString());
        registered_[peer] = session;
        send(session, {{"type", "registered"}});
    }
    else if (type == "dial" && !session->control) {
        const auto target = request.value("target").toString();
        Session* receiver = registered_.value(target, nullptr);
        if (!receiver) {
            const auto community = request.value("community").toString();
            if (!community.isEmpty()) {
                for (auto* candidate : registered_) {
                    if (candidate->peer != peer && candidate->communities.contains(community)) {
                        receiver = candidate;
                        break;
                    }
                }
            }
        }
        if (!receiver || receiver->peer == peer) {
            send(session, {{"type", "unavailable"}});
            session->socket->disconnectFromHost();
            return;
        }
        const auto ticket = randomCode();
        session->peer = receiver->peer;
        waiting_[ticket] = session;
        send(receiver, {{"type", "incoming"}, {"ticket", ticket}, {"peer", peer}});
    }
    else if (type == "accept" && !session->control) {
        auto* caller = waiting_.take(request.value("ticket").toString());
        if (!caller || caller->peer != peer) {
            session->socket->abort();
            return;
        }
        bridge(caller, session);
    }
    else {
        session->socket->abort();
    }
}

void CommunityRouter::bridge(Session* a, Session* b) {
    a->paired = b;
    b->paired = a;
    a->tunnel = true;
    b->tunnel = true;
    // Peers switch to TLS only after this plaintext frame. All bytes after it
    // pass unchanged, including the mutual TLS handshake.
    send(a, {{"type", "ready"}, {"peer", a->peer}});
    send(b, {{"type", "ready"}});
}
} // namespace chat
