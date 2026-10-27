#include "desktop/updater.h"

#include <QCoreApplication>
#include <QDir>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QtDebug>
#include <utility>

namespace {

// Late enough that the build replaced at startup has quit and unlocked its files.
constexpr int kFirstCheckDelayMs = 15 * 1000;
constexpr int kCheckIntervalMs = 6 * 60 * 60 * 1000;
constexpr int kTransferTimeoutMs = 30 * 1000;

QNetworkRequest gitHubRequest(const QUrl& url) {
    QNetworkRequest request(url);
    request.setTransferTimeout(kTransferTimeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setHeader(QNetworkRequest::UserAgentHeader, "disquisition");
    return request;
}

QString replyError(QNetworkReply* reply) {
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() == QNetworkReply::NoError && status != 200) {
        return "GitHub answered with HTTP " + QString::number(status);
    }
    return reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
}

} // namespace

Updater::Updater(QNetworkAccessManager* network, QString repository, int currentBuild,
                 QObject* parent) :
    QObject(parent), network_(network), repository_(std::move(repository)),
    currentBuild_(currentBuild), installation_(currentInstallation()) {
    connect(&timer_, &QTimer::timeout, this, [this] {
        timer_.setInterval(kCheckIntervalMs);
        check();
    });
}

Updater::~Updater() {
    cancelReply();
}

bool Updater::enabled() const {
    return currentBuild_ > 0 && !releaseAssetName().isEmpty();
}

bool Updater::busy() const {
    return state_ == State::Checking || state_ == State::Downloading || state_ == State::Extracting;
}

void Updater::setAutomatic(bool automatic) {
    if (!automatic) {
        timer_.stop();
        return;
    }
    if (enabled() && !timer_.isActive()) {
        timer_.start(kFirstCheckDelayMs);
    }
}

void Updater::check() {
    if (!enabled() || busy() || state_ == State::Ready || state_ == State::Installed) {
        return;
    }
    if (!QDir(installation_.workDir).removeRecursively()) {
        fail("Files from the last update in " + QDir::toNativeSeparators(installation_.workDir) +
             " could not be removed.");
        return;
    }
    error_.clear();
    setState(State::Checking);
    QNetworkRequest request = gitHubRequest(latestReleaseUrl(repository_));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    QNetworkReply* reply = network_->get(request);
    reply_ = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] { receiveRelease(reply); });
}

bool Updater::restart() {
    if (state_ != State::Ready) {
        return false;
    }
    QString error;
    if (!installStaged(installation_, &error)) {
        fail("Could not install build " + QString::number(availableBuild()) + ": " + error + ".");
        return false;
    }
    setState(State::Installed);
    if (!launchInstallation(installation_)) {
        error_ = "Build " + QString::number(availableBuild()) +
                 " is installed, but could not be started. Open the app again to use it.";
        return false;
    }
    QCoreApplication::quit();
    return true;
}

void Updater::setState(State state) {
    if (state_ != state) {
        state_ = state;
        emit stateChanged(state);
    }
}

void Updater::fail(const QString& error) {
    cancelReply();
    archive_.close();
    error_ = error;
    qWarning().noquote() << "Update failed:" << error;
    setState(State::Failed);
}

void Updater::cancelReply() {
    if (reply_) {
        reply_->disconnect(this);
        reply_->abort();
        reply_->deleteLater();
    }
    reply_.clear();
}

void Updater::receiveRelease(QNetworkReply* reply) {
    reply->deleteLater();
    reply_.clear();
    if (const QString error = replyError(reply); !error.isEmpty()) {
        fail("Could not check for updates: " + error);
        return;
    }
    const std::optional<Release> release = parseLatestRelease(reply->readAll(), repository_,
                                                              releaseAssetName());
    if (!release) {
        fail("The latest release has no verifiable " + releaseAssetName() + ".");
        return;
    }
    release_ = *release;
    if (release_.buildNumber <= currentBuild_) {
        setState(State::UpToDate);
        return;
    }
    download();
}

void Updater::download() {
    if (!QDir().mkpath(installation_.payloadDir())) {
        setState(State::ManualInstall);
        return;
    }
    archive_.setFileName(archivePath());
    if (!archive_.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail("Could not save the update: " + archive_.errorString());
        return;
    }
    downloadPercent_ = 0;
    QNetworkRequest request = gitHubRequest(release_.assetUrl);
    request.setRawHeader("Accept", "application/octet-stream");
    QNetworkReply* reply = network_->get(request);
    reply_ = reply;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] { receiveChunk(reply); });
    connect(reply, &QNetworkReply::finished, this, [this, reply] { finishDownload(reply); });
    setState(State::Downloading);
}

void Updater::receiveChunk(QNetworkReply* reply) {
    const QByteArray data = reply->readAll();
    if (archive_.pos() + data.size() > release_.assetSize) {
        fail("The download is larger than GitHub reported.");
        return;
    }
    if (archive_.write(data) != data.size()) {
        fail("Could not save the update: " + archive_.errorString());
        return;
    }
    const int percent = static_cast<int>(archive_.pos() * 100 / release_.assetSize);
    if (percent != downloadPercent_) {
        downloadPercent_ = percent;
        emit downloadProgress(percent);
    }
}

void Updater::finishDownload(QNetworkReply* reply) {
    reply->deleteLater();
    reply_.clear();
    if (const QString error = replyError(reply); !error.isEmpty()) {
        fail("Could not download the update: " + error);
        return;
    }
    receiveChunk(reply);
    if (state_ != State::Downloading) {
        return;
    }
    archive_.close();
    if (archive_.size() != release_.assetSize) {
        fail("The download was cut off before it finished.");
        return;
    }
    extract();
}

void Updater::extract() {
    setState(State::Extracting);
    const QString archive = archivePath();
    const QString payload = installation_.payloadDir();
#if defined(Q_OS_MACOS)
    const QString mount = installation_.workDir + "/mount";
    QDir().mkpath(mount);
    runTool("/usr/bin/hdiutil",
            {"attach", "-nobrowse", "-readonly", "-noautoopen", "-mountpoint", mount, archive},
            [this, mount, payload](bool attached) {
                if (!attached) {
                    finishExtracting(false);
                    return;
                }
                const auto detach = [this, mount](bool copied) {
                    runTool("/usr/bin/hdiutil", {"detach", mount, "-force"},
                            [this, copied](bool) { finishExtracting(copied); });
                };
                const QStringList apps = QDir(mount).entryList({"*.app"},
                                                               QDir::Dirs | QDir::NoDotAndDotDot);
                if (apps.size() != 1) {
                    detach(false);
                    return;
                }
                runTool("/usr/bin/ditto",
                        {mount + "/" + apps.first(), payload + "/" + apps.first()}, detach);
            });
#elif defined(Q_OS_WIN)
    // Git's tar, often first on PATH, can't read zip archives.
    runTool(QDir(qEnvironmentVariable("SystemRoot")).filePath("System32/tar.exe"),
            {"-xf", QDir::toNativeSeparators(archive), "-C", QDir::toNativeSeparators(payload)},
            [this](bool extracted) { finishExtracting(extracted); });
#else
    runTool("tar", {"-xzf", archive, "-C", payload},
            [this](bool extracted) { finishExtracting(extracted); });
#endif
}

void Updater::finishExtracting(bool extracted) {
    QFile::remove(archivePath());
    if (!extracted) {
        fail("Could not extract " + releaseAssetName() + ".");
        return;
    }
    QString error;
    if (planReplacements(installation_, &error).isEmpty()) {
        fail("The update cannot be installed: " + error + ".");
        return;
    }
    if (!markStaged(installation_, availableBuild())) {
        fail("Could not save the update in " + QDir::toNativeSeparators(installation_.workDir) +
             ".");
        return;
    }
    setState(State::Ready);
}

void Updater::runTool(const QString& program, const QStringList& arguments,
                      std::function<void(bool)> done) {
    auto* process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::errorOccurred, this,
            [process, program, done](QProcess::ProcessError error) {
                // Only a program that never started skips finished().
                if (error == QProcess::FailedToStart) {
                    qWarning().noquote() << "Could not start" << program;
                    process->deleteLater();
                    done(false);
                }
            });
    connect(process, &QProcess::finished, this,
            [process, program, done](int exitCode, QProcess::ExitStatus status) {
                process->deleteLater();
                const bool succeeded = status == QProcess::NormalExit && exitCode == 0;
                if (!succeeded) {
                    qWarning().noquote() << program << "failed:" << process->readAll().trimmed();
                }
                done(succeeded);
            });
    process->start(program, arguments);
}

QString Updater::archivePath() const {
    return installation_.workDir + "/" + releaseAssetName();
}
