#include "desktop/penguin_sprites.h"

#include <QBuffer>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>
#include <QtEndian>
#include <utility>

namespace {

constexpr int kStoredSize = 152; // Twice the displayed size, including on Retina.

QByteArray png(const QImage& image) {
    if (image.isNull()) return {};
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

QImage storedImage(const QByteArray& bytes) {
    if (bytes.isEmpty()) return {};
    QBuffer buffer;
    buffer.setData(bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "PNG");
    const QSize size = reader.size();
    if (size.isEmpty() || size.width() > kStoredSize || size.height() > kStoredSize) return {};
    return reader.read();
}

}  // namespace

PenguinSprites PenguinSprites::fromSettings(QSettings& settings) {
    PenguinSprites sprites;
    sprites.enabled = settings.value("penguins/customSprite", false).toBool();
    sprites.standing = storedImage(settings.value("penguins/standingSprite").toByteArray());
    sprites.walking = storedImage(settings.value("penguins/walkingSprite").toByteArray());
    return sprites;
}

void PenguinSprites::save(QSettings& settings) const {
    settings.setValue("penguins/customSprite", enabled);
    settings.setValue("penguins/standingSprite", png(standing));
    settings.setValue("penguins/walkingSprite", png(walking));
}

QByteArray PenguinSprites::wireData() const {
    if (!enabled || !complete()) return {};
    const QByteArray first = png(standing);
    const QByteArray second = png(walking);
    QByteArray bytes(4, '\0');
    qToBigEndian<quint32>(static_cast<quint32>(first.size()), bytes.data());
    return bytes + first + second;
}

PenguinSprites PenguinSprites::fromWireData(const QByteArray& bytes) {
    if (bytes.size() < 4 || bytes.size() > 256 * 1024) return {};
    const auto length = qFromBigEndian<quint32>(bytes.constData());
    if (length == 0 || length >= static_cast<quint32>(bytes.size() - 4)) return {};
    PenguinSprites result;
    result.standing = storedImage(bytes.mid(4, length));
    result.walking = storedImage(bytes.mid(4 + length));
    result.enabled = result.complete();
    return result;
}

QImage PenguinSprites::readImage(const QString& path, QString& error) {
    error.clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = "This file could not be opened. Choose another image.";
        return {};
    }
    if (file.size() > 10 * 1024 * 1024) {
        error = "Choose an image smaller than 10 MB.";
        return {};
    }
    QImageReader reader(&file);
    const auto format = reader.format();
    const QSize size = reader.size();
    if ((format != "png" && format != "jpeg" && format != "webp" && format != "gif") ||
        size.isEmpty()) {
        error = "Choose a PNG, JPEG, WebP or GIF image.";
        return {};
    }
    if (size.width() > 4096 || size.height() > 4096) {
        error = "Choose an image no larger than 4096 by 4096 pixels.";
        return {};
    }
    reader.setAutoTransform(true);
    QImage image = reader.read();
    if (image.isNull()) {
        error = "This image could not be read. Choose another image.";
        return {};
    }
    return image.scaled(kStoredSize, kStoredSize, Qt::KeepAspectRatio,
                        Qt::SmoothTransformation);
}

PenguinSpriteEditor::PenguinSpriteEditor(PenguinSprites sprites, QWidget* parent)
    : QWidget(parent), sprites_(std::move(sprites)), choice_(new QComboBox(this)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    choice_->setObjectName("customSpriteChoice");
    choice_->addItems({"Off (Club Penguin)", "Custom images"});
    choice_->setCurrentIndex(sprites_.enabled ? 1 : 0);
    choice_->setAccessibleName("Custom sprite");
    layout->addWidget(choice_);

    auto* images = new QWidget(this);
    auto* form = new QFormLayout(images);
    form->setContentsMargins(0, 0, 0, 0);
    auto addImage = [this, images, form](const QString& label, bool walking) {
        auto* row = new QWidget(images);
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        auto* preview = new QLabel(row);
        preview->setFixedSize(48, 48);
        preview->setAlignment(Qt::AlignCenter);
        auto* upload = new QPushButton(row);
        upload->setAccessibleName("Upload " + label.toLower() + " sprite");
        auto refresh = [this, preview, upload, walking] {
            const QImage& image = walking ? sprites_.walking : sprites_.standing;
            preview->setPixmap(image.isNull() ? QPixmap() : QPixmap::fromImage(image).scaled(
                48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            upload->setText(image.isNull() ? "Upload image…" : "Replace image…");
        };
        refresh();
        connect(upload, &QPushButton::clicked, this, [this, walking, refresh] {
            const QString path = QFileDialog::getOpenFileName(
                this, walking ? "Choose walking sprite" : "Choose standing sprite", {},
                "Images (*.png *.jpg *.jpeg *.webp *.gif)");
            if (path.isEmpty()) return;
            QString error;
            QImage image = PenguinSprites::readImage(path, error);
            if (image.isNull()) {
                QMessageBox::warning(this, "Custom sprite", error);
                return;
            }
            (walking ? sprites_.walking : sprites_.standing) = std::move(image);
            refresh();
        });
        rowLayout->addWidget(preview);
        rowLayout->addWidget(upload);
        rowLayout->addStretch();
        form->addRow(label, row);
    };
    addImage("Standing", false);
    addImage("Walking", true);
    auto* hint = new QLabel("Standing: face forward. Walking: face left.\n"
                           "Transparent images work best.\n"
                           "Animated images use the first frame.\n"
                           "Your sprite is shared with members of your communities.", images);
    form->addRow(hint);
    layout->addWidget(images);
    images->setVisible(sprites_.enabled);
    connect(choice_, &QComboBox::currentIndexChanged, this, [images](int index) {
        images->setVisible(index == 1);
    });
}

PenguinSprites PenguinSpriteEditor::sprites() const {
    PenguinSprites result = sprites_;
    result.enabled = choice_->currentIndex() == 1;
    return result;
}
