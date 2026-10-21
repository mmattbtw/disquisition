#include "desktop/screen_drawing.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>

#if defined(Q_OS_WIN)
#    include <qt_windows.h>
#endif

QColor screenPenColor(int index) {
    static const char* colors[] = {"#ff5470", "#ffcf5a", "#67e8a5",
                                   "#62cfff", "#c49bff", "#ffffff"};
    return QColor(colors[qBound(0, index, chat::kPenColorCount - 1)]);
}

void ScreenDrawing::setState(bool enabled, std::int64_t epoch) {
    enabled_ = enabled;
    epoch_ = epoch;
    segments_.clear();
    received_.clear();
    if (!enabled)
        penActive_ = false;
    emit changed();
}

void ScreenDrawing::add(const chat::ScreenDraw& segment) {
    if (!enabled_ || segment.epoch != epoch_ ||
        segments_.size() >= static_cast<qsizetype>(chat::kMaxDrawSegments))
        return;
    const QString key = QString::fromStdString(segment.author) + ':' + QString::number(segment.id);
    if (received_.contains(key))
        return;
    received_.insert(key);
    segments_.append(segment);
    emit updated();
}

void ScreenDrawing::setPenActive(bool active) {
    penActive_ = enabled_ && active;
    emit changed();
}

void ScreenDrawing::setColor(int color) {
    color_ = qBound(0, color, chat::kPenColorCount - 1);
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
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(6);
    auto* allow = new QPushButton("allow drawing", this);
    allow->setCheckable(true);
    allow->setVisible(owner);
    allow->setToolTip(
        "Let viewers draw on your shared screen. Turning this off clears all drawings.");
    row->addWidget(allow);
    auto* pen = new QPushButton("pen", this);
    pen->setCheckable(true);
    pen->setToolTip("Draw on the shared screen. Press Esc to return to using your mouse.");
    row->addWidget(pen);
    auto* colors = new QComboBox(this);
    colors->setAccessibleName("Pen color");
    const QStringList names{"red", "yellow", "green", "blue", "purple", "white"};
    for (int i = 0; i < names.size(); ++i) {
        QPixmap swatch(12, 12);
        swatch.fill(screenPenColor(i));
        colors->addItem(QIcon(swatch), names[i]);
    }
    row->addWidget(colors);
    auto* clear = new QPushButton("clear drawings", this);
    clear->setVisible(owner);
    row->addWidget(clear);
    auto* hint = new QLabel("drawing is off", this);
    row->addWidget(hint);
    row->addStretch();
    connect(allow, &QPushButton::toggled, drawing, &ScreenDrawing::permissionRequested);
    connect(pen, &QPushButton::toggled, drawing, &ScreenDrawing::setPenActive);
    connect(colors, &QComboBox::currentIndexChanged, drawing, &ScreenDrawing::setColor);
    connect(clear, &QPushButton::clicked, drawing, &ScreenDrawing::clearRequested);
    auto refresh = [=] {
        const QSignalBlocker blockAllow(allow), blockPen(pen), blockColors(colors);
        allow->setChecked(drawing->enabled());
        pen->setChecked(drawing->penActive());
        colors->setCurrentIndex(drawing->color());
        pen->setVisible(drawing->enabled());
        colors->setVisible(drawing->enabled());
        clear->setEnabled(drawing->enabled());
        const bool previewOnly = owner && !drawing->presenterOverlayAvailable();
        hint->setText(!drawing->status().isEmpty()        ? drawing->status()
                      : previewOnly && drawing->enabled() ? "draw in the video preview"
                                                          : "drawing is off");
        hint->setVisible(!drawing->enabled() || previewOnly || !drawing->status().isEmpty());
    };
    connect(drawing, &ScreenDrawing::changed, this, refresh);
    refresh();
}

DrawingCanvas::DrawingCanvas(QWidget* parent) : QWidget(parent) {
}

void DrawingCanvas::setDrawing(ScreenDrawing* drawing) {
    if (drawing_)
        disconnect(drawing_, nullptr, this, nullptr);
    drawing_ = drawing;
    if (drawing_) {
        connect(drawing_, &ScreenDrawing::updated, this, qOverload<>(&QWidget::update));
        connect(drawing_, &ScreenDrawing::changed, this, [this] {
            previous_.reset();
            setCursor(drawing_->penActive() ? Qt::CrossCursor : Qt::ArrowCursor);
            update();
        });
        setCursor(drawing_->penActive() ? Qt::CrossCursor : Qt::ArrowCursor);
    }
    previous_.reset();
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
    const auto point = [&](int x, int y) {
        return area.topLeft() + QPointF(x * area.width() / chat::kDrawCoordinateMax,
                                        y * area.height() / chat::kDrawCoordinateMax);
    };
    const qreal width = qMax(2.0, area.width() * 0.003);
    for (const auto& segment : drawing_->segments()) {
        painter.setPen(
            QPen(screenPenColor(segment.color), width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const QPointF a = point(segment.x1, segment.y1), b = point(segment.x2, segment.y2);
        if (a == b)
            painter.drawPoint(a);
        else
            painter.drawLine(a, b);
    }
    painter.restore();
}

void DrawingCanvas::drawTo(const QPointF& position, bool first) {
    if (!drawing_ || !drawing_->enabled() || !drawing_->penActive())
        return;
    const QRectF area = drawingRect();
    if (area.isEmpty() || !area.contains(position)) {
        previous_.reset();
        return;
    }
    const QPoint point(
        qBound(0, qRound((position.x() - area.left()) * chat::kDrawCoordinateMax / area.width()),
               chat::kDrawCoordinateMax),
        qBound(0, qRound((position.y() - area.top()) * chat::kDrawCoordinateMax / area.height()),
               chat::kDrawCoordinateMax));
    if (!first && previous_ == point)
        return;
    const QPoint start = first || !previous_ ? point : *previous_;
    previous_ = point;
    chat::ScreenDraw segment;
    segment.epoch = drawing_->epoch();
    segment.id = drawing_->nextSegmentId();
    segment.x1 = start.x();
    segment.y1 = start.y();
    segment.x2 = point.x();
    segment.y2 = point.y();
    segment.color = drawing_->color();
    emit drawing_->segmentDrawn(segment);
}

void DrawingCanvas::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton)
        return;
    previous_.reset();
    drawTo(event->position(), true);
    penClock_.start();
}

void DrawingCanvas::mouseMoveEvent(QMouseEvent* event) {
    if (!(event->buttons() & Qt::LeftButton))
        return;
    if (penClock_.isValid() && penClock_.elapsed() < 16)
        return;
    drawTo(event->position());
    penClock_.restart();
}

void DrawingCanvas::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton)
        return;
    drawTo(event->position());
    previous_.reset();
}

PresenterOverlay::PresenterOverlay(const ShareSource& source, ScreenDrawing* drawing,
                                   QWidget* parent) : DrawingCanvas(parent), source_(source) {
    setObjectName("screenDrawingOverlay");
    setWindowTitle("Screen drawing overlay");
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                   Qt::WindowTransparentForInput);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_MacAlwaysShowToolWindow);
    setDrawing(drawing);
    drawing->setPresenterOverlayAvailable(shareSourceSupportsOverlay(source));
    tools_ = new QWidget(this, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    tools_->setObjectName("screenDrawingToolbar");
    tools_->setWindowTitle("Screen drawing");
    tools_->setAttribute(Qt::WA_MacAlwaysShowToolWindow);
    auto* layout = new QHBoxLayout(tools_);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->addWidget(new DrawingControls(drawing, true, tools_));
    tools_->setStyleSheet("QWidget#screenDrawingToolbar { background: #252830; border: 1px solid "
                          "#535963; border-radius: 8px; } QLabel, QPushButton, QComboBox { color: "
                          "#f3f4f6; } QPushButton, QComboBox { background: #343943; padding: 5px "
                          "8px; border: 1px solid #606874; border-radius: 4px; } "
                          "QPushButton:checked { background: #365e51; border-color: #67e8a5; }");
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
        hide();
        tools_->hide();
        return;
    }
    if (geometry() != bounds)
        setGeometry(bounds);
    const bool transparent = !drawing()->penActive();
    if (windowFlags().testFlag(Qt::WindowTransparentForInput) != transparent) {
        setWindowFlag(Qt::WindowTransparentForInput, transparent);
    }
    show();
    tools_->adjustSize();
    tools_->move(bounds.center().x() - tools_->width() / 2, bounds.top() + 12);
    tools_->show();
#if defined(Q_OS_WIN)
    SetWindowDisplayAffinity(reinterpret_cast<HWND>(winId()),
                             0x00000011 /* WDA_EXCLUDEFROMCAPTURE */);
    SetWindowDisplayAffinity(reinterpret_cast<HWND>(tools_->winId()), 0x00000011);
#endif
}

void PresenterOverlay::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    // Layered windows need nonzero alpha to receive pen input on Windows.
    if (drawing() && drawing()->penActive())
        painter.fillRect(rect(), QColor(0, 0, 0, 1));
    paintDrawing(painter);
    if (drawing() && drawing()->penActive()) {
        painter.setPen(QPen(screenPenColor(drawing()->color()), 2));
        painter.drawRect(rect().adjusted(1, 1, -1, -1));
    }
}
