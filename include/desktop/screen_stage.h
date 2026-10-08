#pragma once

#include <QElapsedTimer>
#include <QFrame>
#include <QImage>
#include <QList>
#include <QPointer>
#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;
class QStackedWidget;

// Draws the newest frame of a stream, scaled to fit and letterboxed.
class StreamView final : public QWidget {
    Q_OBJECT
public:
    explicit StreamView(QWidget* parent = nullptr);
    const QImage& image() const { return image_; }
    void setImage(const QImage& image);
    void clear();

signals:
    void doubleClicked();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    QImage image_;
};

// One stream on the stage. Until the user chooses to watch it, a remote
// stream shows a "watch stream" button instead of video, so nobody downloads
// a stream they did not ask for.
class StreamTile final : public QFrame {
    Q_OBJECT
public:
    StreamTile(const QString& name, bool own, QWidget* parent = nullptr);
    QString name() const { return name_; }
    const QImage& image() const { return view_->image(); }
    void setWatching(bool watching);
    // `available` is false while the tile is alone on the stage.
    void setFocusState(bool available, bool focused);
    // Shown before the frame statistics, such as the encoder in use.
    void setDetail(const QString& detail) { detail_ = detail; }
    void showFrame(const QImage& image, qsizetype encodedBytes);

signals:
    void watchClicked();
    void stopWatchingClicked();
    void focusClicked();
    void fullscreenClicked();

private:
    QString name_;
    bool own_ = false;
    QString detail_;
    QStackedWidget* body_ = nullptr;
    QWidget* invitation_ = nullptr;
    StreamView* view_ = nullptr;
    QLabel* stats_ = nullptr;
    QPushButton* focusButton_ = nullptr;
    QPushButton* fullscreenButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QElapsedTimer statsClock_;
    int statsFrames_ = 0;
    qint64 statsBytes_ = 0;
};

// The screen shares in the room, laid out like a video call: a grid of tiles,
// or one focused tile with the rest in a strip beneath it. Any watched stream
// can also fill the screen in a window of its own. Hidden while nobody is
// sharing.
class ScreenStage final : public QWidget {
    Q_OBJECT
public:
    explicit ScreenStage(QWidget* parent = nullptr);
    // `own` streams are this user's share: always shown, never "watched".
    void addStream(const QString& name, bool own);
    void removeStream(const QString& name);
    void setWatching(const QString& name, bool watching);
    void setDetail(const QString& name, const QString& detail);
    void showFrame(const QString& name, const QImage& image, qsizetype encodedBytes);

signals:
    void watchRequested(const QString& name);
    void stopWatchingRequested(const QString& name);
    // The first share appeared on an empty, hidden stage.
    void opened();

private:
    StreamTile* find(const QString& name) const;
    void toggleFocus(const QString& name);
    void toggleFullscreen(const QString& name);
    void exitFullscreen();
    void relayout();

    QList<StreamTile*> tiles_;
    QString focused_;
    // Closing the window with the system's own controls deletes it.
    QPointer<StreamView> fullscreen_;
    QString fullscreenName_;
};
