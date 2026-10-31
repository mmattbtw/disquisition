#include <QApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QSlider>
#include <QTimer>

#include "check.h"
#include "desktop/volume_menu.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QMainWindow window;
    window.setCentralWidget(new QWidget(&window));
    window.menuBar()->addMenu("disquisition");
    window.resize(600, 400);
    window.show();
    // Also run this executable with QT_QPA_PLATFORM=cocoa on macOS. The
    // offscreen plugin does not supply the native window's safe-area insets.
    QTimer::singleShot(200, [&] {
        QMenu menu(window.centralWidget());
        int volume = 40;
        addVolumeSlider(menu, volume, [&](int value) { volume = value; });
        menu.addSeparator();
        auto* mute = menu.addAction("Mute");
        mute->setCheckable(true);
        // Let Cocoa deliver its first native resize/safe-area update.
        QTimer::singleShot(300, [&] {
            auto* slider = menu.findChild<QSlider*>("streamVolume");
            auto* label = menu.findChild<QLabel*>();
            CHECK(slider && label);
            auto* row = slider->parentWidget();
            for (auto* control : {static_cast<QWidget*>(slider), static_cast<QWidget*>(label)}) {
                CHECK(control->isVisible());
                CHECK(control->width() > 0 && control->height() > 0);
                CHECK(row->rect().contains(control->geometry()));
                CHECK(!control->visibleRegion().isEmpty());
            }
            CHECK(slider->value() == 40 && label->text() == "Volume 40%");
            QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
            QApplication::sendEvent(slider, &right);
            CHECK(volume == 41 && label->text() == "Volume 41%");
            mute->trigger();
            CHECK(mute->isChecked());
            menu.close();
        });
        menu.exec(window.mapToGlobal(QPoint(50, 50)));
        app.quit();
    });
    return app.exec();
}
