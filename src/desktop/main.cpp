#include <QApplication>
#include <QCommandLineParser>
#include <QFileOpenEvent>
#include <QNetworkAccessManager>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <memory>

#include "desktop/bug_report.h"
#include "desktop/crash_report.h"

#include "desktop/app_identity.h"
#include "desktop/app_launch.h"
#include "desktop/app_sound.h"
#include "desktop/main_window.h"
#include "desktop/update_install.h"
#include "desktop/updater.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QString::fromUtf8(disquisition::kSettingsName));
    QApplication::setApplicationDisplayName(QString::fromUtf8(disquisition::kAppName));
    QApplication::setOrganizationName("Disquisition");
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"community-data", "Community identity and metadata directory", "directory"});
    parser.addPositionalArgument("invite", "Optional disquisition://join/ invite link", "[invite]");
    parser.process(app);
    if (parser.isSet("community-data"))
        app.setProperty("communityDataDirectory", parser.value("community-data"));

    // A second copy, such as one Windows or Linux starts for an invite link,
    // hands the link to the app already open instead of starting up itself.
    const QString settingsName = QString::fromUtf8(disquisition::kSettingsName);
    const QString instance = instanceName(
        settingsName, parser.isSet("community-data")
                          ? parser.value("community-data")
                          : QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                                "/communities");
    if (forwardToRunningInstance(instance, inviteArgument(parser.positionalArguments())))
        return 0;
    if (startStagedUpdate(currentInstallation(), disquisition::kBuildNumber)) {
        return 0;
    }

    std::unique_ptr<CrashReportSession> crashSession;
    if (QSettings().value("reports/automaticCrashes", true).toBool()) {
        crashSession = std::make_unique<CrashReportSession>(crashReportDirectory(), bugReportDiagnostics());
        auto* crashNetwork = new QNetworkAccessManager(&app);
        QTimer::singleShot(0, &app, [crashNetwork] {
            sendPendingCrashReports(crashReportDirectory(), crashNetwork, bugReportWebhook());
        });
    }
    MainWindow window;
    window.show();
    InstanceServer instanceServer;
    QObject::connect(&instanceServer, &InstanceServer::launched, &window, [&window](const QString& link) {
        window.setWindowState((window.windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
        window.show();
        window.raise();
        window.activateWindow();
        // The same event macOS sends for a link, which the community panel handles.
        if (!link.isEmpty())
            QCoreApplication::postEvent(qApp, new QFileOpenEvent(QUrl(link)));
    });
    // The relaunched build must start up rather than hand off to this one.
    if (auto* updater = window.findChild<Updater*>())
        QObject::connect(updater, &Updater::aboutToRelaunch, &instanceServer, &InstanceServer::close);
    instanceServer.listen(instance);
    registerInviteScheme(QApplication::applicationDisplayName(), settingsName,
                         QCoreApplication::applicationFilePath());
    playSound(bundledSound("boot_up", &app));
    return app.exec();
}
