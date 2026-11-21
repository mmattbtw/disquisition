#include "desktop/app_launch.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QLocalSocket>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace {

constexpr char kScheme[] = "disquisition";
constexpr qsizetype kMaxMessage = 16 * 1024;

bool isInvite(const QString& argument) {
    return argument.startsWith(QString::fromLatin1(kScheme) + "://");
}

} // namespace

QString instanceName(const QString& settingsName, const QString& dataDirectory) {
    QString user = qEnvironmentVariable("USER");
    if (user.isEmpty()) {
        user = qEnvironmentVariable("USERNAME");
    }
    QString directory = QDir::cleanPath(QDir(dataDirectory).absolutePath());
#if defined(Q_OS_WIN)
    directory = directory.toLower();
#endif
    const QByteArray key = (settingsName + '\n' + directory + '\n' + user).toUtf8();
    return "disquisition-" +
           QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex().left(24));
}

QString inviteArgument(const QStringList& arguments) {
    for (const QString& argument : arguments) {
        if (isInvite(argument)) {
            return argument;
        }
    }
    return {};
}

bool forwardToRunningInstance(const QString& name, const QString& link, int timeoutMs) {
    QLocalSocket socket;
    socket.connectToServer(name);
    if (!socket.waitForConnected(timeoutMs)) {
        return false;
    }
#if defined(Q_OS_WIN)
    // Windows only lets the running app take focus if this process allows it.
    AllowSetForegroundWindow(ASFW_ANY);
#endif
    QByteArray message = isInvite(link) ? link.toUtf8() : QByteArray();
    message.replace('\n', "").replace('\r', "");
    socket.write(message + '\n');
    if (!socket.waitForBytesWritten(timeoutMs)) {
        return false;
    }
    while (!socket.canReadLine()) {
        if (!socket.waitForReadyRead(timeoutMs)) {
            return false;
        }
    }
    return socket.readLine().trimmed() == "ok";
}

InstanceServer::InstanceServer(QObject* parent) : QObject(parent) {
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&server_, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket* socket = server_.nextPendingConnection()) {
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            // A client that never finishes its line is dropped.
            QTimer::singleShot(5000, socket, &QLocalSocket::abort);
            connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
                if (!socket->canReadLine()) {
                    if (socket->bytesAvailable() > kMaxMessage) {
                        socket->abort();
                    }
                    return;
                }
                const QString line = QString::fromUtf8(socket->readLine(kMaxMessage)).trimmed();
                socket->write("ok\n");
                socket->flush();
                socket->disconnectFromServer();
                emit launched(isInvite(line) ? line : QString());
            });
        }
    });
}

bool InstanceServer::listen(const QString& name) {
    if (server_.listen(name)) {
        return true;
    }
    if (server_.serverError() != QAbstractSocket::AddressInUseError) {
        return false;
    }
    QLocalServer::removeServer(name);
    return server_.listen(name);
}

void InstanceServer::close() {
    server_.close();
}

QString windowsOpenCommand(const QString& executable) {
    // "--" stops the link from being read as an option, whatever it contains.
    return '"' + QDir::toNativeSeparators(executable) + "\" -- \"%1\"";
}

QString desktopExecArgument(const QString& argument) {
    // Desktop Entry spec: quote the argument, backslash-escape " ` $ \ inside
    // it, double % for field codes, then escape backslashes again because Exec
    // is itself an escaped string.
    QString quoted;
    for (const QChar c : argument) {
        if (c == '"' || c == '`' || c == '$' || c == '\\') {
            quoted += '\\';
        }
        quoted += c;
    }
    quoted = '"' + quoted + '"';
    quoted.replace("%", "%%");
    quoted.replace("\\", "\\\\");
    return quoted;
}

QString desktopEntryName(const QString& settingsName) {
    return settingsName.toLower().replace(' ', '-') + ".desktop";
}

QString desktopEntry(const QString& appName, const QString& executable) {
    return QStringLiteral("[Desktop Entry]\n"
                          "Type=Application\n"
                          "Name=%1\n"
                          "Exec=%2 -- %u\n"
                          "Terminal=false\n"
                          "NoDisplay=true\n"
                          "MimeType=x-scheme-handler/%3;\n")
        .arg(appName, desktopExecArgument(executable), QString::fromLatin1(kScheme));
}

bool registerInviteScheme(const QString& appName, const QString& settingsName,
                          const QString& executable) {
    if (executable.contains('\n') || executable.contains('\r')) {
        return false;
    }
#if defined(Q_OS_WIN)
    Q_UNUSED(settingsName);
    QSettings key("HKEY_CURRENT_USER\\Software\\Classes\\" + QString::fromLatin1(kScheme),
                  QSettings::NativeFormat);
    key.setValue("Default", "URL:" + appName + " invite");
    key.setValue("URL Protocol", "");
    key.setValue("DefaultIcon/Default", '"' + QDir::toNativeSeparators(executable) + "\",0");
    key.setValue("shell/open/command/Default", windowsOpenCommand(executable));
    key.sync();
    return key.status() == QSettings::NoError;
#elif defined(Q_OS_LINUX)
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
    const QString name = desktopEntryName(settingsName);
    const QString path = directory + "/" + name;
    const QByteArray contents = desktopEntry(appName, executable).toUtf8();
    QFile existing(path);
    const bool current = existing.open(QIODevice::ReadOnly) && existing.readAll() == contents;
    existing.close();
    if (!current) {
        QSaveFile file(path);
        if (!QDir().mkpath(directory) || !file.open(QIODevice::WriteOnly) ||
            file.write(contents) != contents.size() || !file.commit()) {
            return false;
        }
        QProcess::startDetached("update-desktop-database", {directory});
    }
    // Claim the links again on every start, as the last app opened does on macOS.
    return QProcess::startDetached(
        "xdg-mime", {"default", name, "x-scheme-handler/" + QString::fromLatin1(kScheme)});
#else
    Q_UNUSED(appName);
    Q_UNUSED(settingsName);
    Q_UNUSED(executable);
    return true;
#endif
}
