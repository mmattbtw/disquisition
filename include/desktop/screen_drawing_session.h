#pragma once

#include <QPointer>
#include <QSet>
#include <QTimer>

#include "desktop/screen_drawing.h"

// The presenting client owns permissions, subscriptions and bounded history.
// Transport only delivers these messages between named peers.
class ScreenDrawingSession final : public QObject {
    Q_OBJECT
public:
    ScreenDrawingSession(const QString& localName, const QString& sharer, ScreenDrawing* drawing,
                         bool owner, QObject* parent = nullptr);
    void start();
    void stop();
    void receive(const QString& sender, const chat::Message& message);
    void peerChanged(const QString& peer, bool connected);
    void expireRequest();

signals:
    void sendToPeer(const QString& peer, const chat::Message& message);

private:
    void reset(bool enabled);
    void submit(chat::ScreenDraw segment);
    void accept(chat::ScreenDraw segment);
    void erase(const QString& author, const QList<quint32>& strokes);
    void broadcast(const chat::Message& message);
    chat::Message state() const;
    chat::Message watch(bool enabled) const;

    QString localName_;
    QString sharer_;
    QPointer<ScreenDrawing> drawing_;
    bool owner_ = false;
    bool watching_ = false;
    QSet<QString> viewers_;
    QTimer retry_;
    int attempts_ = 0;
};
