#include "desktop/screen_drawing.h"

#include <QApplication>
#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>

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
    return color_ < 0 ? chatColor_ : screenPenColor(color_);
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

QList<chat::ScreenDraw> ScreenDrawing::activeAuthors() const {
    QList<chat::ScreenDraw> result;
    for (const auto& author : activeAuthors_) {
        if (activityClock_.elapsed() - author.seen < 1800)
            result.append(author.segment);
    }
    return result;
}

bool ScreenDrawing::canErase(const chat::ScreenDraw& segment) const {
    return owner_ || QString::fromStdString(segment.author) == localName_;
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
    if (owner_ && enabled_)
        requestErase(selected_);
}

void ScreenDrawing::setSelectionBox(const QRectF& box) {
    selectionBox_ = box;
    emit updated();
}

void ScreenDrawing::setTool(Tool tool) {
    if (!enabled_ || (tool == Tool::Select && !owner_))
        tool = Tool::None;
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

DrawingControls::DrawingControls(ScreenDrawing* drawing, bool owner, QWidget* parent) :
    QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    auto* row = new QHBoxLayout;
    row->setSpacing(6);
    layout->addLayout(row);
    auto* allow = new QPushButton("allow drawing", this);
    allow->setCheckable(true);
    allow->setVisible(owner);
    allow->setToolTip("Let viewers draw on your shared screen. Turning this off clears all drawings.");
    row->addWidget(allow);
    auto* pen = new QPushButton("pen", this);
    pen->setCheckable(true);
    pen->setToolTip("Draw on the shared screen. Press Esc to return to using your mouse.");
    row->addWidget(pen);
    auto* eraser = new QPushButton("eraser", this);
    eraser->setCheckable(true);
    eraser->setToolTip(owner ? "Drag over a stroke to erase it." : "Drag over one of your strokes to erase it.");
    row->addWidget(eraser);
    auto* select = new QPushButton("select", this);
    select->setCheckable(true);
    select->setToolTip("Click a stroke or drag a box, then press Delete to remove selected strokes.");
    row->addWidget(select);
    auto* colors = new QComboBox(this);
    colors->setAccessibleName("Pen color");
    colors->setToolTip("Use your chat color, or choose a color for new strokes.");
    colors->addItem("chat color");
    const QStringList names{"red", "yellow", "green", "blue", "purple", "white"};
    for (int i = 0; i < names.size(); ++i) {
        QPixmap swatch(12, 12);
        swatch.fill(screenPenColor(i));
        colors->addItem(QIcon(swatch), names[i]);
    }
    row->addWidget(colors);
    row->addStretch();
    auto* actions = new QHBoxLayout;
    actions->setSpacing(6);
    layout->addLayout(actions);
    auto* clearMine = new QPushButton("clear mine", this);
    clearMine->setToolTip("Remove all of your drawings from this shared screen.");
    actions->addWidget(clearMine);
    auto* deleteSelected = new QPushButton("delete selected", this);
    actions->addWidget(deleteSelected);
    auto* clear = new QPushButton("clear all", this);
    clear->setVisible(owner);
    clear->setToolTip("Remove everyone's drawings from your shared screen.");
    actions->addWidget(clear);
    auto* hint = new QLabel(this);
    hint->setWordWrap(true);
    actions->addWidget(hint);
    actions->addStretch();
    connect(allow, &QPushButton::toggled, drawing, &ScreenDrawing::permissionRequested);
    const auto toggleTool = [drawing](ScreenDrawing::Tool tool, bool checked) {
        if (checked)
            drawing->setTool(tool);
        else if (drawing->tool() == tool)
            drawing->setTool(ScreenDrawing::Tool::None);
    };
    connect(pen, &QPushButton::toggled, drawing, [=](bool checked) { toggleTool(ScreenDrawing::Tool::Pen, checked); });
    connect(eraser, &QPushButton::toggled, drawing, [=](bool checked) { toggleTool(ScreenDrawing::Tool::Eraser, checked); });
    connect(select, &QPushButton::toggled, drawing, [=](bool checked) { toggleTool(ScreenDrawing::Tool::Select, checked); });
    connect(colors, &QComboBox::currentIndexChanged, drawing, [=](int index) { drawing->setColor(index - 1); });
    connect(clear, &QPushButton::clicked, drawing, &ScreenDrawing::clearRequested);
    connect(clearMine, &QPushButton::clicked, drawing, &ScreenDrawing::clearMineRequested);
    connect(deleteSelected, &QPushButton::clicked, drawing, &ScreenDrawing::deleteSelection);
    auto refresh = [=] {
        const QSignalBlocker blockAllow(allow), blockPen(pen), blockEraser(eraser),
                             blockSelect(select), blockColors(colors);
        allow->setChecked(drawing->enabled());
        pen->setChecked(drawing->tool() == ScreenDrawing::Tool::Pen);
        eraser->setChecked(drawing->tool() == ScreenDrawing::Tool::Eraser);
        select->setChecked(drawing->tool() == ScreenDrawing::Tool::Select);
        colors->setCurrentIndex(drawing->color() + 1);
        QPixmap swatch(12, 12);
        swatch.fill(drawing->chatColor());
        colors->setItemIcon(0, QIcon(swatch));
        pen->setVisible(drawing->enabled());
        eraser->setVisible(drawing->enabled());
        select->setVisible(owner && drawing->enabled());
        colors->setVisible(drawing->enabled());
        clearMine->setVisible(drawing->enabled());
        clearMine->setEnabled(!drawing->strokeIds(drawing->localName()).isEmpty());
        deleteSelected->setVisible(owner && drawing->tool() == ScreenDrawing::Tool::Select);
        deleteSelected->setEnabled(!drawing->selectedStrokes().isEmpty());
        deleteSelected->setText(drawing->selectedStrokes().isEmpty() ? "delete selected" :
                               QString("delete selected (%1)").arg(drawing->selectedStrokes().size()));
        clear->setEnabled(drawing->enabled() && !drawing->segments().isEmpty());
        const bool previewOnly = owner && !drawing->presenterOverlayAvailable();
        const QString toolHint = drawing->tool() == ScreenDrawing::Tool::Select ? "click or drag a box; Delete removes; Esc exits" :
                                 drawing->tool() == ScreenDrawing::Tool::Eraser ? (owner ? "erase strokes; Esc exits" : "erase your strokes; Esc exits") : QString();
        hint->setText(!drawing->status().isEmpty() ? drawing->status() :
                      !drawing->enabled() ? "drawing is off" :
                      previewOnly ? "draw in the video preview" : toolHint);
        hint->setVisible(!hint->text().isEmpty());
    };
    connect(drawing, &ScreenDrawing::changed, this, refresh);
    connect(drawing, &ScreenDrawing::updated, this, refresh);
    refresh();
}

namespace {
QPointF drawingPoint(const QRectF& area, int x, int y) {
    return area.topLeft() + QPointF(x * area.width() / chat::kDrawCoordinateMax,
                                   y * area.height() / chat::kDrawCoordinateMax);
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
            setCursor(drawing_->penActive() ? Qt::CrossCursor : Qt::ArrowCursor);
            update();
        });
        setCursor(drawing_->penActive() ? Qt::CrossCursor : Qt::ArrowCursor);
    }
    previous_.reset();
    gestureStart_.reset();
    eraserPrevious_.reset();
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
    QHash<QString, chat::ScreenDraw> labels;
    for (const auto& segment : drawing_->segments()) {
        if (drawing_->selectedStrokes().contains(screenStrokeKey(segment)))
            labels[QString::fromStdString(segment.author)] = segment;
    }
    for (const auto& author : drawing_->activeAuthors())
        labels[QString::fromStdString(author.author)] = author;
    for (const auto& author : labels) {
        const QString name = painter.fontMetrics().elidedText(QString::fromStdString(author.author), Qt::ElideRight,
                                                              qMax(20, qRound(qMin(180.0, area.width() - 12))));
        const QPointF endpoint = drawingPoint(area, author.x2, author.y2);
        const qreal labelWidth = qMin(area.width(), painter.fontMetrics().horizontalAdvance(name) + 14.0);
        const qreal labelHeight = painter.fontMetrics().height() + 8;
        const qreal left = qBound(area.left(), endpoint.x() - labelWidth / 2, area.right() - labelWidth);
        const qreal top = endpoint.y() - labelHeight - 10 >= area.top() ? endpoint.y() - labelHeight - 10 : endpoint.y() + 10;
        const QRectF label(left, qBound(area.top(), top, qMax(area.top(), area.bottom() - labelHeight)), labelWidth, labelHeight);
        painter.setPen(QPen(screenSegmentColor(author), 1));
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
    if (!drawing_ || !drawing_->enabled() || drawing_->tool() != ScreenDrawing::Tool::Pen)
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
    segment.color = qMax(0, drawing_->color());
    segment.rgb = static_cast<int>(drawing_->penColor().rgb() & 0xffffff);
    emit drawing_->segmentDrawn(segment);
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
    if (drawing_ && drawing_->tool() == ScreenDrawing::Tool::Select &&
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
    layout->setContentsMargins(10, 8, 10, 8);
    layout->addWidget(new DrawingControls(drawing, true, tools_));
    tools_->setStyleSheet("QWidget#screenDrawingToolbar { background: #252830; border: 1px solid "
                          "#535963; border-radius: 8px; } QLabel, QPushButton, QComboBox { color: "
                          "#f3f4f6; } QPushButton, QComboBox { background: #343943; padding: 5px "
                          "8px; border: 1px solid #606874; border-radius: 4px; } "
                          "QPushButton:checked { background: #365e51; border-color: #67e8a5; }");
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
        drawing() && drawing()->penActive() &&
        (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        const bool erase = drawing()->tool() == ScreenDrawing::Tool::Select &&
                           (key == Qt::Key_Delete || key == Qt::Key_Backspace);
        if (key == Qt::Key_Escape || erase) {
            if (event->type() == QEvent::KeyPress) {
                if (erase) drawing()->deleteSelection();
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
