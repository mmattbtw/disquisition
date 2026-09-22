#include "desktop/main_window.h"

#include <QDateTime>
#include <QColor>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QApplication>
#include <QAudioDevice>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QMediaDevices>
#include <QNetworkInterface>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QSplitter>
#include <QStyle>
#include <QTextBrowser>
#include <QTimer>
#include <QUdpSocket>
#include <QVBoxLayout>

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
    buildUi();
    setWindowTitle("Disquisition");
    resize(980, 680);

    connect(&server_, &QTcpSocket::connected, this, [this] {
        if (!peerServer_.listen(QHostAddress::Any, 0)) {
            QMessageBox::critical(this, "Cannot join", peerServer_.errorString());
            server_.disconnectFromHost();
            return;
        }
        connectionLabel_->setText("Signing in");
        sendFrame(&server_, chat::Message {
                                chat::MsgType::Login,
                                {s(name_->text()), std::to_string(peerServer_.serverPort()),
                                 s(advertise_->text()), std::to_string(voicePort_->value())}});
    });
    connect(&server_, &QTcpSocket::readyRead, this, &MainWindow::readServer);
    connect(&server_, &QTcpSocket::disconnected, this, [this] {
        peerServer_.close();
        connectionLabel_->setText("Disconnected");
        connectButton_->setText("Join");
        composer_->setEnabled(false);
        sendButton_->setEnabled(false);
        inputDevice_->setEnabled(true);
        outputDevice_->setEnabled(true);
        muteButton_->setEnabled(false);
        deafenButton_->setEnabled(false);
        muted_ = false;
        deafened_ = false;
        muteButton_->setChecked(false);
        deafenButton_->setChecked(false);
        muteButton_->setText("mute");
        deafenButton_->setText("deafen");
    });
    connect(&server_, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        connectionLabel_->setText(server_.errorString());
    });
    connect(&peerServer_, &QTcpServer::newConnection, this, &MainWindow::acceptPeer);
    connect(connectButton_, &QPushButton::clicked, this, [this] {
        if (server_.state() == QAbstractSocket::UnconnectedState) {
            startJoining();
        } else {
            disconnectAll();
        }
    });
    connect(sendButton_, &QPushButton::clicked, this, &MainWindow::sendMessage);
    connect(composer_, &QLineEdit::returnPressed, this, &MainWindow::sendMessage);
    connect(muteButton_, &QPushButton::clicked, this, [this] {
        muted_ = !muted_;
        voice_.setMuted(muted_);
        muteButton_->setChecked(muted_);
        muteButton_->setText(muted_ ? "unmute" : "mute");
        sendVoiceState();
    });
    connect(deafenButton_, &QPushButton::clicked, this, [this] {
        deafened_ = !deafened_;
        if (deafened_) {
            mutedBeforeDeafen_ = muted_;
        }
        voice_.setDeafened(deafened_);
        deafenButton_->setChecked(deafened_);
        deafenButton_->setText(deafened_ ? "undeafen" : "deafen");
        if (deafened_) {
            muted_ = true;
            muteButton_->setChecked(true);
            muteButton_->setText("unmute");
            muteButton_->setEnabled(false);
        } else {
            muted_ = mutedBeforeDeafen_;
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
    connect(&voice_, &VoiceEngine::localSpeakingChanged, this, [this](bool speaking) {
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
    QTimer::singleShot(0, this, [this] {
        requestMicrophoneAccess(this, [this](bool granted) {
            if (granted) {
                refreshAudioDevices();
                connectionLabel_->setText(
                    "* microphone access granted; " +
                    QString::number(inputDevice_->count() - 1) + " inputs, " +
                    QString::number(outputDevice_->count() - 1) + " outputs");
            } else {
                connectionLabel_->setText("* microphone access denied");
            }
        });
    });
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
    host_ = new QLineEdit("127.0.0.1", root);
    host_->setPlaceholderText("server");
    serverPort_ = new QSpinBox(root);
    serverPort_->setRange(1, 65535);
    serverPort_->setValue(9000);
    name_ = new QLineEdit(root);
    name_->setPlaceholderText("name");
    advertise_ = new QLineEdit(root);
    advertise_->setPlaceholderText("public host (optional)");
    voicePort_ = new QSpinBox(root);
    voicePort_->setRange(1, 65535);
    voicePort_->setValue(5060);
    connectButton_ = new QPushButton("join", root);
    connectButton_->setObjectName("primary");
    setup->addWidget(host_, 2);
    setup->addWidget(serverPort_);
    setup->addWidget(name_, 1);
    setup->addWidget(advertise_, 2);
    setup->addWidget(voicePort_);
    setup->addWidget(connectButton_);
    page->addLayout(setup);

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
    connectButton_->setEnabled(false);
    requestMicrophoneAccess(this, [this](bool granted) {
        connectButton_->setEnabled(true);
        if (granted) {
            refreshAudioDevices();
            // Separate local clients need separate SIP sockets. Preserve the
            // requested port when possible, otherwise select the next free one.
            quint16 candidate = static_cast<quint16>(voicePort_->value());
            QUdpSocket probe;
            while (candidate < 65535 &&
                   !probe.bind(QHostAddress::AnyIPv4, candidate, QUdpSocket::DontShareAddress)) {
                ++candidate;
            }
            if (candidate == 65535 &&
                !probe.bind(QHostAddress::AnyIPv4, candidate, QUdpSocket::DontShareAddress)) {
                QMessageBox::critical(this, "Voice unavailable",
                                      "Could not find a free local SIP port.");
                return;
            }
            if (candidate != voicePort_->value()) {
                connectionLabel_->setText("* voice port " +
                                          QString::number(voicePort_->value()) +
                                          " is busy; using " + QString::number(candidate));
                voicePort_->setValue(candidate);
            }
            probe.close();
            connectToServer();
        } else {
            QMessageBox::warning(this, "Microphone blocked",
                                 "Allow microphone access in System Settings, then try again.");
        }
    });
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
    server_.connectToHost(host_->text().trimmed(), static_cast<quint16>(serverPort_->value()));
}

void MainWindow::disconnectAll() {
    // Notify the room first; stopping the audio process must not delay Leave.
    server_.abort();
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
    refreshMembers();
    voice_.stop();
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
        myName_ = q(message.fields[0]);
        connectionLabel_->setText("* connected to " + host_->text() + ":" +
                                  QString::number(serverPort_->value()) + " as " + myName_);
        connectButton_->setText("Leave");
        composer_->setEnabled(true);
        sendButton_->setEnabled(true);
        inputDevice_->setEnabled(false);
        outputDevice_->setEnabled(false);
        muteButton_->setEnabled(true);
        deafenButton_->setEnabled(true);
        QString error;
        auto selectedDevice = [](QComboBox* combo) {
            return combo->currentData().isValid() ? combo->currentData().toString()
                                                  : combo->currentText();
        };
        if (voice_.start(myName_, static_cast<quint16>(voicePort_->value()),
                         selectedDevice(inputDevice_), selectedDevice(outputDevice_), error)) {
        } else {
            voiceLabel_->setText(error);
        }
        sendFrame(&server_, chat::Message {chat::MsgType::FetchHistory, {}});
        refreshMembers();
    } else if ((message.type == chat::MsgType::Peer ||
                message.type == chat::MsgType::PeerJoined) && message.fields.size() >= 4) {
        addPeer(message);
    } else if (message.type == chat::MsgType::PeerLeft && !message.fields.empty()) {
        removePeer(q(message.fields[0]));
    } else if (message.type == chat::MsgType::History && message.fields.size() >= 3) {
        appendChat(q(message.fields[1]), q(message.fields[2]),
                   message.fields.size() >= 4 ? q(message.fields[3]) : "pink");
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
        dialPeer(peer.name);
        voice_.callPeer(peer.name, peer.host, peer.voicePort);
    }
}

void MainWindow::removePeer(const QString& name) {
    const Peer peer = peers_.take(name);
    if (peer.socket) {
        peer.socket->disconnectFromHost();
    }
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
    const std::string timestamp = std::to_string(QDateTime::currentSecsSinceEpoch());
    const chat::Message live {chat::MsgType::PeerChat,
                              {s(myName_), timestamp, s(body), s(messageColor_)}};
    for (const Peer& peer : std::as_const(peers_)) {
        if (peer.socket && peer.socket->state() == QAbstractSocket::ConnectedState) {
            sendFrame(peer.socket, live);
        }
    }
    sendFrame(&server_, chat::Message {chat::MsgType::Store,
                                       {timestamp, s(body), s(messageColor_)}});
    appendChat(myName_, body, "231");
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
    } else if (name == "/mute") {
        if (!muted_ && !deafened_) {
            muteButton_->click();
        }
        appendChat("Voice", "microphone muted", {}, true);
    } else if (name == "/unmute") {
        if (deafened_) {
            appendChat("Voice", "undeafen before unmuting", {}, true);
        } else {
            if (muted_) {
                muteButton_->click();
            }
            appendChat("Voice", "microphone unmuted", {}, true);
        }
    } else if (name == "/deafen") {
        if (!deafened_) {
            deafenButton_->click();
        }
        appendChat("Voice", "incoming audio disabled", {}, true);
    } else if (name == "/undeafen") {
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
        appendChat("Help", "/mute           mute your microphone", {}, true);
        appendChat("Help", "/unmute         unmute your microphone", {}, true);
        appendChat("Help", "/deafen         mute mic and incoming audio", {}, true);
        appendChat("Help", "/undeafen       restore incoming audio", {}, true);
        appendChat("Help", "/clear          clear the local message pane", {}, true);
        appendChat("Help", "/leave          leave this room", {}, true);
        appendChat("Help", "/quit, /exit    close this app window", {}, true);
    } else {
        appendChat("Command", "unknown command: " + name + " (try /help)", {}, true);
    }
}

void MainWindow::appendChat(const QString& sender, const QString& body,
                            const QString& messageColor, bool system) {
    const QString color = system ? "#b5b65f" : chatColor(messageColor).name();
    const QString prefix = system ? "* " : QDateTime::currentDateTime().toString("HH:mm ");
    transcript_->append("<div style='margin:1px 0;color:" + color + "'>" + prefix +
                        safeHtml(sender) + (system ? " " : ": ") + safeHtml(body) + "</div>");
}

void MainWindow::refreshMembers() {
    members_->clear();
    if (!myName_.isEmpty()) {
        const QString selfState = deafened_ ? "  deafened" : muted_ ? "  muted" :
                                  localSpeaking_ ? "  talking" :
                                  voice_.running() ? "  mic" : "  text";
        auto* self = new QListWidgetItem((localSpeaking_ && !muted_ ? "● " : "○ ") + myName_ +
                                         selfState, members_);
        self->setForeground(QColor(deafened_ ? "#e05252" : muted_ ? "#d9a441" :
                                   localSpeaking_ ? "#6ee7a2" : "#9dccca"));
    }
    QStringList names = peers_.keys();
    names.sort(Qt::CaseInsensitive);
    for (const QString& name : names) {
        const Peer& peer = peers_[name];
        const QString marker = peer.speaking && !peer.muted ? "● " :
                               peer.voiceConnected ? "○ " : "· ";
        const QString state = peer.deafened ? "  deafened" : peer.muted ? "  muted" :
                              peer.speaking ? "  talking" :
                              peer.voiceConnected ? "  voice" : "  text";
        auto* item = new QListWidgetItem(marker + name + state, members_);
        item->setForeground(QColor(peer.deafened ? "#e05252" :
                                  peer.muted ? "#d9a441" : peer.speaking ? "#6ee7a2" :
                                  peer.voiceConnected ? "#9dccca" : "#a7a9aa"));
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
