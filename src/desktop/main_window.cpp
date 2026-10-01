#include "desktop/main_window.h"

#include <QDateTime>
#include <QColor>
#include <QComboBox>
#include <QCheckBox>
#include <QFormLayout>
#include <QFrame>
#include <QApplication>
#include <QAction>
#include <QAudioDevice>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QMediaDevices>
#include <QMenuBar>
#include <QNetworkInterface>
#include <QDir>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStyle>
#include <QTextBrowser>
#include <QTimer>
#include <QUdpSocket>
#include <QVBoxLayout>

#include <exception>
#include <string>
#include <utility>

#include "desktop/audio_permission.h"

namespace {

QString q(const std::string& value) {
    return QString::fromUtf8(value.data(), static_cast<int>(value.size()));
}

std::string s(const QString& value) {
    const QByteArray bytes = value.toUtf8();
    return std::string(bytes.constData(), static_cast<std::size_t>(bytes.size()));
}

QString safeHtml(const QString& value) {
    return value.toHtmlEscaped().replace("\n", "<br>");
}

QColor xtermColor(int index) {
    static const int basic[][3] = {
        {0, 0, 0},       {205, 49, 49},   {13, 188, 121}, {229, 229, 16},
        {36, 114, 200},  {188, 63, 188},  {17, 168, 205}, {229, 229, 229},
        {102, 102, 102}, {241, 76, 76},   {35, 209, 139}, {245, 245, 67},
        {59, 142, 234},  {214, 112, 214}, {41, 184, 219}, {255, 255, 255}};
    if (index < 16) {
        return QColor(basic[index][0], basic[index][1], basic[index][2]);
    }
    if (index < 232) {
        const int value = index - 16;
        const int levels[] = {0, 95, 135, 175, 215, 255};
        return QColor(levels[value / 36], levels[(value / 6) % 6], levels[value % 6]);
    }
    const int gray = 8 + (index - 232) * 10;
    return QColor(gray, gray, gray);
}

QColor chatColor(const QString& value) {
    static const QHash<QString, int> named {
        {"pink", 211}, {"mint", 121}, {"butter", 229}, {"periwinkle", 111},
        {"lilac", 183}, {"aqua", 159}, {"peach", 216}};
    if (named.contains(value)) {
        return xtermColor(named.value(value));
    }
    bool valid = false;
    const int index = value.toInt(&valid);
    return valid && index >= 0 && index <= 255 ? xtermColor(index) : xtermColor(121);
}

QString voiceReachableHost(QString host) {
    const QHostAddress address(host);
    if (!address.isLoopback()) {
        return host;
    }
    for (const QHostAddress& candidate : QNetworkInterface::allAddresses()) {
        if (candidate.protocol() == QAbstractSocket::IPv4Protocol &&
            !candidate.isLoopback() && !candidate.isNull()) {
            return candidate.toString();
        }
    }
    return host;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent), voice_(this) {
    QSettings settings;
    serverHost_ = settings.value("server/host", serverHost_).toString();
    serverPort_ = static_cast<quint16>(qBound(1, settings.value("server/port", 9000).toInt(), 65535));
    advertiseHost_ = settings.value("server/advertisedHost").toString();
    preferredVoicePort_ = static_cast<quint16>(
        qBound(1, settings.value("voice/sipPort", 5060).toInt(), 65534));
    relayHost_ = settings.value("relay/host").toString();
    relayPort_ = static_cast<quint16>(qBound(1, settings.value("relay/port", 3333).toInt(), 65535));
    leakMyIp_ = settings.value("relay/leakMyIp", false).toBool();
    saveMessages_ = settings.value("messages/enabled", false).toBool();
    messageFile_ = settings.value("messages/file",
                                  QDir::homePath() + "/disquisition-messages.db").toString();
    const QString maximum = settings.value("messages/maximum").toString();
    bool validMaximum = false;
    const qlonglong parsedMaximum = maximum.toLongLong(&validMaximum);
    if (validMaximum && parsedMaximum > 0) maxSavedMessages_ = parsedMaximum;
    buildUi();
    setWindowTitle("Disquisition");
    resize(980, 680);

    reconnectTimer_.setSingleShot(true);
    reconnectTimer_.setInterval(3000);
    connect(&reconnectTimer_, &QTimer::timeout, this, &MainWindow::retryConnection);
    relayProbeTimer_.setInterval(3000);
    connect(&relayProbeTimer_, &QTimer::timeout, this, &MainWindow::probeRelay);
    loginTimer_.setSingleShot(true);
    loginTimer_.setInterval(8000);
    connect(&loginTimer_, &QTimer::timeout, this, [this] {
        if (desiredConnected_ && awaitingLogin_) server_.abort();
    });
    connect(&relayProbe_, &QTcpSocket::connected, this, [this] {
        sendFrame(&relayProbe_, chat::Message {chat::MsgType::RelayProbe, {}});
    });
    connect(&relayProbe_, &QTcpSocket::readyRead, this, [this] {
        relayProbeBuffer_ += relayProbe_.readAll();
        std::string buffer(relayProbeBuffer_.constData(),
                           static_cast<std::size_t>(relayProbeBuffer_.size()));
        chat::Message response;
        const auto status = chat::decode(buffer, response);
        relayProbeBuffer_ = QByteArray(buffer.data(), static_cast<qsizetype>(buffer.size()));
        if (status == chat::DecodeStatus::Incomplete) return;
        relayProbe_.abort();
        relayProbeBuffer_.clear();
        if (status != chat::DecodeStatus::Ok || response.type != chat::MsgType::RelayReady ||
            !response.fields.empty() || !desiredConnected_ || usingRelay_ || !leakMyIp_) return;
        relayProbeTimer_.stop();
        reconnectTimer_.stop();
        connectionLabel_->setText("Relay ready; switching privately");
        if (server_.state() == QAbstractSocket::UnconnectedState) {
            startTransport(true);
        } else {
            switchingToRelay_ = true;
            server_.abort();
        }
    });
    connect(&relayProbe_, &QTcpSocket::errorOccurred, this,
            [this](QAbstractSocket::SocketError) {
                relayProbe_.abort();
                relayProbeBuffer_.clear();
            });

    connect(&server_, &QTcpSocket::connected, this, [this] {
        if (!usingRelay_ && !peerServer_.listen(QHostAddress::Any, 0)) {
            QMessageBox::critical(this, "Cannot join", peerServer_.errorString());
            server_.disconnectFromHost();
            return;
        }
        connectionLabel_->setText("Signing in");
        awaitingLogin_ = true;
        loginTimer_.start();
        sendFrame(&server_, chat::Message {
                                chat::MsgType::Login,
                                {s(name_->text()), std::to_string(usingRelay_ ? 0 : peerServer_.serverPort()),
                                 s(advertiseHost_)}});
    });
    connect(&server_, &QTcpSocket::readyRead, this, &MainWindow::readServer);
    connect(&server_, &QTcpSocket::disconnected, this, &MainWindow::handleTransportClosed);
    connect(&server_, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        connectionLabel_->setText(server_.errorString());
        QTimer::singleShot(0, this, [this] {
            if (server_.state() == QAbstractSocket::UnconnectedState) handleTransportClosed();
        });
    });
    connect(&peerServer_, &QTcpServer::newConnection, this, &MainWindow::acceptPeer);
    connect(connectButton_, &QPushButton::clicked, this, [this] {
        if (desiredConnected_) {
            disconnectAll();
        } else {
            startJoining();
        }
    });
    connect(voiceButton_, &QPushButton::clicked, this, [this] {
        if (voiceWanted_) {
            leaveVoice();
        } else if (resumeVoice_) {
            resumeVoice_ = false;
            voiceButton_->setText("join voice");
            voiceLabel_->setText("voice: off");
        } else {
            joinVoice();
        }
    });
    connect(settingsButton_, &QPushButton::clicked, this, &MainWindow::showPreferences);
    connect(sendButton_, &QPushButton::clicked, this, &MainWindow::sendMessage);
    connect(composer_, &QLineEdit::returnPressed, this, &MainWindow::sendMessage);
    connect(muteButton_, &QPushButton::clicked, this, [this] {
        muted_ = !muted_;
        voice_.setMuted(muted_);
        relayAudio_.setMuted(muted_);
        muteButton_->setChecked(muted_);
        muteButton_->setText(muted_ ? "unmute" : "mute");
        sendVoiceState();
    });
    connect(deafenButton_, &QPushButton::clicked, this, [this] {
        deafened_ = !deafened_;
        if (deafened_) {
            mutedBeforeDeafen_ = muted_;
            muted_ = true;
        } else {
            muted_ = mutedBeforeDeafen_;
        }
        voice_.setDeafened(deafened_);
        relayAudio_.setDeafened(deafened_);
        relayAudio_.setMuted(muted_);
        deafenButton_->setChecked(deafened_);
        deafenButton_->setText(deafened_ ? "undeafen" : "deafen");
        if (deafened_) {
            muteButton_->setChecked(true);
            muteButton_->setText("unmute");
            muteButton_->setEnabled(false);
        } else {
            muteButton_->setChecked(muted_);
            muteButton_->setText(muted_ ? "unmute" : "mute");
            muteButton_->setEnabled(true);
        }
        sendVoiceState();
    });
    connect(name_, &QLineEdit::textChanged, this, [this](const QString& value) {
        if (!value.trimmed().isEmpty() && name_->property("invalid").toBool()) {
            name_->setProperty("invalid", false);
            name_->style()->unpolish(name_);
            name_->style()->polish(name_);
        }
    });
    connect(&voice_, &VoiceEngine::statusChanged, voiceLabel_, &QLabel::setText);
    connect(&relayAudio_, &RelayAudio::frameReady, this, [this](const QByteArray& pcm) {
        if (!voiceWanted_ || server_.state() != QAbstractSocket::ConnectedState) return;
        if (!usingRelay_) {
            bool hasRelayVoicePeer = false;
            for (const Peer& peer : std::as_const(peers_)) {
                hasRelayVoicePeer |= peer.voicePort == 65535;
            }
            if (!hasRelayVoicePeer) return;
        }
        sendFrame(&server_, chat::Message {chat::MsgType::VoiceAudio,
                   {std::string(pcm.constData(), static_cast<std::size_t>(pcm.size()))}});
    });
    connect(&relayAudio_, &RelayAudio::speakingChanged, this, [this](bool speaking) {
        if (usingRelay_ && localSpeaking_ != speaking) {
            localSpeaking_ = speaking;
            refreshMembers();
        }
    });
    speakingExpiry_.setInterval(200);
    connect(&speakingExpiry_, &QTimer::timeout, this, [this] {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        bool changed = false;
        for (Peer& peer : peers_) {
            if (peer.lastAudioMs && peer.speaking && now - peer.lastAudioMs > 250) {
                peer.speaking = false;
                changed = true;
            }
        }
        if (changed) refreshMembers();
    });
    speakingExpiry_.start();
    connect(&voice_, &VoiceEngine::ready, this, [this] {
        if (!voiceWanted_ || myName_.isEmpty() || server_.state() != QAbstractSocket::ConnectedState) {
            return;
        }
        sendFrame(&server_, chat::Message {chat::MsgType::VoicePort,
                                           {std::to_string(activeVoicePort_)}});
        for (const Peer& peer : std::as_const(peers_)) {
            if (myName_ < peer.name && peer.voicePort != 65535) {
                voice_.callPeer(peer.name, peer.host, peer.voicePort);
            }
        }
        refreshMembers();
    });
    connect(&voice_, &VoiceEngine::stopped, this, [this] {
        if (voiceWanted_) {
            leaveVoice();
        }
    });
    connect(&voice_, &VoiceEngine::localSpeakingChanged, this, [this](bool speaking) {
        if (usingRelay_) return;
        if (localSpeaking_ != speaking) {
            localSpeaking_ = speaking;
            refreshMembers();
        }
    });
    connect(&voice_, &VoiceEngine::peerVoiceChanged, this,
            [this](const QString& name, bool connected, bool speaking) {
        if (!peers_.contains(name)) {
            return;
        }
        Peer& peer = peers_[name];
        if (peer.voiceConnected != connected || peer.speaking != speaking) {
            peer.voiceConnected = connected;
            peer.speaking = speaking;
            refreshMembers();
        }
            });
    mediaDevices_ = new QMediaDevices(this);
    connect(mediaDevices_, &QMediaDevices::audioInputsChanged, this,
            &MainWindow::refreshAudioDevices);
    connect(mediaDevices_, &QMediaDevices::audioOutputsChanged, this,
            &MainWindow::refreshAudioDevices);
    refreshAudioDevices();
}

MainWindow::~MainWindow() {
    disconnectAll();
}

void MainWindow::buildUi() {
    auto* root = new QWidget(this);
    auto* page = new QVBoxLayout(root);
    page->setContentsMargins(10, 10, 10, 10);
    page->setSpacing(6);

    auto* title = new QLabel(" disquisition", root);
    title->setObjectName("title");
    page->addWidget(title);

    auto* setup = new QHBoxLayout;
    name_ = new QLineEdit(root);
    name_->setPlaceholderText("name");
    connectButton_ = new QPushButton("join", root);
    connectButton_->setObjectName("primary");
    voiceButton_ = new QPushButton("join voice", root);
    voiceButton_->setEnabled(false);
    settingsButton_ = new QPushButton("settings", root);
    setup->addWidget(name_, 1);
    setup->addWidget(connectButton_);
    setup->addWidget(voiceButton_);
    setup->addWidget(settingsButton_);
    page->addLayout(setup);

    auto* appMenu = menuBar()->addMenu("Disquisition");
    auto* preferencesAction = appMenu->addAction("Preferences…");
    preferencesAction->setMenuRole(QAction::PreferencesRole);
    preferencesAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));
    connect(preferencesAction, &QAction::triggered, this, &MainWindow::showPreferences);
    auto* saveAction = appMenu->addAction("Message saving…");
    connect(saveAction, &QAction::triggered, this, &MainWindow::showPreferences);

    auto* devices = new QHBoxLayout;
    devices->setSpacing(6);
    auto* micLabel = new QLabel("mic", root);
    inputDevice_ = new QComboBox(root);
    inputDevice_->setEditable(false);
    inputDevice_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* outputLabel = new QLabel("out", root);
    outputDevice_ = new QComboBox(root);
    outputDevice_->setEditable(false);
    outputDevice_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    muteButton_ = new QPushButton("mute", root);
    muteButton_->setCheckable(true);
    muteButton_->setEnabled(false);
    deafenButton_ = new QPushButton("deafen", root);
    deafenButton_->setCheckable(true);
    deafenButton_->setEnabled(false);
    devices->addWidget(micLabel);
    devices->addWidget(inputDevice_, 1);
    devices->addWidget(outputLabel);
    devices->addWidget(outputDevice_, 1);
    devices->addWidget(muteButton_);
    devices->addWidget(deafenButton_);
    page->addLayout(devices);

    auto* state = new QHBoxLayout;
    connectionLabel_ = new QLabel("* disconnected", root);
    voiceLabel_ = new QLabel("voice: off", root);
    state->addWidget(connectionLabel_);
    state->addStretch();
    state->addWidget(voiceLabel_);
    page->addLayout(state);

    auto* split = new QSplitter(root);
    transcript_ = new QTextBrowser(split);
    transcript_->setOpenExternalLinks(false);
    transcript_->setFrameShape(QFrame::NoFrame);
    members_ = new QListWidget(split);
    members_->setMinimumWidth(190);
    split->addWidget(transcript_);
    split->addWidget(members_);
    split->setStretchFactor(0, 1);
    page->addWidget(split, 1);

    auto* compose = new QHBoxLayout;
    composer_ = new QLineEdit(root);
    composer_->setPlaceholderText("> type a message");
    composer_->setEnabled(false);
    sendButton_ = new QPushButton("send", root);
    sendButton_->setEnabled(false);
    compose->addWidget(composer_, 1);
    compose->addWidget(sendButton_);
    page->addLayout(compose);

    setCentralWidget(root);
    setStyleSheet(R"(
        QWidget { background: #282c34; color: #c5c8c6; font-family: Menlo, Monaco, "Courier New"; font-size: 14px; }
        QLabel#title { background: #9dccca; color: #20262c; font-size: 15px; font-weight: 700; padding: 3px 5px; }
        QLineEdit, QSpinBox, QComboBox, QTextBrowser, QListWidget { background: #282c34; color: #c5c8c6; border: 1px solid #545b66; border-radius: 0; padding: 5px; selection-background-color: #9dccca; selection-color: #20262c; }
        QLineEdit[invalid="true"] { background: #3b2529; color: #ffb3b3; border: 2px solid #e05252; }
        QComboBox QAbstractItemView { background: #282c34; color: #c5c8c6; selection-background-color: #9dccca; selection-color: #20262c; }
        QTextBrowser { border: 0; padding: 0; }
        QListWidget { border-left: 1px solid #545b66; border-top: 0; border-right: 0; border-bottom: 0; }
        QPushButton { background: #333842; color: #d5d7d6; border: 1px solid #777f89; border-radius: 0; padding: 6px 12px; }
        QPushButton:hover { background: #9dccca; color: #20262c; }
        QPushButton#primary { background: #9dccca; color: #20262c; border-color: #9dccca; }
        QPushButton:checked { background: #e05252; color: #fff3f3; border-color: #ff8a8a; }
        QPushButton:disabled { color: #70757c; background: #2d3139; }
        QSplitter::handle { width: 1px; background: #545b66; }
        QScrollBar:vertical { background: #282c34; width: 10px; }
        QScrollBar::handle:vertical { background: #68717c; min-height: 24px; }
    )");
}

void MainWindow::startJoining() {
    if (name_->text().trimmed().isEmpty()) {
        name_->setProperty("invalid", true);
        name_->style()->unpolish(name_);
        name_->style()->polish(name_);
        name_->setFocus();
        return;
    }
    connectToServer();
}

void MainWindow::joinVoice() {
    if (myName_.isEmpty() || server_.state() != QAbstractSocket::ConnectedState) {
        return;
    }
    voiceButton_->setEnabled(false);
    requestMicrophoneAccess(this, [this](bool granted) {
        if (myName_.isEmpty() || server_.state() != QAbstractSocket::ConnectedState) {
            return;
        }
        voiceButton_->setEnabled(true);
        if (!granted) {
            voiceLabel_->setText("voice: microphone access denied");
            return;
        }
        refreshAudioDevices();
        auto selectedDevice = [](QComboBox* combo) {
            return combo->currentData().isValid() ? combo->currentData().toString()
                                                  : combo->currentText();
        };
        if (!relayAudio_.start(selectedDevice(inputDevice_), selectedDevice(outputDevice_))) {
            voiceLabel_->setText("voice: selected audio device cannot use 16 kHz mono");
            return;
        }
        if (usingRelay_) {
            restoreVoiceState();
            voiceWanted_ = true;
            activeVoicePort_ = 65535; // Protocol marker: audio is carried by the relay, not SIP.
            sendFrame(&server_, chat::Message {chat::MsgType::VoicePort, {"65535"}});
            voiceButton_->setText("leave voice");
            voiceLabel_->setText("voice: relayed");
            muteButton_->setEnabled(!deafened_);
            deafenButton_->setEnabled(true);
            inputDevice_->setEnabled(false);
            outputDevice_->setEnabled(false);
            sendVoiceState();
            refreshMembers();
            return;
        }
        quint16 candidate = preferredVoicePort_;
        QUdpSocket probe;
        bool available = probe.bind(QHostAddress::AnyIPv4, candidate,
                                    QUdpSocket::DontShareAddress);
        while (!available && candidate < 65534) {
            ++candidate;
            available = probe.bind(QHostAddress::AnyIPv4, candidate,
                                   QUdpSocket::DontShareAddress);
        }
        if (!available) {
            relayAudio_.stop();
            voiceLabel_->setText("voice: no free SIP port");
            return;
        }
        probe.close();
        activeVoicePort_ = candidate;
        voiceWanted_ = true;
        QString error;
        if (!voice_.start(myName_, activeVoicePort_, selectedDevice(inputDevice_),
                          selectedDevice(outputDevice_), error)) {
            voiceWanted_ = false;
            activeVoicePort_ = 0;
            relayAudio_.stop();
            voiceLabel_->setText(error);
            return;
        }
        restoreVoiceState();
        if (deafened_) voice_.setDeafened(true);
        else if (muted_) voice_.setMuted(true);
        voiceButton_->setText("leave voice");
        muteButton_->setEnabled(!deafened_);
        deafenButton_->setEnabled(true);
        inputDevice_->setEnabled(false);
        outputDevice_->setEnabled(false);
        sendVoiceState();
        refreshMembers();
    });
}

void MainWindow::restoreVoiceState() {
    if (!resumeVoice_) return;
    muted_ = resumeMuted_;
    deafened_ = resumeDeafened_;
    mutedBeforeDeafen_ = resumeMutedBeforeDeafen_;
    relayAudio_.setMuted(muted_);
    relayAudio_.setDeafened(deafened_);
    muteButton_->setChecked(muted_);
    muteButton_->setText(muted_ ? "unmute" : "mute");
    muteButton_->setEnabled(!deafened_);
    deafenButton_->setChecked(deafened_);
    deafenButton_->setText(deafened_ ? "undeafen" : "deafen");
    resumeVoice_ = false;
}

void MainWindow::leaveVoice() {
    const bool wasInVoice = voiceWanted_;
    voiceWanted_ = false;
    if (wasInVoice && server_.state() == QAbstractSocket::ConnectedState) {
        sendFrame(&server_, chat::Message {chat::MsgType::VoicePort, {"0"}});
    }
    activeVoicePort_ = 0;
    voice_.stop();
    relayAudio_.stop();
    muted_ = false;
    deafened_ = false;
    mutedBeforeDeafen_ = false;
    localSpeaking_ = false;
    muteButton_->setChecked(false);
    deafenButton_->setChecked(false);
    muteButton_->setText("mute");
    deafenButton_->setText("deafen");
    muteButton_->setEnabled(false);
    deafenButton_->setEnabled(false);
    inputDevice_->setEnabled(true);
    outputDevice_->setEnabled(true);
    voiceButton_->setText("join voice");
    voiceButton_->setEnabled(!myName_.isEmpty() &&
                             server_.state() == QAbstractSocket::ConnectedState);
    voiceLabel_->setText("voice: off");
    if (wasInVoice) {
        sendVoiceState();
    }
    refreshMembers();
}

void MainWindow::showPreferences() {
    QDialog dialog(this);
    dialog.setWindowTitle("Preferences");
    dialog.setStyleSheet(styleSheet());
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* host = new QLineEdit(serverHost_, &dialog);
    auto* port = new QSpinBox(&dialog);
    port->setRange(1, 65535);
    port->setValue(serverPort_);
    auto* advertisedHost = new QLineEdit(advertiseHost_, &dialog);
    advertisedHost->setPlaceholderText("optional public DNS name or IP");
    auto* sipPort = new QSpinBox(&dialog);
    sipPort->setRange(1, 65534);
    sipPort->setValue(preferredVoicePort_);
    auto* relayHost = new QLineEdit(relayHost_, &dialog);
    relayHost->setPlaceholderText("optional; leave blank for direct connection");
    auto* relayPort = new QSpinBox(&dialog);
    relayPort->setRange(1, 65535);
    relayPort->setValue(relayPort_);
    auto* leakIp = new QCheckBox("Connect directly if relay fails (reveals your IP)", &dialog);
    leakIp->setChecked(leakMyIp_);
    auto* saveMessages = new QCheckBox("Save chat messages locally", &dialog);
    saveMessages->setChecked(saveMessages_);
    auto* messageFile = new QLineEdit(messageFile_, &dialog);
    auto* chooseMessageFile = new QPushButton("browse…", &dialog);
    auto* messageFileRow = new QWidget(&dialog);
    auto* messageFileLayout = new QHBoxLayout(messageFileRow);
    messageFileLayout->setContentsMargins(0, 0, 0, 0);
    messageFileLayout->addWidget(messageFile, 1);
    messageFileLayout->addWidget(chooseMessageFile);
    auto* maximum = new QLineEdit(&dialog);
    maximum->setText(maxSavedMessages_ ? QString::number(*maxSavedMessages_) : QString());
    maximum->setPlaceholderText("unlimited");
    messageFileRow->setEnabled(saveMessages_);
    maximum->setEnabled(saveMessages_);
    connect(saveMessages, &QCheckBox::toggled, messageFileRow, &QWidget::setEnabled);
    connect(saveMessages, &QCheckBox::toggled, maximum, &QWidget::setEnabled);
    connect(chooseMessageFile, &QPushButton::clicked, &dialog, [&, messageFile] {
        const QString chosen = QFileDialog::getSaveFileName(
            &dialog, "Choose message database", messageFile->text(),
            "SQLite database (*.db)", nullptr, QFileDialog::DontConfirmOverwrite);
        if (!chosen.isEmpty()) messageFile->setText(chosen);
    });
    form->addRow("server address", host);
    form->addRow("server port", port);
    form->addRow("public host", advertisedHost);
    form->addRow("voice SIP port", sipPort);
    form->addRow("relay address", relayHost);
    form->addRow("relay port", relayPort);
    form->addRow("", leakIp);
    form->addRow("", saveMessages);
    form->addRow("SQLite file", messageFileRow);
    form->addRow("maximum messages", maximum);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel,
                                          &dialog);
    layout->addWidget(buttons);
    std::optional<std::int64_t> chosenMaximum;
    std::unique_ptr<chat::RecentMessages> replacementStore;
    bool storeChanged = false;
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const QString path = messageFile->text().trimmed();
        const QString count = maximum->text().trimmed();
        if (saveMessages->isChecked() && path.isEmpty()) {
            QMessageBox::warning(&dialog, "Message saving", "Choose a SQLite file.");
            return;
        }
        chosenMaximum.reset();
        if (saveMessages->isChecked() && !count.isEmpty()) {
            bool valid = false;
            const qlonglong parsed = count.toLongLong(&valid);
            if (!valid || parsed <= 0) {
                QMessageBox::warning(&dialog, "Message saving",
                                     "Maximum messages must be a positive number, or blank for unlimited.");
                return;
            }
            chosenMaximum = parsed;
        }
        storeChanged = saveMessages->isChecked() &&
            (!saveMessages_ || path != messageFile_ || chosenMaximum != maxSavedMessages_ ||
             !recentMessages_);
        if (storeChanged) {
            try {
                replacementStore = std::make_unique<chat::RecentMessages>(s(path), chosenMaximum);
            } catch (const std::exception& error) {
                QMessageBox::warning(&dialog, "Message saving", q(error.what()));
                return;
            }
        }
        dialog.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    serverHost_ = host->text().trimmed();
    if (serverHost_.isEmpty()) {
        serverHost_ = "127.0.0.1";
    }
    serverPort_ = static_cast<quint16>(port->value());
    advertiseHost_ = advertisedHost->text().trimmed();
    preferredVoicePort_ = static_cast<quint16>(sipPort->value());
    relayHost_ = relayHost->text().trimmed();
    relayPort_ = static_cast<quint16>(relayPort->value());
    leakMyIp_ = leakIp->isChecked();
    saveMessages_ = saveMessages->isChecked();
    messageFile_ = messageFile->text().trimmed();
    maxSavedMessages_ = chosenMaximum;
    if (!saveMessages_) {
        recentMessages_.reset();
        savedMessagesLoaded_ = false;
        savingFailed_ = false;
    } else if (storeChanged) {
        recentMessages_ = std::move(replacementStore);
        savedMessagesLoaded_ = false;
        savingFailed_ = false;
        openSavedMessages();
    }
    QSettings settings;
    settings.setValue("server/host", serverHost_);
    settings.setValue("server/port", serverPort_);
    settings.setValue("server/advertisedHost", advertiseHost_);
    settings.setValue("voice/sipPort", preferredVoicePort_);
    settings.setValue("relay/host", relayHost_);
    settings.setValue("relay/port", relayPort_);
    settings.setValue("relay/leakMyIp", leakMyIp_);
    settings.setValue("messages/enabled", saveMessages_);
    settings.setValue("messages/file", messageFile_);
    settings.setValue("messages/maximum",
                      maxSavedMessages_ ? QString::number(*maxSavedMessages_) : QString());
}

void MainWindow::openSavedMessages() {
    if (!saveMessages_ || savingFailed_) return;
    try {
        if (!recentMessages_) {
            recentMessages_ = std::make_unique<chat::RecentMessages>(s(messageFile_),
                                                                      maxSavedMessages_);
        }
        if (savedMessagesLoaded_ || myName_.isEmpty()) return;
        const auto saved = recentMessages_->recent();
        savedMessagesLoaded_ = true;
        if (!saved.empty()) {
            appendChat("History", "recent messages from " + messageFile_, {}, true);
            for (const chat::RecentMessage& message : saved) {
                appendChat(q(message.sender), q(message.body), q(message.color), false,
                           message.timestamp);
            }
        }
    } catch (const std::exception& error) {
        savingFailed_ = true;
        recentMessages_.reset();
        QMessageBox::warning(this, "Message saving", q(error.what()));
    }
}

void MainWindow::recordChat(const chat::RecentMessage& message) {
    if (!saveMessages_ || savingFailed_) return;
    openSavedMessages();
    if (!recentMessages_) return;
    try {
        recentMessages_->append(message);
    } catch (const std::exception& error) {
        savingFailed_ = true;
        recentMessages_.reset();
        QMessageBox::warning(this, "Message saving", q(error.what()));
    }
}

void MainWindow::connectToServer() {
    if (name_->text().trimmed().isEmpty()) {
        name_->setProperty("invalid", true);
        name_->style()->unpolish(name_);
        name_->style()->polish(name_);
        name_->setFocus();
        return;
    }
    name_->setProperty("invalid", false);
    connectionLabel_->setText("Connecting");
    connectButton_->setText("Leave");
    intentionalDisconnect_ = false;
    desiredConnected_ = true;
    startTransport(!relayHost_.isEmpty());
}

void MainWindow::startTransport(bool relay) {
    if (!desiredConnected_ || server_.state() != QAbstractSocket::UnconnectedState) return;
    usingRelay_ = relay;
    transportClosedHandled_ = false;
    awaitingLogin_ = false;
    connectionLabel_->setText(relay ? "Connecting to relay" : "Connecting directly (IP visible)");
    server_.connectToHost(relay ? relayHost_ : serverHost_, relay ? relayPort_ : serverPort_);
    if (!relay && !relayHost_.isEmpty() && leakMyIp_) relayProbeTimer_.start();
    if (relay) relayProbeTimer_.stop();
}

void MainWindow::retryConnection() {
    if (desiredConnected_ && server_.state() == QAbstractSocket::UnconnectedState) {
        startTransport(usingRelay_);
    }
}

void MainWindow::probeRelay() {
    if (!desiredConnected_ || usingRelay_ || relayHost_.isEmpty() || !leakMyIp_ ||
        relayProbe_.state() != QAbstractSocket::UnconnectedState) return;
    relayProbeBuffer_.clear();
    relayProbe_.connectToHost(relayHost_, relayPort_);
    QTimer::singleShot(2500, this, [this] {
        if (relayProbe_.state() != QAbstractSocket::UnconnectedState) {
            relayProbe_.abort();
            relayProbeBuffer_.clear();
        }
    });
}

void MainWindow::handleTransportClosed() {
    if (transportClosedHandled_) return;
    transportClosedHandled_ = true;
    loginTimer_.stop();
    awaitingLogin_ = false;
    if (voiceWanted_) {
        resumeVoice_ = true;
        resumeMuted_ = muted_;
        resumeDeafened_ = deafened_;
        resumeMutedBeforeDeafen_ = mutedBeforeDeafen_;
    }
    leaveVoice();
    peerServer_.close();
    for (const Peer& peer : std::as_const(peers_)) {
        if (peer.socket) peer.socket->abort();
    }
    peers_.clear();
    peerBuffers_.clear();
    serverBuffer_.clear();
    myName_.clear();
    refreshMembers();
    voiceButton_->setEnabled(false);
    composer_->setEnabled(false);
    sendButton_->setEnabled(false);

    if (!desiredConnected_ || intentionalDisconnect_) {
        connectionLabel_->setText("Disconnected");
        connectButton_->setText("Join");
        name_->setEnabled(true);
        return;
    }
    if (switchingToRelay_) {
        switchingToRelay_ = false;
        connectionLabel_->setText("Relay restored; reconnecting");
        QTimer::singleShot(0, this, [this] { startTransport(true); });
    } else if (usingRelay_ && leakMyIp_) {
        connectionLabel_->setText("Relay unavailable; connecting directly (IP visible)");
        QTimer::singleShot(0, this, [this] { startTransport(false); });
    } else {
        connectionLabel_->setText(usingRelay_ ? "Relay unavailable; retrying" :
                                                "Server unavailable; retrying");
        reconnectTimer_.start();
    }
}

void MainWindow::disconnectAll() {
    intentionalDisconnect_ = true;
    desiredConnected_ = false;
    switchingToRelay_ = false;
    resumeVoice_ = false;
    resumeMuted_ = false;
    resumeDeafened_ = false;
    resumeMutedBeforeDeafen_ = false;
    awaitingLogin_ = false;
    reconnectTimer_.stop();
    relayProbeTimer_.stop();
    loginTimer_.stop();
    relayProbe_.abort();
    relayProbeBuffer_.clear();
    // Notify the room first; stopping the audio process must not delay Leave.
    server_.abort();
    leaveVoice();
    peerServer_.close();
    for (const Peer& peer : std::as_const(peers_)) {
        if (peer.socket) {
            peer.socket->abort();
        }
    }
    peers_.clear();
    peerBuffers_.clear();
    serverBuffer_.clear();
    myName_.clear();
    localSpeaking_ = false;
    connectButton_->setText("Join");
    name_->setEnabled(true);
    voiceButton_->setEnabled(false);
    composer_->setEnabled(false);
    sendButton_->setEnabled(false);
    connectionLabel_->setText("Disconnected");
    refreshMembers();
}

void MainWindow::sendFrame(QTcpSocket* socket, const chat::Message& message) {
    const std::string frame = chat::encode(message);
    socket->write(frame.data(), static_cast<qint64>(frame.size()));
}

void MainWindow::sendVoiceState() {
    if (myName_.isEmpty()) {
        return;
    }
    const chat::Message state {chat::MsgType::VoiceState,
                               {s(myName_), muted_ ? "1" : "0", deafened_ ? "1" : "0"}};
    if (server_.state() == QAbstractSocket::ConnectedState) sendFrame(&server_, state);
    for (const Peer& peer : std::as_const(peers_)) {
        if (peer.socket && peer.socket->state() == QAbstractSocket::ConnectedState) {
            sendFrame(peer.socket, state);
        }
    }
    refreshMembers();
}

void MainWindow::readServer() {
    serverBuffer_.append(server_.readAll());
    std::string buffer(serverBuffer_.constData(), static_cast<std::size_t>(serverBuffer_.size()));
    chat::Message message;
    while (chat::decode(buffer, message) == chat::DecodeStatus::Ok) {
        handleServerMessage(message);
    }
    serverBuffer_ = QByteArray(buffer.data(), static_cast<int>(buffer.size()));
}

void MainWindow::handleServerMessage(const chat::Message& message) {
    if (message.type == chat::MsgType::LoginOk && !message.fields.empty()) {
        awaitingLogin_ = false;
        loginTimer_.stop();
        myName_ = q(message.fields[0]);
        connectionLabel_->setText("* connected to " +
                                  (usingRelay_ ? "relay " + relayHost_ + ":" + QString::number(relayPort_)
                                               : serverHost_ + ":" + QString::number(serverPort_)) +
                                  " as " + myName_ +
                                  (!usingRelay_ && !relayHost_.isEmpty() ? " (direct; IP visible)" : ""));
        connectButton_->setText("Leave");
        name_->setEnabled(false);
        voiceButton_->setEnabled(true);
        composer_->setEnabled(true);
        sendButton_->setEnabled(true);
        openSavedMessages();
        refreshMembers();
        if (resumeVoice_) {
            QTimer::singleShot(0, this, [this] {
                if (desiredConnected_ && resumeVoice_ && !voiceWanted_) joinVoice();
            });
        }
    } else if ((message.type == chat::MsgType::Peer ||
                message.type == chat::MsgType::PeerJoined) && message.fields.size() >= 4) {
        addPeer(message);
    } else if (message.type == chat::MsgType::PeerLeft && !message.fields.empty()) {
        removePeer(q(message.fields[0]));
    } else if (message.type == chat::MsgType::VoicePort && message.fields.size() == 2) {
        const QString peerName = q(message.fields[0]);
        if (peers_.contains(peerName)) {
            Peer& peer = peers_[peerName];
            bool valid = false;
            const uint port = q(message.fields[1]).toUInt(&valid);
            if (valid && port <= 65535) {
                peer.voicePort = static_cast<quint16>(port);
                if (port == 0) {
                    peer.voiceConnected = false;
                    peer.speaking = false;
                    peer.muted = false;
                    peer.deafened = false;
                } else if (voiceWanted_ && !usingRelay_ && port != 65535 && myName_ < peerName) {
                    voice_.callPeer(peer.name, peer.host, peer.voicePort);
                }
                refreshMembers();
            }
        }
    } else if (message.type == chat::MsgType::VoiceAudio && message.fields.size() == 2) {
        const QString sender = q(message.fields[0]);
        if (peers_.contains(sender) && voiceWanted_ &&
            (usingRelay_ || peers_[sender].voicePort == 65535)) {
            const std::string& bytes = message.fields[1];
            relayAudio_.receive(sender, QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size())));
            if (bytes.size() == 640) {
                double sum = 0;
                for (std::size_t i = 0; i < bytes.size(); i += 2) {
                    const auto lo = static_cast<unsigned char>(bytes[i]);
                    const auto hi = static_cast<unsigned char>(bytes[i + 1]);
                    const auto sample = static_cast<qint16>(lo | (hi << 8));
                    sum += static_cast<double>(sample) * sample;
                }
                Peer& peer = peers_[sender];
                const bool speaking = sum / 320 > 450.0 * 450.0;
                peer.lastAudioMs = QDateTime::currentMSecsSinceEpoch();
                if (peer.speaking != speaking) {
                    peer.speaking = speaking;
                    refreshMembers();
                }
            }
        }
    } else if (message.type == chat::MsgType::VoiceState && message.fields.size() == 3) {
        const QString sender = q(message.fields[0]);
        if (peers_.contains(sender)) {
            peers_[sender].muted = message.fields[1] == "1";
            peers_[sender].deafened = message.fields[2] == "1";
            refreshMembers();
        }
    } else if (message.type == chat::MsgType::PeerChat && message.fields.size() >= 4) {
        appendChat(q(message.fields[0]), q(message.fields[2]), q(message.fields[3]));
        recordChat({q(message.fields[1]).toLongLong(), message.fields[0],
                    message.fields[2], message.fields[3]});
    } else if (message.type == chat::MsgType::History && message.fields.size() >= 3) {
        appendChat(q(message.fields[1]), q(message.fields[2]),
                   message.fields.size() >= 4 ? q(message.fields[3]) : "pink");
        recordChat({q(message.fields[0]).toLongLong(), message.fields[1], message.fields[2],
                    message.fields.size() >= 4 ? message.fields[3] : "pink"});
    } else if (message.type == chat::MsgType::Error && !message.fields.empty()) {
        appendChat("Server", q(message.fields[0]), {}, true);
    }
}

void MainWindow::addPeer(const chat::Message& message) {
    Peer peer;
    peer.name = q(message.fields[0]);
    peer.host = voiceReachableHost(q(message.fields[1]));
    peer.chatPort = static_cast<quint16>(QString::fromStdString(message.fields[2]).toUShort());
    if (message.fields.size() >= 5) {
        peer.voicePort = static_cast<quint16>(QString::fromStdString(message.fields[4]).toUShort());
    }
    if (peers_.contains(peer.name)) {
        const Peer& existing = peers_[peer.name];
        peer.socket = existing.socket;
        peer.voiceConnected = existing.voiceConnected;
        peer.speaking = existing.speaking;
        peer.muted = existing.muted;
        peer.deafened = existing.deafened;
    }
    peers_[peer.name] = peer;
    refreshMembers();
    if (myName_ < peer.name) {
        if (!usingRelay_) dialPeer(peer.name);
        if (voiceWanted_ && !usingRelay_ && peer.voicePort != 65535) {
            voice_.callPeer(peer.name, peer.host, peer.voicePort);
        }
    }
}

void MainWindow::removePeer(const QString& name) {
    const Peer peer = peers_.take(name);
    if (peer.socket) {
        peer.socket->disconnectFromHost();
    }
    relayAudio_.remove(name);
    refreshMembers();
    appendChat("Room", name + " left", {}, true);
}

void MainWindow::dialPeer(const QString& name) {
    Peer& peer = peers_[name];
    if (peer.socket || peer.chatPort == 0) {
        return;
    }
    auto* socket = new QTcpSocket(this);
    peer.socket = socket;
    attachPeerSocket(socket);
    connect(socket, &QTcpSocket::connected, this, [this, socket, name] {
        sendFrame(socket, chat::Message {chat::MsgType::Hello, {s(myName_), s(name)}});
        sendFrame(socket, chat::Message {chat::MsgType::VoiceState,
                                         {s(myName_), muted_ ? "1" : "0",
                                          deafened_ ? "1" : "0"}});
    });
    socket->connectToHost(peer.host, peer.chatPort);
}

void MainWindow::acceptPeer() {
    while (peerServer_.hasPendingConnections()) {
        attachPeerSocket(peerServer_.nextPendingConnection());
    }
}

void MainWindow::attachPeerSocket(QTcpSocket* socket) {
    peerBuffers_.insert(socket, {});
    connect(socket, &QTcpSocket::readyRead, this, [this, socket] { readPeer(socket); });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
        for (Peer& peer : peers_) {
            if (peer.socket == socket) {
                peer.socket = nullptr;
                peer.muted = false;
                peer.deafened = false;
            }
        }
        peerBuffers_.remove(socket);
        socket->deleteLater();
        refreshMembers();
    });
}

void MainWindow::readPeer(QTcpSocket* socket) {
    QByteArray& bytes = peerBuffers_[socket];
    bytes.append(socket->readAll());
    std::string buffer(bytes.constData(), static_cast<std::size_t>(bytes.size()));
    chat::Message message;
    while (chat::decode(buffer, message) == chat::DecodeStatus::Ok) {
        handlePeerMessage(socket, message);
    }
    bytes = QByteArray(buffer.data(), static_cast<int>(buffer.size()));
}

void MainWindow::handlePeerMessage(QTcpSocket* socket, const chat::Message& message) {
    if (message.type == chat::MsgType::Hello && message.fields.size() >= 2 &&
        q(message.fields[1]) == myName_) {
        const QString name = q(message.fields[0]);
        if (peers_.contains(name)) {
            peers_[name].socket = socket;
            sendFrame(socket, chat::Message {chat::MsgType::HelloOk, {s(myName_)}});
            sendFrame(socket, chat::Message {chat::MsgType::VoiceState,
                                             {s(myName_), muted_ ? "1" : "0",
                                              deafened_ ? "1" : "0"}});
        }
    } else if (message.type == chat::MsgType::PeerChat && message.fields.size() >= 4) {
        appendChat(q(message.fields[0]), q(message.fields[2]), q(message.fields[3]));
        recordChat({q(message.fields[1]).toLongLong(), message.fields[0],
                    message.fields[2], message.fields[3]});
    } else if (message.type == chat::MsgType::VoiceState && message.fields.size() >= 3) {
        const QString name = q(message.fields[0]);
        if (peers_.contains(name) && peers_[name].socket == socket &&
            (message.fields[1] == "0" || message.fields[1] == "1") &&
            (message.fields[2] == "0" || message.fields[2] == "1")) {
            peers_[name].muted = message.fields[1] == "1";
            peers_[name].deafened = message.fields[2] == "1";
            refreshMembers();
        }
    }
}

void MainWindow::sendMessage() {
    const QString body = composer_->text().trimmed();
    if (body.isEmpty() || myName_.isEmpty()) {
        return;
    }
    if (body.startsWith('/')) {
        runCommand(body);
        composer_->clear();
        return;
    }
    const qint64 timestampSeconds = QDateTime::currentSecsSinceEpoch();
    const std::string timestamp = std::to_string(timestampSeconds);
    const chat::Message live {chat::MsgType::PeerChat,
                              {s(myName_), timestamp, s(body), s(messageColor_)}};
    if (usingRelay_) sendFrame(&server_, live);
    for (const Peer& peer : std::as_const(peers_)) {
        if (peer.socket && peer.socket->state() == QAbstractSocket::ConnectedState) {
            sendFrame(peer.socket, live);
        }
    }
    appendChat(myName_, body, "231");
    recordChat({timestampSeconds, s(myName_), s(body), s(messageColor_)});
    composer_->clear();
}

void MainWindow::runCommand(const QString& command) {
    const QStringList parts = command.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    const QString name = parts.value(0).toLower();
    if (name == "/color") {
        if (parts.size() < 2) {
            appendChat("Color", "usage: /color <pink|mint|butter|periwinkle|lilac|aqua|peach|0-255>",
                       {}, true);
            appendChat("Color", "current color: " + messageColor_, messageColor_);
            return;
        }
        const QString requested = parts.at(1).toLower();
        if (!chat::isValidColor(s(requested))) {
            appendChat("Color", "unknown or reserved color: " + requested, {}, true);
        } else {
            messageColor_ = requested;
            appendChat("Color", "you chose " + requested, requested);
        }
    } else if (name == "/users") {
        if (peers_.isEmpty()) {
            appendChat("Users", "nobody else is here yet", {}, true);
            return;
        }
        QStringList names = peers_.keys();
        names.sort(Qt::CaseInsensitive);
        QStringList descriptions;
        for (const QString& peerName : names) {
            const Peer& peer = peers_[peerName];
            QString description = peerName + " (" + peer.host + ":" +
                                  QString::number(peer.chatPort) + ")";
            if (peer.socket && peer.socket->state() == QAbstractSocket::ConnectedState) {
                description += " (direct)";
            }
            if (peer.voiceConnected) {
                description += " (voice)";
            }
            descriptions.append(description);
        }
        appendChat("Users", "online: " + descriptions.join("; "), {}, true);
    } else if (name == "/clear") {
        transcript_->clear();
    } else if (name == "/save") {
        showPreferences();
    } else if (name == "/voice") {
        if (!voiceWanted_) {
            joinVoice();
        }
    } else if (name == "/leavevoice") {
        if (voiceWanted_) {
            leaveVoice();
        }
    } else if (name == "/mute") {
        if (!voiceWanted_) {
            appendChat("Voice", "join voice first", {}, true);
            return;
        }
        if (!muted_ && !deafened_) {
            muteButton_->click();
        }
        appendChat("Voice", "microphone muted", {}, true);
    } else if (name == "/unmute") {
        if (!voiceWanted_) {
            appendChat("Voice", "join voice first", {}, true);
            return;
        }
        if (deafened_) {
            appendChat("Voice", "undeafen before unmuting", {}, true);
        } else {
            if (muted_) {
                muteButton_->click();
            }
            appendChat("Voice", "microphone unmuted", {}, true);
        }
    } else if (name == "/deafen") {
        if (!voiceWanted_) {
            appendChat("Voice", "join voice first", {}, true);
            return;
        }
        if (!deafened_) {
            deafenButton_->click();
        }
        appendChat("Voice", "incoming audio disabled", {}, true);
    } else if (name == "/undeafen") {
        if (!voiceWanted_) {
            appendChat("Voice", "join voice first", {}, true);
            return;
        }
        if (deafened_) {
            deafenButton_->click();
        }
        appendChat("Voice", "incoming audio restored", {}, true);
    } else if (name == "/leave") {
        disconnectAll();
    } else if (name == "/quit" || name == "/exit") {
        close();
    } else if (name == "/help") {
        appendChat("Help", "/users          list everyone online", {}, true);
        appendChat("Help", "/color <value>  set a candy shade or xterm color 0-255", {}, true);
        appendChat("Help", "/voice          join the voice call", {}, true);
        appendChat("Help", "/leavevoice     leave voice and stay in chat", {}, true);
        appendChat("Help", "/mute           mute your microphone", {}, true);
        appendChat("Help", "/unmute         unmute your microphone", {}, true);
        appendChat("Help", "/deafen         mute mic and incoming audio", {}, true);
        appendChat("Help", "/undeafen       restore incoming audio", {}, true);
        appendChat("Help", "/clear          clear the local message pane", {}, true);
        appendChat("Help", "/save           configure continuous local message saving", {}, true);
        appendChat("Help", "/leave          leave this room", {}, true);
        appendChat("Help", "/quit, /exit    close this app window", {}, true);
    } else {
        appendChat("Command", "unknown command: " + name + " (try /help)", {}, true);
    }
}

void MainWindow::appendChat(const QString& sender, const QString& body,
                            const QString& messageColor, bool system, qint64 timestamp) {
    const QString color = system ? "#b5b65f" : chatColor(messageColor).name();
    const QString prefix = system ? "* " :
                           (timestamp >= 0 ? QDateTime::fromSecsSinceEpoch(timestamp)
                                           : QDateTime::currentDateTime()).toString("HH:mm ");
    transcript_->append("<div style='margin:1px 0;color:" + color + "'>" + prefix +
                        safeHtml(sender) + (system ? " " : ": ") + safeHtml(body) + "</div>");
}

void MainWindow::refreshMembers() {
    members_->clear();
    if (!myName_.isEmpty()) {
        const QString selfState = deafened_ ? "  deafened" : muted_ ? "  muted" :
                                  localSpeaking_ ? "  talking" :
                                  voiceWanted_ ? "  mic" : "  text";
        auto* self = new QListWidgetItem((localSpeaking_ && !muted_ ? "● " : "○ ") + myName_ +
                                         selfState, members_);
        self->setForeground(QColor(deafened_ ? "#e05252" : muted_ ? "#d9a441" :
                                   localSpeaking_ ? "#6ee7a2" : "#9dccca"));
    }
    QStringList names = peers_.keys();
    names.sort(Qt::CaseInsensitive);
    for (const QString& name : names) {
        const Peer& peer = peers_[name];
        const bool inVoice = peer.voicePort != 0;
        const QString marker = peer.speaking && !peer.muted && inVoice ? "● " :
                               inVoice ? "○ " : "· ";
        const QString state = !inVoice ? "  text" : peer.deafened ? "  deafened" : peer.muted ? "  muted" :
                              peer.speaking ? "  talking" :
                              peer.voiceConnected ? "  voice" : "  in voice";
        auto* item = new QListWidgetItem(marker + name + state, members_);
        item->setForeground(QColor(!inVoice ? "#a7a9aa" : peer.deafened ? "#e05252" :
                                  peer.muted ? "#d9a441" : peer.speaking ? "#6ee7a2" :
                                  "#9dccca"));
    }
}

void MainWindow::refreshAudioDevices() {
    const QString selectedInput = inputDevice_ ? inputDevice_->currentText() : QString();
    const QString selectedOutput = outputDevice_ ? outputDevice_->currentText() : QString();
    if (!inputDevice_ || !outputDevice_) {
        return;
    }
    inputDevice_->clear();
    outputDevice_->clear();
    inputDevice_->addItem("system default", "default");
    outputDevice_->addItem("system default", "default");
    for (const QString& device : availableAudioInputs()) {
        inputDevice_->addItem(device, device);
    }
    for (const QString& device : availableAudioOutputs()) {
        outputDevice_->addItem(device, device);
    }
    const int inputIndex = inputDevice_->findText(selectedInput);
    const int outputIndex = outputDevice_->findText(selectedOutput);
    inputDevice_->setCurrentIndex(inputIndex >= 0 ? inputIndex : 0);
    outputDevice_->setCurrentIndex(outputIndex >= 0 ? outputIndex : 0);
}
