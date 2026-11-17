#pragma once

#include <QFile>
#include <QString>
#include <memory>

class QNetworkAccessManager;
class QUrl;

// One per desktop process. Prepare diagnostics while healthy; the fatal handler
// only writes one byte to an already-open file, then leaves termination to the OS.
class CrashReportSession {
public:
    explicit CrashReportSession(const QString& directory, const QString& diagnostics);
    ~CrashReportSession();
    CrashReportSession(const CrashReportSession&) = delete;
    CrashReportSession& operator=(const CrashReportSession&) = delete;
private:
    struct Handlers;
    std::unique_ptr<Handlers> handlers_;
    QFile file_;
};

QString crashReportDirectory();
void sendPendingCrashReports(const QString& directory, QNetworkAccessManager* network,
                             const QUrl& webhook);
