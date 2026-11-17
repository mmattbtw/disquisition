#include "desktop/crash_report.h"
#include "desktop/bug_report.h"

#include <QDateTime>
#include <QDir>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QStandardPaths>
#include <QUuid>
#include <csignal>
#include <cstdlib>
#ifdef Q_OS_WIN
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {
volatile std::sig_atomic_t crashFd = -1;
#ifdef Q_OS_WIN
HANDLE crashHandle = INVALID_HANDLE_VALUE;
#endif
constexpr int fatalSignals[] = {SIGABRT, SIGSEGV, SIGILL, SIGFPE
#ifndef Q_OS_WIN
                               , SIGBUS
#endif
};

void recordCrash(char marker) {
#ifdef Q_OS_WIN
    DWORD written;
    WriteFile(crashHandle, &marker, 1, &written, nullptr);
#else
    const auto ignored = ::write(crashFd, &marker, 1);
    (void)ignored;
#endif
}

void fatalSignal(int signal) {
    char marker = 'X';
    switch (signal) {
    case SIGABRT: marker = 'A'; break;
    case SIGSEGV: marker = 'S'; break;
    case SIGILL: marker = 'I'; break;
    case SIGFPE: marker = 'F'; break;
#ifndef Q_OS_WIN
    case SIGBUS: marker = 'B'; break;
#endif
    }
    recordCrash(marker);
#ifdef Q_OS_WIN
    std::signal(signal, SIG_DFL);
    // Continue through abort's Windows Error Reporting path rather than the
    // CRT default signal action, which exits before native reporting runs.
    if (signal == SIGABRT)
        std::abort();
    std::raise(signal);
    std::_Exit(128 + signal);
#else
    // Re-raise unblocked with the default disposition so native crash reporting
    // and the exit status still describe the original signal.
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    sigaction(signal, &action, nullptr);
    sigset_t unblocked;
    sigemptyset(&unblocked);
    sigaddset(&unblocked, signal);
    sigprocmask(SIG_UNBLOCK, &unblocked, nullptr);
    ::kill(::getpid(), signal);
    ::_exit(128 + signal);
#endif
}
#ifdef Q_OS_WIN
LONG WINAPI unhandledException(EXCEPTION_POINTERS*) {
    recordCrash('W');
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

QString crashReason(char marker) {
    switch (marker) {
    case 'A': return "SIGABRT";
    case 'S': return "SIGSEGV";
    case 'I': return "SIGILL";
    case 'F': return "SIGFPE";
    case 'B': return "SIGBUS";
    case 'W': return "Windows unhandled exception";
    default: return {};
    }
}
} // namespace

struct CrashReportSession::Handlers {
#ifdef Q_OS_WIN
    using SignalHandler = void (*)(int);
    SignalHandler previous[4]{};
    LPTOP_LEVEL_EXCEPTION_FILTER exceptionFilter = nullptr;
#else
    struct sigaction previous[5]{};
#endif
};

CrashReportSession::CrashReportSession(const QString& directory, const QString& diagnostics) {
    if (crashFd != -1 || !QDir().mkpath(directory))
        return;
    file_.setFileName(QDir(directory).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + ".crash"));
    if (!file_.open(QIODevice::WriteOnly | QIODevice::Unbuffered))
        return;
    file_.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    const auto contents = ("0\nSession started: " + QDateTime::currentDateTimeUtc().toString(Qt::ISODate) +
                           "\n" + diagnostics).toUtf8();
    if (file_.write(contents) != contents.size() || !file_.flush() || !file_.seek(0))
        return;
    crashFd = file_.handle();
#ifdef Q_OS_WIN
    crashHandle = reinterpret_cast<HANDLE>(_get_osfhandle(file_.handle()));
#endif
    handlers_ = std::make_unique<Handlers>();
    int i = 0;
    for (const int signal : fatalSignals) {
#ifdef Q_OS_WIN
        handlers_->previous[i++] = std::signal(signal, fatalSignal);
#else
        struct sigaction action{};
        action.sa_handler = fatalSignal;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESETHAND;
        sigaction(signal, &action, &handlers_->previous[i++]);
#endif
    }
#ifdef Q_OS_WIN
    handlers_->exceptionFilter = SetUnhandledExceptionFilter(unhandledException);
#endif
}

CrashReportSession::~CrashReportSession() {
    if (handlers_) {
        int i = 0;
        for (const int signal : fatalSignals) {
#ifdef Q_OS_WIN
            std::signal(signal, handlers_->previous[i++]);
#else
            sigaction(signal, &handlers_->previous[i++], nullptr);
#endif
        }
#ifdef Q_OS_WIN
        SetUnhandledExceptionFilter(handlers_->exceptionFilter);
#endif
        crashFd = -1;
    }
    if (file_.isOpen()) {
        const auto path = file_.fileName();
        file_.close();
        QFile::remove(path);
    }
}

QString crashReportDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/crash-reports";
}

void sendPendingCrashReports(const QString& directory, QNetworkAccessManager* network,
                             const QUrl& webhook) {
    if (webhook.isEmpty())
        return;
    for (const auto& name : QDir(directory).entryList({"*.crash"}, QDir::Files)) {
        const auto path = QDir(directory).filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const auto contents = file.read(16000);
        const auto reason = contents.isEmpty() ? QString() : crashReason(contents[0]);
        if (reason.isEmpty())
            continue; // Running sessions and forced kills have no crash marker.
        auto* reply = sendBugReport(network, webhook, "Automatic crash report",
                                   "The desktop app crashed with " + reason + ".\nReport ID: " + name,
                                   QString::fromUtf8(contents.mid(2)).left(3000));
        QObject::connect(reply, &QNetworkReply::finished, network, [reply, path] {
            if (bugReportSucceeded(reply))
                QFile::remove(path);
            reply->deleteLater();
        });
    }
}
