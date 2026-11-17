#pragma once

#include <QDialog>
#include <QUrl>

// The URL argument lets tests exercise delivery against a local HTTP server.
class BugReportDialog final : public QDialog {
public:
    explicit BugReportDialog(QWidget* parent = nullptr);
    BugReportDialog(const QUrl& webhook, QWidget* parent);
};
