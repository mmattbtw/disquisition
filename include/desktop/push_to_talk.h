#pragma once

#include <QObject>
#include <QPushButton>
#include <QString>
#include <QTimer>
#include <optional>

class QKeyEvent;
struct xcb_connection_t;
struct __IOHIDManager;

// Reads the key globally on Windows, X11 and macOS with Input Monitoring;
// elsewhere only presses while this app is active count.
class PushToTalk final : public QObject {
    Q_OBJECT

public:
    explicit PushToTalk(QObject* parent = nullptr);
    ~PushToTalk() override;
    void start(quint32 key);
    void stop();
    bool held() const { return held_; }
    static quint32 nativeKey(const QKeyEvent* event);
    static QString keyName(const QKeyEvent* event);

signals:
    void heldChanged(bool held);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void poll();
    void setHeld(bool held);

    QTimer poll_;
    std::optional<quint32> key_;
    bool held_ = false;
    bool global_ = false;
    xcb_connection_t* x11_ = nullptr;
    __IOHIDManager* capsLock_ = nullptr;
};

class PushToTalkKeyButton final : public QPushButton {
    Q_OBJECT

public:
    explicit PushToTalkKeyButton(QWidget* parent = nullptr);
    void setKey(std::optional<quint32> key, const QString& name);
    std::optional<quint32> key() const { return key_; }
    QString keyName() const { return name_; }

protected:
    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    void showKey();

    std::optional<quint32> key_;
    QString name_;
    bool capturing_ = false;
};
