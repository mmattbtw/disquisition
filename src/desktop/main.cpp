#include <QApplication>

#include "desktop/main_window.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("Disquisition");
    QApplication::setOrganizationName("Disquisition");
    MainWindow window;
    window.show();
    return app.exec();
}
