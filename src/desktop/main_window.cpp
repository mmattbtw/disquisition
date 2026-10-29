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
#include "desktop/updater.h"

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QString::fromUtf8(disquisition::kAppName));
    resize(1080, 740);
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
    setStyleSheet(R"(
        QWidget { background: #282c34; color: #c5c8c6; font-family: Inter, "Segoe UI", "Helvetica Neue", sans-serif; font-size: 14px; }
        QLabel#title { background: #9dccca; color: #20262c; font-family: "Cal Sans", Inter, sans-serif; font-size: 17px; font-weight: 400; padding: 3px 5px; }
        QLineEdit, QPlainTextEdit, QSpinBox, QComboBox, QTextBrowser, QListWidget, QTreeWidget { background: #282c34; color: #c5c8c6; border: 1px solid #545b66; border-radius: 0; padding: 5px; selection-background-color: #9dccca; selection-color: #20262c; }
        QLineEdit[invalid="true"] { background: #3b2529; color: #ffb3b3; border: 2px solid #e05252; }
        QComboBox QAbstractItemView { background: #282c34; color: #c5c8c6; selection-background-color: #9dccca; selection-color: #20262c; }
        QTextBrowser { border: 0; padding: 0; font-weight: 600; }
        QListWidget { border-left: 1px solid #545b66; border-top: 0; border-right: 0; border-bottom: 0; }
        QPushButton { background: #333842; color: #d5d7d6; border: 1px solid #777f89; border-radius: 0; padding: 6px 12px; font-family: "Cal Sans", Inter, sans-serif; font-size: 15px; font-weight: 400; }
        QPushButton:hover { background: #9dccca; color: #20262c; }
        QPushButton#primary { background: #9dccca; color: #20262c; border-color: #9dccca; }
        QPushButton:checked { background: #e05252; color: #fff3f3; border-color: #ff8a8a; }
        QPushButton:disabled { color: #70757c; background: #2d3139; }
        QTreeWidget { border: 0; background: #242830; padding: 6px; }
        QTreeWidget::item { padding: 6px 2px; }
        QTreeWidget::item:selected { background: #39484b; color: #d5e9e6; }
        QListWidget#communityManageCategories { background: #242830; border: 0; padding: 8px; }
        QListWidget#communityManageCategories::item { padding: 10px 12px; margin-bottom: 3px; }
        QListWidget#communityManageCategories::item:selected { background: #39484b; color: #d5e9e6; }
        QListWidget#userSettingsCategories { background: #242830; border: 0; padding: 16px 8px; }
        QListWidget#userSettingsCategories::item { padding: 12px; margin-bottom: 4px; }
        QListWidget#userSettingsCategories::item:selected { background: #39484b; color: #d5e9e6; }
        QLabel#settingsDescription { color: #a6b5bd; }
        QPushButton#voiceMemberProfile { background: transparent; border: 0; padding: 0; text-align: left; font-family: Inter, "Segoe UI", sans-serif; font-size: 14px; }
        QPushButton#voiceMemberProfile:hover { background: #39484b; color: #e2f1ef; }
        QPushButton#voiceMemberProfile:focus { border: 1px solid #9dccca; }
        QDialog#userProfileCard { background: #242830; border: 1px solid #687c83; }
        QDialog#userProfileCard QLabel, QDialog#userProfileCard QScrollArea, QDialog#userProfileCard QScrollArea QWidget { background: transparent; }
        QLabel#profileDisplayName { color: #e2f1ef; font-size: 21px; font-weight: 700; }
        QLabel#profileBio { color: #c5c8c6; }
        QLabel#settingsHeading { font-size: 19px; font-weight: 700; margin-bottom: 12px; }
        QWidget#communityVoiceControls { background: #242f32; }
        QLabel#communityJoiningLabel { color: #9dccca; }
        QProgressBar#communityJoiningProgress { background: #333842; border: 0; border-radius: 2px; }
        QProgressBar#communityJoiningProgress::chunk { background: #9dccca; border-radius: 2px; }
        QPushButton#communityJoinVoice { color: #9dccca; border-color: #596f70; padding: 4px 8px; }
        QPushButton#update { background: #9dccca; color: #20262c; border-color: #9dccca; padding: 1px 8px; font-size: 13px; }
        QLabel#updateStatus { color: #8a9099; font-size: 13px; }
        QProgressBar#updateProgress { background: #333842; border: 0; }
        QProgressBar#updateProgress::chunk { background: #9dccca; }
        QSplitter::handle { width: 1px; background: #545b66; }
        QSplitter::handle:vertical { height: 1px; }
        QFrame#streamTile { border: 1px solid #545b66; }
        QLabel#streamName { font-family: "Cal Sans", Inter, sans-serif; font-size: 15px; }
        QLabel#live { background: #e05252; color: #fff3f3; font-size: 11px; font-weight: 700; padding: 1px 5px; }
        QLabel#streamStats { color: #8a9099; font-family: "JetBrains Mono", monospace; font-size: 12px; }
        QSlider#streamVolume::groove:horizontal { background: #333842; height: 4px; }
        QSlider#streamVolume::sub-page:horizontal { background: #9dccca; }
        QSlider#streamVolume::handle:horizontal { background: #d5d7d6; width: 10px; margin: -5px 0; }
        QSlider#streamVolume::handle:horizontal:hover { background: #9dccca; }
        QScrollBar:vertical { background: #282c34; width: 10px; }
        QScrollBar::handle:vertical { background: #68717c; min-height: 24px; }
        QFrame#gifPicker { border: 1px solid #777f89; }
        QFrame#gifPicker QListWidget { border: 0; padding: 0; }
        QFrame#gifPicker QListWidget::item:hover, QFrame#gifPicker QListWidget::item:selected { background: #9dccca; }
        QLabel#gifStatus, QLabel#gifAttribution { color: #8a9099; font-size: 12px; }
        QLabel#dropOverlay { background: rgba(32, 38, 44, 225); color: #9dccca; border: 2px dashed #9dccca; font-size: 16px; font-weight: 700; }
    )");
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
