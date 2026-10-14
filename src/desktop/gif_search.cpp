#include "desktop/gif_search.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>

#include "desktop/remote_media.h"

namespace {

// GIPHY rejects offsets past 499 on its trending endpoint, so the picker
// stops paging there for both endpoints.
constexpr int kGiphyMaxOffset = 499;
constexpr int kRequestTimeoutMs = 15000;

QJsonObject parseObject(const QByteArray& json) {
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &error);
    return error.error == QJsonParseError::NoError ? document.object() : QJsonObject();
}

// GIPHY sends tracking parameters with each media URL; the GIF loads without them.
QUrl withoutQuery(QUrl url) {
    url.setQuery(QString());
    url.setFragment(QString());
    return url;
}

QSize jsonSize(const QJsonObject& rendition) {
    // GIPHY sends sizes as strings, KLIPY as numbers.
    const auto number = [&](const char* key) {
        const QJsonValue value = rendition.value(key);
        return value.isString() ? value.toString().toInt() : value.toInt();
    };
    return QSize(number("width"), number("height"));
}

void addResult(GifPage& page, GifResult result) {
    if (isTrustedMediaUrl(result.previewUrl) && isTrustedMediaUrl(result.shareUrl)) {
        page.results.append(std::move(result));
    }
}

} // namespace

QString gifProviderName(GifProvider provider) {
    return provider == GifProvider::Giphy ? "GIPHY" : "KLIPY";
}

QUrl giphyRequestUrl(const QString& apiKey, const QString& query, int page) {
    const QString trimmed = query.trimmed();
    QUrl url(trimmed.isEmpty() ? "https://api.giphy.com/v1/gifs/trending"
                               : "https://api.giphy.com/v1/gifs/search");
    QUrlQuery parameters;
    parameters.addQueryItem("api_key", apiKey);
    if (!trimmed.isEmpty()) {
        // GIPHY caps search terms at 50 characters.
        parameters.addQueryItem("q", trimmed.left(50));
    }
    parameters.addQueryItem("limit", QString::number(kGifPageSize));
    parameters.addQueryItem("offset", QString::number(page * kGifPageSize));
    parameters.addQueryItem("rating", "pg-13");
    parameters.addQueryItem("bundle", "messaging_non_clips");
    url.setQuery(parameters);
    return url;
}

QUrl klipyRequestUrl(const QString& apiKey, const QString& customerId, const QString& query,
                     int page) {
    const QString trimmed = query.trimmed();
    QUrl url;
    url.setScheme("https");
    url.setHost("api.klipy.com");
    url.setPath("/api/v1/" + QString::fromLatin1(QUrl::toPercentEncoding(apiKey)) + "/gifs/" +
                    (trimmed.isEmpty() ? "trending" : "search"),
                QUrl::TolerantMode);
    QUrlQuery parameters;
    if (!trimmed.isEmpty()) {
        parameters.addQueryItem("q", trimmed);
    }
    parameters.addQueryItem("page", QString::number(page + 1));
    parameters.addQueryItem("per_page", QString::number(kGifPageSize));
    parameters.addQueryItem("customer_id", customerId);
    parameters.addQueryItem("content_filter", "medium");
    parameters.addQueryItem("format_filter", "gif");
    url.setQuery(parameters);
    return url;
}

std::optional<GifPage> parseGiphyResponse(const QByteArray& json) {
    const QJsonObject root = parseObject(json);
    if (!root.value("data").isArray()) {
        return std::nullopt;
    }
    GifPage page;
    for (const QJsonValue& value : root.value("data").toArray()) {
        const QJsonObject images = value.toObject().value("images").toObject();
        const auto rendition = [&](std::initializer_list<const char*> names) {
            for (const char* name : names) {
                const QJsonObject candidate = images.value(name).toObject();
                if (!candidate.value("url").toString().isEmpty()) {
                    return candidate;
                }
            }
            return QJsonObject();
        };
        const QJsonObject preview = rendition(
            {"fixed_width_downsampled", "fixed_width", "downsized", "original"});
        const QJsonObject share = rendition({"downsized", "downsized_medium", "original"});
        addResult(page,
                  GifResult{value.toObject().value("title").toString(),
                            withoutQuery(QUrl(preview.value("url").toString())),
                            withoutQuery(QUrl(share.value("url").toString())), jsonSize(preview)});
    }
    const QJsonObject pagination = root.value("pagination").toObject();
    const int next = pagination.value("offset").toInt() + pagination.value("count").toInt();
    page.hasMore = next < pagination.value("total_count").toInt() && next <= kGiphyMaxOffset;
    return page;
}

std::optional<GifPage> parseKlipyResponse(const QByteArray& json) {
    const QJsonObject root = parseObject(json);
    const QJsonObject data = root.value("data").toObject();
    if (!root.value("result").toBool() || !data.value("data").isArray()) {
        return std::nullopt;
    }
    GifPage page;
    for (const QJsonValue& value : data.value("data").toArray()) {
        const QJsonObject item = value.toObject();
        // Advertisements share the list when a key has ads turned on.
        if (item.value("type").toString("gif") != "gif") {
            continue;
        }
        const QJsonObject files = item.value("file").toObject();
        const auto gif = [&](std::initializer_list<const char*> sizes) {
            for (const char* size : sizes) {
                const QJsonObject candidate = files.value(size).toObject().value("gif").toObject();
                if (!candidate.value("url").toString().isEmpty()) {
                    return candidate;
                }
            }
            return QJsonObject();
        };
        const QJsonObject preview = gif({"sm", "xs", "md", "hd"});
        const QJsonObject share = gif({"md", "hd", "sm"});
        addResult(page,
                  GifResult{item.value("title").toString(), QUrl(preview.value("url").toString()),
                            QUrl(share.value("url").toString()), jsonSize(preview)});
    }
    page.hasMore = data.value("has_next").toBool();
    return page;
}

QString gifErrorMessage(const QByteArray& json) {
    const QJsonObject root = parseObject(json);
    // KLIPY: {"result": false, "errors": {"message": ["..."]}}
    const QJsonValue klipy = root.value("errors").toObject().value("message");
    if (klipy.isArray() && !klipy.toArray().isEmpty()) {
        return klipy.toArray().first().toString();
    }
    // GIPHY: {"meta": {"msg": "..."}} or {"message": "..."}
    const QString giphy = root.value("meta").toObject().value("msg").toString();
    return giphy.isEmpty() ? root.value("message").toString() : giphy;
}

GifSearch::GifSearch(QNetworkAccessManager* network, QObject* parent) :
    QObject(parent), network_(network) {
}

GifSearch::~GifSearch() {
    cancel();
}

void GifSearch::setKeys(const QString& giphyKey, const QString& klipyKey,
                        const QString& klipyCustomerId) {
    giphyKey_ = giphyKey.trimmed();
    klipyKey_ = klipyKey.trimmed();
    klipyCustomerId_ = klipyCustomerId;
}

bool GifSearch::hasKey(GifProvider provider) const {
    return !(provider == GifProvider::Giphy ? giphyKey_ : klipyKey_).isEmpty();
}

void GifSearch::cancel() {
    if (reply_) {
        reply_->disconnect(this);
        reply_->abort();
        reply_->deleteLater();
    }
    reply_.clear();
}

void GifSearch::request(GifProvider provider, const QString& query, int page) {
    cancel();
    if (!hasKey(provider)) {
        emit failed(provider, query, page,
                    gifProviderName(provider) + " search is not available in this build.");
        return;
    }
    QNetworkRequest request(provider == GifProvider::Giphy
                                ? giphyRequestUrl(giphyKey_, query, page)
                                : klipyRequestUrl(klipyKey_, klipyCustomerId_, query, page));
    request.setTransferTimeout(kRequestTimeoutMs);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setHeader(QNetworkRequest::UserAgentHeader, "disquisition");
    QNetworkReply* reply = network_->get(request);
    reply_ = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, provider, query, page] {
        reply->deleteLater();
        if (reply_ == reply) {
            reply_.clear();
        }
        const QByteArray body = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const std::optional<GifPage> results = status == 200 ? (provider == GifProvider::Giphy
                                                                    ? parseGiphyResponse(body)
                                                                    : parseKlipyResponse(body))
                                                             : std::nullopt;
        if (results) {
            emit pageReady(provider, query, page, *results);
            return;
        }
        QString message = gifErrorMessage(body);
        if (message.isEmpty()) {
            message = reply->error() != QNetworkReply::NoError ? reply->errorString()
                                                               : "unexpected response";
        }
        emit failed(provider, query, page, gifProviderName(provider) + ": " + message);
    });
}
