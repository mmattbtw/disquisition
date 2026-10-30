#include "desktop/main_window.h"

#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>

#include "desktop/app_identity.h"
#include "desktop/community_panel.h"
#include "desktop/notification_center.h"
#include "desktop/theme.h"
#include "desktop/updater.h"

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    theme::loadFonts();
    QApplication::setPalette(theme::palette());
    setWindowTitle(QString::fromUtf8(disquisition::kAppName));
    resize(1240, 800);
    auto* communities = new CommunityPanel(this);
    setCentralWidget(communities);
    auto* appMenu = menuBar()->addMenu("disquisition");
    auto* preferences = appMenu->addAction("User Settings…");
    preferences->setMenuRole(QAction::PreferencesRole);
    preferences->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));
    connect(preferences, &QAction::triggered, communities, &CommunityPanel::showSettings);
    auto* notifications = findChild<NotificationCenter*>();
    appMenu->addAction(notifications->muteAction());
    appMenu->addAction(communities->penguinsAction());
    connect(notifications, &NotificationCenter::badgeChanged, this, [this](const UnreadBadge& badge) {
        setWindowTitle(badgedTitle(QString::fromUtf8(disquisition::kAppName), badge));
    });
    auto* network = new QNetworkAccessManager(this);
    updater_ = new Updater(network, QString::fromUtf8(disquisition::kUpdateRepository),
                           disquisition::kBuildNumber, this);
    updateLabel_ = new QLabel(this);
    updateLabel_->setObjectName("updateStatus");
    updateLabel_->hide();
    updateProgress_ = new QProgressBar(this);
    updateProgress_->setObjectName("updateProgress");
    updateProgress_->setTextVisible(false);
    updateProgress_->setFixedSize(120, 6);
    updateProgress_->hide();
    updateButton_ = new QPushButton(this);
    updateButton_->setObjectName("update");
    updateButton_->hide();
    if (updater_->enabled()) {
        auto* checkForUpdatesAction = appMenu->addAction("Check for Updates…");
        checkForUpdatesAction->setMenuRole(QAction::ApplicationSpecificRole);
        connect(checkForUpdatesAction, &QAction::triggered, this, &MainWindow::checkForUpdates);
        auto* automaticUpdates = appMenu->addAction("Update automatically");
        automaticUpdates->setCheckable(true);
        automaticUpdates->setChecked(QSettings().value("updates/automatic", true).toBool());
        connect(automaticUpdates, &QAction::toggled, this, [this](bool automatic) {
            QSettings().setValue("updates/automatic", automatic);
            updater_->setAutomatic(automatic);
        });
        statusBar()->addPermanentWidget(updateLabel_);
        statusBar()->addPermanentWidget(updateProgress_);
        statusBar()->addPermanentWidget(updateButton_);
    }
    connect(updater_, &Updater::stateChanged, this, &MainWindow::showUpdateState);
    connect(updater_, &Updater::downloadProgress, updateProgress_, &QProgressBar::setValue);
    connect(updater_, &Updater::aboutToRelaunch, communities, &CommunityPanel::releaseStore);
    connect(updateButton_, &QPushButton::clicked, this, [this] {
        if (updater_->state() == Updater::State::ManualInstall) {
            QDesktopServices::openUrl(updater_->releasePage());
        } else {
            offerUpdateRestart();
        }
    });
    updater_->setAutomatic(QSettings().value("updates/automatic", true).toBool());
    setStyleSheet(theme::styleSheet());
}

void MainWindow::checkForUpdates() {
    updateCheckRequested_ = true;
    updater_->check();
    showUpdateState();
}

void MainWindow::showUpdateState() {
    using State = Updater::State;
    const State state = updater_->state();
    const QString build = "Build " + QString::number(updater_->availableBuild());
    const QString nextStart =
        " installs the next time you open " + QApplication::applicationDisplayName() + ".";
    const bool working = state == State::Downloading || state == State::Extracting;
    updateLabel_->setVisible(working);
    updateProgress_->setVisible(working);
    if (working) {
        updateLabel_->setText(state == State::Downloading ? "downloading update"
                                                          : "preparing update");
        updateLabel_->setToolTip(build + nextStart);
        updateProgress_->setValue(state == State::Downloading ? 0 : 100);
    }
    updateButton_->setVisible(state == State::Ready || state == State::ManualInstall);
    if (state == State::Ready) {
        updateButton_->setText("Update Ready");
        updateButton_->setToolTip(build + nextStart + " Click to restart now.");
    } else if (state == State::ManualInstall) {
        updateButton_->setText("Download Update");
        updateButton_->setToolTip(build + " cannot be installed in this folder.");
    }

    if (!updateCheckRequested_ || state == State::Idle || state == State::Checking) {
        return;
    }
    updateCheckRequested_ = false;
    const QString title = "Check for Updates";
    switch (state) {
    case State::UpToDate:
        QMessageBox::information(this, title,
                                 "You have the latest build (" +
                                     QString::number(updater_->currentBuild()) + ").");
        break;
    case State::Downloading:
        QMessageBox::information(this, title, build + " is downloading. It" + nextStart);
        break;
    case State::Ready:
        offerUpdateRestart();
        break;
    case State::ManualInstall:
        if (QMessageBox::question(this, title,
                                  build + " is available, but the app cannot update itself in "
                                          "the folder it is in. Download it from GitHub?") ==
            QMessageBox::Yes) {
            QDesktopServices::openUrl(updater_->releasePage());
        }
        break;
    case State::Installed:
        QMessageBox::information(this, title, build + " is installed and starts next time.");
        break;
    case State::Failed:
        QMessageBox::warning(this, title, updater_->error());
        break;
    default:
        break;
    }
}

void MainWindow::offerUpdateRestart() {
    const QString question = "Build " + QString::number(updater_->availableBuild()) +
                             " installs the next time you open " +
                             QApplication::applicationDisplayName() + ". Restart now?";
    if (QMessageBox::question(this, "Update Ready", question) == QMessageBox::Yes &&
        !updater_->restart()) {
        QMessageBox::warning(this, "Update", updater_->error());
    }
}
