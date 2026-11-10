#include "desktop/app_sound.h"

#include <QFile>
#include <QMediaDevices>
#include <QSettings>
#include <QSoundEffect>
#include <QUrl>
#include <memory>

#include "desktop/audio_device.h"

QSoundEffect* bundledSound(const QString& name, QObject* parent) {
    const QString path = ":/sounds/" + name + ".wav";
    if (!QFile::exists(path)) return nullptr;
    auto* sound = new QSoundEffect(parent);
    sound->setSource(QUrl("qrc" + path));
    return sound;
}

void playSound(QSoundEffect* sound) {
    if (!sound || sound->status() == QSoundEffect::Error) return;
    const QAudioDevice device =
        findAudioDevice(QMediaDevices::audioOutputs(),
                        QSettings().value("community/output").toString(),
                        QMediaDevices::defaultAudioOutput());
    if (sound->audioDevice() != device) sound->setAudioDevice(device);
    if (sound->status() == QSoundEffect::Ready) {
        sound->play();
        return;
    }
    // Just created: wait for the file to load rather than drop the sound.
    auto waiting = std::make_shared<QMetaObject::Connection>();
    *waiting = QObject::connect(sound, &QSoundEffect::statusChanged, sound, [sound, waiting] {
        if (sound->status() == QSoundEffect::Loading) return;
        QObject::disconnect(*waiting);
        if (sound->status() == QSoundEffect::Ready) sound->play();
    });
}
