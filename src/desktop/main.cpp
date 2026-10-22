#include <QApplication>

#include "desktop/app_identity.h"
#include "desktop/main_window.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QString::fromUtf8(disquisition::kSettingsName));
    QApplication::setApplicationDisplayName(QString::fromUtf8(disquisition::kAppName));
    QApplication::setOrganizationName("Disquisition");
    MainWindow window;
    window.show();
    return app.exec();
}
