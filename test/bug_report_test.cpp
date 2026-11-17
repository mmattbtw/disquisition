#include <QApplication>
#include <QDeadlineTimer>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkProxy>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <functional>

#include "check.h"
#include "desktop/app_identity.h"
#include "desktop/build_info.h"
#include "desktop/bug_report.h"

static bool until(const std::function<bool()>& condition) {
    QDeadlineTimer deadline(3000);
    while (!condition() && !deadline.hasExpired()) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    return condition();
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    // No endpoint, whitespace and oversized reports cannot be submitted.
    BugReportDialog unavailable(QUrl(), nullptr);
    unavailable.findChild<QPlainTextEdit*>("bugDescription")->setPlainText("a bug");
    CHECK(!unavailable.findChild<QPushButton*>("sendBugReport")->isEnabled());
    CHECK(unavailable.findChild<QLabel*>("bugReportStatus")->text().contains("not configured"));

    QTcpServer server;
    CHECK(server.listen(QHostAddress::LocalHost));
    const QUrl url("http://127.0.0.1:" + QString::number(server.serverPort()) + "/webhook");
    BugReportDialog dialog(url, nullptr);
    auto* description = dialog.findChild<QPlainTextEdit*>("bugDescription");
    auto* send = dialog.findChild<QPushButton*>("sendBugReport");
    auto* status = dialog.findChild<QLabel*>("bugReportStatus");
    description->setPlainText(" \n ");
    CHECK(!send->isEnabled());
    description->setPlainText(QString(3001, 'a'));
    CHECK(!send->isEnabled());
    const QString report = "Audio broke after joining. < !channel > & \"hello\"\n日本語";
    description->setPlainText(report);
    CHECK(send->isEnabled());
    dialog.show();
    QApplication::processEvents();
    CHECK(dialog.grab().save("bug-report-dialog.png"));

    // Exercise actual HTTP serialization and a Slack-shaped failure followed by retry.
    for (int attempt = 0; attempt < 4; ++attempt) {
        send->click();
        CHECK(!send->isEnabled());
        CHECK(description->isReadOnly());
        CHECK(until([&] { return server.hasPendingConnections(); }));
        auto* socket = server.nextPendingConnection();
        QByteArray request;
        CHECK(until([&] {
            request += socket->readAll();
            const int split = request.indexOf("\r\n\r\n");
            if (split < 0) return false;
            const auto headers = request.left(split).split('\n');
            int length = -1;
            for (const auto& header : headers)
                if (header.toLower().startsWith("content-length:"))
                    length = header.mid(15).trimmed().toInt();
            return length >= 0 && request.size() >= split + 4 + length;
        }));
        CHECK(request.startsWith("POST /webhook HTTP/1.1"));
        CHECK(request.toLower().contains("content-type: application/json"));
        const auto payload = QJsonDocument::fromJson(request.mid(request.indexOf("\r\n\r\n") + 4)).object();
        const auto blocks = payload["blocks"].toArray();
        CHECK(blocks.size() == 2);
        QString text;
        for (const auto& block : blocks) {
            const auto content = block.toObject()["text"].toObject();
            CHECK(content["type"].toString() == "plain_text");
            text += content["text"].toString();
        }
        CHECK(text.contains(report));
        CHECK(text.contains("Build: " + QString::number(disquisition::kBuildNumber)));
        CHECK(text.contains(QString::fromUtf8(disquisition::kGitCommit)));
        CHECK(text.contains(QString::fromUtf8(disquisition::kBuildChannel)));
        CHECK(text.contains("OS: ") && text.contains("Architecture: ") && text.contains("Qt: "));
        CHECK(!payload["mrkdwn"].toBool(true));
        CHECK(payload["parse"].toString() == "none");
        CHECK(!payload["unfurl_links"].toBool(true));
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        if (attempt < 3) {
            if (attempt == 0)
                socket->write("HTTP/1.1 403 Forbidden\r\nContent-Length: 17\r\nConnection: close\r\n\r\naction_prohibited");
            else if (attempt == 1)
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: 13\r\nConnection: close\r\n\r\ninvalid_token");
            else
                socket->write("HTTP/1.1 302 Found\r\nLocation: /other\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            socket->disconnectFromHost();
            CHECK(until([&] { return send->isEnabled(); }));
            CHECK(!description->isReadOnly());
            CHECK(description->toPlainText() == report);
            CHECK(status->text().contains("Could not send"));
        } else {
            socket->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok");
            socket->disconnectFromHost();
            CHECK(until([&] { return status->text().contains("Report sent"); }));
            CHECK(!send->isEnabled());
        }
    }
    std::puts("bug_report_test: ok");
}
