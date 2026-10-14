#pragma once

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QSize>
#include <QUrl>
#include <deque>
#include <map>
#include <memory>

class MediaFetcher;
class QMovie;
class QTextBrowser;

// A movie that plays `bytes`, owning the buffer it reads from. Returns
// nothing when Qt cannot play them.
std::unique_ptr<QMovie> movieFromBytes(const QByteArray& bytes);

// Shows GIFs and images inline in the chat transcript.
//
// QTextBrowser does not animate images, so each animated embed replaces its
// document resource with the movie's current frame. Only the newest few
// embeds keep playing, to bound CPU use in a long chat. Clicking an uploaded
// image opens it in a viewer; clicking a GIF opens its source in the browser.
//
// This takes over the browser's link handling.
class ChatMedia final : public QObject {
    Q_OBJECT

public:
    static constexpr int kMaxPlaying = 8;
    // Embeds are scaled to fit within this many logical pixels.
    static constexpr int kMaxWidth = 360;
    static constexpr int kMaxHeight = 270;

    ChatMedia(QTextBrowser* view, MediaFetcher* fetcher, QObject* parent = nullptr);
    ~ChatMedia() override;

    // Appends a GIF or image from a trusted host as its own paragraph.
    void appendRemote(const QUrl& url);
    // Appends image bytes, such as an upload. Returns false, and appends
    // nothing, when the bytes are not a displayable image.
    bool appendImage(const QByteArray& bytes);
    // Forgets every embed. Call it after clearing the transcript.
    void clear();

private:
    struct Embed {
        QUrl source;       // empty for uploads
        QByteArray bytes;  // kept for uploads, which have no other copy
        int position = 0;  // of the image in the document
        QSize displaySize; // invalid until the media has loaded
        QSize shownSize;   // of the resource the document shows now
        std::unique_ptr<QMovie> movie;
    };

    int insert(Embed embed, const QImage& placeholder);
    void show(int id, const QByteArray& bytes);
    void setFrame(int id, const QImage& frame);
    void startPlaying(int id);
    void open(const QUrl& link);
    static QUrl resourceUrl(int id);

    QTextBrowser* view_;
    MediaFetcher* fetcher_;
    std::map<int, Embed> embeds_;
    std::deque<int> playing_;
    // Ids are never reused, so a fetch that finishes after clear() finds
    // nothing to update.
    int nextId_ = 1;
    bool pinnedToBottom_ = true;
};
