#include "desktop/unread_badge.h"

#include <QColor>
#include <QFont>
#include <QPainter>
#include <QPen>
#include <algorithm>

namespace {

const QString kDot = QStringLiteral("•");

}  // namespace

QString UnreadBadge::label() const {
    if (mentions > 99) return QStringLiteral("99+");
    if (mentions > 0) return QString::number(mentions);
    return unread ? kDot : QString();
}

QString badgedTitle(const QString& title, const UnreadBadge& badge) {
    if (badge.mentions > 0) return "(" + badge.label() + ") " + title;
    if (badge.unread) return kDot + " " + title;
    return title;
}

QImage drawBadge(const QImage& icon, const UnreadBadge& badge) {
    QImage image = icon.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (badge.isEmpty() || image.isNull()) return image;

    const qreal side = std::min(image.width(), image.height());
    // A count needs room for two digits; a dot only needs to be seen.
    const qreal diameter = side * (badge.mentions > 0 ? 0.6 : 0.4);
    const QRectF circle(image.width() - diameter, image.height() - diameter, diameter, diameter);
    // A dark rim keeps the badge distinct from the artwork beneath it and
    // from light taskbars.
    const qreal rim = std::max<qreal>(1.0, side / 24.0);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setPen(QPen(QColor(0x1e, 0x1f, 0x22), rim));
    painter.setBrush(badge.mentions > 0 ? QColor(0xf2, 0x3f, 0x43) : QColor(Qt::white));
    painter.drawEllipse(circle.adjusted(rim / 2, rim / 2, -rim / 2, -rim / 2));
    if (badge.mentions > 0) {
        // Taskbar icons are too small for "99+".
        const QString text = badge.mentions > 9 ? QStringLiteral("9+")
                                                : QString::number(badge.mentions);
        QFont font = painter.font();
        font.setBold(true);
        font.setPixelSize(std::max(1, qRound(diameter * (text.size() > 1 ? 0.5 : 0.62))));
        painter.setFont(font);
        painter.setPen(Qt::white);
        painter.drawText(circle, Qt::AlignCenter, text);
    }
    return image;
}
