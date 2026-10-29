#include "desktop/volume_menu.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QSlider>
#include <QWidgetAction>

void addVolumeSlider(QMenu& menu, int percent, std::function<void(int)> changed) {
    auto* row = new QWidget(&menu);
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(10, 6, 10, 6);
    auto* label = new QLabel(row);
    label->setMinimumWidth(label->fontMetrics().horizontalAdvance("Volume 100%"));
    label->setText(QString("Volume %1%").arg(percent));
    auto* slider = new QSlider(Qt::Horizontal, row);
    slider->setObjectName("streamVolume");
    slider->setRange(0, 100);
    slider->setFixedWidth(140);
    slider->setValue(percent);
    layout->addWidget(label);
    layout->addWidget(slider);
    auto* action = new QWidgetAction(&menu);
    action->setDefaultWidget(row);
    menu.addAction(action);
    QObject::connect(slider, &QSlider::valueChanged, &menu,
                     [label, changed = std::move(changed)](int value) {
                         label->setText(QString("Volume %1%").arg(value));
                         changed(value);
                     });
}
