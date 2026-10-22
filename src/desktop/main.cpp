#include <QApplication>
#include <QFont>
#include <QFontDatabase>

#include "desktop/main_window.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    // Keep the existing settings and application-data paths when changing the display name.
    QApplication::setApplicationName("Disquisition");
    QApplication::setApplicationDisplayName("disquisition");
    QApplication::setOrganizationName("Disquisition");
    
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
