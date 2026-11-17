#include "desktop/symbols.h"

#include <QFile>
#include <QHash>
#include <QIconEngine>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>

#include <cmath>

namespace symbols {
namespace {

QByteArray source(const QString& name) {
    static QHash<QString, QByteArray> loaded;
    auto found = loaded.find(name);
    if (found == loaded.end()) {
        QFile file(":/icons/" + name + ".svg");
        found = loaded.insert(name, file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray());
    }
    return *found;
}

QImage render(const QString& name, const QColor& color, QSize pixels) {
    QImage image(pixels, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QSvgRenderer renderer(source(name));
    if (!renderer.isValid() || pixels.isEmpty())
        return image;
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    // Symbols are square; keep them square in a wider or taller box.
    const int side = std::min(pixels.width(), pixels.height());
    renderer.render(&painter, QRectF((pixels.width() - side) / 2.0,
                                     (pixels.height() - side) / 2.0, side, side));
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(image.rect(), color);
    return image;
}

class SymbolEngine final : public QIconEngine {
public:
    SymbolEngine(QString name, QColor color, QString checkedName, QColor checkedColor)
        : name_(std::move(name)), color_(color), checkedName_(std::move(checkedName)),
          checkedColor_(checkedColor) {}

    QIconEngine* clone() const override { return new SymbolEngine(*this); }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode,
               QIcon::State state) override {
        const qreal scale = painter->device() ? painter->device()->devicePixelRatioF() : 1;
        painter->drawPixmap(rect, scaledPixmap(rect.size(), mode, state, scale));
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return scaledPixmap(size, mode, state, 1);
    }

    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state,
                         qreal scale) override {
        const bool checked = state == QIcon::On && !checkedName_.isEmpty();
        QColor color = checked && checkedColor_.isValid() ? checkedColor_ : color_;
        if (mode == QIcon::Disabled)
            color.setAlphaF(color.alphaF() * 0.38);
        const QSize pixels(std::lround(size.width() * scale), std::lround(size.height() * scale));
        const auto key = QString("%1/%2x%3/%4").arg(checked).arg(pixels.width())
                             .arg(pixels.height()).arg(color.name(QColor::HexArgb));
        auto found = cache_.find(key);
        if (found == cache_.end())
            found = cache_.insert(key, QPixmap::fromImage(
                                           render(checked ? checkedName_ : name_, color, pixels)));
        QPixmap result = *found;
        result.setDevicePixelRatio(scale);
        return result;
    }

private:
    QString name_;
    QColor color_;
    QString checkedName_;
    QColor checkedColor_;
    QHash<QString, QPixmap> cache_;
};

} // namespace

QPixmap pixmap(const QString& name, const QColor& color, int size, qreal scale) {
    const int pixels = std::lround(size * scale);
    auto result = QPixmap::fromImage(render(name, color, QSize(pixels, pixels)));
    result.setDevicePixelRatio(scale);
    return result;
}

QIcon icon(const QString& name, const QColor& color, const QString& checkedName,
           const QColor& checkedColor) {
    return QIcon(new SymbolEngine(name, color, checkedName, checkedColor));
}

} // namespace symbols
