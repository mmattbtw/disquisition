#pragma once

#include <QFile>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <functional>

#include "desktop/update_install.h"
#include "desktop/update_release.h"

class QNetworkAccessManager;
class QNetworkReply;

// Keeps a production build up to date with the latest main release.
//
// A check asks GitHub for the latest release. If it is newer than this
// build, its archive for this platform is downloaded beside the
// installation and extracted.
// startStagedUpdate() installs it the next time the app starts, so an
// update never interrupts a call, or restart() installs it right away.
class Updater final : public QObject {
    Q_OBJECT

public:
    enum class State {
        Idle,
        Checking,
        Downloading,
        Extracting,
        UpToDate,
        Ready,
        Installed,
        ManualInstall,
        Failed,
    };

    // `currentBuild` is 0 for builds that do not update themselves.
    Updater(QNetworkAccessManager* network, QString repository, int currentBuild,
            QObject* parent = nullptr);
    ~Updater() override;

    bool enabled() const;
    State state() const { return state_; }
    int currentBuild() const { return currentBuild_; }
    int availableBuild() const { return release_.buildNumber; }
    QUrl releasePage() const { return release_.page; }
    QString error() const { return error_; }

    void setAutomatic(bool automatic);
    void check();
    bool restart();

signals:
    void stateChanged(Updater::State state);
    void downloadProgress(int percent);

private:
    bool busy() const;
    void setState(State state);
    void fail(const QString& error);
    void cancelReply();
    void receiveRelease(QNetworkReply* reply);
    void download();
    void receiveChunk(QNetworkReply* reply);
    void finishDownload(QNetworkReply* reply);
    void extract();
    void finishExtracting(bool extracted);
    void runTool(const QString& program, const QStringList& arguments,
                 std::function<void(bool)> done);
    QString archivePath() const;

    QNetworkAccessManager* network_;
    QString repository_;
    int currentBuild_;
    Installation installation_;
    State state_ = State::Idle;
    QString error_;
    Release release_;
    QPointer<QNetworkReply> reply_;
    QFile archive_;
    int downloadPercent_ = 0;
    QTimer timer_;
};
