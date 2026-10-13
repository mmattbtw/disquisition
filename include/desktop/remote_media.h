#pragma once

#include <QByteArray>
#include <QCache>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

// GIF and image links that the chat shows inline, as Discord does.
//
// Only HTTPS media hosts of GIPHY, KLIPY and Tenor are trusted. Fetching any
// other link would let whoever posted it learn the IP address of everyone
// who sees the message, which the relay exists to hide.

constexpr int kMaxMediaLinksPerMessage = 3;

struct MediaLink {
    QString text; // as written in the message
    QUrl url;     // the media to fetch
};

bool isTrustedMediaUrl(const QUrl& url);

// Finds up to kMaxMediaLinksPerMessage distinct media links in `body`, in
// order. A GIPHY page link resolves to its GIF.
QList<MediaLink> findMediaLinks(const QString& body);

// Downloads GIFs and images for the chat and the GIF picker.
//
// Only trusted media hosts are contacted, and a redirect is followed only to
// another trusted host. Downloads are capped at kMaxMediaBytes, cookies are
// neither sent nor kept, and recent downloads are cached in memory, so the
// same GIF posted twice is fetched once.
class MediaFetcher final : public QObject {
    Q_OBJECT

public:
    static constexpr qint64 kMaxMediaBytes = 8LL * 1024 * 1024;
    // Receives the downloaded bytes, or an empty array on failure.
    using Callback = std::function<void(const QByteArray& bytes)>;

    explicit MediaFetcher(QNetworkAccessManager* network, QObject* parent = nullptr);
    ~MediaFetcher() override;

    // Calls `done` later, and only while `context` still exists.
    void fetch(const QUrl& url, QObject* context, Callback done);

private:
    struct Waiter {
        QPointer<QObject> context;
        Callback done;
    };

    void finish(const QString& key, QNetworkReply* reply);

    QNetworkAccessManager* network_;
    QCache<QString, QByteArray> cache_;
    QHash<QString, QList<Waiter>> waiting_;
};
