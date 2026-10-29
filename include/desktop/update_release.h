#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <optional>

// Empty on platforms without a release archive.
QString releaseAssetName();

std::optional<int> releaseBuildNumber(const QString& tag);

QUrl latestReleaseUrl(const QString& repository);

struct Release {
    int buildNumber = 0;
    QUrl page;
    QUrl assetUrl;
    qint64 assetSize = 0;
};

std::optional<Release> parseLatestRelease(const QByteArray& json, const QString& repository,
                                          const QString& assetName);
