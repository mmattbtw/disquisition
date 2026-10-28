#include <QApplication>
#include <QCommandLineParser>
#include <QFont>
#include <QFontDatabase>

#include "desktop/app_identity.h"
#include "desktop/main_window.h"

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

    // load font
    for (const char* font : {":/fonts/Inter-Regular.ttf", ":/fonts/Inter-SemiBold.ttf",
                             ":/fonts/Inter-Bold.ttf", ":/fonts/CalSans-Regular.ttf",
                             ":/fonts/JetBrainsMono-Regular.ttf"}) {
        QFontDatabase::addApplicationFont(font);
    }

    // Stylesheets cannot set word spacing; the stylesheet keeps it when it sets the family.
    QFont buttonFont = QApplication::font();
    buttonFont.setWordSpacing(3);
    QApplication::setFont(buttonFont, "QPushButton");
    QApplication::setFont(buttonFont, "QTabBar");

    MainWindow window;
    window.show();
    return app.exec();
}
