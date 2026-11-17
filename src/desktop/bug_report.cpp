#include "desktop/bug_report.h"

#include <QDialogButtonBox>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSysInfo>
#include <QVBoxLayout>

#include "desktop/app_identity.h"
#include "desktop/build_info.h"

QUrl bugReportWebhook() {
    const QUrl url(qEnvironmentVariable("DISQUISITION_BUG_REPORT_WEBHOOK",
                                       QString::fromUtf8(disquisition::kBugReportWebhook)));
    return url.isValid() && url.scheme() == "https" && !url.host().isEmpty() ? url : QUrl();
}
QString bugReportDiagnostics() {
    return QString("App: %1\nBuild: %2\nChannel: %3\nBranch: %4\nCommit: %5\n"
                   "OS: %6\nKernel: %7 %8\nArchitecture: %9\nQt: %10")
        .arg(QString::fromUtf8(disquisition::kAppName))
        .arg(disquisition::kBuildNumber)
        .arg(QString::fromUtf8(disquisition::kBuildChannel),
             QString::fromUtf8(disquisition::kGitBranch),
             QString::fromUtf8(disquisition::kGitCommit), QSysInfo::prettyProductName(),
             QSysInfo::kernelType(), QSysInfo::kernelVersion(), QSysInfo::currentCpuArchitecture(),
             QString::fromLatin1(qVersion()));
}

QNetworkReply* sendBugReport(QNetworkAccessManager* network, const QUrl& webhook,
                             const QString& title, const QString& description,
                             const QString& diagnostics) {
    QNetworkRequest request(webhook);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(15000);
    const auto section = [](const QString& text) {
        return QJsonObject{{"type", "section"},
                           {"text", QJsonObject{{"type", "plain_text"}, {"text", text}}}};
    };
    // Plain text prevents user input from pinging Slack users or channels.
    const QJsonObject payload{{"text", title},
                              {"blocks", QJsonArray{section(description), section(diagnostics)}},
                              {"mrkdwn", false}, {"parse", "none"},
                              {"unfurl_links", false}, {"unfurl_media", false}};
    return network->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
}

bool bugReportSucceeded(QNetworkReply* reply) {
    return reply->error() == QNetworkReply::NoError &&
           reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 &&
           reply->readAll().trimmed() == "ok";
}

BugReportDialog::BugReportDialog(QWidget* parent) : BugReportDialog(bugReportWebhook(), parent) {}

BugReportDialog::BugReportDialog(const QUrl& webhook, QWidget* parent) : QDialog(parent) {
    setWindowTitle("Report a Bug");
    setAttribute(Qt::WA_DeleteOnClose);
    resize(540, 540);
    auto* layout = new QVBoxLayout(this);
    auto* prompt = new QLabel("What went wrong? Include what you expected and how to reproduce it.", this);
    prompt->setWordWrap(true);
    layout->addWidget(prompt);
    auto* description = new QPlainTextEdit(this);
    description->setObjectName("bugDescription");
    description->setAccessibleName("Bug description");
    description->setPlaceholderText("Describe the bug…");
    layout->addWidget(description, 1);
    auto* notice = new QLabel("Your report and the diagnostics below will be sent to the developers' "
                             "Slack channel. Do not include passwords or private conversations.", this);
    notice->setWordWrap(true);
    layout->addWidget(notice);
    const QString debug = bugReportDiagnostics();
    auto* details = new QPlainTextEdit(debug, this);
    details->setObjectName("bugDiagnostics");
    details->setAccessibleName("Included diagnostics");
    details->setReadOnly(true);
    details->setMaximumHeight(170);
    layout->addWidget(details);
    auto* status = new QLabel(this);
    status->setObjectName("bugReportStatus");
    status->setWordWrap(true);
    layout->addWidget(status);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    auto* send = buttons->addButton("Send Report", QDialogButtonBox::AcceptRole);
    send->setObjectName("sendBugReport");
    send->setEnabled(false);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    if (webhook.isEmpty())
        status->setText("Bug reporting is not configured for this build.");
    const auto validate = [=] {
        const auto text = description->toPlainText().trimmed();
        send->setEnabled(!webhook.isEmpty() && !text.isEmpty() && text.size() <= 3000);
        if (!webhook.isEmpty())
            status->setText(text.size() > 3000 ? "Keep the report under 3,000 characters." : "");
    };
    connect(description, &QPlainTextEdit::textChanged, this, validate);
    auto* network = new QNetworkAccessManager(this);
    connect(send, &QPushButton::clicked, this, [=, this] {
        send->setEnabled(false);
        description->setReadOnly(true);
        status->setText("Sending…");
        auto* reply = sendBugReport(network, webhook, "Bug report",
                                    description->toPlainText().trimmed(), debug);
        connect(this, &QDialog::finished, reply, &QNetworkReply::abort);
        connect(reply, &QNetworkReply::finished, this, [=] {
            const bool ok = bugReportSucceeded(reply);
            reply->deleteLater();
            if (ok) {
                status->setText("Report sent. Thank you!");
                buttons->button(QDialogButtonBox::Cancel)->setText("Close");
            } else {
                description->setReadOnly(false);
                validate();
                // validate updates length errors; retain the delivery error for valid input.
                status->setText("Could not send the report. Check your connection and try again.");
            }
        });
    });
    description->setFocus();
}
