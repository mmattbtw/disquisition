#pragma once

#include <QImage>
#include <QWidget>

class QComboBox;
class QSettings;

struct PenguinSprites {
    bool enabled = false;
    QImage standing;
    QImage walking;

    bool complete() const { return !standing.isNull() && !walking.isNull(); }
    static PenguinSprites fromSettings(QSettings& settings);
    void save(QSettings& settings) const;
    static QImage readImage(const QString& path, QString& error);
};

// Edits a copy. Preferences commits it only when Save is accepted.
class PenguinSpriteEditor final : public QWidget {
public:
    explicit PenguinSpriteEditor(PenguinSprites sprites, QWidget* parent = nullptr);
    PenguinSprites sprites() const;

private:
    PenguinSprites sprites_;
    QComboBox* choice_;
};
