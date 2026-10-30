#include <QApplication>
#include <QCommandLineParser>

#include "desktop/app_identity.h"
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

    MainWindow window;
    window.show();
    return app.exec();
}
