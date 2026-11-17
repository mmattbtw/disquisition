#include <QApplication>
#include <QCommandLineParser>
#include <QNetworkAccessManager>
#include <QSettings>
#include <QTimer>
#include <memory>

#include "desktop/bug_report.h"
#include "desktop/crash_report.h"

#include "desktop/app_identity.h"
#include "desktop/app_sound.h"
#include "desktop/main_window.h"
#include "desktop/update_install.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QString::fromUtf8(disquisition::kSettingsName));
    QApplication::setApplicationDisplayName(QString::fromUtf8(disquisition::kAppName));
    QApplication::setOrganizationName("Disquisition");
    if (startStagedUpdate(currentInstallation(), disquisition::kBuildNumber)) {
        return 0;
    }
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"community-data", "Community identity and metadata directory", "directory"});
    parser.addPositionalArgument("invite", "Optional disquisition://join/ invite link", "[invite]");
    parser.process(app);
    if (parser.isSet("community-data"))
        app.setProperty("communityDataDirectory", parser.value("community-data"));

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
    playSound(bundledSound("boot_up", &app));
    return app.exec();
}
