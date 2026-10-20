#include <QApplication>
#include <QFontDatabase>

#include "desktop/main_window.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    // Keep the existing settings and application-data paths when changing the display name.
    QApplication::setApplicationName("Disquisition");
    QApplication::setApplicationDisplayName("disquisition");
    QApplication::setOrganizationName("Disquisition");
    
    // load font
    for (const char* font : {":/fonts/Inter-Regular.ttf", ":/fonts/Inter-Bold.ttf",
                             ":/fonts/JetBrainsMono-Regular.ttf"}) {
        QFontDatabase::addApplicationFont(font);
    }

    MainWindow window;
    window.show();
    return app.exec();
}
