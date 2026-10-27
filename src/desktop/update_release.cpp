#include "desktop/update_release.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

QString releaseAssetName() {
#if defined(Q_OS_MACOS) && defined(Q_PROCESSOR_ARM_64)
    return "disquisition-desktop-macos-arm64.dmg";
#elif defined(Q_OS_WIN) && defined(Q_PROCESSOR_X86_64)
    return "disquisition-desktop-windows-x64.zip";
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_X86_64)
    return "disquisition-desktop-linux-x64.tar.gz";
#else
    return {};
#endif
}

std::optional<int> releaseBuildNumber(const QString& tag) {
    static const QRegularExpression pattern("^main-([1-9][0-9]{0,8})$");
    const QRegularExpressionMatch match = pattern.match(tag);
    if (!match.hasMatch()) {
        return std::nullopt;
    }
    return match.captured(1).toInt();
}

QUrl latestReleaseUrl(const QString& repository) {
    return QUrl("https://api.github.com/repos/" + repository + "/releases/latest");
}

std::optional<Release> parseLatestRelease(const QByteArray& json, const QString& repository,
                                          const QString& assetName) {
    QJsonParseError error{};
    const QJsonObject root = QJsonDocument::fromJson(json, &error).object();
    if (error.error != QJsonParseError::NoError || root.value("draft").toBool() ||
        root.value("prerelease").toBool()) {
        return std::nullopt;
    }
    const QString tag = root.value("tag_name").toString();
    const std::optional<int> build = releaseBuildNumber(tag);
    if (!build) {
        return std::nullopt;
    }
    const QString base = "https://github.com/" + repository + "/releases/";
    for (const QJsonValue& value : root.value("assets").toArray()) {
        const QJsonObject asset = value.toObject();
        if (asset.value("name").toString() != assetName ||
            asset.value("state").toString() != "uploaded") {
            continue;
        }
        // Only download from this repository's own release, wherever GitHub
        // then redirects to.
        const QUrl url(asset.value("browser_download_url").toString(), QUrl::StrictMode);
        if (url != QUrl(base + "download/" + tag + "/" + assetName)) {
            return std::nullopt;
        }
        const qint64 size = asset.value("size").toInteger();
        if (size <= 0) {
            return std::nullopt;
        }
        return Release{*build, QUrl(base + "tag/" + tag), url, size};
    }
    return std::nullopt;
}
