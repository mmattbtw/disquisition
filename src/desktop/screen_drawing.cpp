#include "desktop/screen_drawing.h"

#include <QApplication>
#include <QConicalGradient>
#include <QFrame>
#include <QStyle>
#include <QColorDialog>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSignalBlocker>
#include <QShortcut>
#include <QVBoxLayout>
#include <cmath>
#include <utility>

#if defined(Q_OS_MACOS)
#    include "desktop/mac_drawing_panel.h"
#endif

#if defined(Q_OS_WIN)
#    include <qt_windows.h>
#endif

QColor screenPenColor(int index) {
    static const char* colors[] = {"#ff5470", "#ffcf5a", "#67e8a5",
                                   "#62cfff", "#c49bff", "#ffffff"};
    return QColor(colors[qBound(0, index, chat::kPenColorCount - 1)]);
}

QColor screenSegmentColor(const chat::ScreenDraw& segment) {
    return segment.rgb >= 0 ? QColor::fromRgb(static_cast<QRgb>(segment.rgb))
                            : screenPenColor(segment.color);
}

QString screenStrokeKey(const chat::ScreenDraw& segment) {
    return QString::fromStdString(segment.author) + ':' +
           QString::number(segment.stroke ? segment.stroke : segment.id);
}

ScreenDrawing::ScreenDrawing(QObject* parent) : QObject(parent) {
    activityClock_.start();
    activityTimer_.setInterval(250);
    connect(&activityTimer_, &QTimer::timeout, this, [this] {
        const auto now = activityClock_.elapsed();
        for (auto it = activeAuthors_.begin(); it != activeAuthors_.end();) {
            if (now - it->seen >= 1800)
                it = activeAuthors_.erase(it);
            else
                ++it;
        }
        if (activeAuthors_.isEmpty())
            activityTimer_.stop();
        emit updated();
    });
    laserTimer_.setInterval(16);
    connect(&laserTimer_, &QTimer::timeout, this, [this] {
        bool repaint = false;
        for (auto it = lasers_.begin(); it != lasers_.end();) {
            repaint |= !it->trail.isEmpty();
            it->trail.removeIf([this](const LaserSample& sample) { return laserAge(sample.seen) >= laserTrailMs; });
            if (it->point.visible && laserAge(it->seen) >= laserTimeoutMs) {
                it->point.visible = false;
                repaint = true;
            }
            if (!it->point.visible && it->trail.isEmpty())
                it = lasers_.erase(it);
            else
                ++it;
        }
        if (lasers_.isEmpty())
            laserTimer_.stop();
        if (repaint)
            emit updated();
    });
}

void ScreenDrawing::setIdentity(const QString& name, bool owner) {
    localName_ = name;
    owner_ = owner;
    emit changed();
}

void ScreenDrawing::setChatColor(const QColor& color) {
    if (color.isValid()) {
        chatColor_ = color;
        emit changed();
    }
}

QColor ScreenDrawing::penColor() const {
    if (color_ == chat::kPenColorCount && customColor_.isValid())
        return customColor_;
    return color_ < 0 ? chatColor_ : screenPenColor(color_);
}

void ScreenDrawing::setCustomColor(const QColor& color) {
    if (!color.isValid())
        return;
    customColor_ = QColor::fromRgb(color.rgb());
    color_ = chat::kPenColorCount;
    emit changed();
}

bool ScreenDrawing::canUndo() const {
    if (!drawingAllowed() || localName_.isEmpty())
        return false;
    for (const auto& segment : segments_) {
        if (QString::fromStdString(segment.author) == localName_)
            return true;
    }
    return false;
}

void ScreenDrawing::undo() {
    if (!canUndo())
        return;
    for (auto it = segments_.crbegin(); it != segments_.crend(); ++it) {
        if (QString::fromStdString(it->author) == localName_) {
            const quint32 stroke = it->stroke ? it->stroke : it->id;
            emit eraseRequested(localName_, {stroke});
            // Cancel an in-progress gesture so it cannot extend an undone stroke.
            emit changed();
            return;
        }
    }
}

void ScreenDrawing::setState(bool enabled, std::int64_t epoch) {
    enabled_ = enabled;
    epoch_ = epoch;
    segments_.clear();
    received_.clear();
    removed_.clear();
    selected_.clear();
    selectionBox_ = {};
    activeAuthors_.clear();
    activityTimer_.stop();
    lasers_.clear();
    laserIds_.clear();
    laserTimer_.stop();
    if (!enabled)
        tool_ = Tool::None;
    emit changed();
}

void ScreenDrawing::add(const chat::ScreenDraw& segment) {
    if (!enabled_ || segment.epoch != epoch_ || historySize() >= static_cast<qsizetype>(chat::kMaxDrawSegments) ||
        removed_.contains(screenStrokeKey(segment)))
        return;
    const QString author = QString::fromStdString(segment.author);
    const QString key = author + ':' + QString::number(segment.id);
    if (received_.contains(key))
        return;
    received_.insert(key);
    segments_.append(segment);
    if (!author.isEmpty()) {
        activeAuthors_[author] = {segment, activityClock_.elapsed()};
        if (!activityTimer_.isActive())
            activityTimer_.start();
    }
    emit updated();
}

bool ScreenDrawing::addLaser(const chat::ScreenLaser& point) {
    const QString author = QString::fromStdString(point.author);
    if (!enabled_ || point.epoch != epoch_ || author.isEmpty() || point.id <= laserIds_.value(author))
        return false;
    laserIds_[author] = point.id;
    auto& laser = lasers_[author];
    const auto now = activityClock_.elapsed();
    const QPoint position(point.x, point.y);
    if (point.visible && (laser.trail.isEmpty() || laser.trail.last().position != position || !laser.point.visible)) {
        const bool start = !laser.point.visible || laserAge(laser.seen) >= laserTrailMs;
        laser.trail.append({position, now, start});
        // Bound transient storage even if a peer submits faster than our UI.
        if (laser.trail.size() > 64)
            laser.trail.removeFirst();
    }
    laser.point = point;
    laser.seen = now;
    if (!laserTimer_.isActive())
        laserTimer_.start();
    emit updated();
    return true;
}

QList<chat::ScreenDraw> ScreenDrawing::activeAuthors() const {
    QList<chat::ScreenDraw> result;
    for (const auto& author : activeAuthors_) {
        if (activityClock_.elapsed() - author.seen < 1800)
            result.append(author.segment);
    }
    return result;
}

bool ScreenDrawing::canErase(const chat::ScreenDraw& segment) const {
    return !banned_ && (owner_ || QString::fromStdString(segment.author) == localName_);
}

QList<quint32> ScreenDrawing::strokeIds(const QString& author) const {
    QSet<quint32> ids;
    for (const auto& segment : segments_) {
        if (QString::fromStdString(segment.author) == author)
            ids.insert(segment.stroke ? segment.stroke : segment.id);
    }
    return ids.values();
}

void ScreenDrawing::remove(const QString& author, const QList<quint32>& strokes) {
    QSet<QString> keys;
    for (const auto id : strokes)
        keys.insert(author + ':' + QString::number(id));
    const bool removed = segments_.removeIf([&](const chat::ScreenDraw& segment) {
        const auto key = screenStrokeKey(segment);
        if (!keys.contains(key))
            return false;
        removed_.insert(key);
        selected_.remove(key);
        return true;
    }) != 0;
    if (!removed)
        return;
    const auto active = activeAuthors_.find(author);
    if (active != activeAuthors_.end() && keys.contains(screenStrokeKey(active->segment)))
        activeAuthors_.erase(active);
    emit updated();
}

void ScreenDrawing::requestErase(const QSet<QString>& strokes) {
    QHash<QString, QSet<quint32>> byAuthor;
    for (const auto& segment : segments_) {
        if (canErase(segment) && strokes.contains(screenStrokeKey(segment)))
            byAuthor[QString::fromStdString(segment.author)].insert(segment.stroke ? segment.stroke : segment.id);
    }
    for (auto it = byAuthor.cbegin(); it != byAuthor.cend(); ++it)
        emit eraseRequested(it.key(), it.value().values());
}

void ScreenDrawing::select(const QSet<QString>& strokes) {
    selected_ = owner_ ? strokes : QSet<QString>{};
    emit updated();
}

void ScreenDrawing::deleteSelection() {
    if (owner_ && drawingAllowed())
        requestErase(selected_);
}

void ScreenDrawing::setSelectionBox(const QRectF& box) {
    selectionBox_ = box;
    emit updated();
}

void ScreenDrawing::setBanned(bool banned) {
    if (banned_ == banned)
        return;
    banned_ = banned;
    if (banned_) {
        tool_ = Tool::None;
        selected_.clear();
        selectionBox_ = {};
    }
    emit changed();
}

void ScreenDrawing::setTool(Tool tool) {
    if (!drawingAllowed() || (tool == Tool::Select && !owner_))
        tool = Tool::None;
    if (tool_ == Tool::Laser && tool != Tool::Laser) {
        chat::ScreenLaser stop;
        stop.id = nextSegmentId();
        stop.visible = false;
        emit laserPointed(stop);
    }
    tool_ = tool;
    selected_.clear();
    selectionBox_ = {};
    emit changed();
}

void ScreenDrawing::setPenActive(bool active) {
    setTool(active ? Tool::Pen : Tool::None);
}

void ScreenDrawing::setColor(int color) {
    color_ = qBound(-1, color, chat::kPenColorCount - 1);
    emit changed();
}

void ScreenDrawing::setPresenterOverlayAvailable(bool available) {
    presenterOverlayAvailable_ = available;
    emit changed();
}

void ScreenDrawing::setStatus(const QString& status) {
    status_ = status;
    emit changed();
}

namespace {
// Line icons for the drawing toolbar, after the ones on disquisition.app.
QIcon toolIcon(const QString& kind) {
    QIcon icon;
    for (const auto& [mode, color] : {std::pair{QIcon::Normal, QColor("#dfe4e4")},
                                      std::pair{QIcon::Disabled, QColor("#5f6872")}}) {
        for (const qreal scale : {1.0, 2.0}) {
            QPixmap image(QSize(16, 16) * scale);
            image.setDevicePixelRatio(scale);
            image.fill(Qt::transparent);
            QPainter painter(&image);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(QPen(color, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            QPainterPath path;
            if (kind == "pen") {
                path.moveTo(2.8, 13.2);
                path.lineTo(3.4, 10.4);
                path.lineTo(10.6, 3.2);
                path.lineTo(12.8, 5.4);
                path.lineTo(5.6, 12.6);
                path.closeSubpath();
                path.moveTo(9.2, 4.6);
                path.lineTo(11.4, 6.8);
            } else if (kind == "eraser") {
                path.moveTo(6.2, 13.2);
                path.lineTo(2.9, 9.9);
                path.lineTo(9.4, 3.4);
                path.lineTo(13.1, 7.1);
                path.lineTo(7.0, 13.2);
                path.lineTo(13.5, 13.2);
                path.moveTo(5.4, 7.0);
                path.lineTo(9.5, 11.1);
            } else if (kind == "select") {
                path.addRoundedRect(QRectF(2.8, 2.8, 10.4, 10.4), 2.2, 2.2);
                painter.setBrush(color);
                painter.drawEllipse(QPointF(8, 8), 1.4, 1.4);
                painter.setBrush(Qt::NoBrush);
            } else if (kind == "laser") {
                path.moveTo(8, 2.2);
                path.lineTo(12.8, 4);
                path.lineTo(12.8, 7.8);
                path.cubicTo(12.8, 10.8, 10.6, 12.9, 8, 13.9);
                path.cubicTo(5.4, 12.9, 3.2, 10.8, 3.2, 7.8);
                path.lineTo(3.2, 4);
                path.closeSubpath();
                path.moveTo(6, 7.9);
                path.lineTo(7.5, 9.4);
                path.lineTo(10.2, 6.5);
            } else if (kind == "undo") {
                path.arcMoveTo(QRectF(3, 3, 10, 10), 120);
                path.arcTo(QRectF(3, 3, 10, 10), 120, 290);
                path.moveTo(5.5, 2.4);
                path.lineTo(5.6, 4.9);
                path.lineTo(8.1, 4.8);
            } else if (kind == "delete") {
                path.moveTo(3, 4.5);
                path.lineTo(13, 4.5);
                path.moveTo(6.3, 4.5);
                path.lineTo(6.6, 2.8);
                path.lineTo(9.4, 2.8);
                path.lineTo(9.7, 4.5);
                path.moveTo(4.4, 4.5);
                path.lineTo(5.2, 13.2);
                path.lineTo(10.8, 13.2);
                path.lineTo(11.6, 4.5);
            } else if (kind == "picker") {
                path.moveTo(3, 13);
                path.lineTo(4, 10);
                path.lineTo(9.2, 4.8);
                path.lineTo(11.2, 6.8);
                path.lineTo(6, 12);
                path.closeSubpath();
                path.moveTo(9.6, 3.4);
                path.cubicTo(11, 2, 12.6, 2.2, 13.2, 2.8);
                path.cubicTo(13.8, 3.4, 14, 5, 12.6, 6.4);
            }
            painter.drawPath(path);
            painter.end();
            icon.addPixmap(image, mode);
        }
    }
    return icon;
}

// A round ink color. The chosen one wears a light ring; the custom swatch is a
// color wheel until a custom color is picked.
class Swatch final : public QPushButton {
public:
    Swatch(const QString& name, QWidget* parent) : QPushButton(parent) {
        setObjectName("drawingSwatch");
        setCheckable(true);
        setFixedSize(22, 22);
        setCursor(Qt::PointingHandCursor);
        setAccessibleName(name);
        setToolTip(name);
    }
    void setColor(const QColor& color) {
        color_ = color;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QPointF center = QRectF(rect()).center();
        if (isChecked()) {
            painter.setPen(QPen(QColor("#effff8"), 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(center, 9.6, 9.6);
        }
        QBrush fill(color_);
        if (!color_.isValid()) {
            QConicalGradient wheel(center, 90);
            for (int i = 0; i <= 6; ++i)
                wheel.setColorAt(i / 6.0, QColor::fromHsvF(std::fmod(i / 6.0, 1.0), 0.65, 1.0));
            fill = QBrush(wheel);
        }
        painter.setPen(QPen(QColor(255, 255, 255, 50), 1.2));
        painter.setBrush(fill);
        const qreal radius = underMouse() && !isChecked() ? 7.4 : 6.8;
        painter.drawEllipse(center, radius, radius);
    }

private:
    QColor color_;
};

QFrame* separator(QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setObjectName("drawingSeparator");
    line->setFixedSize(1, 20);
    return line;
}

const char* kControlsStyle = R"(
QWidget#drawingControls[floating="true"] { background: rgba(22, 25, 31, 238); border: 1px solid #5f6974; border-radius: 12px; }
QPushButton { color: #dfe4e4; background: transparent; border: 1px solid transparent; border-radius: 8px;
    padding: 4px 9px; font-family: Inter, sans-serif; font-size: 12px; font-weight: 600; }
QPushButton:hover { background: rgba(255, 255, 255, 18); }
QPushButton:checked { color: #effff8; background: #2f5549; border-color: #67e8a5; }
QPushButton:disabled { color: #5f6872; }
QPushButton#drawingClear, QPushButton#drawingClearMine { color: #ff8a8a; font-weight: 500; }
QPushButton#drawingClear:disabled, QPushButton#drawingClearMine:disabled { color: #6b4b4f; }
QPushButton#drawingUndo, QPushButton#drawingPickColor { padding: 4px 6px; }
QLineEdit#drawingHexColor { background: #1f2228; border: 1px solid #3a3f48; border-radius: 6px; padding: 2px 6px;
    color: #e6eae9; font-family: "JetBrains Mono", monospace; font-size: 11px;
    selection-background-color: #9dccca; selection-color: #12171a; }
QLineEdit#drawingHexColor:focus { border-color: #9dccca; }
QFrame#drawingSeparator { background: #3a424c; border: 0; }
QLabel { color: #8b959e; background: transparent; font-family: Inter, sans-serif; font-size: 11px; padding: 0 4px; }
)";
} // namespace

DrawingControls::DrawingControls(ScreenDrawing* drawing, bool owner, QWidget* parent) :
    QWidget(parent) {
    setObjectName("drawingControls");
    setAttribute(Qt::WA_StyledBackground);
    setStyleSheet(kControlsStyle);
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(5, 5, 5, 5);
    row->setSpacing(2);
    const auto tool = [this, row](const QString& text, const QString& icon, const char* name) {
        auto* result = new QPushButton(text, this);
        result->setObjectName(name);
        if (!icon.isEmpty())
            result->setIcon(toolIcon(icon));
        result->setIconSize(QSize(14, 14));
        result->setCursor(Qt::PointingHandCursor);
        row->addWidget(result);
        return result;
    };
    auto* allow = tool("allow drawing", {}, "drawingAllow");
    allow->setCheckable(true);
    allow->setVisible(owner);
    allow->setToolTip("Let viewers draw on your shared screen. Turning this off clears all drawings.");
    auto* toolsLine = separator(this);
    row->addWidget(toolsLine);
    auto* pen = tool("pen", "pen", "drawingPen");
    pen->setCheckable(true);
    pen->setToolTip("Draw on the shared screen. Press Esc to return to using your mouse.");
    auto* eraser = tool("eraser", "eraser", "drawingEraser");
    eraser->setCheckable(true);
    eraser->setToolTip(owner ? "Drag over a stroke to erase it." : "Drag over one of your strokes to erase it.");
    auto* select = tool("select", "select", "drawingSelect");
    select->setCheckable(true);
    select->setToolTip("Click a stroke or drag a box, then press Delete to remove selected strokes.");
    auto* colorsLine = separator(this);
    row->addWidget(colorsLine);
    // Swatch 0 follows the chat color, 1-6 are the presets and the last is custom.
    QList<Swatch*> swatches;
    const QStringList names{"chat color", "red", "yellow", "green", "blue", "purple", "white",
                            "custom color"};
    for (int i = 0; i < names.size(); ++i) {
        auto* swatch = new Swatch(names[i], this);
        swatch->setObjectName(QString("drawingSwatch%1").arg(i));
        if (i > 0 && i <= chat::kPenColorCount)
            swatch->setColor(screenPenColor(i - 1));
        row->addWidget(swatch);
        swatches.append(swatch);
    }
    auto* hex = new QLineEdit(this);
    hex->setObjectName("drawingHexColor");
    hex->setAccessibleName("Hex pen color");
    hex->setPlaceholderText("#RRGGBB");
    hex->setMaxLength(7);
    hex->setFixedWidth(74);
    hex->setToolTip("Enter a color as #RRGGBB.");
    row->addWidget(hex);
    auto* pick = tool({}, "picker", "drawingPickColor");
    pick->setAccessibleName("Pick color");
    pick->setToolTip("Open the color picker.");
    auto* actionsLine = separator(this);
    row->addWidget(actionsLine);
    auto* laser = tool("laser", "laser", "drawingLaser");
    laser->setCheckable(true);
    laser->setToolTip("Move your mouse to point with a fading trail. No click needed. Press Esc to exit.");
    auto* undo = tool({}, "undo", "drawingUndo");
    undo->setAccessibleName("undo");
    undo->setToolTip(QString("Undo your last stroke (%1).")
                     .arg(QKeySequence(QKeySequence::Undo).toString(QKeySequence::NativeText)));
    auto* undoShortcut = new QShortcut(QKeySequence::Undo, this);
    undoShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(undoShortcut, &QShortcut::activated, drawing, &ScreenDrawing::undo);
    connect(undo, &QPushButton::clicked, drawing, &ScreenDrawing::undo);
    auto* deleteSelected = tool({}, "delete", "drawingDeleteSelected");
    deleteSelected->setAccessibleName("delete selected");
    deleteSelected->setToolTip("Delete the selected strokes.");
    auto* clearLine = separator(this);
    row->addWidget(clearLine);
    auto* clearMine = tool("clear mine", {}, "drawingClearMine");
    clearMine->setToolTip("Remove all of your drawings from this shared screen.");
    auto* clear = tool("clear all", {}, "drawingClear");
    clear->setToolTip("Remove everyone's drawings from your shared screen.");
    auto* hint = new QLabel(this);
    hint->setObjectName("drawingHint");
    row->addWidget(hint);

    connect(allow, &QPushButton::toggled, drawing, &ScreenDrawing::permissionRequested);
    const auto toggleTool = [drawing](ScreenDrawing::Tool tool, bool checked) {
        if (checked)
            drawing->setTool(tool);
        else if (drawing->tool() == tool)
            drawing->setTool(ScreenDrawing::Tool::None);
    };
    connect(pen, &QPushButton::toggled, drawing, [=](bool checked) { toggleTool(ScreenDrawing::Tool::Pen, checked); });
    connect(laser, &QPushButton::toggled, drawing, [=](bool checked) { toggleTool(ScreenDrawing::Tool::Laser, checked); });
    connect(eraser, &QPushButton::toggled, drawing, [=](bool checked) { toggleTool(ScreenDrawing::Tool::Eraser, checked); });
    connect(select, &QPushButton::toggled, drawing, [=](bool checked) { toggleTool(ScreenDrawing::Tool::Select, checked); });
    for (int i = 0; i < swatches.size(); ++i) {
        connect(swatches[i], &QPushButton::clicked, drawing, [=] {
            if (i == chat::kPenColorCount + 1)
                drawing->setCustomColor(drawing->customColor().isValid() ? drawing->customColor()
                                                                         : drawing->penColor());
            else
                drawing->setColor(i - 1);
        });
    }
    connect(pick, &QPushButton::clicked, this, [=, this] {
        // Cancel leaves the previous color intact and the nonmodal tool stays above the share.
        if (!colorPicker_) {
            colorPicker_ = new QColorDialog(this);
            colorPicker_->setObjectName("screenDrawingColorPicker");
            colorPicker_->setWindowTitle("Pen color");
            colorPicker_->setOption(QColorDialog::DontUseNativeDialog);
            QPalette palette = colorPicker_->palette();
            palette.setColor(QPalette::Window, QColor("#24272e"));
            palette.setColor(QPalette::WindowText, QColor("#e6eae9"));
            palette.setColor(QPalette::Base, QColor("#1f2228"));
            palette.setColor(QPalette::Text, QColor("#e6eae9"));
            palette.setColor(QPalette::Button, QColor("#2c3038"));
            palette.setColor(QPalette::ButtonText, QColor("#e6eae9"));
            colorPicker_->setPalette(palette);
            colorPicker_->setWindowFlag(Qt::Tool);
            colorPicker_->setWindowFlag(Qt::WindowStaysOnTopHint,
                                        window()->windowFlags().testFlag(Qt::WindowStaysOnTopHint));
            colorPicker_->setAttribute(Qt::WA_DeleteOnClose);
            connect(colorPicker_, &QColorDialog::colorSelected, drawing, &ScreenDrawing::setCustomColor);
        }
        colorPicker_->setCurrentColor(drawing->penColor());
        colorPicker_->show();
        colorPicker_->raise();
    });
    const auto applyHex = [drawing](QString text) {
        text = text.trimmed();
        if (text.startsWith('#')) text.remove(0, 1);
        const QColor color('#' + text);
        if (text.size() == 6 && color.isValid())
            drawing->setCustomColor(color);
    };
    connect(hex, &QLineEdit::textEdited, drawing, applyHex);
    const auto refreshColors = [=](bool force) {
        swatches.first()->setColor(drawing->chatColor());
        swatches.last()->setColor(drawing->customColor());
        const int current = drawing->color() + 1;
        for (int i = 0; i < swatches.size(); ++i) {
            const QSignalBlocker block(swatches[i]);
            swatches[i]->setChecked(i == current);
        }
        if (force || !hex->hasFocus()) {
            const QColor custom = drawing->customColor().isValid() ? drawing->customColor()
                                                                   : drawing->penColor();
            hex->setText(custom.name(QColor::HexRgb).toUpper());
        }
    };
    connect(hex, &QLineEdit::editingFinished, this, [=] {
        applyHex(hex->text());
        refreshColors(true);
    });
    connect(clear, &QPushButton::clicked, drawing, &ScreenDrawing::clearRequested);
    connect(clearMine, &QPushButton::clicked, drawing, &ScreenDrawing::clearMineRequested);
    connect(deleteSelected, &QPushButton::clicked, drawing, &ScreenDrawing::deleteSelection);
    auto refresh = [=] {
        const QSignalBlocker blockAllow(allow), blockPen(pen), blockLaser(laser), blockEraser(eraser),
                             blockSelect(select);
        const bool allowed = drawing->drawingAllowed();
        const bool custom = drawing->color() == chat::kPenColorCount;
        allow->setChecked(drawing->enabled());
        pen->setChecked(drawing->tool() == ScreenDrawing::Tool::Pen);
        laser->setChecked(drawing->tool() == ScreenDrawing::Tool::Laser);
        eraser->setChecked(drawing->tool() == ScreenDrawing::Tool::Eraser);
        select->setChecked(drawing->tool() == ScreenDrawing::Tool::Select);
        refreshColors(false);
        pen->setVisible(allowed);
        laser->setVisible(allowed);
        eraser->setVisible(allowed);
        select->setVisible(owner && allowed);
        for (auto* swatch : swatches)
            swatch->setVisible(allowed);
        hex->setVisible(allowed && custom);
        pick->setVisible(allowed && custom);
        undo->setVisible(drawing->enabled());
        undo->setEnabled(drawing->canUndo());
        undoShortcut->setEnabled(drawing->canUndo());
        deleteSelected->setVisible(owner && drawing->tool() == ScreenDrawing::Tool::Select);
        deleteSelected->setEnabled(!drawing->selectedStrokes().isEmpty());
        deleteSelected->setText(drawing->selectedStrokes().isEmpty()
                                    ? QString() : QString::number(drawing->selectedStrokes().size()));
        clearMine->setVisible(!owner && drawing->enabled());
        clearMine->setEnabled(allowed && !drawing->strokeIds(drawing->localName()).isEmpty());
        clear->setVisible(owner);
        clear->setEnabled(drawing->enabled() && !drawing->segments().isEmpty());
        // A divider sits between each pair of visible groups.
        const bool groups[]{owner, allowed, allowed, allowed || drawing->enabled(),
                            owner || drawing->enabled()};
        QFrame* lines[]{nullptr, toolsLine, colorsLine, actionsLine, clearLine};
        bool earlier = false;
        for (int i = 0; i < 5; ++i) {
            if (lines[i])
                lines[i]->setVisible(groups[i] && earlier);
            earlier |= groups[i];
        }
        const bool previewOnly = owner && !drawing->presenterOverlayAvailable();
        const QString toolHint = drawing->tool() == ScreenDrawing::Tool::Select ? "click or drag a box; Delete removes; Esc exits" :
                                 drawing->tool() == ScreenDrawing::Tool::Eraser ? (owner ? "erase strokes; Esc exits" : "erase your strokes; Esc exits") : QString();
        hint->setText(!drawing->status().isEmpty() ? drawing->status() :
                      drawing->banned() ? "you are banned from drawing on this stream" :
                      !drawing->enabled() ? "drawing is off" :
                      previewOnly ? "draw in the video preview" : toolHint);
        hint->setVisible(!hint->text().isEmpty());
    };
    connect(drawing, &ScreenDrawing::changed, this, refresh);
    connect(drawing, &ScreenDrawing::updated, this, refresh);
    refresh();
}

void DrawingControls::setFloating(bool floating) {
    setProperty("floating", floating);
    style()->unpolish(this);
    style()->polish(this);
}

namespace {
QPointF drawingPoint(const QRectF& area, int x, int y) {
    return area.topLeft() + QPointF(x * area.width() / chat::kDrawCoordinateMax,
                                   y * area.height() / chat::kDrawCoordinateMax);
}

void paintLaserTrail(QPainter& painter, const QRectF& area,
                     const ScreenDrawing::LaserPointer& laser, const ScreenDrawing& drawing) {
    struct Section { QPainterPath path; QPointF a, b; QColor from, to; };
    QList<Section> sections;
    QRectF bounds;
    const auto position = [&](qsizetype index) {
        const auto point = laser.trail[index].position;
        return drawingPoint(area, point.x(), point.y());
    };
    const auto color = [&](qsizetype index) {
        QColor result = QColor::fromRgb(static_cast<QRgb>(laser.point.rgb));
        result.setAlphaF(qBound(0.0, 1.0 - double(drawing.laserAge(laser.trail[index].seen)) /
                                         ScreenDrawing::laserTrailMs, 1.0));
        return result;
    };
    for (qsizetype i = 1; i < laser.trail.size(); ++i) {
        if (laser.trail[i].start)
            continue;
        const QPointF a = position(i - 1), b = position(i);
        const QPointF before = i > 1 && !laser.trail[i - 1].start ? position(i - 2) : a;
        const QPointF after = i + 1 < laser.trail.size() && !laser.trail[i + 1].start ? position(i + 1) : b;
        QPainterPath path(a);
        path.cubicTo(a + (b - before) / 6, b - (after - a) / 6, b);
        bounds = bounds.united(path.controlPointRect().adjusted(-3, -3, 3, 3));
        sections.append({path, a, b, color(i - 1), color(i)});
    }
    bounds = bounds.intersected(area);
    if (bounds.isEmpty())
        return;
    const qreal scale = painter.device()->devicePixelRatioF();
    QImage layer(QSize(qCeil(bounds.width() * scale), qCeil(bounds.height() * scale)),
                 QImage::Format_ARGB32_Premultiplied);
    layer.setDevicePixelRatio(scale);
    layer.fill(Qt::transparent);
    QPainter trail(&layer);
    trail.setRenderHint(QPainter::Antialiasing);
    trail.translate(-bounds.topLeft());
    // Replace overlapping ink in this isolated layer instead of accumulating
    // alpha at every sample. Composite the finished trail onto the video once.
    trail.setCompositionMode(QPainter::CompositionMode_Source);
    for (const auto& section : sections) {
        QLinearGradient fade(section.a, section.b);
        fade.setColorAt(0, section.from);
        fade.setColorAt(1, section.to);
        trail.setPen(QPen(QBrush(fade), 3.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        trail.drawPath(section.path);
    }
    trail.end();
    painter.drawImage(bounds.topLeft(), layer);
}

qreal pointLineDistance(const QPointF& point, const QLineF& line) {
    const QPointF vector = line.p2() - line.p1();
    const qreal length = QPointF::dotProduct(vector, vector);
    const qreal t = length > 0 ? qBound(0.0, QPointF::dotProduct(point - line.p1(), vector) / length, 1.0) : 0;
    return QLineF(point, line.p1() + t * vector).length();
}

qreal lineDistance(const QLineF& a, const QLineF& b) {
    if (a.intersects(b) == QLineF::BoundedIntersection)
        return 0;
    return qMin(qMin(pointLineDistance(a.p1(), b), pointLineDistance(a.p2(), b)),
                qMin(pointLineDistance(b.p1(), a), pointLineDistance(b.p2(), a)));
}

bool lineInBox(const QLineF& line, const QRectF& box) {
    if (box.contains(line.p1()) || box.contains(line.p2()))
        return true;
    for (const QLineF edge : {QLineF(box.topLeft(), box.topRight()), QLineF(box.topRight(), box.bottomRight()),
                              QLineF(box.bottomRight(), box.bottomLeft()), QLineF(box.bottomLeft(), box.topLeft())}) {
        if (line.intersects(edge) == QLineF::BoundedIntersection)
            return true;
    }
    return false;
}
} // namespace

DrawingCanvas::DrawingCanvas(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    laserTimer_.setInterval(33);
    connect(&laserTimer_, &QTimer::timeout, this, &DrawingCanvas::sendLaser);
}

void DrawingCanvas::setDrawing(ScreenDrawing* drawing) {
    if (drawing_)
        disconnect(drawing_, nullptr, this, nullptr);
    drawing_ = drawing;
    if (drawing_) {
        connect(drawing_, &ScreenDrawing::updated, this, qOverload<>(&QWidget::update));
        connect(drawing_, &ScreenDrawing::changed, this, [this] {
            previous_.reset();
            gestureStart_.reset();
            eraserPrevious_.reset();
            if (drawing_->tool() != ScreenDrawing::Tool::Laser) {
                laserTimer_.stop();
                laserPosition_.reset();
                lastLaserSent_.reset();
                laserSendClock_.invalidate();
            }
            setCursor(drawing_->tool() == ScreenDrawing::Tool::Laser ? Qt::BlankCursor :
                      drawing_->penActive() ? Qt::CrossCursor : Qt::ArrowCursor);
            update();
        });
        setCursor(drawing_->tool() == ScreenDrawing::Tool::Laser ? Qt::BlankCursor :
                  drawing_->penActive() ? Qt::CrossCursor : Qt::ArrowCursor);
    }
    previous_.reset();
    gestureStart_.reset();
    eraserPrevious_.reset();
    laserTimer_.stop();
    laserPosition_.reset();
    lastLaserSent_.reset();
    laserSendClock_.invalidate();
    update();
}

void DrawingCanvas::paintDrawing(QPainter& painter) {
    if (!drawing_ || !drawing_->enabled())
        return;
    const QRectF area = drawingRect();
    if (area.isEmpty())
        return;
    painter.save();
    painter.setClipRect(area);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal width = qMax(2.0, area.width() * 0.003);
    for (const auto& segment : drawing_->segments()) {
        const QPointF a = drawingPoint(area, segment.x1, segment.y1);
        const QPointF b = drawingPoint(area, segment.x2, segment.y2);
        if (drawing_->selectedStrokes().contains(screenStrokeKey(segment))) {
            painter.setPen(QPen(QColor("#2682ff"), width + 5, Qt::SolidLine, Qt::RoundCap));
            if (a == b) painter.drawPoint(a); else painter.drawLine(a, b);
        }
        painter.setPen(QPen(screenSegmentColor(segment), width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (a == b) painter.drawPoint(a); else painter.drawLine(a, b);
    }
    if (!drawing_->selectionBox().isNull()) {
        const auto box = drawing_->selectionBox();
        const QRectF selection(drawingPoint(area, qRound(box.left()), qRound(box.top())),
                               drawingPoint(area, qRound(box.right()), qRound(box.bottom())));
        painter.setPen(QPen(QColor("#62cfff"), 1, Qt::DashLine));
        painter.setBrush(QColor(98, 207, 255, 35));
        painter.drawRect(selection);
        painter.setBrush(Qt::NoBrush);
    }
    QFont labelFont = font();
    labelFont.setPixelSize(12);
    painter.setFont(labelFont);
    struct Label { QPointF position; QColor color; };
    QHash<QString, Label> labels;
    for (const auto& segment : drawing_->segments()) {
        if (drawing_->selectedStrokes().contains(screenStrokeKey(segment)))
            labels[QString::fromStdString(segment.author)] = {drawingPoint(area, segment.x2, segment.y2), screenSegmentColor(segment)};
    }
    for (const auto& author : drawing_->activeAuthors())
        labels[QString::fromStdString(author.author)] = {drawingPoint(area, author.x2, author.y2), screenSegmentColor(author)};
    for (auto it = drawing_->lasers().cbegin(); it != drawing_->lasers().cend(); ++it) {
        const auto& laser = it.value();
        const QColor color = QColor::fromRgb(static_cast<QRgb>(laser.point.rgb));
        paintLaserTrail(painter, area, laser, *drawing_);
        if (laser.point.visible && drawing_->laserAge(laser.seen) < ScreenDrawing::laserTimeoutMs) {
            const QPointF point = drawingPoint(area, laser.point.x, laser.point.y);
            painter.setPen(QPen(color, 2));
            painter.setBrush(color);
            painter.drawEllipse(point, 5, 5);
            painter.setPen(Qt::NoPen);
            painter.setBrush(Qt::white);
            painter.drawEllipse(point, 2, 2);
            painter.setBrush(Qt::NoBrush);
            labels[it.key()] = {point, color};
        }
    }
    for (auto it = labels.cbegin(); it != labels.cend(); ++it) {
        const auto& labelData = it.value();
        const QString name = painter.fontMetrics().elidedText(drawing_->authorName(it.key()), Qt::ElideRight,
                                                              qMax(20, qRound(qMin(180.0, area.width() - 12))));
        const QPointF endpoint = labelData.position;
        const qreal labelWidth = qMin(area.width(), painter.fontMetrics().horizontalAdvance(name) + 14.0);
        const qreal labelHeight = painter.fontMetrics().height() + 8;
        const qreal left = qBound(area.left(), endpoint.x() - labelWidth / 2, area.right() - labelWidth);
        const qreal top = endpoint.y() - labelHeight - 10 >= area.top() ? endpoint.y() - labelHeight - 10 : endpoint.y() + 10;
        const QRectF label(left, qBound(area.top(), top, qMax(area.top(), area.bottom() - labelHeight)), labelWidth, labelHeight);
        painter.setPen(QPen(labelData.color, 1));
        painter.setBrush(QColor("#171b22"));
        painter.drawRoundedRect(label, 4, 4);
        painter.setPen(QColor("#f3f4f6"));
        painter.drawText(label, Qt::AlignCenter, name);
        painter.setBrush(Qt::NoBrush);
    }
    painter.restore();
}

void DrawingCanvas::renderDrawing(QPainter& painter) {
    if (drawing_ && drawing_->penActive())
        painter.fillRect(rect(), QColor(0, 0, 0, 1));
    paintDrawing(painter);
    if (drawing_ && drawing_->penActive()) {
        painter.setPen(QPen(drawing_->penColor(), 2));
        painter.drawRect(rect().adjusted(1, 1, -1, -1));
    }
}

QPointF DrawingCanvas::normalized(const QPointF& position) const {
    const auto area = drawingRect();
    return QPointF(qBound(0.0, (position.x() - area.left()) * chat::kDrawCoordinateMax / area.width(), double(chat::kDrawCoordinateMax)),
                   qBound(0.0, (position.y() - area.top()) * chat::kDrawCoordinateMax / area.height(), double(chat::kDrawCoordinateMax)));
}

void DrawingCanvas::drawTo(const QPointF& position, bool first) {
    if (!drawing_ || !drawing_->drawingAllowed() || drawing_->tool() != ScreenDrawing::Tool::Pen)
        return;
    const QRectF area = drawingRect();
    if (area.isEmpty() || !area.contains(position)) {
        previous_.reset();
        return;
    }
    const QPoint point = normalized(position).toPoint();
    if (!first && previous_ == point)
        return;
    const QPoint start = first || !previous_ ? point : *previous_;
    previous_ = point;
    chat::ScreenDraw segment;
    segment.epoch = drawing_->epoch();
    segment.id = drawing_->nextSegmentId();
    if (first || !stroke_)
        stroke_ = segment.id;
    segment.stroke = stroke_;
    segment.x1 = start.x(); segment.y1 = start.y();
    segment.x2 = point.x(); segment.y2 = point.y();
    segment.color = qBound(0, drawing_->color(), chat::kPenColorCount - 1);
    segment.rgb = static_cast<int>(drawing_->penColor().rgb() & 0xffffff);
    emit drawing_->segmentDrawn(segment);
}

void DrawingCanvas::pointLaser(const QPointF& position) {
    if (!drawing_ || !drawing_->drawingAllowed() || drawing_->tool() != ScreenDrawing::Tool::Laser)
        return;
    if (drawingRect().isEmpty() || !drawingRect().contains(position)) {
        setCursor(Qt::ArrowCursor);
        stopLaser();
        return;
    }
    if (cursor().shape() != Qt::BlankCursor)
        setCursor(Qt::BlankCursor);
    laserPosition_ = normalized(position).toPoint();
    sendLaser();
    laserTimer_.start();
}

void DrawingCanvas::sendLaser() {
    if (!drawing_ || drawing_->tool() != ScreenDrawing::Tool::Laser || !laserPosition_)
        return;
    // About 30 updates/sec while moving; a quiet heartbeat keeps a stationary
    // pointer visible, while a disconnected sender expires automatically.
    if (laserSendClock_.isValid() && laserSendClock_.elapsed() <
        (lastLaserSent_ == laserPosition_ ? 250 : 33))
        return;
    chat::ScreenLaser point;
    point.id = drawing_->nextSegmentId();
    point.x = laserPosition_->x();
    point.y = laserPosition_->y();
    point.rgb = static_cast<int>(drawing_->penColor().rgb() & 0xffffff);
    laserSendClock_.start();
    lastLaserSent_ = laserPosition_;
    emit drawing_->laserPointed(point);
}

void DrawingCanvas::stopLaser() {
    laserTimer_.stop();
    if (drawing_ && laserPosition_) {
        chat::ScreenLaser stop;
        stop.id = drawing_->nextSegmentId();
        stop.visible = false;
        emit drawing_->laserPointed(stop);
    }
    laserPosition_.reset();
    lastLaserSent_.reset();
    laserSendClock_.invalidate();
}

void DrawingCanvas::leaveEvent(QEvent* event) {
    stopLaser();
    QWidget::leaveEvent(event);
}

void DrawingCanvas::eraseTo(const QPointF& position) {
    if (!drawing_ || drawingRect().isEmpty())
        return;
    const auto area = drawingRect();
    // Clip the scrub to the video so letterboxing never erases hidden content.
    if (!area.contains(position)) {
        eraserPrevious_.reset();
        return;
    }
    const QLineF sweep(eraserPrevious_.value_or(position), position);
    eraserPrevious_ = position;
    QSet<QString> strokes;
    const qreal radius = 9 + qMax(2.0, area.width() * 0.003) / 2;
    for (const auto& segment : drawing_->segments()) {
        const QLineF line(drawingPoint(area, segment.x1, segment.y1), drawingPoint(area, segment.x2, segment.y2));
        if (drawing_->canErase(segment) && lineDistance(sweep, line) <= radius)
            strokes.insert(screenStrokeKey(segment));
    }
    drawing_->requestErase(strokes);
}

void DrawingCanvas::selectTo(const QPointF& position, bool finished) {
    if (!drawing_ || !gestureStart_ || drawingRect().isEmpty())
        return;
    const QPointF start = *gestureStart_;
    const QRectF box = QRectF(start, position).normalized().intersected(drawingRect());
    const bool click = QLineF(start, position).length() < 4;
    drawing_->setSelectionBox(finished || click ? QRectF() : QRectF(normalized(box.topLeft()), normalized(box.bottomRight())));
    if (!finished)
        return;
    QSet<QString> strokes;
    for (auto it = drawing_->segments().crbegin(); it != drawing_->segments().crend(); ++it) {
        const QLineF line(drawingPoint(drawingRect(), it->x1, it->y1), drawingPoint(drawingRect(), it->x2, it->y2));
        if ((click && pointLineDistance(position, line) <= 8) || (!click && lineInBox(line, box))) {
            strokes.insert(screenStrokeKey(*it));
            if (click)
                break;
        }
    }
    drawing_->select(strokes);
}

void DrawingCanvas::mousePressEvent(QMouseEvent* event) {
    if (drawing_ && drawing_->tool() == ScreenDrawing::Tool::Laser) {
        pointLaser(event->position());
        return;
    }
    if (event->button() != Qt::LeftButton || !drawing_ || !drawing_->penActive() ||
        drawingRect().isEmpty() || !drawingRect().contains(event->position()))
        return;
    previous_.reset();
    stroke_ = 0;
    gestureStart_ = event->position();
    eraserPrevious_.reset();
    if (drawing_->tool() == ScreenDrawing::Tool::Pen)
        drawTo(event->position(), true);
    else if (drawing_->tool() == ScreenDrawing::Tool::Eraser)
        eraseTo(event->position());
    else
        drawing_->select({});
    penClock_.start();
}

void DrawingCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (drawing_ && drawing_->tool() == ScreenDrawing::Tool::Laser) {
        pointLaser(event->position());
        return;
    }
    if (!(event->buttons() & Qt::LeftButton) || !drawing_ || !gestureStart_)
        return;
    if (penClock_.isValid() && penClock_.elapsed() < 16)
        return;
    if (drawing_->tool() == ScreenDrawing::Tool::Pen)
        drawTo(event->position());
    else if (drawing_->tool() == ScreenDrawing::Tool::Eraser)
        eraseTo(event->position());
    else if (drawing_->tool() == ScreenDrawing::Tool::Select)
        selectTo(event->position(), false);
    penClock_.restart();
}

void DrawingCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (drawing_ && drawing_->tool() == ScreenDrawing::Tool::Laser) {
        pointLaser(event->position());
        return;
    }
    if (event->button() != Qt::LeftButton || !drawing_ || !gestureStart_)
        return;
    if (drawing_->tool() == ScreenDrawing::Tool::Pen)
        drawTo(event->position());
    else if (drawing_->tool() == ScreenDrawing::Tool::Eraser)
        eraseTo(event->position());
    else if (drawing_->tool() == ScreenDrawing::Tool::Select)
        selectTo(event->position(), true);
    previous_.reset();
    gestureStart_.reset();
    eraserPrevious_.reset();
}

void DrawingCanvas::keyPressEvent(QKeyEvent* event) {
    if (drawing_ && drawing_->drawingAllowed() && event->matches(QKeySequence::Undo)) {
        drawing_->undo();
        event->accept();
    } else if (drawing_ && drawing_->tool() == ScreenDrawing::Tool::Select &&
        (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)) {
        drawing_->deleteSelection();
        event->accept();
    } else if (drawing_ && drawing_->penActive() && event->key() == Qt::Key_Escape) {
        drawing_->setPenActive(false);
        event->accept();
    } else {
        QWidget::keyPressEvent(event);
    }
}

PresenterOverlay::PresenterOverlay(const ShareSource& source, ScreenDrawing* drawing,
                                   QWidget* parent) : DrawingCanvas(parent), source_(source) {
    setObjectName("screenDrawingOverlay");
    setWindowTitle("Screen drawing overlay");
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
#if !defined(Q_OS_MACOS)
    setWindowFlag(Qt::WindowTransparentForInput);
#endif
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_MacAlwaysShowToolWindow);
    setDrawing(drawing);
    drawing->setPresenterOverlayAvailable(shareSourceSupportsOverlay(source));
    tools_ = new QWidget(this, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    tools_->setObjectName("screenDrawingToolbar");
    tools_->setWindowTitle("Screen drawing");
    tools_->setAttribute(Qt::WA_MacAlwaysShowToolWindow);
    tools_->setAttribute(Qt::WA_ShowWithoutActivating);
    qApp->installEventFilter(this);
    auto* layout = new QHBoxLayout(tools_);
    layout->setContentsMargins(3, 1, 3, 1);
    layout->addWidget(new DrawingControls(drawing, true, tools_));
    tools_->setStyleSheet("QWidget#screenDrawingToolbar { background: #16191f; border: 1px solid "
                          "#5f6974; border-radius: 12px; }");
#if defined(Q_OS_MACOS)
    if (QGuiApplication::platformName() == "cocoa") {
        canvasPanel_ = new MacDrawingPanel(this, drawing);
        connect(drawing, &ScreenDrawing::updated, canvasPanel_, &MacDrawingPanel::update);
        connect(drawing, &ScreenDrawing::changed, canvasPanel_, &MacDrawingPanel::update);
        toolsPanel_ = new MacDrawingPanel(tools_);
    }
#endif
    connect(drawing, &ScreenDrawing::changed, this, &PresenterOverlay::refresh);
    positionTimer_.setInterval(100);
    connect(&positionTimer_, &QTimer::timeout, this, &PresenterOverlay::refresh);
    positionTimer_.start();
    refresh();
}

PresenterOverlay::~PresenterOverlay() {
    delete tools_;
}

void PresenterOverlay::refresh() {
    const QRect bounds = shareSourceSupportsOverlay(source_) ? shareSourceGeometry(source_)
                                                             : QRect();
    if (!drawing() || !drawing()->enabled() || bounds.isEmpty()) {
#if defined(Q_OS_MACOS)
        if (canvasPanel_) {
            canvasPanel_->hide();
            toolsPanel_->hide();
            return;
        }
#endif
        hide();
        tools_->hide();
        return;
    }
    const bool transparent = !drawing()->penActive();
    tools_->adjustSize();
    const QRect toolsBounds(bounds.center().x() - tools_->width() / 2, bounds.top() + 12,
                            tools_->width(), tools_->height());
#if defined(Q_OS_MACOS)
    if (canvasPanel_) {
        canvasPanel_->show(bounds, transparent);
        toolsPanel_->show(toolsBounds);
        return;
    }
#endif
    if (geometry() != bounds)
        setGeometry(bounds);
    if (windowFlags().testFlag(Qt::WindowTransparentForInput) != transparent) {
        setWindowFlag(Qt::WindowTransparentForInput, transparent);
    }
    if (!isVisible())
        show();
    tools_->move(toolsBounds.topLeft());
    if (!tools_->isVisible())
        tools_->show();
#if defined(Q_OS_WIN)
    SetWindowDisplayAffinity(reinterpret_cast<HWND>(winId()),
                             0x00000011 /* WDA_EXCLUDEFROMCAPTURE */);
    SetWindowDisplayAffinity(reinterpret_cast<HWND>(tools_->winId()), 0x00000011);
#endif
}

bool PresenterOverlay::eventFilter(QObject* watched, QEvent* event) {
    // Native nonactivating panels receive keys while another app stays active.
    const auto* widget = qobject_cast<QWidget*>(watched);
    if (widget && (widget->window() == this || widget->window() == tools_) &&
        drawing() && drawing()->enabled() &&
        (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
        const auto* keyEvent = static_cast<QKeyEvent*>(event);
        const int key = keyEvent->key();
        const bool editing = qobject_cast<const QLineEdit*>(widget);
        const bool undo = !editing && drawing()->drawingAllowed() && keyEvent->matches(QKeySequence::Undo);
        const bool erase = !editing && drawing()->tool() == ScreenDrawing::Tool::Select &&
                           (key == Qt::Key_Delete || key == Qt::Key_Backspace);
        if ((key == Qt::Key_Escape && drawing()->penActive()) || erase || undo) {
            if (event->type() == QEvent::KeyPress) {
                if (undo) drawing()->undo();
                else if (erase) drawing()->deleteSelection();
                else drawing()->setPenActive(false);
            }
            event->accept();
            return true;
        }
    }
    return DrawingCanvas::eventFilter(watched, event);
}

void PresenterOverlay::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    renderDrawing(painter);
}
