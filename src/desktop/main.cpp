#include <QApplication>

#include "desktop/main_window.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    // Keep the existing settings and application-data paths when changing the display name.
    QApplication::setApplicationName("Disquisition");
    QApplication::setApplicationDisplayName("disquisition");
    QApplication::setOrganizationName("Disquisition");
    MainWindow window;
    window.show();
    return app.exec();
}
