#pragma once

#include <QLocalServer>
#include <QObject>
#include <QString>
#include <QStringList>

// Opening a disquisition:// link starts the app with the link as an argument.
// macOS hands the link to the running app itself; Windows and Linux start a
// second copy, so that copy passes the link to the first one and exits.

// The local socket a running app listens on. Apps from different channels or
// with different --community-data directories run side by side.
QString instanceName(const QString& settingsName, const QString& dataDirectory);

// The first disquisition:// argument, or an empty string.
QString inviteArgument(const QStringList& arguments);

// Asks the app already running under `name` to come to the front and open
// `link` (if not empty). True once it confirms; false if none is running.
bool forwardToRunningInstance(const QString& name, const QString& link, int timeoutMs = 2000);

class InstanceServer final : public QObject {
    Q_OBJECT
public:
    explicit InstanceServer(QObject* parent = nullptr);

    // Call only after forwardToRunningInstance() failed, which shows that a
    // leftover socket from a crashed app is safe to replace.
    bool listen(const QString& name);
    void close();

signals:
    // `link` is empty when the app was opened again without one.
    void launched(const QString& link);

private:
    QLocalServer server_;
};

// Makes disquisition:// links open this executable: the registry on Windows,
// a desktop entry and xdg-mime on Linux. macOS reads the bundle's Info.plist,
// so this does nothing there. The last app opened gets the links, like macOS.
bool registerInviteScheme(const QString& appName, const QString& settingsName,
                          const QString& executable);

// Exposed for tests.
QString windowsOpenCommand(const QString& executable);
QString desktopExecArgument(const QString& argument);
QString desktopEntryName(const QString& settingsName);
QString desktopEntry(const QString& appName, const QString& executable);
