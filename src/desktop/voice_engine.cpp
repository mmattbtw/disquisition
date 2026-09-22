#include "desktop/voice_engine.h"

#include <QDir>
#include <QCoreApplication>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTextStream>

#include <utility>

VoiceEngine::VoiceEngine(QObject* parent) : QObject(parent) {
    controlRetry_.setInterval(150);
    controlRetry_.setSingleShot(true);
    connect(&controlRetry_, &QTimer::timeout, this, &VoiceEngine::connectControl);
    connect(&control_, &QTcpSocket::readyRead, this, &VoiceEngine::readControl);
    connect(&control_, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        if (running()) {
            controlRetry_.start();
        }
    });
    process_.setProcessChannelMode(QProcess::MergedChannels);
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this] {
        const QString output = QString::fromUtf8(process_.readAllStandardOutput());
        for (const QString& line : output.split('\n', Qt::SkipEmptyParts)) {
            lastOutput_ = line.trimmed();
            emit logMessage(lastOutput_);
            if (line.contains("baresip is ready", Qt::CaseInsensitive)) {
                initialized_ = true;
                emit statusChanged("voice: ready");
                const QSet<QString> calls = pendingCalls_;
                pendingCalls_.clear();
                for (const QString& uri : calls) {
                    dialed_.insert(uri);
                    command("dial " + uri);
                }
            } else if (line.contains("Call established", Qt::CaseInsensitive)) {
                emit statusChanged("voice: connected");
            } else if (line.contains("Incoming call", Qt::CaseInsensitive)) {
                command("accept");
            }
        }
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        emit statusChanged("voice: baresip failed");
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int exitCode, QProcess::ExitStatus) {
                if (exitCode == 0) {
                    emit statusChanged("voice: off");
                } else {
                    emit statusChanged("voice error: " +
                                       (lastOutput_.isEmpty() ? "baresip exited" : lastOutput_));
                }
            });
}

bool VoiceEngine::writeProfile(const QString& name, quint16 sipPort, const QString& inputDevice,
                               const QString& outputDevice, QString& error) {
    QString profileName = name;
    for (QChar& character : profileName) {
        if (!character.isLetterOrNumber() && character != '-' && character != '_') {
            character = '_';
        }
    }
    profilePath_ = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                   + "/baresip/" + profileName + "-" + QString::number(sipPort);
    if (!QDir().mkpath(profilePath_)) {
        error = "Cannot create the baresip profile directory";
        return false;
    }

    QFile config(profilePath_ + "/config");
    if (!config.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        error = config.errorString();
        return false;
    }
    QTextStream out(&config);
    const QString moduleSuffix =
#if defined(Q_OS_WIN)
        ".dll";
#else
        ".so";
#endif
    const QString audioModule =
#if defined(Q_OS_MACOS)
        "coreaudio";
#elif defined(Q_OS_WIN)
        "wasapi";
#else
        "alsa";
#endif
    QString modulePath;
#if defined(Q_OS_MACOS)
    for (const QString& candidate : {QStringLiteral("/opt/homebrew/lib/baresip/modules"),
                                     QStringLiteral("/usr/local/lib/baresip/modules")}) {
        if (QDir(candidate).exists()) {
            modulePath = candidate;
            break;
        }
    }
#elif defined(Q_OS_WIN)
    modulePath = QCoreApplication::applicationDirPath() + "/baresip/modules";
#else
    for (const QString& candidate : {QStringLiteral("/usr/lib/baresip/modules"),
                                     QStringLiteral("/usr/local/lib/baresip/modules")}) {
        if (QDir(candidate).exists()) {
            modulePath = candidate;
            break;
        }
    }
#endif
    auto cleanDevice = [](QString device) {
        return device.replace('\n', ' ').replace('\r', ' ').trimmed();
    };
    const QString source = cleanDevice(inputDevice).isEmpty() ? "default" : cleanDevice(inputDevice);
    const QString player = cleanDevice(outputDevice).isEmpty() ? "default" : cleanDevice(outputDevice);
    if (!modulePath.isEmpty()) {
        out << "module_path " << modulePath << "\n";
    }
    out << "sip_listen 0.0.0.0:" << sipPort << "\n"
        << "call_max_calls 32\n"
        << "call_hold_other_calls no\n"
        << "call_accept yes\n"
        << "audio_srate 48000\n"
        << "audio_channels 1\n"
        << "audio_source " << audioModule << "," << source << "\n"
        << "audio_player " << audioModule << "," << player << "\n"
        << "audio_alert " << audioModule << "," << player << "\n"
        << "ctrl_tcp_listen 127.0.0.1:" << controlPort_ << "\n"
        << "module g711" << moduleSuffix << "\n"
        << "module menu" << moduleSuffix << "\n"
        << "module mixminus" << moduleSuffix << "\n"
        << "module vumeter" << moduleSuffix << "\n"
        << "module_app account" << moduleSuffix << "\n"
        << "module_app ctrl_tcp" << moduleSuffix << "\n";
#if defined(Q_OS_MACOS)
    out << "module coreaudio" << moduleSuffix << "\n";
#elif defined(Q_OS_WIN)
    out << "module wasapi" << moduleSuffix << "\n";
#else
    out << "module alsa" << moduleSuffix << "\n";
#endif
    config.close();

    QFile accounts(profilePath_ + "/accounts");
    if (!accounts.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        error = accounts.errorString();
        return false;
    }
    QTextStream accountOut(&accounts);
    accountOut << "<sip:" << name << "@localhost>;regint=0;answermode=auto\n";
    return true;
}

bool VoiceEngine::start(const QString& name, quint16 sipPort, const QString& inputDevice,
                        const QString& outputDevice, QString& error) {
    stop();
    lastOutput_.clear();
    QTcpServer portProbe;
    if (!portProbe.listen(QHostAddress::LocalHost, 0)) {
        error = "Cannot reserve a local baresip control port";
        return false;
    }
    controlPort_ = portProbe.serverPort();
    portProbe.close();
    if (!writeProfile(name, sipPort, inputDevice, outputDevice, error)) {
        return false;
    }
    QString executable = QStandardPaths::findExecutable("baresip");
#if defined(Q_OS_MACOS)
    if (executable.isEmpty() && QFile::exists("/opt/homebrew/bin/baresip")) {
        executable = "/opt/homebrew/bin/baresip";
    }
    if (executable.isEmpty() && QFile::exists("/usr/local/bin/baresip")) {
        executable = "/usr/local/bin/baresip";
    }
#endif
    if (executable.isEmpty()) {
        error = "voice error: baresip is not installed";
        return false;
    }
    process_.start(executable, {"-f", profilePath_});
    if (!process_.waitForStarted(3000)) {
        error = "voice error: could not start baresip";
        return false;
    }
    if (process_.waitForFinished(250)) {
        const QString output = QString::fromUtf8(process_.readAllStandardOutput()).trimmed();
        error = "voice error: " + (output.isEmpty() ? "baresip exited" : output.section('\n', -1));
        return false;
    }
    controlRetry_.start();
    emit statusChanged("voice: starting");
    return true;
}

void VoiceEngine::callPeer(const QString& name, const QString& host, quint16 sipPort) {
    if (!running() || sipPort == 0) {
        return;
    }
    const QString uri = "sip:" + name + "@" + host + ":" + QString::number(sipPort);
    if (dialed_.contains(uri) || pendingCalls_.contains(uri)) {
        return;
    }
    if (!initialized_) {
        pendingCalls_.insert(uri);
        return;
    }
    dialed_.insert(uri);
    command("dial " + uri);
}

void VoiceEngine::setMuted(bool muted) {
    if (running() && muted_ != muted) {
        for (const QString& callId : std::as_const(callIds_)) {
            command("callfind " + callId);
            command(muted ? "mute yes" : "mute no");
        }
        muted_ = muted;
        emit statusChanged(muted ? "voice: muted" : "voice: connected");
    }
}

void VoiceEngine::setDeafened(bool deafened) {
    if (!running() || deafened_ == deafened) {
        return;
    }
    if (deafened) {
        mutedBeforeDeafen_ = muted_;
        setMuted(true);
    }
    for (const QString& callId : std::as_const(callIds_)) {
        command("medialdir audio=" + QString(deafened ? "sendonly" : "sendrecv") +
                " callid=" + callId);
        command("callfind " + callId);
        command("reinvite");
    }
    deafened_ = deafened;
    if (!deafened) {
        setMuted(mutedBeforeDeafen_);
    }
    emit statusChanged(deafened ? "voice: deafened" :
                       (muted_ ? "voice: muted" : "voice: connected"));
}

void VoiceEngine::command(const QString& text) {
    if (!running()) {
        return;
    }
    if (control_.state() != QAbstractSocket::ConnectedState) {
        QTimer::singleShot(100, this, [this, text] { command(text); });
        return;
    }
    QString value = text;
    if (value.startsWith('/')) {
        value.remove(0, 1);
    }
    const qsizetype separator = value.indexOf(' ');
    QJsonObject request;
    request.insert("command", separator < 0 ? value : value.left(separator));
    if (separator >= 0) {
        request.insert("params", value.mid(separator + 1));
    }
    const QByteArray payload = QJsonDocument(request).toJson(QJsonDocument::Compact);
    control_.write(QByteArray::number(payload.size()) + ':' + payload + ',');
}

void VoiceEngine::stop() {
    controlRetry_.stop();
    if (running()) {
        process_.terminate();
        if (!process_.waitForFinished(100)) {
            process_.kill();
            process_.waitForFinished(100);
        }
    }
    control_.abort();
    controlBuffer_.clear();
    dialed_.clear();
    pendingCalls_.clear();
    callIds_.clear();
    muted_ = false;
    deafened_ = false;
    mutedBeforeDeafen_ = false;
    initialized_ = false;
}

bool VoiceEngine::running() const {
    return process_.state() != QProcess::NotRunning;
}

void VoiceEngine::connectControl() {
    if (!running() || control_.state() != QAbstractSocket::UnconnectedState) {
        return;
    }
    control_.connectToHost(QHostAddress::LocalHost, controlPort_);
}

void VoiceEngine::readControl() {
    controlBuffer_.append(control_.readAll());
    for (;;) {
        const qsizetype colon = controlBuffer_.indexOf(':');
        if (colon < 1) {
            return;
        }
        bool valid = false;
        const qlonglong size = controlBuffer_.left(colon).toLongLong(&valid);
        if (!valid || size < 0 || size > 1024 * 1024) {
            controlBuffer_.clear();
            return;
        }
        const qsizetype total = colon + 1 + size + 1;
        if (controlBuffer_.size() < total) {
            return;
        }
        if (controlBuffer_.at(total - 1) != ',') {
            controlBuffer_.clear();
            return;
        }
        handleControlEvent(controlBuffer_.mid(colon + 1, static_cast<qsizetype>(size)));
        controlBuffer_.remove(0, total);
    }
}

QString VoiceEngine::peerName(const QString& uri) {
    QString value = uri;
    if (value.startsWith("sip:")) {
        value.remove(0, 4);
    }
    const qsizetype at = value.indexOf('@');
    return at < 0 ? QString() : value.left(at);
}

void VoiceEngine::handleControlEvent(const QByteArray& json) {
    const QJsonObject event = QJsonDocument::fromJson(json).object();
    if (event.value("response").toBool() && !event.value("ok").toBool()) {
        const QString error = event.value("data").toString().trimmed();
        emit statusChanged("voice error: " + (error.isEmpty() ? "command failed" : error));
        return;
    }
    if (!event.value("event").toBool()) {
        return;
    }
    const QString type = event.value("type").toString();
    const QString name = peerName(event.value("peeruri").toString());
    if ((type == "CALL_ESTABLISHED" || type == "CALL_CLOSED") && !name.isEmpty()) {
        const QString callId = event.value("id").toString();
        if (!callId.isEmpty()) {
            if (type == "CALL_ESTABLISHED") {
                if (callIds_.contains(callId)) {
                    return;
                }
                callIds_.insert(callId);
                if (muted_) {
                    command("callfind " + callId);
                    command("mute yes");
                }
                if (deafened_) {
                    command("medialdir audio=sendonly callid=" + callId);
                    command("callfind " + callId);
                    command("reinvite");
                }
            } else {
                callIds_.remove(callId);
            }
        }
        emit peerVoiceChanged(name, type == "CALL_ESTABLISHED", false);
        return;
    }
    if (type != "VU_TX_REPORT" && type != "VU_RX_REPORT") {
        return;
    }
    bool valid = false;
    const double level = event.value("param").toString().toDouble(&valid);
    if (!valid) {
        return;
    }
    const bool speaking = level > -42.0;
    if (type == "VU_TX_REPORT") {
        emit localSpeakingChanged(speaking);
    } else if (!name.isEmpty()) {
        emit peerVoiceChanged(name, true, speaking);
    }
}
