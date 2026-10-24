#include "desktop/screen_drawing_session.h"

#include <QDateTime>

namespace {
// Unique across clears, new shares and process restarts without server state.
std::int64_t nextEpoch() {
    static std::int64_t previous = 0;
    previous = qMax<std::int64_t>(previous + 1, QDateTime::currentMSecsSinceEpoch() * 1000);
    return previous;
}
} // namespace

ScreenDrawingSession::ScreenDrawingSession(const QString& localName, const QString& sharer,
                                           ScreenDrawing* drawing, bool owner, QObject* parent) :
    QObject(parent), localName_(localName), sharer_(sharer), drawing_(drawing), owner_(owner) {
    drawing->setIdentity(localName, owner);
    connect(drawing, &ScreenDrawing::permissionRequested, this, [this](bool enabled) {
        if (owner_)
            reset(enabled);
    });
    connect(drawing, &ScreenDrawing::clearRequested, this, [this] {
        if (owner_ && drawing_ && drawing_->enabled())
            reset(true);
    });
    connect(drawing, &ScreenDrawing::segmentDrawn, this, &ScreenDrawingSession::submit);
    connect(drawing, &ScreenDrawing::laserPointed, this, &ScreenDrawingSession::submitLaser);
    connect(drawing, &ScreenDrawing::eraseRequested, this, &ScreenDrawingSession::erase);
    connect(drawing, &ScreenDrawing::clearMineRequested, this, [this] {
        if (drawing_)
            erase(localName_, drawing_->strokeIds(localName_));
    });
    retry_.setInterval(1000);
    connect(&retry_, &QTimer::timeout, this, [this] {
        if (++attempts_ >= 5) {
            expireRequest();
            return;
        }
        emit sendToPeer(sharer_, watch(true));
    });
    if (owner_)
        reset(false);
}

chat::Message ScreenDrawingSession::state(const QString& viewer) const {
    chat::Message message{chat::MsgType::ScreenDrawState,
                          {sharer_.toStdString(), drawing_->enabled() ? "1" : "0",
                           std::to_string(drawing_->epoch())}};
    // Preserve the existing state shape for unrestricted viewers. Updated
    // clients interpret a missing personal permission as permission to draw.
    if (isBanned(viewer))
        message.fields.push_back("0");
    return message;
}

chat::Message ScreenDrawingSession::watch(bool enabled) const {
    return {chat::MsgType::ScreenDrawWatch, {sharer_.toStdString(), enabled ? "1" : "0"}};
}

void ScreenDrawingSession::broadcast(const chat::Message& message) {
    const auto recipients = viewers_;
    for (const QString& viewer : recipients)
        emit sendToPeer(viewer, message);
}

void ScreenDrawingSession::reset(bool enabled) {
    if (!drawing_)
        return;
    drawing_->setStatus({});
    drawing_->setState(enabled, nextEpoch());
    const auto recipients = viewers_;
    for (const auto& viewer : recipients)
        emit sendToPeer(viewer, state(viewer));
}

void ScreenDrawingSession::sendSnapshot(const QString& viewer) {
    emit sendToPeer(viewer, state(viewer));
    const auto history = drawing_->segments();
    for (const auto& segment : history)
        emit sendToPeer(viewer, chat::screenDrawMessage(segment, true));
}

void ScreenDrawingSession::setBanned(const QString& peer, bool banned) {
    if (!owner_ || !drawing_ || peer.isEmpty() || peer == localName_ || isBanned(peer) == banned)
        return;
    if (banned)
        banned_.insert(peer);
    else
        banned_.remove(peer);
    // Replay confirmed ink to discard any optimistic stroke rejected by the ban.
    if (viewers_.contains(peer))
        sendSnapshot(peer);
}

void ScreenDrawingSession::start() {
    if (owner_ || !drawing_)
        return;
    watching_ = true;
    drawing_->setState(false, 0);
    drawing_->setStatus("connecting drawing...");
    attempts_ = 0;
    // Start before emitting: a synchronous peer response may stop this timer.
    retry_.start();
    emit sendToPeer(sharer_, watch(true));
}

void ScreenDrawingSession::stop() {
    retry_.stop();
    if (owner_)
        viewers_.clear();
    else if (watching_) {
        watching_ = false;
        emit sendToPeer(sharer_, watch(false));
    }
}

void ScreenDrawingSession::expireRequest() {
    if (owner_ || !watching_ || !retry_.isActive() || !drawing_)
        return;
    retry_.stop();
    drawing_->setState(false, 0);
    drawing_->setStatus("drawing unavailable: update both apps and the relay");
}

void ScreenDrawingSession::peerChanged(const QString& peer, bool connected) {
    if (owner_) {
        if (!connected)
            viewers_.remove(peer);
    }
    else if (peer == sharer_ && watching_)
        start();
}

void ScreenDrawingSession::submit(chat::ScreenDraw segment) {
    if (!drawing_ || !drawing_->drawingAllowed())
        return;
    segment.sharer = sharer_.toStdString();
    segment.author = localName_.toStdString();
    segment.epoch = drawing_->epoch();
    if (owner_)
        accept(segment);
    else if (watching_) {
        drawing_->add(segment); // local preview, deduplicated on the owner's echo
        emit sendToPeer(sharer_, chat::screenDrawMessage(segment, false));
    }
}

void ScreenDrawingSession::accept(chat::ScreenDraw segment) {
    if (!drawing_ || !drawing_->enabled() || segment.epoch != drawing_->epoch())
        return;
    if (drawing_->historySize() >= static_cast<qsizetype>(chat::kMaxDrawSegments)) {
        reset(true);
        return;
    }
    const qsizetype before = drawing_->segments().size();
    drawing_->add(segment);
    if (drawing_->segments().size() != before)
        broadcast(chat::screenDrawMessage(segment, true));
}

void ScreenDrawingSession::submitLaser(chat::ScreenLaser point) {
    if (!drawing_ || !drawing_->drawingAllowed())
        return;
    point.sharer = sharer_.toStdString();
    point.author = localName_.toStdString();
    point.epoch = drawing_->epoch();
    if (owner_)
        acceptLaser(point);
    else if (watching_) {
        drawing_->addLaser(point);
        emit sendToPeer(sharer_, chat::screenLaserMessage(point, false));
    }
}

void ScreenDrawingSession::acceptLaser(const chat::ScreenLaser& point) {
    if (drawing_ && drawing_->addLaser(point))
        broadcast(chat::screenLaserMessage(point, true));
}

void ScreenDrawingSession::erase(const QString& author, const QList<quint32>& strokes) {
    if (!drawing_ || !drawing_->enabled() || (!owner_ && (!watching_ || drawing_->banned() || author != localName_)))
        return;
    for (qsizetype offset = 0; offset < strokes.size(); offset += chat::kMaxEraseStrokes) {
        chat::ScreenErase request;
        request.sharer = sharer_.toStdString();
        request.author = author.toStdString();
        request.epoch = drawing_->epoch();
        const auto batch = strokes.mid(offset, chat::kMaxEraseStrokes);
        request.strokes.assign(batch.begin(), batch.end());
        drawing_->remove(author, batch);
        if (owner_)
            broadcast(chat::screenEraseMessage(request));
        else
            emit sendToPeer(sharer_, chat::screenEraseMessage(request));
    }
}

void ScreenDrawingSession::receive(const QString& sender, const chat::Message& message) {
    if (!drawing_ || sender == localName_)
        return;
    if (owner_) {
        if (message.type == chat::MsgType::ScreenDrawWatch && message.fields.size() == 2 &&
            message.fields[0] == sharer_.toStdString()) {
            if (message.fields[1] == "0")
                viewers_.remove(sender);
            else if (message.fields[1] == "1") {
                viewers_.insert(sender);
                sendSnapshot(sender);
            }
        }
        else if (message.type == chat::MsgType::ScreenDraw && viewers_.contains(sender) && !isBanned(sender)) {
            chat::ScreenDraw segment;
            if (chat::parseScreenDraw(message, false, segment) &&
                segment.sharer == sharer_.toStdString()) {
                segment.author = sender.toStdString();
                accept(segment);
            }
        }
        else if (message.type == chat::MsgType::ScreenLaser && viewers_.contains(sender) && !isBanned(sender)) {
            chat::ScreenLaser point;
            if (chat::parseScreenLaser(message, false, point) && point.sharer == sharer_.toStdString()) {
                point.author = sender.toStdString();
                acceptLaser(point);
            }
        }
        else if (message.type == chat::MsgType::ScreenDrawErase && viewers_.contains(sender) && !isBanned(sender)) {
            chat::ScreenErase request;
            if (chat::parseScreenErase(message, request) && request.sharer == sharer_.toStdString() &&
                request.epoch == drawing_->epoch() && request.author == sender.toStdString()) {
                const QList<quint32> ids(request.strokes.begin(), request.strokes.end());
                erase(sender, ids);
            }
        }
        return;
    }
    if (!watching_ || sender != sharer_)
        return;
    if (message.type == chat::MsgType::ScreenDrawState &&
        (message.fields.size() == 3 || message.fields.size() == 4) &&
        (message.fields.size() == 3 || message.fields[3] == "0" || message.fields[3] == "1") &&
        message.fields[0] == sharer_.toStdString() &&
        (message.fields[1] == "0" || message.fields[1] == "1")) {
        std::int64_t epoch = 0;
        if (!chat::parseInt64(message.fields[2], epoch) || epoch <= 0)
            return;
        retry_.stop();
        drawing_->setBanned(message.fields.size() == 4 && message.fields[3] == "0");
        drawing_->setStatus({});
        drawing_->setState(message.fields[1] == "1", epoch);
    }
    else if (message.type == chat::MsgType::ScreenLaser) {
        chat::ScreenLaser point;
        if (chat::parseScreenLaser(message, true, point) && point.sharer == sharer_.toStdString())
            drawing_->addLaser(point);
    }
    else if (message.type == chat::MsgType::ScreenDrawErase) {
        chat::ScreenErase request;
        if (chat::parseScreenErase(message, request) && request.sharer == sharer_.toStdString() &&
            request.epoch == drawing_->epoch() && drawing_->enabled()) {
            drawing_->remove(QString::fromStdString(request.author),
                             QList<quint32>(request.strokes.begin(), request.strokes.end()));
        }
    }
    else if (message.type == chat::MsgType::ScreenDraw) {
        chat::ScreenDraw segment;
        if (chat::parseScreenDraw(message, true, segment) &&
            segment.sharer == sharer_.toStdString())
            drawing_->add(segment);
    }
}
