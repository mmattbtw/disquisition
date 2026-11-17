#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <functional>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <csignal>
#include <cstdlib>
#ifdef Q_OS_WIN
#include <windows.h>
#include <crtdbg.h>
#else
#include <sys/resource.h>
#endif

#include "check.h"
#include "desktop/crash_report.h"

static bool until(const std::function<bool()>& condition) {
    QDeadlineTimer deadline(4000);
    while (!condition() && !deadline.hasExpired()) {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QThread::msleep(1);
    }
    return condition();
}

static QStringList reports(const QString& directory) {
    return QDir(directory).entryList({"*.crash"}, QDir::Files);
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().contains("--probe")) {
#ifdef Q_OS_WIN
        SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#else
        struct rlimit limit{0, 0};
        setrlimit(RLIMIT_CORE, &limit);
#endif
        CrashReportSession session(app.arguments()[2], "Build: crashed-123\nOS: test-os");
        const auto mode = app.arguments()[3];
        if (mode == "abort") std::abort();
        if (mode == "segv") std::raise(SIGSEGV);
#ifdef Q_OS_WIN
        if (mode == "seh") RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
#endif
        return 0;
    }
    QTemporaryDir directory;
    CHECK(directory.isValid());
    const auto probe = [&](const QString& mode, bool crash) {
        QProcess child;
        child.start(app.applicationFilePath(), {"--probe", directory.path(), mode});
        CHECK(child.waitForStarted());
        CHECK(child.waitForFinished(10000));
        if (crash)
            CHECK(child.exitStatus() == QProcess::CrashExit || child.exitCode() != 0);
        else
            CHECK(child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0);
    };
    probe("clean", false);
    CHECK(reports(directory.path()).isEmpty());
    // Each subprocess really terminates from a fatal signal, leaving a marker
    // and the diagnostics of the crashed build for a later process to send.
    probe("abort", true);
    CHECK(reports(directory.path()).size() == 1);
    probe("segv", true);
    CHECK(reports(directory.path()).size() == 2);
#ifdef Q_OS_WIN
    probe("seh", true);
    CHECK(reports(directory.path()).size() == 3);
#endif
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    QNetworkAccessManager network;
    QTcpServer server;
    CHECK(server.listen(QHostAddress::LocalHost));
    const QUrl webhook("http://127.0.0.1:" + QString::number(server.serverPort()) + "/report");
    const auto pending = reports(directory.path());
    sendPendingCrashReports(directory.path(), &network, {});
    CHECK(reports(directory.path()) == pending);
    // An active process is not mistaken for a crash, even during an upload scan.
    CrashReportSession live(directory.path(), "Build: current-456");
    const auto totalFiles = reports(directory.path()).size();
    CHECK(totalFiles == pending.size() + 1);
    for (int attempt = 0; attempt < 2; ++attempt) {
        sendPendingCrashReports(directory.path(), &network, webhook);
        int received = 0;
        while (received < pending.size()) {
            CHECK(until([&] { return server.hasPendingConnections(); }));
            auto* socket = server.nextPendingConnection();
            QByteArray request;
            CHECK(until([&] {
                request += socket->readAll();
                const auto split = request.indexOf("\r\n\r\n");
                if (split < 0) return false;
                int length = -1;
                for (const auto& line : request.left(split).split('\n'))
                    if (line.toLower().startsWith("content-length:"))
                        length = line.mid(15).trimmed().toInt();
                return length >= 0 && request.size() >= split + 4 + length;
            }));
            const auto json = QJsonDocument::fromJson(request.mid(request.indexOf("\r\n\r\n") + 4)).object();
            CHECK(json["text"].toString() == "Automatic crash report");
            const auto blocks = json["blocks"].toArray();
            const auto debug = blocks[1].toObject()["text"].toObject()["text"].toString();
            CHECK(debug.contains("Build: crashed-123"));
            CHECK(!debug.contains("current-456"));
            const auto message = blocks[0].toObject()["text"].toObject()["text"].toString();
            CHECK(message.contains("SIGABRT") || message.contains("SIGSEGV") || message.contains("Windows unhandled exception"));
            CHECK(message.contains("Report ID:"));
            socket->write(attempt == 0 ?
                "HTTP/1.1 503 Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n" :
                "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok");
            // Windows completes socket writes asynchronously. Keep the server
            // socket alive until its response has drained and it disconnects.
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            socket->disconnectFromHost();
            ++received;
        }
        if (attempt == 0) {
            CHECK(until([&] { return network.findChildren<QNetworkReply*>().isEmpty(); }));
            CHECK(reports(directory.path()).size() == totalFiles);
        } else {
            CHECK(until([&] { return reports(directory.path()).size() == 1; }));
        }
    }
    sendPendingCrashReports(directory.path(), &network, webhook);
    QCoreApplication::processEvents();
    CHECK(!server.hasPendingConnections());
    std::puts("crash_report_test: ok");
}
