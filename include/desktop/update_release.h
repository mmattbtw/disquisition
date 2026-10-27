#pragma once

#include <QByteArray>
#include <QString>
#include <QUrl>
#include <optional>

// Every push to main publishes a GitHub release tagged main-<run number>,
// holding one archive per platform, and marks the newest one latest. See
// .github/scripts/publish-desktop-release.sh.

// The release archive for this platform, or an empty string where the
// desktop workflow publishes none.
QString releaseAssetName();

std::optional<int> releaseBuildNumber(const QString& tag);

QUrl latestReleaseUrl(const QString& repository);

struct Release {
    int buildNumber = 0;
    QUrl page; // on github.com, for downloading by hand
    QUrl assetUrl;
    qint64 assetSize = 0;
};

// Reads GitHub's latest-release response. Returns nothing unless it is a
// main-<number> release whose `assetName` downloads from `repository`.
std::optional<Release> parseLatestRelease(const QByteArray& json, const QString& repository,
                                          const QString& assetName);
