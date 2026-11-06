#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QFrame>
#include <QImage>
#include <QList>
#include <QPixmap>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "desktop/screen_drawing.h"

class QLabel;
class QPushButton;
class QStackedWidget;
class QShortcut;

// Draws the newest frame of a stream, scaled to fit and letterboxed.
class StreamView final : public DrawingCanvas {
    Q_OBJECT
public:
    explicit StreamView(QWidget* parent = nullptr);
    const QImage& image() const { return image_; }
    void setImage(const QImage& image);
    void clear();

signals:
    void doubleClicked();
    void menuRequested(const QPoint& globalPosition);

protected:
    QRectF drawingRect() const override;
    void paintEvent(QPaintEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    QImage image_;
};

// One stream on the stage. Until the user chooses to watch it, a remote
// stream shows a "watch stream" button instead of video, so nobody downloads
// a stream they did not ask for. In the grid it is a video tile with a name
// and LIVE badge; focused, it gains a header with its controls and the
// drawing toolbar floats over the video.
class StreamTile final : public QFrame {
    Q_OBJECT
public:
    StreamTile(const QString& name, bool own, QWidget* parent = nullptr, const QString& label = {});
    QString name() const { return name_; }
    bool own() const { return own_; }
    bool watching() const { return watching_; }
    QString displayName() const { return label_; }
    void setDisplayName(const QString& label);
    const QImage& image() const { return view_->image(); }
    void setWatching(bool watching);
    // The stream's sound, from 0 to 100 percent, set from a menu on the video.
    void setVolume(int percent) { volume_ = percent; }
    void showVolumeMenu(const QPoint& globalPosition);
    // Focused tiles show their controls; tiles in the grid or strip stay bare.
    void setFocused(bool focused);
    bool isFocused() const { return focused_; }
    // Shown before the frame statistics, such as the encoder in use.
    void setDetail(const QString& detail) { detail_ = detail; }
    void showFrame(const QImage& image, qsizetype encodedBytes);
    ScreenDrawing* drawing() const { return drawing_; }
    // Who is watching, as overlapping avatars that list their names when clicked.
    void setAudience(const QList<QPixmap>& avatars, const QStringList& names);

signals:
    void watchClicked();
    void stopWatchingClicked();
    void focusClicked();
    void showAllClicked();
    void fullscreenClicked();
    void volumeChanged(int percent);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void refreshChrome();
    void placeOverlays();
    void showAudience();

    QString name_;
    QString label_;
    bool own_ = false;
    bool focused_ = false;
    QString detail_;
    QWidget* header_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* stats_ = nullptr;
    QLabel* invitationText_ = nullptr;
    QStackedWidget* body_ = nullptr;
    QWidget* invitation_ = nullptr;
    StreamView* view_ = nullptr;
    ScreenDrawing* drawing_ = nullptr;
    DrawingControls* drawingControls_ = nullptr;
    QLabel* badge_ = nullptr;
    QLabel* quality_ = nullptr;
    QLabel* nameplate_ = nullptr;
    QPushButton* showAllButton_ = nullptr;
    QPushButton* fullscreenButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QPushButton* audience_ = nullptr;
    QList<QPixmap> audienceAvatars_;
    QStringList audienceNames_;
    int volume_ = 100;
    bool watching_ = false;
    QElapsedTimer statsClock_;
    int statsFrames_ = 0;
    qint64 statsBytes_ = 0;
};

// Someone in the call, for the stage's tiles.
struct StageParticipant {
    QString id;
    QString name;
    QPixmap avatar;
    bool muted = false;
    bool deafened = false;
    bool sharing = false;
};

// A person in the call as a 16:9 tile, showing their camera when it is on. It
// glows green while they talk.
class ParticipantTile final : public QWidget {
    Q_OBJECT
public:
    explicit ParticipantTile(QWidget* parent = nullptr);
    void setParticipant(const StageParticipant& person);
    const StageParticipant& participant() const { return person_; }
    void setSpeaking(bool speaking);
    bool speaking() const { return speaking_; }
    // The newest camera frame, or a null image to show their picture instead.
    void setVideo(const QImage& image);
    const QImage& video() const { return video_; }

signals:
    void clicked(const QPoint& globalPosition);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    StageParticipant person_;
    bool speaking_ = false;
    QImage video_;
};

// The call, laid out like a video call. Screen shares and people share one
// grid of 16:9 tiles. Clicking a share focuses it: it fills the stage with the
// rest in a strip beneath, which can be hidden. Any watched stream can also
// fill the screen in a window of its own.
class ScreenStage final : public QWidget {
    Q_OBJECT
public:
    explicit ScreenStage(QWidget* parent = nullptr);
    // `own` streams are this user's share: always shown, never "watched".
    void addStream(const QString& name, bool own, const QString& label = {});
    void removeStream(const QString& name);
    void setWatching(const QString& name, bool watching);
    void setDisplayName(const QString& name, const QString& label);
    void setVolume(const QString& name, int percent);
    void setDetail(const QString& name, const QString& detail);
    void showFrame(const QString& name, const QImage& image, qsizetype encodedBytes);
    ScreenDrawing* drawing(const QString& name) const;
    int count() const { return static_cast<int>(tiles_.size()); }
    // The newest frame of a stream, or a null image.
    QImage image(const QString& name) const;
    void setAudience(const QString& name, const QList<QPixmap>& avatars, const QStringList& names);
    void setParticipants(const QList<StageParticipant>& people);
    void setSpeaking(const QString& id, bool speaking);
    // Shows someone's camera in their tile; a null image ends it.
    void setCameraFrame(const QString& id, const QImage& image);
    QImage cameraFrame(const QString& id) const { return cameras_.value(id); }
    QString focused() const { return focused_; }
    void setFocused(const QString& name);

signals:
    void watchRequested(const QString& name);
    void stopWatchingRequested(const QString& name);
    void volumeChanged(const QString& name, int percent);
    void participantClicked(const QString& id, const QPoint& globalPosition);
    // The first share appeared on an empty stage.
    void opened();

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    StreamTile* find(const QString& name) const;
    void toggleFullscreen(const QString& name);
    void exitFullscreen();
    void relayout();
    void refreshEscapeShortcut();

    QList<StreamTile*> tiles_;
    QList<ParticipantTile*> people_;
    QSet<QString> speaking_;
    QHash<QString, QImage> cameras_;
    QString focused_;
    bool stripHidden_ = false;
    QPushButton* stripToggle_ = nullptr;
    // Closing the window with the system's own controls deletes it.
    QPointer<StreamView> fullscreen_;
    QString fullscreenName_;
    QShortcut* escape_ = nullptr;
};
