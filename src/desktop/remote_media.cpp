#include "desktop/remote_media.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStringList>
#include <QTimer>
#include <algorithm>
#include <optional>

namespace {

bool isTrustedHost(const QString& host) {
    static const QRegularExpression giphy("^(i|media[0-9]?)\\.giphy\\.com$");
    static const QStringList others{"media.tenor.com", "media1.tenor.com", "c.tenor.com",
                                    "static.klipy.com"};
    return giphy.match(host).hasMatch() || others.contains(host);
}

// giphy.com/gifs/<slug>-<id>, giphy.com/gifs/<id> and giphy.com/embed/<id>.
std::optional<QUrl> giphyPageMedia(const QUrl& url) {
    if (url.scheme() != "https" || (url.host() != "giphy.com" && url.host() != "www.giphy.com")) {
        return std::nullopt;
    }
    const QStringList parts = url.path().split('/', Qt::SkipEmptyParts);
    if (parts.size() != 2 || (parts[0] != "gifs" && parts[0] != "embed")) {
        return std::nullopt;
    }
    static const QRegularExpression id("^[A-Za-z0-9]{6,40}$");
    const QString candidate = parts[1].section('-', -1);
    if (!id.match(candidate).hasMatch()) {
        return std::nullopt;
    }
    return QUrl("https://media.giphy.com/media/" + candidate + "/giphy.gif");
}

constexpr qsizetype kCacheBytes = 48LL * 1024 * 1024;
constexpr int kTransferTimeoutMs = 20000;
constexpr int kMaxRedirects = 3;

} // namespace

bool isTrustedMediaUrl(const QUrl& url) {
    return url.isValid() && url.scheme() == "https" && url.userInfo().isEmpty() &&
           (url.port() == -1 || url.port() == 443) && isTrustedHost(url.host());
}

QList<MediaLink> findMediaLinks(const QString& body) {
    static const QRegularExpression link("https://[^\\s<>\"']+",
                                         QRegularExpression::CaseInsensitiveOption);
    QList<MediaLink> links;
    auto matches = link.globalMatch(body);
    while (matches.hasNext() && links.size() < kMaxMediaLinksPerMessage) {
        QString text = matches.next().captured();
        // Trailing punctuation usually ends the sentence, not the link.
        while (!text.isEmpty() && QStringLiteral(".,!?;:)]").contains(text.back())) {
            text.chop(1);
        }
        const QUrl url(text, QUrl::StrictMode);
        QUrl media;
        if (const auto page = giphyPageMedia(url)) {
            media = *page;
        }
        else if (isTrustedMediaUrl(url)) {
            media = url;
        }
        else {
            continue;
        }
        const bool duplicate = std::any_of(
            links.cbegin(), links.cend(), [&](const MediaLink& seen) { return seen.url == media; });
        if (!duplicate) {
            links.append({text, media});
        }
    }
    return links;
}

MediaFetcher::MediaFetcher(QNetworkAccessManager* network, QObject* parent) :
    QObject(parent), network_(network), cache_(kCacheBytes) {
}

MediaFetcher::~MediaFetcher() = default;

void MediaFetcher::fetch(const QUrl& url, QObject* context, Callback done) {
    if (!isTrustedMediaUrl(url)) {
        QTimer::singleShot(0, context, [done = std::move(done)] { done({}); });
        return;
    }
    const QString key = url.toString(QUrl::FullyEncoded);
    if (const QByteArray* cached = cache_.object(key)) {
        QTimer::singleShot(0, context, [done = std::move(done), bytes = *cached] { done(bytes); });
        return;
    }
    QList<Waiter>& waiters = waiting_[key];
    waiters.append({context, std::move(done)});
    if (waiters.size() > 1) {
        return; // already downloading
    }

    QNetworkRequest request(url);
    request.setTransferTimeout(kTransferTimeoutMs);
    request.setMaximumRedirectsAllowed(kMaxRedirects);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::UserVerifiedRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setHeader(QNetworkRequest::UserAgentHeader, "disquisition");
    // QTextBrowser shows GIFs, and Qt may lack a WebP plugin; ask for GIF first.
    request.setRawHeader("Accept", "image/gif,image/png,image/jpeg;q=0.9,image/*;q=0.5");
    QNetworkReply* reply = network_->get(request);
    connect(reply, &QNetworkReply::redirected, reply, [reply](const QUrl& target) {
        if (isTrustedMediaUrl(target)) {
            emit reply->redirectAllowed();
        }
        else {
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64 total) {
        if (received > kMaxMediaBytes || total > kMaxMediaBytes) {
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, key, reply] { finish(key, reply); });
}

void MediaFetcher::finish(const QString& key, QNetworkReply* reply) {
    reply->deleteLater();
    QByteArray bytes;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() == QNetworkReply::NoError && status == 200) {
        bytes = reply->readAll();
        if (bytes.size() > kMaxMediaBytes) {
            bytes.clear();
        }
    }
    if (!bytes.isEmpty()) {
        cache_.insert(key, new QByteArray(bytes), bytes.size());
    }
    // A callback may fetch this URL again, so take the list first.
    const QList<Waiter> waiters = waiting_.take(key);
    for (const Waiter& waiter : waiters) {
        if (waiter.context) {
            waiter.done(bytes);
        }
    }
}
