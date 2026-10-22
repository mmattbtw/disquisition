#include <QApplication>
#include <QCommandLineParser>

#include "desktop/main_window.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    // Keep the existing settings and application-data paths when changing the display name.
    QApplication::setApplicationName("Disquisition");
    QApplication::setApplicationDisplayName("disquisition");
    QApplication::setOrganizationName("Disquisition");
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({"community-data", "Community identity and metadata directory", "directory"});
    parser.addPositionalArgument("invite", "Optional disquisition://join/ invite link", "[invite]");
    parser.process(app);
    if (parser.isSet("community-data")) app.setProperty("communityDataDirectory", parser.value("community-data"));
    MainWindow window;
    window.show();
    return app.exec();
}
