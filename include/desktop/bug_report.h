#pragma once

#include <QDialog>
#include <QUrl>

// The URL argument lets tests exercise delivery against a local HTTP server.
class BugReportDialog final : public QDialog {
public:
    explicit BugReportDialog(QWidget* parent = nullptr);
    BugReportDialog(const QUrl& webhook, QWidget* parent);
};

class QNetworkAccessManager;
class QNetworkReply;
QUrl bugReportWebhook();
QString bugReportDiagnostics();
QNetworkReply* sendBugReport(QNetworkAccessManager* network, const QUrl& webhook,
                             const QString& title, const QString& description,
                             const QString& diagnostics);
bool bugReportSucceeded(QNetworkReply* reply);
