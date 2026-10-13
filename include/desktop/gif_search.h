#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QUrl>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;

// Searches GIPHY and KLIPY for the GIF picker. Each provider needs its own
// API key. Tenor is not offered: Google shut down the Tenor API on
// 30 June 2026, though Tenor links people paste still show inline.
enum class GifProvider { Giphy, Klipy };

constexpr int kGifPageSize = 24;

QString gifProviderName(GifProvider provider);

struct GifResult {
    QString title;
    QUrl previewUrl; // small animated rendition for the picker
    QUrl shareUrl;   // rendition posted to the chat
    QSize previewSize;
};

struct GifPage {
    QList<GifResult> results;
    bool hasMore = false;
};

// An empty query asks for trending GIFs. Pages count from 0.
QUrl giphyRequestUrl(const QString& apiKey, const QString& query, int page);
QUrl klipyRequestUrl(const QString& apiKey, const QString& customerId, const QString& query,
                     int page);

// Return nothing when the reply is not the provider's response format.
// Results whose media is not on a trusted host are left out.
std::optional<GifPage> parseGiphyResponse(const QByteArray& json);
std::optional<GifPage> parseKlipyResponse(const QByteArray& json);

// The provider's explanation of a failed request, or an empty string.
QString gifErrorMessage(const QByteArray& json);

class GifSearch final : public QObject {
    Q_OBJECT

public:
    explicit GifSearch(QNetworkAccessManager* network, QObject* parent = nullptr);
    ~GifSearch() override;

    // `klipyCustomerId` should stay the same for this user across sessions.
    void setKeys(const QString& giphyKey, const QString& klipyKey, const QString& klipyCustomerId);
    bool hasKey(GifProvider provider) const;

    // Replaces any request still in flight.
    void request(GifProvider provider, const QString& query, int page);
    void cancel();

signals:
    void pageReady(GifProvider provider, const QString& query, int page, const GifPage& results);
    void failed(GifProvider provider, const QString& query, int page, const QString& message);

private:
    QNetworkAccessManager* network_;
    QPointer<QNetworkReply> reply_;
    QString giphyKey_;
    QString klipyKey_;
    QString klipyCustomerId_;
};
