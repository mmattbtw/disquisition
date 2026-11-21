#include <QCoreApplication>
#include <QEventLoop>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <cstdio>

#include "check.h"
#include "desktop/app_launch.h"

namespace {

const QString kLink = "disquisition://join/cSVWchK9g9zZA2NhL7J8uj";

void testInstanceName() {
    const QString name = instanceName("Disquisition", "/tmp/a");
    CHECK(name.startsWith("disquisition-"));
    CHECK(name == instanceName("Disquisition", "/tmp/a/"));
    CHECK(name == instanceName("Disquisition", "/tmp/b/../a"));
    CHECK(name != instanceName("Disquisition staging", "/tmp/a"));
    CHECK(name != instanceName("Disquisition", "/tmp/b"));
}

void testInviteArgument() {
    CHECK(inviteArgument({}).isEmpty());
    CHECK(inviteArgument({"https://example.com", "notes"}).isEmpty());
    CHECK(inviteArgument({"other", kLink}) == kLink);
}

void testWindowsOpenCommand() {
    const QString command = windowsOpenCommand("C:/Users/a b/disquisition/disquisition-desktop.exe");
#if defined(Q_OS_WIN)
    CHECK(command ==
          "\"C:\\Users\\a b\\disquisition\\disquisition-desktop.exe\" -- \"%1\"");
#else
    CHECK(command.endsWith("disquisition-desktop.exe\" -- \"%1\""));
#endif
}

void testDesktopEntry() {
    CHECK(desktopExecArgument("/opt/disquisition/app") == "\"/opt/disquisition/app\"");
    CHECK(desktopExecArgument("/home/a b/app") == "\"/home/a b/app\"");
    // %  -> %%; $ and " are backslash-escaped, and the backslashes doubled.
    CHECK(desktopExecArgument("/x/100%/$\"") == "\"/x/100%%/\\\\$\\\\\"\"");
    CHECK(desktopExecArgument("/x\\y") == "\"/x\\\\\\\\y\"");

    CHECK(desktopEntryName("Disquisition") == "disquisition.desktop");
    CHECK(desktopEntryName("Disquisition staging") == "disquisition-staging.desktop");

    const QString entry = desktopEntry("disquisition", "/home/a b/disquisition-desktop");
    CHECK(entry.startsWith("[Desktop Entry]\n"));
    CHECK(entry.contains("\nExec=\"/home/a b/disquisition-desktop\" -- %u\n"));
    CHECK(entry.contains("\nMimeType=x-scheme-handler/disquisition;\n"));
}

// The second copy blocks on its socket, so it runs on its own thread while
// this one runs the server's event loop.
bool forwardFromThread(const QString& name, const QString& link) {
    std::atomic<bool> forwarded = false;
    QThread* thread = QThread::create([&] { forwarded = forwardToRunningInstance(name, link, 3000); });
    QEventLoop loop;
    QObject::connect(thread, &QThread::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    thread->start();
    loop.exec();
    CHECK(thread->wait(1000));
    delete thread;
    QCoreApplication::processEvents();
    return forwarded;
}

void testHandoff() {
    const QString name = instanceName("Disquisition test", QCoreApplication::applicationDirPath() +
                                                               "/" + QString::number(QCoreApplication::applicationPid()));
    CHECK(!forwardToRunningInstance(name, kLink, 500));

    InstanceServer server;
    QStringList received;
    QObject::connect(&server, &InstanceServer::launched, [&](const QString& link) { received.append(link); });
    CHECK(server.listen(name));

    CHECK(forwardFromThread(name, kLink));
    CHECK(forwardFromThread(name, ""));
    // Anything that is not an invite only brings the app forward.
    CHECK(forwardFromThread(name, "--community-data=/elsewhere"));
    CHECK(received == QStringList({kLink, "", ""}));

    server.close();
    CHECK(!forwardToRunningInstance(name, kLink, 500));
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    testInstanceName();
    testInviteArgument();
    testWindowsOpenCommand();
    testDesktopEntry();
    testHandoff();
    std::puts("app_launch_test passed");
    return 0;
}
