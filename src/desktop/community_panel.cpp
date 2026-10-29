#include "desktop/community_panel.h"

#include <QAction>
#include <QAudio>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFrame>
#include <QFileDialog>
#include <QFileOpenEvent>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImageReader>
#include <QInputDialog>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMediaDevices>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScreen>
#include <QCameraDevice>
#include <QMediaDevices>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QStandardPaths>
#include <QStyle>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextTable>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <QWindow>
#include <algorithm>
#include <cmath>

#include "community/qr.h"
#include "desktop/audio_permission.h"
#include "desktop/chat_media.h"
#include "desktop/chat_color.h"
#include "desktop/community_tree.h"
#include "desktop/penguin_overlay.h"
#include "desktop/penguin_sprites.h"
#include "desktop/gif_picker.h"
#include "desktop/gif_search.h"
#include "desktop/mention_completer.h"
#include "desktop/notification_center.h"
#include "desktop/remote_media.h"
#include "desktop/screen_stage.h"
#include "desktop/share_picker.h"
#include "desktop/theme.h"
#include "desktop/volume_menu.h"

using community::freshId;
namespace {
const QString kDefaultRelay = QStringLiteral("relay.disquisition.app");
const QStringList kChatColors{"pink", "mint", "butter", "periwinkle", "lilac", "aqua", "peach"};
bool validChatColor(const QString& value) {
    if (kChatColors.contains(value)) return true;
    if (value.isEmpty() || value.size() > 3) return false;
    for (const auto character : value)
        if (character < QLatin1Char('0') || character > QLatin1Char('9')) return false;
    const int index = value.toInt();
    return index <= 255 && index != 1 && index != 2 && index != 250;
}
QString packetColor(const QJsonObject& data) {
    const auto color = data.value("color").toString();
    return validChatColor(color) ? color : QStringLiteral("mint");
}

QString key(const QString& cid, const QString& channel) {
    return cid + '/' + channel;
}
QString text(const std::string& value) {
    return QString::fromStdString(value);
}
constexpr double kVoiceActivityLevel = 180; // RMS, about -45 dBFS.
constexpr int kVoiceActivityHoldFrames = 10;
void paintSpeaking(QWidget* row, bool speaking) {
    row->setStyleSheet(speaking ? QStringLiteral("QWidget#voiceParticipantRow {"
                                                 "background-color: rgba(126, 217, 87, 34);"
                                                 "border: 1px solid rgba(126, 217, 87, 150);"
                                                 "border-radius: 8px; }")
                                : QString());
}
QPushButton* button(const QString& title, const char* name, QWidget* parent) {
    auto* result = new QPushButton(title, parent);
    result->setObjectName(name);
    return result;
}
QList<community::Channel> orderedChannels(const community::Model& model) {
    auto channels = model.channels.values();
    std::sort(channels.begin(), channels.end(), [](const auto& a, const auto& b) {
        if (a.type != b.type)
            return a.type == "text";
        if (a.name == "general" || b.name == "general")
            return a.name == "general" && b.name != "general";
        const int order = QString::compare(a.name, b.name, Qt::CaseInsensitive);
        return order == 0 ? a.id < b.id : order < 0;
    });
    return channels;
}
QPixmap voiceIndicator(const QString& kind) {
    QPixmap image(16, 16);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(kind == "sharing screen" || kind == "camera on" ? QColor("#9dccca")
                                                                     : QColor("#aebcbf"),
                        1.5));
    if (kind == "muted") {
        painter.drawRoundedRect(QRectF(6, 1, 4, 8), 2, 2);
        painter.drawArc(QRectF(4, 4, 8, 7), 180 * 16, 180 * 16);
        painter.drawLine(8, 11, 8, 14);
        painter.drawLine(5, 14, 11, 14);
    }
    else if (kind == "deafened") {
        painter.drawArc(QRectF(3, 2, 10, 12), 0, 180 * 16);
        painter.drawRoundedRect(QRectF(2, 8, 3, 6), 1, 1);
        painter.drawRoundedRect(QRectF(11, 8, 3, 6), 1, 1);
    }
    else if (kind == "muted by you") {
        QPainterPath speaker;
        speaker.moveTo(2, 6);
        speaker.lineTo(5, 6);
        speaker.lineTo(9, 3);
        speaker.lineTo(9, 13);
        speaker.lineTo(5, 10);
        speaker.lineTo(2, 10);
        speaker.closeSubpath();
        painter.drawPath(speaker);
    }
    else if (kind == "camera on") {
        painter.drawRoundedRect(QRectF(1.5, 4, 9, 8), 2, 2);
        painter.drawPolygon(QPolygonF({QPointF(10.5, 7), QPointF(14.5, 4.5), QPointF(14.5, 11.5),
                                       QPointF(10.5, 9)}));
    }
    else {
        painter.drawRect(QRectF(2, 2, 12, 9));
        painter.drawLine(8, 11, 8, 14);
        painter.drawLine(5, 14, 11, 14);
    }
    if (kind != "sharing screen" && kind != "camera on")
        painter.drawLine(2, 1, 14, 15);
    return image;
}
// Slider positions follow loudness as heard, which is roughly logarithmic.
qreal linearVolume(int percent) {
    return QAudio::convertVolume(percent / 100.0, QAudio::LogarithmicVolumeScale,
                                 QAudio::LinearVolumeScale);
}
constexpr int kUnreadRole = Qt::UserRole + 20;
constexpr int kNoteRole = Qt::UserRole + 21;
constexpr int kLiveRole = Qt::UserRole + 22;
constexpr int kSectionRole = Qt::UserRole + 30;
constexpr int kBadgeRole = Qt::UserRole + 31;
constexpr int kStateRole = Qt::UserRole + 32;
constexpr int kPresentRole = Qt::UserRole + 33;
constexpr int kStateColorRole = Qt::UserRole + 34;

QFont monoFont(qreal pixels, qreal tracking = 0) {
    QFont font("JetBrains Mono");
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(qRound(pixels));
    font.setLetterSpacing(QFont::AbsoluteSpacing, tracking);
    return font;
}

// Restyle a widget after changing a property its stylesheet selects on.
void setFlag(QWidget* widget, const char* name, bool value) {
    if (widget->property(name).toBool() == value)
        return;
    widget->setProperty(name, value);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
}

QPixmap statusDot(const QColor& color, qreal scale) {
    QPixmap image(QSize(8, 8) * scale);
    image.setDevicePixelRatio(scale);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(QRectF(0, 0, 8, 8));
    return image;
}

// Small bold capitals, as on the site's role and LIVE tags.
QFont tagFont() {
    QFont font("Inter");
    font.setPixelSize(9);
    font.setWeight(QFont::Bold);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
    return font;
}

// Draws a small rounded tag and returns its width.
int paintTag(QPainter* painter, const QRectF& area, const QString& text, const QColor& fill,
             const QColor& ink, const QColor& border = Qt::transparent) {
    const auto font = tagFont();
    const int width = QFontMetrics(font).horizontalAdvance(text) + 9;
    const QRectF tag(area.right() - width, area.center().y() - 7.5, width, 15);
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(border == Qt::transparent ? Qt::NoPen : QPen(border, 1));
    painter->setBrush(fill);
    painter->drawRoundedRect(tag.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    painter->setFont(font);
    painter->setPen(ink);
    painter->drawText(tag, Qt::AlignCenter, text);
    painter->restore();
    return width;
}

struct DialogFrame {
    QVBoxLayout* body;
    QHBoxLayout* footer;
};

// Title block, divider, body and footer bar shared by the larger dialogs.
DialogFrame dialogFrame(QDialog& dialog, const QString& title, const QString& subtitle) {
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* head = new QVBoxLayout;
    head->setContentsMargins(32, 28, 32, 20);
    head->setSpacing(6);
    auto* heading = new QLabel(title, &dialog);
    heading->setObjectName("dialogTitle");
    heading->setTextFormat(Qt::PlainText);
    head->addWidget(heading);
    auto* detail = new QLabel(subtitle, &dialog);
    detail->setObjectName("dialogSubtitle");
    detail->setTextFormat(Qt::PlainText);
    detail->setWordWrap(true);
    head->addWidget(detail);
    layout->addLayout(head);
    auto* rule = new QFrame(&dialog);
    rule->setObjectName("dialogRule");
    layout->addWidget(rule);
    auto* body = new QVBoxLayout;
    body->setContentsMargins(32, 22, 32, 24);
    body->setSpacing(10);
    layout->addLayout(body, 1);
    auto* footer = new QWidget(&dialog);
    footer->setObjectName("dialogFooter");
    footer->setAttribute(Qt::WA_StyledBackground);
    auto* row = new QHBoxLayout(footer);
    row->setContentsMargins(32, 14, 24, 14);
    row->setSpacing(8);
    layout->addWidget(footer);
    return {body, row};
}

QLabel* fieldLabel(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text.toUpper(), parent);
    label->setObjectName("fieldLabel");
    label->setFont(monoFont(11, 1.4));
    return label;
}

// Moves the window when dragged, like a title bar, and zooms it on a double click.
class DragBar final : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && window()->windowHandle())
            window()->windowHandle()->startSystemMove();
    }
    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton)
            window()->isMaximized() ? window()->showNormal() : window()->showMaximized();
    }
};

// Lets the whole picture-in-picture act as one button.
class ClickFrame final : public QFrame {
public:
    ClickFrame(std::function<void()> clicked, QWidget* parent)
        : QFrame(parent), clicked_(std::move(clicked)) {
        setCursor(Qt::PointingHandCursor);
    }

protected:
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint()))
            clicked_();
    }

private:
    std::function<void()> clicked_;
};

class ChannelDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        if (index.data(Qt::SizeHintRole).isValid())
            return size;
        size.setHeight(index.parent().isValid() ? 28 : 34);
        return size;
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        QStyleOptionViewItem item(option);
        initStyleOption(&item, index);
        const QString label = item.text;
        item.text.clear();
        const QWidget* widget = option.widget;
        auto* style = widget ? widget->style() : QApplication::style();
        const bool root = !index.parent().isValid();
        if (root)
            item.state &= ~(QStyle::State_Selected | QStyle::State_MouseOver);
        style->drawControl(QStyle::CE_ItemViewItem, &item, painter, widget);
        if (label.isEmpty())
            return; // Participant rows are widgets.
        QRect text = style->subElementRect(QStyle::SE_ItemViewItemText, &item, widget);
        painter->save();
        painter->setRenderHint(QPainter::TextAntialiasing);
        const bool selected = option.state & QStyle::State_Selected;
        const int unread = index.data(kUnreadRole).toInt();
        auto right = QRectF(text).adjusted(0, 0, -4, 0);
        if (unread > 0) {
            const auto count = unread > 99 ? QString("99+") : QString::number(unread);
            QFont font = item.font;
            font.setPixelSize(10);
            font.setWeight(QFont::Bold);
            const int width = qMax(17, QFontMetrics(font).horizontalAdvance(count) + 9);
            const QRectF badge(right.right() - width, right.center().y() - 8.5, width, 17);
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(theme::kTeal));
            painter->drawRoundedRect(badge, 8.5, 8.5);
            painter->setFont(font);
            painter->setPen(QColor(theme::kInk));
            painter->drawText(badge, Qt::AlignCenter, count);
            right.setRight(badge.left() - 6);
        }
        if (index.data(kLiveRole).toBool())
            right.setRight(right.right() - paintTag(painter, right, "LIVE", QColor("#a5dfdc"),
                                                    QColor(theme::kInk)) - 6);
        const auto note = index.data(kNoteRole).toString();
        if (!note.isEmpty()) {
            QFont small = item.font;
            small.setPixelSize(11);
            small.setBold(false);
            painter->setFont(small);
            painter->setPen(QColor(theme::kDim));
            const int width = QFontMetrics(small).horizontalAdvance(note);
            painter->drawText(right, Qt::AlignVCenter | Qt::AlignRight, note);
            right.setRight(right.right() - width - 8);
        }
        auto font = item.font;
        font.setPixelSize(root ? 14 : 13);
        font.setWeight(root ? QFont::DemiBold : unread ? QFont::DemiBold : QFont::Normal);
        painter->setFont(font);
        const auto foreground = index.data(Qt::ForegroundRole);
        QColor color = root ? QColor("#edf0ef")
                       : foreground.isValid() ? foreground.value<QBrush>().color()
                       : selected || unread   ? QColor("#edf0ef")
                                              : QColor("#a9b2b9");
        QString name = label;
        if (name.startsWith("# ")) {
            name = name.mid(2);
            painter->setPen(QColor(theme::kDim));
            painter->drawText(right, Qt::AlignVCenter | Qt::AlignLeft, "#");
            right.setLeft(right.left() + QFontMetrics(font).horizontalAdvance("#") + 7);
        }
        painter->setPen(color);
        painter->drawText(right, Qt::AlignVCenter | Qt::AlignLeft,
                          QFontMetrics(font).elidedText(name, Qt::ElideRight,
                                                        qMax(0, int(right.width()))));
        painter->restore();
    }
};

class MemberDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex& index) const override {
        return QSize(180, kRow + (index.data(kSectionRole).toString().isEmpty() ? 0 : kHeader));
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setRenderHint(QPainter::TextAntialiasing);
        QRect row = option.rect;
        const auto section = index.data(kSectionRole).toString();
        if (!section.isEmpty()) {
            QRect header = row;
            header.setHeight(kHeader);
            painter->setFont(monoFont(10, 1.4));
            painter->setPen(QColor(theme::kDim));
            painter->drawText(header.adjusted(6, 0, 0, -6), Qt::AlignLeft | Qt::AlignBottom,
                              section);
            row.setTop(row.top() + kHeader);
        }
        if (option.state & (QStyle::State_MouseOver | QStyle::State_Selected)) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor("#2e3239"));
            painter->drawRoundedRect(QRectF(row).adjusted(0, 1, 0, -1), 6, 6);
        }
        const bool present = index.data(kPresentRole).toBool();
        const auto icon = index.data(Qt::DecorationRole).value<QIcon>();
        const QRect avatar(row.left() + 6, row.center().y() - 13, 26, 26);
        painter->setOpacity(present ? 1.0 : 0.45);
        icon.paint(painter, avatar);
        painter->setOpacity(1.0);
        const int left = avatar.right() + 9;
        const int width = row.right() - 6 - left;
        QFont name = option.font;
        name.setPixelSize(13);
        name.setWeight(QFont::DemiBold);
        const QFontMetrics nameMetrics(name);
        const auto badge = index.data(kBadgeRole).toString();
        const int badgeWidth = badge.isEmpty()
            ? 0 : QFontMetrics(tagFont()).horizontalAdvance(badge) + 15;
        const auto label = nameMetrics.elidedText(index.data(Qt::DisplayRole).toString(),
                                                  Qt::ElideRight, qMax(0, width - badgeWidth));
        const QRect nameRect(left, row.center().y() - 16, width, 17);
        painter->setFont(name);
        const auto foreground = index.data(Qt::ForegroundRole);
        painter->setPen(present && foreground.isValid() ? foreground.value<QBrush>().color()
                                                        : QColor(present ? "#edf0ef" : "#8b959e"));
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, label);
        if (!badge.isEmpty()) {
            const int at = left + nameMetrics.horizontalAdvance(label) + 6 + badgeWidth - 6;
            paintTag(painter, QRectF(left, nameRect.top(), at - left, nameRect.height()), badge,
                     badge == "OWNER" ? QColor("#a5dfdc") : QColor("#f2d27a"),
                     QColor("#1a1d22"));
        }
        QFont small = option.font;
        small.setPixelSize(11);
        painter->setFont(small);
        const auto stateColor = index.data(kStateColorRole);
        painter->setPen(stateColor.isValid() ? stateColor.value<QColor>() : QColor(theme::kMuted));
        painter->drawText(QRect(left, row.center().y() + 1, width, 15),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(small).elidedText(index.data(kStateRole).toString(),
                                                         Qt::ElideRight, width));
        painter->restore();
    }

private:
    static constexpr int kRow = 40;
    static constexpr int kHeader = 26;
};

QPixmap profileAvatar(const community::Member& member, int size, qreal scale) {
    QPixmap result(QSize(size, size) * scale);
    result.setDevicePixelRatio(scale);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath tile;
    tile.addRoundedRect(QRectF(0, 0, size, size), size * 0.26, size * 0.26);
    painter.setClipPath(tile);
    QImage decoded;
    if (member.avatar.size() <= community::kMaxAvatarBytes) {
        ImageBytes source(member.avatar);
        const auto dimensions = source.size();
        if (source.canRead() && dimensions.width() > 0 && dimensions.height() > 0 &&
            dimensions.width() <= 256 && dimensions.height() <= 256)
            decoded = source.read();
    }
    if (!decoded.isNull()) {
        const int side = std::min(decoded.width(), decoded.height());
        painter.drawImage(QRectF(0, 0, size, size), decoded,
                          QRectF((decoded.width() - side) / 2, (decoded.height() - side) / 2, side,
                                 side));
    }
    else {
        painter.setClipping(false);
        painter.drawPixmap(0, 0, theme::initialAvatar(member.name, size, scale));
    }
    return result;
}
} // namespace

CommunityPanel::CommunityPanel(QWidget* parent, const QString& directory) : QWidget(parent), pushToTalk_(this) {
    try {
        const auto override = qApp->property("communityDataDirectory").toString();
        const auto path = !directory.isEmpty() ? directory
                          : !override.isEmpty()
                              ? override
                              : QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                                    "/communities";
        node_ = std::make_unique<community::Node>(path, this);
    }
    catch (const std::exception& error) {
        failure_ = QString::fromUtf8(error.what());
    }
    messageColor_ = kChatColors[QRandomGenerator::global()->bounded(kChatColors.size())];
    QSettings penguinSettings;
    const auto savedColor = penguinSettings.value("community/color").toString().toLower();
    if (validChatColor(savedColor)) messageColor_ = savedColor;
    penguinSettings.setValue("community/color", messageColor_);
    penguins_ = new PenguinOverlay(this);
    penguins_->setEnabled(penguinSettings.value("penguins/unlocked", false).toBool() &&
                          penguinSettings.value("penguins/enabled", false).toBool());
    penguins_->setWandering(penguinSettings.value("penguins/wander", true).toBool());
    penguins_->setIdleTimeoutMinutes(penguinSettings.value("penguins/idleMinutes", 10).toInt());
    penguinsAction_ = new QAction("Desktop penguins", this);
    penguinsAction_->setObjectName("desktopPenguins");
    penguinsAction_->setCheckable(true);
    penguinsAction_->setVisible(penguinSettings.value("penguins/unlocked", false).toBool());
    penguinsAction_->setChecked(penguinsAction_->isVisible() &&
                               penguinSettings.value("penguins/enabled", false).toBool());
    connect(penguinsAction_, &QAction::toggled, this, [this](bool enabled) {
        penguins_->setEnabled(enabled);
        QSettings().setValue("penguins/enabled", enabled);
    });
    notifications_ = new NotificationCenter(window());
    buildUi();
    connect(notifications_, &NotificationCenter::settingsChanged, this,
            [this](const NotificationSettings& settings) {
                if (settings.muted)
                    unread_.clear();
                refresh();
            });
    connect(&pushToTalk_, &PushToTalk::heldChanged, this, &CommunityPanel::voiceState);
    imagePool_.setMaxThreadCount(1);
    if (node_) {
        connect(node_.get(), &community::Node::changed, this, [this] {
            if (refreshQueued_)
                return;
            refreshQueued_ = true;
            QTimer::singleShot(0, this, [this] {
                refreshQueued_ = false;
                refresh();
            });
        });
        connect(node_.get(), &community::Node::joined, this, [this](const QString& cid) {
            joiningCommunities_.remove(cid);
            updateJoining();
            selectCommunity(cid);
            note("Community connected. Messages are delivered live.");
        });
        connect(node_.get(), &community::Node::joinRejected, this,
                [this](const QString& cid, const QString&) {
                    joiningCommunities_.remove(cid);
                    updateJoining();
                    noteError("Invite rejected. Check the code with the community owner.");
                });
        connect(node_.get(), &community::Node::errorOccurred, this, &CommunityPanel::noteError);
        connect(node_.get(), &community::Node::relayConnectionChanged, this,
                [this](bool connected) {
                    note(connected ? "Relay connected."
                                   : "Relay connection lost. Reconnecting automatically…");
                });
        connect(node_.get(), &community::Node::packetReceived, this, &CommunityPanel::receive);
        connect(node_.get(), &community::Node::voiceMoved, this,
                [this](const QString& cid, const QString& channel) {
                    // The node has already changed presence. End subscriptions and capture
                    // from the previous channel before refreshing the existing audio session.
                    // Stop capture before stopSharing so it cannot republish the
                    // previous voice channel while applying the administrator's move.
                    stopCamera(false);
                    capture_.stop();
                    stopSharing();
                    const auto watching = watching_;
                    for (const auto& peer : watching) watch(peer, false);
                    for (const auto& peer : streams_) {
                        stopDrawing(peer);
                        stage_->removeStream(peer);
                        screenAudio_.remove(peer);
                    }
                    for (const auto& peer : cameras_)
                        stage_->setCameraFrame(peer, {});
                    cameras_.clear();
                    cameraWatching_.clear();
                    streams_.clear();
                    watching_.clear();
                    lastScreenFrames_.clear();
                    frames_ = ScreenFrameAssembler{};
                    decoders_.clear();
                    voiceCid_ = cid;
                    voiceChannel_ = channel;
                    selectedVoiceCid_ = cid;
                    selectedVoiceChannel_ = channel;
                    refresh();
                    note("An administrator moved you to " +
                         node_->communities().value(cid).channels.value(channel).name + '.');
                });
    }
    qApp->installEventFilter(this);
    imageTimer_.setInterval(2);
    speakingTimer_.setInterval(500);
    connect(&speakingTimer_, &QTimer::timeout, this, [this] {
        if (!node_ || voiceCid_.isEmpty())
            return;
        const auto cutoff = QDateTime::currentMSecsSinceEpoch() - 1000;
        QStringList expired;
        for (const auto& peer : speaking_)
            if (peer != node_->peerId() && lastAudioAt_.value(peer) < cutoff)
                expired.append(peer);
        for (const auto& peer : expired) {
            setSpeaking(peer, false);
            quietAudioFrames_.remove(peer);
        }
    });
    speakingTimer_.start();
    connect(&imageTimer_, &QTimer::timeout, this, [this] {
        if (!online_ || uploads_.isEmpty()) {
            imageTimer_.stop();
            return;
        }
        if (node_->mediaBacklog() > 64 * 1024)
            return;
        auto& upload = uploads_.first();
        attempt([&] {
            node_->sendPacket(upload.cid, upload.channel, "image",
                              {{"transfer", double(upload.transfer)},
                               {"index", upload.next},
                               {"count", upload.chunks.size()},
                               {"color", upload.color},
                               {"bytes",
                                QString::fromLatin1(upload.chunks[upload.next].toBase64())}});
        });
        if (++upload.next == upload.chunks.size())
            uploads_.removeFirst();
    });
    connect(&audio_, &RelayAudio::frameReady, this, [this](const QByteArray& pcm) {
        if (online_ && !voiceCid_.isEmpty())
            attempt([&] {
                node_->sendPacket(voiceCid_, voiceChannel_, "audio",
                                  {{"bytes", QString::fromLatin1(pcm.toBase64())}});
            });
    });
    connect(&audio_, &RelayAudio::speakingChanged, this, [this](bool speaking) {
        if (node_ && !voiceCid_.isEmpty())
            setSpeaking(node_->peerId(), speaking);
    });
    cameraCapture_.setMaxSize({1280, 720});
    cameraCapture_.setFrameRate(30);
    cameraCapture_.setBacklogProbe([this] { return node_ ? node_->mediaBacklog() : qint64(0); });
    connect(&cameraCapture_, &ScreenCapture::frameReady, this,
            [this](quint32 frame, const QList<QByteArray>& chunks, bool keyframe) {
        if (!online_ || voiceCid_.isEmpty()) return;
        const auto id = "camera/" + node_->peerId();
        QByteArray encoded;
        for (const auto& chunk : chunks) encoded += chunk;
        const auto image = decoders_[id].decode(frame, keyframe, encoded);
        if (!image.isNull())
            stage_->setCameraFrame(node_->peerId(), image);
        const auto watchers = cameraWatchers_;
        for (const auto& peer : watchers)
            attempt([&] { node_->sendScreenFrame(voiceCid_, voiceChannel_, peer, frame,
                                               chunks, keyframe, true); });
    });
    connect(&cameraCapture_, &ScreenCapture::errorOccurred, this, [this](const QString& error) {
        stopCamera();
        noteError("Camera: " + error);
    });
    capture_.setBacklogProbe([this] { return node_ ? node_->mediaBacklog() : qint64(0); });
    connect(&capture_, &ScreenCapture::frameReady, this,
            [this](quint32 frame, const QList<QByteArray>& chunks, bool keyframe) {
                if (!online_ || voiceCid_.isEmpty())
                    return;
                QByteArray encoded;
                for (const auto& chunk : chunks)
                    encoded += chunk;
                auto image = decoders_[node_->peerId()].decode(frame, keyframe, encoded);
                if (!image.isNull())
                    showFrame(node_->peerId(), image, encoded.size());
                const auto watchers = watchers_;
                for (const auto& peer : watchers)
                    attempt([&] {
                        node_->sendScreenFrame(voiceCid_, voiceChannel_, peer, frame, chunks,
                                               keyframe);
                    });
            });
    streamRetry_.setInterval(2000);
    connect(&streamRetry_, &QTimer::timeout, this, [this] {
        if (!online_ || voiceCid_.isEmpty())
            return;
        const auto now = QDateTime::currentMSecsSinceEpoch();
        for (const auto& peer : cameraWatching_)
            if (node_->online(voiceCid_).contains(peer) &&
                node_->presence(voiceCid_, peer).camera &&
                now - lastScreenFrames_.value("camera/" + peer) > 5000)
                attempt([&] { node_->sendPacket(voiceCid_, voiceChannel_, "watch-camera",
                                               {{"target", peer}, {"enabled", true}}); });
        for (const auto& peer : watching_)
            if (node_->online(voiceCid_).contains(peer) &&
                node_->presence(voiceCid_, peer).sharing &&
                now - lastScreenFrames_.value(peer) > 5000)
                attempt([&] {
                    node_->sendPacket(voiceCid_, voiceChannel_, "watch",
                                      {{"target", peer}, {"enabled", true}});
                });
    });
    connect(&capture_, &ScreenCapture::audioReady, this, [this](const QByteArray& packet) {
        if (!online_ || voiceCid_.isEmpty())
            return;
        const auto bytes = QString::fromLatin1(packet.toBase64());
        const auto watchers = watchers_;
        for (const auto& peer : watchers)
            if (!node_->presence(voiceCid_, peer).deafened)
                attempt([&] {
                    node_->sendPacket(voiceCid_, voiceChannel_, "screen-audio",
                                      {{"target", peer}, {"bytes", bytes}});
                });
    });
    connect(&capture_, &ScreenCapture::audioUnavailable, this, [this](const QString& reason) {
        commandReply("Screen sharing", "Sharing without sound: " + reason);
    });
    connect(&capture_, &ScreenCapture::errorOccurred, this, [this](const QString& error) {
        stopSharing();
        noteError(error);
    });
    refresh();
    render();
    for (const auto& argument : QCoreApplication::arguments())
        if (argument.startsWith("disquisition://join/"))
            pendingLink_ = argument;
    if (node_ && !name_->text().trimmed().isEmpty())
        QTimer::singleShot(0, this, [this] {
            if (!online_)
                connectNode();
        });
}

CommunityPanel::~CommunityPanel() {
    qApp->removeEventFilter(this);
    leaveVoice();
}

void CommunityPanel::releaseStore() {
    // leaveVoice first: capture/audio threads reference node_.
    leaveVoice();
    node_.reset();
    online_ = false;
}

void CommunityPanel::attempt(const std::function<void()>& action) {
    try {
        action();
    }
    catch (const std::exception& error) {
        noteError(QString::fromUtf8(error.what()));
    }
}
void CommunityPanel::note(const QString& message) {
    // Routine status stays out of the footer; only failures surface there.
    Q_UNUSED(message);
    status_->clear();
    status_->hide();
}
void CommunityPanel::noteError(const QString& message) {
    status_->setText(message);
    status_->setToolTip(message);
    status_->setVisible(!message.isEmpty());
}
void CommunityPanel::updateJoining() {
    if (joining_)
        joining_->setVisible(!joiningCommunities_.isEmpty());
}
void CommunityPanel::setSpeaking(const QString& peer, bool speaking) {
    if (speaking == speaking_.contains(peer))
        return;
    if (speaking)
        speaking_.insert(peer);
    else
        speaking_.remove(peer);
    for (QTreeWidgetItemIterator item(tree_); *item; ++item) {
        if ((*item)->data(0, Qt::UserRole + 2).toString() != peer)
            continue;
        if (auto* row = tree_->itemWidget(*item, 0))
            paintSpeaking(row, speaking);
    }
    stage_->setSpeaking(peer, speaking);
}

void CommunityPanel::buildUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    // Takes the place of the system title bar where the app draws under it.
    titleBar_ = new DragBar(this);
    titleBar_->setObjectName("titleBar");
    titleBar_->setAttribute(Qt::WA_StyledBackground);
    titleBar_->setFixedHeight(30);
    auto* titleRow = new QHBoxLayout(titleBar_);
    titleRow->setContentsMargins(80, 0, 80, 0);
    titleRow->setSpacing(7);
    auto* titleMark = new QLabel(titleBar_);
    titleMark->setPixmap(theme::brandMark(14, devicePixelRatioF()));
    windowTitle_ = new QLabel(titleBar_);
    windowTitle_->setObjectName("windowTitle");
    titleRow->addStretch(1);
    titleRow->addWidget(titleMark);
    titleRow->addWidget(windowTitle_);
    titleRow->addStretch(1);
    titleBar_->hide();
    layout->addWidget(titleBar_);
    auto* topBar = new DragBar(this);
    topBar->setObjectName("topBar");
    topBar->setAttribute(Qt::WA_StyledBackground);
    auto* connection = new QHBoxLayout(topBar);
    connection->setContentsMargins(12, 7, 14, 7);
    connection->setSpacing(6);
    brandMark_ = new QLabel(topBar);
    brandMark_->setPixmap(theme::brandMark(18, devicePixelRatioF()));
    brandName_ = new QLabel("disquisition", topBar);
    brandName_->setObjectName("brandName");
    connection->addWidget(brandMark_);
    connection->addWidget(brandName_);
    brandGap_ = new QWidget(topBar);
    brandGap_->setFixedWidth(8);
    connection->addWidget(brandGap_);
    name_ = new QLineEdit(this);
    name_->setObjectName("communityName");
    name_->setPlaceholderText("Your name");
    name_->setMaxLength(64);
    name_->setFixedWidth(180);
    name_->setText(node_ && !node_->profile().name.isEmpty()
                       ? node_->profile().name
                       : QSettings().value("community/name").toString());
    connect_ = button("Connect", "communityConnect", this);
    connect_->setProperty("compact", true);
    connection->addWidget(name_);
    connection->addWidget(connect_);
    connection->addStretch(1);
    connectionStatus_ = new QLabel(topBar);
    connectionStatus_->setObjectName("connectionStatus");
    connection->addWidget(connectionStatus_);
    layout->addWidget(topBar);
    connect(connect_, &QPushButton::clicked, this, &CommunityPanel::connectNode);
    connect(name_, &QLineEdit::returnPressed, this, &CommunityPanel::connectNode);
    auto* split = new QSplitter(this);
    split->setChildrenCollapsible(false);
    auto* sidebar = new QWidget(split);
    sidebar->setObjectName("sidebar");
    sidebar->setAttribute(Qt::WA_StyledBackground);
    auto* side = new QVBoxLayout(sidebar);
    side->setContentsMargins(0, 0, 0, 0);
    side->setSpacing(0);
    auto* actions = new QHBoxLayout;
    actions->setContentsMargins(10, 10, 10, 6);
    actions->setSpacing(6);
    create_ = button("Create", "communityCreate", sidebar);
    join_ = button("Join", "communityJoin", sidebar);
    for (auto* action : {create_, join_})
        action->setProperty("compact", true);
    actions->addWidget(create_);
    actions->addWidget(join_);
    side->addLayout(actions);
    tree_ = new CommunityTree(sidebar);
    tree_->setObjectName("communityTree");
    tree_->setHeaderHidden(true);
    tree_->setMinimumWidth(200);
    tree_->setIndentation(12);
    tree_->setIconSize(QSize(16, 16));
    tree_->setItemDelegate(new ChannelDelegate(tree_));
    tree_->setMouseTracking(true);
    joining_ = new QWidget(sidebar);
    joining_->setObjectName("communityJoining");
    auto* joiningLayout = new QVBoxLayout(joining_);
    joiningLayout->setContentsMargins(12, 2, 12, 6);
    joiningLayout->setSpacing(5);
    auto* joiningLabel = new QLabel("Joining community…", joining_);
    joiningLabel->setObjectName("communityJoiningLabel");
    joiningLayout->addWidget(joiningLabel);
    auto* joiningProgress = new QProgressBar(joining_);
    joiningProgress->setObjectName("communityJoiningProgress");
    joiningProgress->setRange(0, 0);
    joiningProgress->setTextVisible(false);
    joiningProgress->setFixedHeight(4);
    joiningLayout->addWidget(joiningProgress);
    joining_->hide();
    side->addWidget(joining_);
    side->addWidget(tree_, 1);
    auto* userCard = new QWidget(sidebar);
    userCard->setObjectName("userCard");
    userCard->setAttribute(Qt::WA_StyledBackground);
    auto* cardRow = new QHBoxLayout(userCard);
    cardRow->setContentsMargins(10, 8, 6, 8);
    cardRow->setSpacing(9);
    userAvatar_ = new QLabel(userCard);
    userAvatar_->setFixedSize(28, 28);
    cardRow->addWidget(userAvatar_);
    auto* identity = new QVBoxLayout;
    identity->setSpacing(0);
    userName_ = new QLabel(userCard);
    userName_->setObjectName("userCardName");
    userName_->setTextFormat(Qt::PlainText);
    userState_ = new QLabel(userCard);
    userState_->setObjectName("userCardState");
    userState_->setTextFormat(Qt::PlainText);
    identity->addWidget(userName_);
    identity->addWidget(userState_);
    cardRow->addLayout(identity, 1);
    auto* settingsButton = button(QString(), "communitySettings", userCard);
    settingsButton->setProperty("quiet", true);
    settingsButton->setIcon(theme::gearIcon(devicePixelRatioF()));
    settingsButton->setIconSize(QSize(18, 18));
    settingsButton->setFixedSize(28, 28);
    settingsButton->setToolTip("User Settings");
    settingsButton->setAccessibleName("User Settings");
    cardRow->addWidget(settingsButton);
    side->addWidget(userCard);
    connect(settingsButton, &QPushButton::clicked, this, &CommunityPanel::settings);
    connect(static_cast<CommunityTree*>(tree_), &CommunityTree::voiceChannelDoubleClicked, this,
            [this](const QString& cid, const QString& channel) {
                if (cid == voiceCid_ && channel == voiceChannel_)
                    return;
                selectedVoiceCid_ = cid;
                selectedVoiceChannel_ = channel;
                stageOpen_ = true;
                joinVoice();
            });
    connect(static_cast<CommunityTree*>(tree_), &CommunityTree::voiceMoveRequested, this,
            [this](const QString& cid, const QString& peer, const QString& channel) {
                attempt([&] { node_->moveVoice(cid, peer, channel); });
            });
    connect(create_, &QPushButton::clicked, this, &CommunityPanel::create);
    connect(join_, &QPushButton::clicked, this, &CommunityPanel::join);
    connect(tree_, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* item) { select(item); });
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        auto* item = tree_->itemAt(position);
        if (!item || item->parent() || !node_)
            return;
        const auto target = item->data(0, Qt::UserRole).toString();
        const auto model = node_->communities().value(target);
        if (target.isEmpty() || !model.isMember(node_->peerId()) ||
            model.owner == node_->peerId())
            return;
        QMenu menu(this);
        leaveAction_->setData(target);
        leaveAction_->setVisible(true);
        leaveAction_->setEnabled(true);
        menu.addAction(leaveAction_);
        menu.exec(tree_->viewport()->mapToGlobal(position));
        leaveAction_->setData({});
        refresh();
    });
    leaveAction_ = new QAction("Leave community…", this);
    leaveAction_->setObjectName("communityLeave");
    connect(leaveAction_, &QAction::triggered, this, [this] {
        const auto target = leaveAction_->data().toString();
        leaveAction_->setData({});
        leaveCommunity(target);
    });
    auto* conversation = new QWidget(split);
    conversation->setObjectName("conversation");
    conversation->setAttribute(Qt::WA_StyledBackground);
    auto* outer = new QVBoxLayout(conversation);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    views_ = new QStackedWidget(conversation);
    outer->addWidget(views_, 1);
    auto* chatPage = new QWidget(views_);
    auto* chat = new QVBoxLayout(chatPage);
    chat->setContentsMargins(0, 0, 0, 0);
    chat->setSpacing(0);
    views_->addWidget(chatPage);
    auto* stagePage = new QWidget(views_);
    stagePage->setObjectName("stagePage");
    stagePage->setAttribute(Qt::WA_StyledBackground);
    auto* stageLayout = new QVBoxLayout(stagePage);
    stageLayout->setContentsMargins(16, 12, 16, 14);
    stageLayout->setSpacing(12);
    auto* callHeader = new QHBoxLayout;
    callHeader->setSpacing(8);
    auto* callTitles = new QVBoxLayout;
    callTitles->setSpacing(0);
    callTitle_ = new QLabel(stagePage);
    callTitle_->setObjectName("channelHeading");
    callTitle_->setTextFormat(Qt::PlainText);
    callSubtitle_ = new QLabel(stagePage);
    callSubtitle_->setObjectName("channelTopic");
    callSubtitle_->setTextFormat(Qt::PlainText);
    callTitles->addWidget(callTitle_);
    callTitles->addWidget(callSubtitle_);
    callHeader->addLayout(callTitles, 1);
    joinCall_ = button("Join voice", "communityJoinVoice", stagePage);
    joinCall_->setProperty("primary", true);
    callHeader->addWidget(joinCall_, 0, Qt::AlignVCenter);
    stageLayout->addLayout(callHeader);
    connect(joinCall_, &QPushButton::clicked, this, &CommunityPanel::joinVoice);
    views_->addWidget(stagePage);
    auto* header = new QWidget(chatPage);
    header->setObjectName("channelHeader");
    header->setAttribute(Qt::WA_StyledBackground);
    auto* headingRow = new QHBoxLayout(header);
    headingRow->setContentsMargins(20, 8, 14, 8);
    headingRow->setSpacing(6);
    auto* titles = new QVBoxLayout;
    titles->setSpacing(0);
    heading_ = new QLabel("Your communities", header);
    heading_->setObjectName("channelHeading");
    heading_->setTextFormat(Qt::RichText);
    topic_ = new QLabel("Create a community or join with an invite link or QR image.", header);
    topic_->setObjectName("channelTopic");
    topic_->setTextFormat(Qt::PlainText);
    topic_->setWordWrap(true);
    titles->addWidget(heading_);
    titles->addWidget(topic_);
    headingRow->addLayout(titles, 1);
    invite_ = button("Invite", "communityInvite", header);
    manage_ = button("Manage", "communityManage", header);
    for (auto* action : {invite_, manage_}) {
        action->setProperty("compact", true);
        headingRow->addWidget(action, 0, Qt::AlignVCenter);
    }
    chat->addWidget(header);
    connect(invite_, &QPushButton::clicked, this, &CommunityPanel::invite);
    connect(manage_, &QPushButton::clicked, this, &CommunityPanel::manage);
    auto* content = new QSplitter(chatPage);
    content->setChildrenCollapsible(false);
    auto* column = new QWidget(content);
    auto* columnLayout = new QVBoxLayout(column);
    columnLayout->setContentsMargins(0, 0, 0, 0);
    columnLayout->setSpacing(0);
    stage_ = new ScreenStage(stagePage);
    stageLayout->addWidget(stage_, 1);
    // Another channel's call has nobody's shares to show, only its people.
    otherCall_ = new ScreenStage(stagePage);
    otherCall_->setObjectName("otherCall");
    stageLayout->addWidget(otherCall_, 1);
    for (auto* call : {stage_, otherCall_})
        connect(call, &ScreenStage::participantClicked, this,
                [this](const QString& peer, const QPoint& at) {
                    showProfile(selectedVoiceCid_.isEmpty() ? voiceCid_ : selectedVoiceCid_, peer,
                                at);
                });
    transcript_ = new QTextBrowser(column);
    transcript_->setObjectName("communityTranscript");
    transcript_->setMinimumHeight(120);
    transcript_->document()->setDocumentMargin(10);
    columnLayout->addWidget(transcript_, 1);
    composerFrame_ = new QFrame(column);
    composerFrame_->setObjectName("composerFrame");
    auto* compose = new QHBoxLayout(composerFrame_);
    compose->setContentsMargins(10, 3, 4, 3);
    compose->setSpacing(5);
    auto* prefix = new QLabel("#", composerFrame_);
    prefix->setObjectName("composerPrefix");
    composer_ = new QLineEdit(composerFrame_);
    composer_->setObjectName("communityComposer");
    composer_->setPlaceholderText("Message this channel");
    composer_->installEventFilter(this);
    mentions_ = new MentionCompleter(composer_);
    send_ = button("Send", "communitySend", composerFrame_);
    image_ = button("Image", "communityImage", composerFrame_);
    gif_ = button("GIF", "communityGif", composerFrame_);
    send_->setProperty("primary", true);
    for (auto* action : {send_, image_, gif_})
        action->setProperty("compact", true);
    compose->addWidget(prefix);
    compose->addWidget(composer_, 1);
    compose->addWidget(image_);
    compose->addWidget(gif_);
    compose->addWidget(send_);
    auto* composeArea = new QVBoxLayout;
    composeArea->setContentsMargins(16, 4, 16, 12);
    composeArea->setSpacing(4);
    composeArea->addWidget(composerFrame_);
    status_ = new QLabel(column);
    status_->setObjectName("communityStatus");
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::PlainText);
    status_->hide();
    composeArea->addWidget(status_);
    columnLayout->addLayout(composeArea);
    pip_ = new ClickFrame([this] { showCall(true); }, column);
    pip_->setObjectName("streamPip");
    pip_->setToolTip("Open the stream");
    pip_->setFixedSize(304, 171);
    auto* pipLayout = new QGridLayout(pip_);
    pipLayout->setContentsMargins(1, 1, 1, 1);
    pipView_ = new StreamView(pip_);
    pipView_->setAttribute(Qt::WA_TransparentForMouseEvents);
    pipLayout->addWidget(pipView_, 0, 0);
    auto* pipBar = new QHBoxLayout;
    pipBar->setContentsMargins(8, 7, 6, 0);
    pipBar->setSpacing(6);
    auto* pipLive = new QLabel("LIVE", pip_);
    pipLive->setObjectName("live");
    pipName_ = new QLabel(pip_);
    pipName_->setObjectName("pipName");
    pipName_->setTextFormat(Qt::PlainText);
    auto* pipClose = button(QString::fromUtf8("\u2715"), "streamPipClose", pip_);
    pipClose->setToolTip("Hide");
    pipClose->setAccessibleName("Hide picture in picture");
    pipClose->setFixedSize(22, 22);
    pipBar->addWidget(pipLive);
    pipBar->addWidget(pipName_);
    pipBar->addStretch(1);
    pipBar->addWidget(pipClose, 0, Qt::AlignTop);
    pipLayout->addLayout(pipBar, 0, 0, Qt::AlignTop);
    pip_->hide();
    connect(pipClose, &QPushButton::clicked, this, [this] {
        pipDismissed_ = pipPeer_;
        refreshPip();
    });
    transcript_->installEventFilter(this);
    members_ = new QListWidget(content);
    members_->setObjectName("communityMembers");
    members_->setMinimumWidth(180);
    members_->setContextMenuPolicy(Qt::CustomContextMenu);
    members_->setIconSize(QSize(26, 26));
    members_->setItemDelegate(new MemberDelegate(members_));
    members_->setMouseTracking(true);
    members_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(members_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        showProfile(cid_, item->data(Qt::UserRole).toString(),
                    members_->viewport()->mapToGlobal(members_->visualItemRect(item).topLeft()));
    });
    connect(members_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        showProfile(cid_, item->data(Qt::UserRole).toString(),
                    members_->viewport()->mapToGlobal(members_->visualItemRect(item).topLeft()));
    });
    connect(members_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& position) {
        auto* item = members_->itemAt(position);
        if (!item || !capture_.running() || cid_ != voiceCid_)
            return;
        const auto peer = item->data(Qt::UserRole).toString();
        const auto own = drawings_.find(node_->peerId());
        if (peer == node_->peerId() || own == drawings_.end())
            return;
        const bool banned = own->second->isBanned(peer);
        QMenu menu(this);
        auto* toggle = menu.addAction(banned ? "Unban from drawing on my stream"
                                            : "Ban from drawing on my stream");
        if (menu.exec(members_->viewport()->mapToGlobal(position)) == toggle) {
            const auto session = drawings_.find(node_->peerId());
            if (session != drawings_.end())
                session->second->setBanned(peer, !banned);
        }
    });
    content->addWidget(column);
    content->addWidget(members_);
    content->setStretchFactor(0, 1);
    content->setSizes({800, 190});
    chat->addWidget(content, 1);
    voiceControls_ = new QWidget(conversation);
    voiceControls_->setObjectName("communityVoiceControls");
    voiceControls_->setAttribute(Qt::WA_StyledBackground);
    auto* voiceRow = new QHBoxLayout(voiceControls_);
    voiceRow->setContentsMargins(16, 6, 12, 6);
    voiceRow->setSpacing(6);
    auto* voiceDot = new QLabel(voiceControls_);
    voiceDot->setPixmap(statusDot(theme::kOnline, devicePixelRatioF()));
    voiceStatus_ = new QLabel(voiceControls_);
    voiceStatus_->setObjectName("communityVoiceStatus");
    voiceStatus_->setTextFormat(Qt::PlainText);
    voiceDetail_ = new QLabel(voiceControls_);
    voiceDetail_->setObjectName("voiceDetail");
    voiceDetail_->setTextFormat(Qt::PlainText);
    voiceDetail_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    stageToggle_ = button("Watch stream", "communityStageToggle", voiceControls_);
    stageToggle_->hide();
    voice_ = button("Leave voice", "communityVoice", voiceControls_);
    mute_ = button("Mute", "communityMute", voiceControls_);
    deafen_ = button("Deafen", "communityDeafen", voiceControls_);
    share_ = button("Share screen", "communityShare", voiceControls_);
    camera_ = button("Turn camera on", "communityCamera", voiceControls_);
    share_->setProperty("primary", true);
    mute_->setCheckable(true);
    deafen_->setCheckable(true);
    voiceRow->addWidget(voiceDot);
    voiceRow->addWidget(voiceStatus_);
    voiceRow->addSpacing(6);
    voiceRow->addWidget(voiceDetail_, 1);
    for (auto* action : {stageToggle_, voice_, mute_, deafen_, camera_, share_}) {
        action->setProperty("compact", true);
        voiceRow->addWidget(action);
    }
    outer->addWidget(voiceControls_);
    voiceControls_->hide();
    split->addWidget(sidebar);
    split->addWidget(conversation);
    split->setStretchFactor(1, 1);
    split->setSizes({228, 1012});
    layout->addWidget(split, 1);
    connect(voice_, &QPushButton::clicked, this, &CommunityPanel::leaveVoice);
    connect(mute_, &QPushButton::toggled, this, &CommunityPanel::voiceState);
    connect(deafen_, &QPushButton::toggled, this, &CommunityPanel::voiceState);
    connect(share_, &QPushButton::clicked, this, &CommunityPanel::share);
    connect(camera_, &QPushButton::clicked, this, &CommunityPanel::toggleCamera);
    connect(stage_, &ScreenStage::watchRequested, this, [this](const QString& peer) {
        watch(peer, true);
        showCall(true);
    });
    connect(stageToggle_, &QPushButton::clicked, this, [this] { showCall(!stageOpen_); });
    connect(stage_, &ScreenStage::stopWatchingRequested, this,
            [this](const QString& peer) { watch(peer, false); });
    connect(stage_, &ScreenStage::volumeChanged, this, [this](const QString& peer, int percent) {
        QSettings().setValue("community/streamVolume/" + peer, percent);
        setStreamVolume(peer, percent);
    });
    connect(send_, &QPushButton::clicked, this, &CommunityPanel::sendChat);
    connect(composer_, &QLineEdit::returnPressed, this, &CommunityPanel::sendChat);
    connect(image_, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(
            this, "Send an image", {}, "Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp)");
        if (!path.isEmpty())
            prepareUpload([path] { return prepareImageFile(path); });
    });
    addAction(notifications_->muteAction());
    auto* network = new QNetworkAccessManager(this);
    auto* fetcher = new MediaFetcher(network, this);
    media_ = new ChatMedia(transcript_, fetcher, this);
    connect(transcript_, &QTextBrowser::anchorClicked, this, [this](const QUrl& link) {
        if (link.scheme() == "profile")
            showProfile(link.host(), link.path().mid(1), QCursor::pos());
        else if ((link.scheme() == "http" || link.scheme() == "https") &&
                 link.isValid() && !link.host().isEmpty())
            QDesktopServices::openUrl(link);
    });
    gifSearch_ = new GifSearch(network, this);
    QSettings settings;
    auto customer = settings.value("gifs/klipyCustomerId").toString();
    if (customer.isEmpty()) {
        customer = freshId();
        settings.setValue("gifs/klipyCustomerId", customer);
    }
    gifSearch_->setKeys(DISQUISITION_GIPHY_API_KEY, DISQUISITION_KLIPY_API_KEY, customer);
    gifPicker_ = new GifPicker(gifSearch_, fetcher, this);
    connect(gif_, &QPushButton::clicked, this, [this] { gifPicker_->popup(gif_); });
    connect(gifPicker_, &GifPicker::chosen, this, [this](const QUrl& url) {
        composer_->setText(url.toString());
        sendChat();
    });
    gif_->setVisible(gifSearch_->hasKey(GifProvider::Giphy) ||
                     gifSearch_->hasKey(GifProvider::Klipy));
    setAcceptDrops(true);
}

void CommunityPanel::connectNode() {
    if (!node_) {
        if (!failure_.isEmpty())
            noteError(failure_);
        return;
    }
    if (online_) {
        leaveVoice();
        node_->stop();
        online_ = false;
        uploads_.clear();
        images_.clear();
        joiningCommunities_.clear();
        updateJoining();
        note("Disconnected. Saved communities remain available.");
    }
    else {
        QSettings settings;
        const auto port = settings.value("community/port", 0).toUInt();
        online_ =
            node_->start(name_->text().trimmed(), settings.value("community/host").toString(),
                         static_cast<quint16>(port),
                         settings.value("community/relay", kDefaultRelay).toString());
        if (online_) {
            settings.setValue("community/name", name_->text().trimmed());
            joiningCommunities_ = node_->pendingCommunities();
            updateJoining();
            openHistory();
            const QUrl endpoint(node_->endpoint());
            note(endpoint.scheme() == "relay"
                     ? "Using relay " + endpoint.authority() +
                           ". Communities reconnect automatically."
                     : "Listening for direct connections at " + endpoint.authority());
            if (!pendingLink_.isEmpty()) {
                const auto link = pendingLink_;
                pendingLink_.clear();
                acceptInvite(link);
            }
        }
    }
    refresh();
}

void CommunityPanel::settings() {
    QSettings saved;
    QDialog dialog(this);
    dialog.setObjectName("userSettings");
    dialog.setWindowTitle("User Settings");
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* body = new QWidget(&dialog);
    auto* columns = new QHBoxLayout(body);
    columns->setContentsMargins(0, 0, 0, 0);
    columns->setSpacing(0);
    auto* categories = new QListWidget(body);
    categories->setObjectName("userSettingsCategories");
    categories->setAccessibleName("Settings categories");
    categories->setFixedWidth(180);
    auto* pages = new QStackedWidget(body);
    pages->setObjectName("userSettingsPages");
    columns->addWidget(categories);
    columns->addWidget(pages, 1);
    layout->addWidget(body, 1);
    const auto page = [&](const QString& title, const QString& description) {
        categories->addItem(title);
        auto* scroll = new QScrollArea(pages);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto* content = new QWidget(scroll);
        auto* section = new QVBoxLayout(content);
        section->setContentsMargins(28, 24, 28, 24);
        section->setSpacing(16);
        auto* heading = new QLabel(title, content);
        heading->setObjectName("settingsHeading");
        section->addWidget(heading);
        auto* help = new QLabel(description, content);
        help->setObjectName("settingsDescription");
        help->setWordWrap(true);
        section->addWidget(help);
        auto* form = new QFormLayout;
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setHorizontalSpacing(16);
        form->setVerticalSpacing(16);
        section->addLayout(form);
        section->addStretch();
        scroll->setWidget(content);
        pages->addWidget(scroll);
        return form;
    };
    auto* userForm = page("User", "Your profile is shared with members of your communities.");
    auto* color = new QLineEdit(messageColor_, &dialog);
    color->setObjectName("userChatColor");
    color->setToolTip("pink, mint, butter, periwinkle, lilac, aqua, peach, or an allowed 0-255 color");
    userForm->addRow("Chat color", color);
    auto* showPenguins = new QCheckBox("Show desktop penguins for this community", &dialog);
    showPenguins->setObjectName("communityShowPenguins");
    showPenguins->setChecked(penguinsAction_->isChecked());
    showPenguins->setToolTip("Penguins appear above other apps and let clicks pass through.");
    auto* wander = new QCheckBox("Let penguins wander", &dialog);
    wander->setChecked(saved.value("penguins/wander", true).toBool());
    auto* idle = new QSpinBox(&dialog);
    idle->setRange(0, 1440);
    idle->setSuffix(" min");
    idle->setSpecialValueText("Never");
    idle->setValue(qBound(0, saved.value("penguins/idleMinutes", 10).toInt(), 1440));
    auto* spriteEditor = new PenguinSpriteEditor(PenguinSprites::fromSettings(saved), &dialog);
    spriteEditor->setObjectName("communitySpriteEditor");
    userForm->addRow(showPenguins);
    userForm->addRow(wander);
    userForm->addRow("Hide inactive penguins after", idle);
    userForm->addRow("Custom sprite", spriteEditor);
    const bool unlocked = saved.value("penguins/unlocked", false).toBool();
    for (QWidget* widget : QList<QWidget*>{showPenguins, wander, idle, spriteEditor})
        userForm->setRowVisible(widget, unlocked);
    wander->setEnabled(showPenguins->isChecked());
    idle->setEnabled(showPenguins->isChecked());
    connect(showPenguins, &QCheckBox::toggled, wander, &QWidget::setEnabled);
    connect(showPenguins, &QCheckBox::toggled, idle, &QWidget::setEnabled);
    auto* audioForm = page("Audio", "Choose your microphone, output device, and how you talk.");
    auto* notificationForm = page("Notifications",
                                  "Choose which messages and activity get your attention.");
    auto* connectionForm =
        page("Connection",
             online_ ? "Disconnect before changing your relay or direct connection."
                     : "Use a relay to connect, or leave Relay blank for direct connections.");
    auto* historyForm = page("Message History",
                             "Optionally save chat on this device. Missed messages are not "
                             "downloaded. Community metadata is always saved.");
    connect(categories, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
    categories->setCurrentRow(0);

    const auto current = node_ ? node_->profile() : community::Profile{};
    QByteArray avatar = current.avatar;
    auto* displayName = new QLineEdit(current.name.isEmpty() ? name_->text() : current.name,
                                      &dialog);
    displayName->setObjectName("userDisplayName");
    displayName->setMaxLength(64);
    displayName->setPlaceholderText("How people see you");
    auto* bio = new QPlainTextEdit(current.bio, &dialog);
    bio->setObjectName("userBio");
    bio->setAccessibleName("Bio");
    bio->setPlaceholderText("A little about you");
    bio->setMinimumHeight(104);
    bio->setMaximumHeight(144);
    auto* count = new QLabel(&dialog);
    count->setObjectName("userBioCount");
    count->setAlignment(Qt::AlignRight);
    const auto updateCount = [=] {
        const auto length = bio->toPlainText().size();
        count->setText(QString::number(length) + " / " + QString::number(community::kMaxBioLength));
        count->setStyleSheet(length > community::kMaxBioLength ? "color:#ffb3b3;"
                                                               : "color:#a6b5bd;");
    };
    connect(bio, &QPlainTextEdit::textChanged, &dialog, updateCount);
    updateCount();
    auto* picture = new QWidget(&dialog);
    auto* pictureRow = new QHBoxLayout(picture);
    pictureRow->setContentsMargins(0, 0, 0, 0);
    pictureRow->setSpacing(16);
    auto* preview = new QLabel(picture);
    preview->setObjectName("userAvatarPreview");
    preview->setFixedSize(80, 80);
    preview->setStyleSheet("padding:0; background:transparent;");
    auto* pictureActions = new QVBoxLayout;
    auto* choose = button("Change picture", "userChooseAvatar", picture);
    auto* remove = button("Remove picture", "userRemoveAvatar", picture);
    pictureActions->addWidget(choose);
    pictureActions->addWidget(remove);
    pictureRow->addWidget(preview);
    pictureRow->addLayout(pictureActions);
    pictureRow->addStretch();
    const auto updatePreview = [&] {
        community::Member member;
        member.name = displayName->text();
        member.avatar = avatar;
        preview->setPixmap(profileAvatar(member, 80, devicePixelRatioF()));
        remove->setEnabled(!avatar.isEmpty());
    };
    connect(displayName, &QLineEdit::textChanged, &dialog, updatePreview);
    connect(remove, &QPushButton::clicked, &dialog, [&] {
        avatar.clear();
        updatePreview();
    });
    updatePreview();
    userForm->addRow("Profile picture", picture);
    userForm->addRow("Display name", displayName);
    userForm->addRow("Bio", bio);
    userForm->addRow(count);
    auto* footerWidget = new QWidget(&dialog);
    footerWidget->setObjectName("dialogFooter");
    footerWidget->setAttribute(Qt::WA_StyledBackground);
    auto* footer = new QHBoxLayout(footerWidget);
    footer->setContentsMargins(20, 12, 20, 12);
    auto* error = new QLabel(footerWidget);
    error->setObjectName("userSettingsError");
    error->setTextFormat(Qt::PlainText);
    error->setWordWrap(true);
    error->setStyleSheet("color:#ffb3b3;");
    error->hide();
    footer->addWidget(error, 1);
    connect(choose, &QPushButton::clicked, &dialog, [&] {
        const auto path = QFileDialog::getOpenFileName(&dialog, "Choose a profile picture", {},
                                                       "Images (*.png *.jpg *.jpeg *.webp *.bmp)");
        if (path.isEmpty())
            return;
        const auto prepared = prepareImageFile(path);
        auto image = QImage::fromData(prepared.bytes);
        if (image.isNull()) {
            error->setText(prepared.error.isEmpty() ? "This image could not be opened."
                                                    : prepared.error);
            error->show();
            return;
        }
        const int side = std::min(image.width(), image.height());
        image = image.copy((image.width() - side) / 2, (image.height() - side) / 2, side, side)
                    .scaled(128, 128, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        if (bytes.size() > community::kMaxAvatarBytes) {
            bytes.clear();
            buffer.seek(0);
            image.save(&buffer, "JPEG", 85);
        }
        if (bytes.isEmpty() || bytes.size() > community::kMaxAvatarBytes) {
            error->setText("This picture could not be prepared. Try another image.");
            error->show();
            return;
        }
        avatar = bytes;
        error->hide();
        updatePreview();
    });
    auto relayAddress = saved.value("community/relay", kDefaultRelay).toString();
    if (relayAddress.startsWith("relay://"))
        relayAddress.remove(0, 8);
    auto* relay = new QLineEdit(relayAddress, &dialog);
    relay->setObjectName("communityRelayAddress");
    relay->setAccessibleName("Relay");
    relay->setPlaceholderText("relay.disquisition.app, or leave blank for direct connections");
    auto* host = new QLineEdit(saved.value("community/host").toString(), &dialog);
    host->setPlaceholderText("Automatic LAN address");
    auto* port = new QSpinBox(&dialog);
    port->setRange(0, 65535);
    port->setSpecialValueText("Automatic");
    port->setValue(saved.value("community/port", 0).toInt());
    relay->setEnabled(!online_);
    host->setEnabled(!online_);
    port->setEnabled(!online_);
    auto* resetRelay = new QPushButton("Reset to default", &dialog);
    resetRelay->setObjectName("communityResetRelay");
    resetRelay->setToolTip("Use " + kDefaultRelay);
    const auto updateResetRelay = [this, relay, resetRelay] {
        resetRelay->setEnabled(!online_ && relay->text().trimmed() != kDefaultRelay);
    };
    connect(relay, &QLineEdit::textChanged, &dialog, updateResetRelay);
    connect(resetRelay, &QPushButton::clicked, relay,
            [relay] { relay->setText(kDefaultRelay); });
    updateResetRelay();
    auto* relayRow = new QHBoxLayout;
    relayRow->addWidget(relay, 1);
    relayRow->addWidget(resetRelay);
    connectionForm->addRow("Relay", relayRow);
    connectionForm->addRow("Direct advertised host", host);
    connectionForm->addRow("Direct listen port", port);
    auto* saving = new QCheckBox("Save messages received or sent by this client", &dialog);
    saving->setObjectName("communitySaveMessages");
    saving->setChecked(saved.value("messages/enabled", false).toBool());
    auto* path = new QLineEdit(
        saved.value("messages/file", QDir::homePath() + "/disquisition-messages.db").toString(),
        &dialog);
    path->setObjectName("communityMessageFile");
    auto* maximum = new QLineEdit(saved.value("messages/maximum").toString(), &dialog);
    maximum->setPlaceholderText("Unlimited");
    auto* linkedMedia = new QCheckBox("Show linked GIFs and images in chat", &dialog);
    linkedMedia->setObjectName("communityShowLinkedMedia");
    linkedMedia->setChecked(saved.value("gifs/showLinkedMedia", true).toBool());
    historyForm->addRow(linkedMedia);
    historyForm->addRow(saving);
    historyForm->addRow("SQLite file", path);
    historyForm->addRow("Maximum saved messages", maximum);
    path->setEnabled(saving->isChecked());
    maximum->setEnabled(saving->isChecked());
    connect(saving, &QCheckBox::toggled, path, &QWidget::setEnabled);
    connect(saving, &QCheckBox::toggled, maximum, &QWidget::setEnabled);
    auto* input = new QComboBox(&dialog);
    auto* output = new QComboBox(&dialog);
    input->setObjectName("communityInputDevice");
    output->setObjectName("communityOutputDevice");
    input->addItem("System default", "");
    output->addItem("System default", "");
    for (const auto& device : QMediaDevices::audioInputs())
        input->addItem(device.description(), QString::fromUtf8(device.id()));
    for (const auto& device : QMediaDevices::audioOutputs())
        output->addItem(device.description(), QString::fromUtf8(device.id()));
    input->setCurrentIndex(std::max(0, input->findData(saved.value("community/input", ""))));
    output->setCurrentIndex(std::max(0, output->findData(saved.value("community/output", ""))));
    audioForm->addRow("Microphone", input);
    audioForm->addRow("Audio output", output);
    auto* audioHelp = new QLabel(voiceCid_.isEmpty()
                                     ? "Audio devices apply the next time you join voice."
                                     : "Changing audio devices reconnects audio without leaving "
                                       "the call.",
                                 &dialog);
    audioHelp->setWordWrap(true);
    audioForm->addRow(audioHelp);
    auto* ptt = new QCheckBox("Hold a key to talk", &dialog);
    ptt->setObjectName("communityPushToTalk");
    ptt->setChecked(saved.value("voice/pushToTalk", false).toBool());
    auto* pttKey = new PushToTalkKeyButton(&dialog);
    pttKey->setObjectName("communityPushToTalkKey");
    pttKey->setKey(saved.contains("voice/pushToTalkKey")
                       ? std::optional<quint32>(saved.value("voice/pushToTalkKey").toUInt())
                       : std::nullopt, saved.value("voice/pushToTalkKeyName").toString());
    pttKey->setEnabled(ptt->isChecked());
    connect(ptt, &QCheckBox::toggled, pttKey, &QWidget::setEnabled);
    audioForm->addRow(ptt, pttKey);
    auto* pttHelp = new QLabel("Global push-to-talk needs Input Monitoring on macOS. "
                              "Restart after granting access. On Wayland it works while the app is focused.", &dialog);
    pttHelp->setWordWrap(true);
    audioForm->addRow(pttHelp);
    auto* muted = new QCheckBox("Mute all notifications", &dialog);
    muted->setObjectName("communityMuteNotifications");
    muted->setChecked(notifications_->settings().muted);
    auto* level = new QComboBox(&dialog);
    level->addItem("All messages", int(NotifyLevel::AllMessages));
    level->addItem("Only @mentions", int(NotifyLevel::MentionsOnly));
    level->setCurrentIndex(level->findData(int(notifications_->settings().level)));
    auto* suppress = new QCheckBox("Suppress @everyone and @here", &dialog);
    suppress->setChecked(notifications_->settings().suppressEveryone);
    auto* activity = new QCheckBox("Notify on joins and screen shares", &dialog);
    activity->setChecked(notifications_->settings().activity);
    notificationForm->addRow(muted);
    notificationForm->addRow("Notify for", level);
    notificationForm->addRow(suppress);
    notificationForm->addRow(activity);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel,
                                         &dialog);
    footer->addWidget(buttons);
    setFlag(buttons->button(QDialogButtonBox::Save), "primary", true);
    layout->addWidget(footerWidget);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        try {
            if (!validChatColor(color->text().trimmed().toLower()))
                throw std::runtime_error("Choose a valid chat color. Use /color for available colors.");
            const auto sprites = spriteEditor->sprites();
            if (unlocked && sprites.enabled && !sprites.complete())
                throw std::runtime_error("Upload both a standing and walking image, or turn custom sprites off.");
            const community::Profile profile{displayName->text().trimmed(), bio->toPlainText(),
                                             avatar};
            community::Profile::fromJson(profile.toJson());
            if (ptt->isChecked() && !pttKey->key())
                throw std::runtime_error("Choose a push-to-talk key before enabling it.");
            const auto address = relay->text().trimmed();
            community::normalizeRelayEndpoint(address);
            bool validMaximum = false;
            const auto count = maximum->text().toLongLong(&validMaximum);
            if (!maximum->text().trimmed().isEmpty() && (!validMaximum || count <= 0))
                throw std::runtime_error("Maximum messages must be a positive number or blank.");
            if (saving->isChecked() && path->text().trimmed().isEmpty())
                throw std::runtime_error("Choose a message file.");
            std::unique_ptr<chat::RecentMessages> replacement;
            if (saving->isChecked())
                replacement = std::make_unique<chat::RecentMessages>(
                    path->text().trimmed().toStdString(), maximum->text().trimmed().isEmpty()
                                                              ? std::nullopt
                                                              : std::optional<std::int64_t>(count));
            if (!node_)
                throw std::runtime_error("The local identity is unavailable.");
            node_->setProfile(profile);
            name_->setText(profile.name);
            saved.setValue("community/name", profile.name);
            runCommand("/color " + color->text().trimmed());
            saved.setValue("voice/pushToTalk", ptt->isChecked());
            if (pttKey->key()) {
                saved.setValue("voice/pushToTalkKey", *pttKey->key());
                saved.setValue("voice/pushToTalkKeyName", pttKey->keyName());
            }
            notifications_->setSettings({muted->isChecked(), NotifyLevel(level->currentData().toInt()),
                                         suppress->isChecked(), activity->isChecked()});
            updatePushToTalk();
            const auto inputId = input->currentData().toString();
            const auto outputId = output->currentData().toString();
            const auto previousInput = saved.value("community/input").toString();
            const auto previousOutput = saved.value("community/output").toString();
            if (!voiceCid_.isEmpty() &&
                (inputId != previousInput || outputId != previousOutput)) {
                if (!audio_.start(inputId, outputId)) {
                    if (!audio_.start(previousInput, previousOutput) && !audio_.start({}, {})) {
                        leaveVoice();
                        throw std::runtime_error(
                            "The selected audio devices are unavailable, and the call could not "
                            "be restored. You have left voice; choose available devices and join "
                            "again.");
                    }
                    voiceState();
                    throw std::runtime_error(
                        "The selected audio devices are unavailable. The call is using other "
                        "devices.");
                }
            }
            voiceState();
            saved.setValue("community/relay", address);
            saved.setValue("community/host", host->text().trimmed());
            saved.setValue("community/port", port->value());
            saved.setValue("community/input", inputId);
            saved.setValue("community/output", outputId);
            if (outputId != previousOutput)
                screenAudio_.setOutputDevice(outputId);
            saved.setValue("gifs/showLinkedMedia", linkedMedia->isChecked());
            saved.setValue("messages/enabled", saving->isChecked());
            saved.setValue("messages/file", path->text().trimmed());
            saved.setValue("messages/maximum", maximum->text().trimmed());
            if (unlocked) {
                sprites.save(saved);
                penguinsAction_->setChecked(showPenguins->isChecked());
                saved.setValue("penguins/wander", wander->isChecked());
                saved.setValue("penguins/idleMinutes", idle->value());
                penguins_->setWandering(wander->isChecked());
                penguins_->setIdleTimeoutMinutes(idle->value());
                if (online_)
                    for (const auto& community : node_->communities()) publishSprite(community.id);
                refreshPenguins();
            }
            history_ = std::move(replacement);
            historyLoaded_.clear();
            loadHistory(cid_, channel_);
            render();
            dialog.accept();
        }
        catch (const std::exception& exception) {
            error->setText(QString::fromUtf8(exception.what()));
            error->show();
        }
    });
    dialog.resize(820, 660);
    dialog.setMinimumSize(680, 520);
    dialog.exec();
}

void CommunityPanel::create() {
    bool ok = false;
    const auto name = QInputDialog::getText(this, "Create community", "Community name",
                                            QLineEdit::Normal, {}, &ok);
    if (ok && !name.trimmed().isEmpty())
        attempt([&] { selectCommunity(node_->createCommunity(name)); });
}

void CommunityPanel::showProfile(const QString& cid, const QString& peer, const QPoint& position) {
    if (!node_ || !node_->communities().value(cid).isMember(peer))
        return;
    if (auto* previous = findChild<QDialog*>("userProfileCard"))
        previous->close();
    const auto model = node_->communities().value(cid);
    const auto member = model.members.value(peer);
    auto* card = new QDialog(this, Qt::Popup);
    card->setObjectName("userProfileCard");
    card->setAttribute(Qt::WA_DeleteOnClose);
    card->setWindowTitle(member.name + "'s profile");
    card->setFixedWidth(340);
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);
    auto* avatar = new QLabel(card);
    avatar->setObjectName("profileAvatar");
    avatar->setFixedSize(80, 80);
    avatar->setPixmap(profileAvatar(member, 80, devicePixelRatioF()));
    avatar->setStyleSheet("padding:0; background:transparent;");
    layout->addWidget(avatar);
    auto* displayName = new QLabel(member.name, card);
    displayName->setObjectName("profileDisplayName");
    displayName->setTextFormat(Qt::PlainText);
    displayName->setWordWrap(true);
    displayName->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(displayName);
    const auto role = peer == model.owner ? "Owner" : member.admin ? "Admin" : "Member";
    const auto state = online_ && node_->online(cid).contains(peer) ? "Online" : "Offline";
    auto* detail = new QLabel(QString(state) + " · " + role, card);
    detail->setObjectName("settingsDescription");
    layout->addWidget(detail);
    auto* bio = new QLabel(member.bio.isEmpty() ? "No bio yet." : member.bio, card);
    bio->setObjectName("profileBio");
    bio->setTextFormat(Qt::PlainText);
    bio->setWordWrap(true);
    bio->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* scroll = new QScrollArea(card);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setWidget(bio);
    scroll->setFixedHeight(std::clamp(bio->heightForWidth(280) + 16, 56, 200));
    layout->addWidget(scroll);
    if (peer == node_->peerId()) {
        auto* edit = button("Edit profile", "profileEdit", card);
        layout->addWidget(edit);
        connect(edit, &QPushButton::clicked, this, [this, card] {
            card->close();
            settings();
        });
    }
    card->adjustSize();
    const auto* screen = QGuiApplication::screenAt(position);
    const auto bounds = (screen ? screen : QGuiApplication::primaryScreen())->availableGeometry();
    card->move(std::clamp(position.x(), bounds.left(),
                          std::max(bounds.left(), bounds.right() - card->width())),
               std::clamp(position.y(), bounds.top(),
                          std::max(bounds.top(), bounds.bottom() - card->height())));
    card->show();
}

void CommunityPanel::acceptInvite(const QString& link) {
    emit activated();
    if (!online_) {
        pendingLink_ = link;
        note("Enter your name and connect to accept this invite.");
        return;
    }
    attempt([&] {
        const auto invite = community::Invite::fromLink(link);
        joiningCommunities_.insert(invite.community);
        updateJoining();
        try {
            node_->joinCommunity(link);
        }
        catch (...) {
            joiningCommunities_.remove(invite.community);
            updateJoining();
            throw;
        }
        note("Looking for a community host. Retrying automatically if nobody is reachable.");
    });
}

void CommunityPanel::join() {
    QDialog dialog(this);
    dialog.setObjectName("joinDialog");
    dialog.setWindowTitle("Join a community");
    const auto frame =
        dialogFrame(dialog, "Join a community", "Paste an invite link, or open a QR code image.");
    frame.body->addWidget(fieldLabel("Invite link", &dialog));
    auto* link = new QPlainTextEdit(&dialog);
    link->setObjectName("communityInviteInput");
    link->setPlaceholderText("disquisition://join/...");
    link->setFont(monoFont(13));
    link->setFixedHeight(96);
    frame.body->addWidget(link);
    auto* qr = button("Open QR image", "communityOpenQr", &dialog);
    qr->setProperty("compact", true);
    frame.body->addWidget(qr, 0, Qt::AlignLeft);
    connect(qr, &QPushButton::clicked, &dialog, [&] {
        const auto path = QFileDialog::getOpenFileName(&dialog, "Open invite QR image", {},
                                                       "Images (*.png *.jpg *.jpeg *.webp *.bmp)");
        if (path.isEmpty())
            return;
        attempt([&] {
            const auto prepared = prepareImageFile(path);
            if (!prepared.error.isEmpty())
                throw std::runtime_error(prepared.error.toStdString());
            link->setPlainText(community::readInviteQr(QImage::fromData(prepared.bytes)));
        });
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    frame.footer->addStretch(1);
    frame.footer->addWidget(buttons);
    buttons->button(QDialogButtonBox::Ok)->setText("Join community");
    setFlag(buttons->button(QDialogButtonBox::Ok), "primary", true);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        attempt([&] {
            community::Invite::fromLink(link->toPlainText().trimmed());
            acceptInvite(link->toPlainText().trimmed());
            dialog.accept();
        });
    });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.resize(600, 380);
    dialog.exec();
}

void CommunityPanel::invite() {
    attempt([&] {
        const auto link = node_->invite(cid_);
        const auto name = node_->communities()[cid_].name;
        QDialog dialog(this);
        dialog.setObjectName("inviteDialog");
        dialog.setWindowTitle("Invite to " + name);
        const auto frame = dialogFrame(dialog, "Invite people in",
                                       "Share a link or a QR code to bring people into " + name +
                                           ".");
        auto* columns = new QHBoxLayout;
        columns->setSpacing(28);
        frame.body->addLayout(columns, 1);
        auto* left = new QVBoxLayout;
        left->setSpacing(10);
        columns->addLayout(left, 1);
        left->addWidget(fieldLabel("Invite link", &dialog));
        auto* linkRow = new QHBoxLayout;
        linkRow->setSpacing(8);
        auto* code = new QLineEdit(link, &dialog);
        code->setObjectName("communityInviteLink");
        code->setReadOnly(true);
        code->setFont(monoFont(13));
        code->setCursorPosition(0);
        code->setMinimumHeight(40);
        linkRow->addWidget(code, 1);
        auto* copy = button("Copy", "communityCopyInvite", &dialog);
        copy->setProperty("primary", true);
        copy->setMinimumSize(76, 40);
        linkRow->addWidget(copy);
        left->addLayout(linkRow);
        auto* hint = new QLabel(
            "Anyone with this link can join as a member while the community exists.", &dialog);
        hint->setObjectName("fieldHint");
        hint->setWordWrap(true);
        left->addWidget(hint);
        left->addStretch(1);
        auto* save = button("Save QR image", "communitySaveQr", &dialog);
        save->setProperty("compact", true);
        left->addWidget(save, 0, Qt::AlignLeft);
        const auto qr = community::inviteQr(link);
        auto* card = new QFrame(&dialog);
        card->setObjectName("inviteQr");
        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(14, 14, 14, 12);
        cardLayout->setSpacing(8);
        auto* image = new QLabel(card);
        image->setPixmap(
            QPixmap::fromImage(qr).scaled(176, 176, Qt::KeepAspectRatio, Qt::FastTransformation));
        image->setAlignment(Qt::AlignCenter);
        cardLayout->addWidget(image);
        auto* caption = new QLabel("scan to join", card);
        caption->setAlignment(Qt::AlignCenter);
        cardLayout->addWidget(caption);
        columns->addWidget(card, 0, Qt::AlignTop);
        connect(copy, &QPushButton::clicked, &dialog, [link, copy] {
            QApplication::clipboard()->setText(link);
            copy->setText("Copied");
            QTimer::singleShot(1500, copy, [copy] { copy->setText("Copy"); });
        });
        connect(save, &QPushButton::clicked, &dialog, [&] {
            const auto path = QFileDialog::getSaveFileName(&dialog, "Save invite QR",
                                                           "community-invite.png",
                                                           "PNG image (*.png)");
            if (!path.isEmpty() && !qr.save(path))
                noteError("Could not save QR image.");
        });
        auto* note = new QLabel("Invites are signed with the community's key.", &dialog);
        note->setObjectName("footerNote");
        frame.footer->addWidget(note, 1);
        auto* done = button("Done", "communityInviteDone", &dialog);
        done->setProperty("primary", true);
        done->setDefault(true);
        frame.footer->addWidget(done);
        connect(done, &QPushButton::clicked, &dialog, &QDialog::accept);
        dialog.resize(740, 440);
        dialog.exec();
    });
}

void CommunityPanel::manage() {
    if (!node_ || !node_->communities().value(cid_).canAdmin(node_->peerId()))
        return;
    const auto cid = cid_;
    const auto model = node_->communities().value(cid);
    QDialog dialog(this);
    dialog.setWindowTitle("Manage " + model.name);
    dialog.setObjectName("communityManageDialog");
    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* content = new QHBoxLayout;
    content->setSpacing(0);
    auto* categories = new QListWidget(&dialog);
    categories->setObjectName("communityManageCategories");
    categories->setFixedWidth(190);
    auto* tabs = new QStackedWidget(&dialog);
    tabs->setObjectName("communityManagePages");
    content->addWidget(categories);
    content->addWidget(tabs, 1);
    layout->addLayout(content, 1);
    connect(categories, &QListWidget::currentRowChanged, tabs, &QStackedWidget::setCurrentIndex);
    const auto addPage = [&](QWidget* page, const QString& title) {
        auto* heading = new QLabel(title, page);
        heading->setObjectName("settingsHeading");
        page->layout()->setContentsMargins(28, 22, 28, 18);
        if (auto* form = qobject_cast<QFormLayout*>(page->layout())) {
            form->insertRow(0, heading);
            form->setVerticalSpacing(14);
            form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        }
        else if (auto* vertical = qobject_cast<QVBoxLayout*>(page->layout()))
            vertical->insertWidget(0, heading);
        categories->addItem(title);
        tabs->addWidget(page);
    };
    auto* result = new QLabel(&dialog);
    result->setWordWrap(true);
    const auto apply = [&](const QString& kind, const QJsonObject& data) {
        try {
            node_->edit(cid, kind, data);
            result->setText("Saved. Updates are being distributed to connected members.");
            return true;
        }
        catch (const std::exception& error) {
            result->setText(QString::fromUtf8(error.what()));
            return false;
        }
    };
    auto* general = new QWidget(tabs);
    auto* form = new QFormLayout(general);
    auto* name = new QLineEdit(model.name, general);
    auto* saveName = button("Save name", "communitySaveName", general);
    form->addRow("Community name", name);
    form->addRow(saveName);
    connect(saveName, &QPushButton::clicked, &dialog,
            [&] { apply("rename", {{"name", name->text().trimmed()}}); });
    auto* icon = button("Choose community icon", "communityChooseIcon", general);
    form->addRow(icon);
    connect(icon, &QPushButton::clicked, &dialog, [&] {
        const auto path = QFileDialog::getOpenFileName(&dialog, "Community icon", {},
                                                       "Images (*.png *.jpg *.jpeg *.webp)");
        if (path.isEmpty())
            return;
        const auto prepared = prepareImageFile(path);
        if (!prepared.error.isEmpty()) {
            result->setText(prepared.error);
            return;
        }
        const auto image = QImage::fromData(prepared.bytes)
                               .scaled(128, 128, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        apply("icon", {{"bytes", QString::fromLatin1(bytes.toBase64())}});
    });
    auto* ownershipHelp =
        new QLabel("The creator keeps ownership. Admins can make changes independently. "
                   "Any member can host and accept existing valid invites.",
                   general);
    ownershipHelp->setWordWrap(true);
    form->addRow(ownershipHelp);
    if (model.owner == node_->peerId()) {
        auto* deletionHelp = new QLabel(
            "Deleting this community removes it for all members. This cannot be undone.", general);
        deletionHelp->setWordWrap(true);
        auto* remove = button("Delete community…", "communityDelete", general);
        remove->setStyleSheet("color:#ffb3b3; border-color:#e05252;");
        form->addRow(deletionHelp);
        form->addRow(remove);
        connect(remove, &QPushButton::clicked, &dialog, [this, &dialog, cid] {
            if (deleteCommunity(cid)) dialog.accept();
        });
    }
    addPage(general, "Community");

    auto* channelsPage = new QWidget(tabs);
    auto* channelLayout = new QFormLayout(channelsPage);
    auto* channels = new QComboBox(channelsPage);
    auto* channelName = new QLineEdit(channelsPage);
    auto* topic = new QLineEdit(channelsPage);
    auto* type = new QComboBox(channelsPage);
    type->addItem("Text", "text");
    type->addItem("Voice", "voice");
    const auto fillChannels = [&] {
        channels->clear();
        channels->addItem("New channel", "");
        for (const auto& item : node_->communities()[cid].channels)
            channels->addItem(item.name, item.id);
    };
    fillChannels();
    channelLayout->addRow("Channel", channels);
    channelLayout->addRow("Name", channelName);
    channelLayout->addRow("Type", type);
    channelLayout->addRow("Topic", topic);
    auto* saveChannel = button("Save channel", "communitySaveChannel", channelsPage);
    auto* removeChannel = button("Delete channel", "communityDeleteChannel", channelsPage);
    channelLayout->addRow(saveChannel);
    channelLayout->addRow(removeChannel);
    connect(channels, &QComboBox::currentIndexChanged, &dialog, [&] {
        const auto item = node_->communities()[cid].channels.value(
            channels->currentData().toString());
        channelName->setText(item.name);
        topic->setText(item.topic);
        type->setCurrentIndex(item.type == "voice" ? 1 : 0);
        removeChannel->setEnabled(!item.id.isEmpty());
        type->setEnabled(item.id.isEmpty());
    });
    removeChannel->setEnabled(false);
    connect(saveChannel, &QPushButton::clicked, &dialog, [&] {
        const auto selected = channels->currentData().toString();
        const auto id = selected.isEmpty() ? freshId() : selected;
        if (apply("channel", {{"id", id},
                              {"name", channelName->text().trimmed()},
                              {"topic", topic->text()},
                              {"type", type->currentData().toString()}})) {
            fillChannels();
            channels->setCurrentIndex(channels->findData(id));
        }
    });
    connect(removeChannel, &QPushButton::clicked, &dialog, [&] {
        if (apply("delete-channel", {{"id", channels->currentData().toString()}}))
            fillChannels();
    });
    addPage(channelsPage, "Channels");

    auto* people = new QWidget(tabs);
    auto* peopleForm = new QFormLayout(people);
    auto* member = new QComboBox(people);
    member->setObjectName("communityMember");
    for (const auto& item : model.members)
        member->addItem(item.name + " · " + item.id.left(8), item.id);
    auto* admin = new QCheckBox("Administrator", people);
    auto* saveRole = button("Save role", "communitySaveRole", people);
    auto* kick = button("Kick member", "communityKickMember", people);
    auto* remove = button("Remove and block member", "communityRemoveMember", people);
    auto* bannedMember = new QComboBox(people);
    bannedMember->setObjectName("communityBannedMember");
    auto* unban = button("Unban member", "communityUnbanMember", people);
    const auto fillBans = [&] {
        bannedMember->clear();
        auto peers = node_->communities()[cid].banned.values();
        peers.sort();
        QMap<QString, QPair<int, QString>> names;
        for (const auto& value : node_->communities()[cid].toJson().value("operations").toArray()) {
            const auto operation = value.toObject();
            const auto data = operation.value("data").toObject();
            const auto intent = data.value("intent").toObject();
            const bool profile = operation.value("kind") == "profile";
            const auto peer = profile ? operation.value("author").toString()
                                      : intent.value("peer").toString();
            const auto name = profile ? data.value("name").toString()
                                      : intent.value("name").toString();
            const auto clock = operation.value("clock").toInt();
            if (!name.isEmpty() && (!names.contains(peer) || clock > names[peer].first))
                names[peer] = {clock, name};
        }
        for (const auto& peer : peers) {
            const auto name = names.value(peer).second;
            bannedMember->addItem(name.isEmpty() ? peer : name + " · " + peer.left(8), peer);
            bannedMember->setItemData(bannedMember->count() - 1, peer, Qt::ToolTipRole);
        }
        unban->setEnabled(bannedMember->count() > 0);
    };
    fillBans();
    peopleForm->addRow("Member", member);
    peopleForm->addRow(admin);
    peopleForm->addRow(saveRole);
    peopleForm->addRow(kick);
    peopleForm->addRow(remove);
    auto* removalHelp = new QLabel(
        "Kicked members can rejoin with an invite. Blocked members must be unbanned first. "
        "Unbanning lets them rejoin; it does not restore their membership or role.", people);
    removalHelp->setWordWrap(true);
    peopleForm->addRow(removalHelp);
    peopleForm->addRow("Banned member", bannedMember);
    peopleForm->addRow(unban);
    const auto memberChanged = [&] {
        const auto peer = member->currentData().toString();
        const auto current = node_->communities()[cid];
        admin->setChecked(current.members.value(peer).admin);
        admin->setEnabled(!peer.isEmpty() && node_->peerId() == current.owner && peer != current.owner);
        saveRole->setEnabled(admin->isEnabled());
        kick->setEnabled(!peer.isEmpty() && peer != current.owner);
        remove->setEnabled(kick->isEnabled());
    };
    connect(member, &QComboBox::currentIndexChanged, &dialog, memberChanged);
    memberChanged();
    connect(saveRole, &QPushButton::clicked, &dialog, [&] {
        apply("admin",
              {{"peer", member->currentData().toString()}, {"enabled", admin->isChecked()}});
    });
    connect(kick, &QPushButton::clicked, &dialog, [&] {
        if (apply("remove", {{"peer", member->currentData().toString()}}))
            member->removeItem(member->currentIndex());
    });
    connect(remove, &QPushButton::clicked, &dialog, [&] {
        if (apply("kick", {{"peer", member->currentData().toString()}})) {
            member->removeItem(member->currentIndex());
            fillBans();
        }
    });
    connect(unban, &QPushButton::clicked, &dialog, [&] {
        if (apply("unban", {{"peer", bannedMember->currentData().toString()}}))
            fillBans();
    });
    addPage(people, "Members");

    auto* invitesPage = new QWidget(tabs);
    auto* invitesForm = new QFormLayout(invitesPage);
    auto* invites = new QComboBox(invitesPage);
    for (const auto& hash : model.inviteIds())
        invites->addItem(hash.left(16) +
                             (model.revokedInvites.contains(hash) ? " · revoked" : " · active"),
                         hash);
    auto* revoke = button("Revoke invite", "communityRevokeInvite", invitesPage);
    revoke->setEnabled(invites->count() > 0);
    invitesForm->addRow("Invite ID", invites);
    invitesForm->addRow(revoke);
    auto* inviteExplanation = new QLabel(
        "Existing valid invites work while admins are offline. Revoking an invite prevents future "
        "admissions once peers receive the update; it does not remove existing members.",
        invitesPage);
    inviteExplanation->setWordWrap(true);
    invitesForm->addRow(inviteExplanation);
    connect(revoke, &QPushButton::clicked, &dialog, [&] {
        if (apply("revoke", {{"hash", invites->currentData().toString()}}))
            invites->setItemText(invites->currentIndex(),
                                 invites->currentData().toString().left(16) + " · revoked");
    });
    addPage(invitesPage, "Invites");

    auto* hostsPage = new QWidget(tabs);
    auto* hostsLayout = new QVBoxLayout(hostsPage);
    auto* hostHelp =
        new QLabel("Drag members into host priority order. If a member disconnects, the next "
                   "reachable member becomes the preferred host. This does not change permissions.",
                   hostsPage);
    hostHelp->setWordWrap(true);
    hostsLayout->addWidget(hostHelp);
    auto* priority = new QListWidget(hostsPage);
    priority->setDragDropMode(QAbstractItemView::InternalMove);
    auto ordered = model.priority;
    if (!ordered.contains(model.owner))
        ordered.prepend(model.owner);
    for (const auto& peer : model.members.keys())
        if (!ordered.contains(peer))
            ordered.append(peer);
    for (const auto& peer : ordered) {
        if (!model.members.contains(peer))
            continue;
        auto* item = new QListWidgetItem(model.members[peer].name + " · " + peer.left(8), priority);
        item->setData(Qt::UserRole, peer);
    }
    hostsLayout->addWidget(priority);
    auto* savePriority = button("Save host priority", "communitySavePriority", hostsPage);
    hostsLayout->addWidget(savePriority);
    connect(savePriority, &QPushButton::clicked, &dialog, [&] {
        QJsonArray peers;
        for (int i = 0; i < priority->count(); ++i)
            peers.append(priority->item(i)->data(Qt::UserRole).toString());
        apply("priority", {{"peers", peers}});
    });
    addPage(hostsPage, "Host priority");
    auto* footerWidget = new QWidget(&dialog);
    footerWidget->setObjectName("dialogFooter");
    footerWidget->setAttribute(Qt::WA_StyledBackground);
    auto* footer = new QHBoxLayout(footerWidget);
    footer->setContentsMargins(20, 12, 20, 12);
    result->setObjectName("footerNote");
    footer->addWidget(result, 1);
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    footer->addWidget(close);
    layout->addWidget(footerWidget);
    connect(close, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    categories->setCurrentRow(0);
    dialog.resize(780, 500);
    dialog.exec();
}

void CommunityPanel::selectCommunity(const QString& cid) {
    selectedVoiceCid_.clear();
    selectedVoiceChannel_.clear();
    cid_ = cid;
    channel_.clear();
    if (node_->communities().contains(cid))
        for (const auto& item : orderedChannels(node_->communities()[cid]))
            if (item.type == "text") {
                channel_ = item.id;
                break;
            }
    loadHistory(cid_, channel_);
    refresh();
    render();
}

void CommunityPanel::select(QTreeWidgetItem* item) {
    if (!item)
        return;
    const auto cid = item->data(0, Qt::UserRole).toString();
    const auto channel = item->data(0, Qt::UserRole + 1).toString();
    // A row holding a button, such as "Join voice", becomes current when the
    // button takes focus on a press. Selecting it would rebuild the tree and
    // delete the button before the click completes.
    if (cid.isEmpty())
        return;
    if (channel.isEmpty()) {
        if (cid != cid_)
            selectCommunity(cid);
        return;
    }
    if (node_->communities().value(cid).channels.value(channel).type == "voice") {
        selectedVoiceCid_ = cid;
        selectedVoiceChannel_ = channel;
        stageOpen_ = true;
        refresh();
        return;
    }
    selectedVoiceCid_.clear();
    selectedVoiceChannel_.clear();
    openStage(false);
    cid_ = cid;
    channel_ = channel;
    unread_[key(cid_, channel_)] = 0;
    loadHistory(cid_, channel_);
    refresh();
    render();
}

QString CommunityPanel::memberName(const QString& cid, const QString& peer) const {
    if (!node_ || !node_->communities().contains(cid))
        return peer.left(8);
    return node_->communities()[cid].members.value(peer).name;
}

void CommunityPanel::refresh() {
    refreshPenguins();
    const bool usable = node_ != nullptr;
    if (usable &&
        !node_->communities().value(selectedVoiceCid_).channels.contains(selectedVoiceChannel_)) {
        selectedVoiceCid_.clear();
        selectedVoiceChannel_.clear();
    }
    QString currentVoiceCid, currentVoiceChannel;
    if (usable && online_) {
        for (const auto& model : node_->communities()) {
            const auto presence = node_->presence(model.id, node_->peerId());
            if (!presence.voiceChannel.isEmpty()) {
                currentVoiceCid = model.id;
                currentVoiceChannel = presence.voiceChannel;
                break;
            }
        }
    }
    if (!voiceCid_.isEmpty() && currentVoiceCid.isEmpty()) {
        // A metadata sync may briefly hide presence. Keep the call alive while
        // the retained membership and voice channel still exist.
        const auto model = node_->communities().value(voiceCid_);
        if (online_ && model.isMember(node_->peerId()) &&
            model.channels.value(voiceChannel_).type == "voice") {
            currentVoiceCid = voiceCid_;
            currentVoiceChannel = voiceChannel_;
            if (node_->presence(currentVoiceCid, node_->peerId()).voiceChannel.isEmpty())
                node_->setVoice(currentVoiceCid, currentVoiceChannel, mute_->isChecked(),
                                deafen_->isChecked(), capture_.running(), cameraCapture_.running());
        }
        else {
            leaveVoice();
            return;
        }
    }
    voiceCid_ = currentVoiceCid;
    voiceChannel_ = currentVoiceChannel;
    if (!voiceCid_.isEmpty()) {
        const auto presence = node_->presence(voiceCid_, node_->peerId());
        QSignalBlocker mute(mute_), deafen(deafen_);
        deafen_->setChecked(presence.deafened);
        if (!presence.deafened && !QSettings().value("voice/pushToTalk", false).toBool())
            mute_->setChecked(presence.muted);
    }
    for (auto* action : {invite_, manage_, send_, image_, gif_, voice_, mute_, deafen_, share_, camera_})
        action->setEnabled(false);
    const bool composerFocused = composer_->hasFocus();
    composer_->setEnabled(false);
    connect_->setEnabled(usable);
    connect_->setText(online_ ? "Disconnect" : "Connect");
    setFlag(connect_, "primary", !online_);
    name_->setEnabled(!online_);
    if (usable && online_)
        connectionStatus_->setText(
            QString("<span style='color:%1'>●</span>&nbsp; Connected").arg(theme::kOnline));
    else
        connectionStatus_->setText(
            QString("<span style='color:%1'>●</span>&nbsp; Offline").arg(theme::kDim));
    {
        community::Member self;
        if (usable) {
            self.name = node_->profile().name;
            self.avatar = node_->profile().avatar;
        }
        if (self.name.isEmpty())
            self.name = name_->text().trimmed();
        userAvatar_->setPixmap(profileAvatar(self, 28, devicePixelRatioF()));
        userName_->setText(self.name.isEmpty() ? "Not connected" : self.name);
        const auto voiceModel = usable ? node_->communities().value(voiceCid_) : community::Model{};
        userState_->setText(!online_ ? "offline"
                            : voiceCid_.isEmpty()
                                ? "online"
                                : (capture_.running() ? "sharing · " : "in voice · ") +
                                      voiceModel.channels.value(voiceChannel_).name);
    }
    create_->setEnabled(usable && online_);
    join_->setEnabled(usable && online_);
    voiceControls_->setVisible(online_ && !voiceCid_.isEmpty());
    if (!usable) {
        if (!failure_.isEmpty())
            noteError(failure_);
        else
            note(QString());
        return;
    }
    if (!node_->communities().value(cid_).isMember(node_->peerId())) {
        cid_.clear();
        channel_.clear();
        selectedVoiceCid_.clear();
        selectedVoiceChannel_.clear();
    }
    if (cid_.isEmpty()) {
        for (const auto& model : node_->communities())
            if (model.isMember(node_->peerId())) { cid_ = model.id; break; }
        for (const auto& channel : orderedChannels(node_->communities()[cid_]))
            if (channel.type == "text") {
                channel_ = channel.id;
                break;
            }
        loadHistory(cid_, channel_);
        render();
    }
    QMap<QString, bool> expanded;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        const auto* item = tree_->topLevelItem(i);
        expanded[item->data(0, Qt::UserRole).toString()] = item->isExpanded();
    }
    QSignalBlocker block(tree_);
    tree_->clear();
    for (const auto& model : node_->communities()) {
        if (!model.isMember(node_->peerId())) continue;
        auto* root = new QTreeWidgetItem(tree_, {model.name});
        root->setData(0, Qt::UserRole, model.id);
        root->setExpanded(expanded.value(model.id, true));
        if (online_)
            root->setData(0, kNoteRole,
                          QString::number(node_->online(model.id).size()) + " online");
        if (!model.icon.isEmpty()) {
            ImageBytes image(model.icon);
            if (image.canRead() && image.size().width() <= 512 && image.size().height() <= 512)
                root->setIcon(0, QIcon(QPixmap::fromImage(image.read())));
        }
        if (root->icon(0).isNull())
            root->setIcon(0, QIcon(theme::communityMark(model.name, 16, devicePixelRatioF())));
        for (const auto& channel : orderedChannels(model)) {
            const auto unread = unread_.value(key(model.id, channel.id));
            const auto count =
                channel.type == "voice"
                    ? std::count_if(model.members.begin(), model.members.end(),
                                    [&](const auto& member) {
                                        return node_->online(model.id).contains(member.id) &&
                                               node_->presence(model.id, member.id).voiceChannel ==
                                                   channel.id;
                                    })
                    : 0;
            const auto label = (channel.type == "text" ? "# " : "") + channel.name;
            auto* child = new QTreeWidgetItem(root, {label});
            child->setData(0, kUnreadRole, unread);
            if (count)
                child->setToolTip(0, QString::number(count) + " in voice");
            child->setData(0, kLiveRole,
                           channel.type == "voice" &&
                               std::any_of(model.members.begin(), model.members.end(),
                                           [&](const auto& member) {
                                               const auto state =
                                                   node_->presence(model.id, member.id);
                                               return state.voiceChannel == channel.id &&
                                                      state.sharing &&
                                                      node_->online(model.id).contains(member.id);
                                           }));
            if (channel.type == "voice")
                child->setIcon(0, theme::voiceIcon(devicePixelRatioF()));
            child->setData(0, Qt::UserRole, model.id);
            child->setData(0, Qt::UserRole + 1, channel.id);
            if (channel.type == "voice")
                static_cast<CommunityTree*>(tree_)->configureVoiceChannel(
                    child, model.id, channel.id, online_ && model.canAdmin(node_->peerId()));
            const bool selectedVoice = model.id == selectedVoiceCid_ &&
                                       channel.id == selectedVoiceChannel_;
            const bool activeVoice = model.id == voiceCid_ && channel.id == voiceChannel_;
            if (selectedVoice ||
                (selectedVoiceChannel_.isEmpty() && model.id == cid_ && channel.id == channel_))
                tree_->setCurrentItem(child);
            if (channel.type != "voice")
                continue;
            child->setExpanded(true);
            if (activeVoice)
                child->setForeground(0, QColor("#9dccca"));
            for (const auto& peer : model.members) {
                const auto state = node_->presence(model.id, peer.id);
                if (!node_->online(model.id).contains(peer.id) || state.voiceChannel != channel.id)
                    continue;
                QStringList activity;
                if (state.muted)
                    activity.append("muted");
                if (state.deafened)
                    activity.append("deafened");
                if (state.sharing)
                    activity.append("sharing screen");
                if (state.camera)
                    activity.append("camera on");
                if (QSettings().value("community/voiceMuted/" + peer.id, false).toBool())
                    activity.append("muted by you");
                auto label = peer.name;
                if (peer.id == node_->peerId())
                    label += " (you)";
                // The profile button paints the name. A display label on the
                // tree item would show through its transparent background.
                auto* participant = new QTreeWidgetItem(child);
                participant->setData(0, Qt::AccessibleTextRole, label);
                participant->setFlags(Qt::ItemIsEnabled);
                participant->setData(0, Qt::UserRole + 2, peer.id);
                participant->setData(0, Qt::UserRole + 3, activity);
                participant->setData(0, Qt::UserRole, model.id);
                // Tree item padding is outside the widget's content rectangle.
                // Leave room for the avatar plus both the row and item insets.
                participant->setSizeHint(0, QSize(160, 30));
                auto* row = new QWidget(tree_);
                row->setObjectName("voiceParticipantRow");
                row->setAccessibleName(label + ", " + activity.join(", "));
                auto* line = new QHBoxLayout(row);
                line->setContentsMargins(2, 1, 6, 1);
                line->setSpacing(4);
                auto* profile = button(QString(label).replace('&', "&&"), "voiceMemberProfile",
                                       row);
                profile->setProperty("peer", peer.id);
                profile->setIcon(QIcon(profileAvatar(peer, 20, devicePixelRatioF())));
                profile->setIconSize(QSize(20, 20));
                profile->setMinimumHeight(26);
                profile->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                profile->setToolTip("View " + peer.name + "'s profile");
                profile->setAccessibleName("View profile: " + label);
                line->addWidget(profile, 1);
                connect(profile, &QPushButton::clicked, this,
                        [this, profile, cid = model.id, id = peer.id] {
                            showProfile(cid, id,
                                        profile->mapToGlobal(QPoint(0, profile->height())));
                        });
                for (const auto& kind : activity) {
                    auto* indicator = new QLabel(row);
                    indicator->setObjectName("voiceActivity");
                    indicator->setPixmap(voiceIndicator(kind));
                    indicator->setToolTip(kind);
                    indicator->setAccessibleName(kind);
                    indicator->setFixedSize(16, 16);
                    line->addWidget(indicator);
                }
                if (!state.muted && !state.deafened) {
                    auto* mic = new QLabel(row);
                    mic->setObjectName("voiceMic");
                    mic->setPixmap(theme::micIcon(false, devicePixelRatioF()));
                    mic->setFixedSize(16, 16);
                    line->addWidget(mic);
                }
                participant->setSizeHint(0,
                                         QSize(160, std::max(30, row->sizeHint().height() + 2)));
                tree_->setItemWidget(participant, 0, row);
                paintSpeaking(row, speaking_.contains(peer.id));
                if (peer.id != node_->peerId()) {
                    row->setContextMenuPolicy(Qt::CustomContextMenu);
                    connect(row, &QWidget::customContextMenuRequested, this,
                            [this, row, id = peer.id](const QPoint& at) {
                                showVoiceMemberMenu(id, row->mapToGlobal(at));
                            });
                }
                static_cast<CommunityTree*>(tree_)->configureVoiceParticipant(
                    participant, row, model.id, channel.id, peer.id,
                    online_ && model.canAdmin(node_->peerId()));
            }
        }
    }
    for (const auto& community : node_->communities()) {
        const auto online = online_ ? node_->online(community.id) : QSet<QString>{};
        QSet<QString> sharing;
        for (const auto& peer : online) {
            if (peer == node_->peerId()) continue;
            if (node_->presence(community.id, peer).sharing) sharing.insert(peer);
            if (previousOnline_.contains(community.id) && !previousOnline_[community.id].contains(peer))
                notifications_->activityOccurred(memberName(community.id, peer), "joined " + community.name);
        }
        for (const auto& peer : sharing - previousSharing_.value(community.id))
            notifications_->activityOccurred(memberName(community.id, peer), "started sharing in " + community.name);
        previousOnline_[community.id] = online;
        previousSharing_[community.id] = sharing;
    }
    const auto model = node_->communities().value(cid_);
    QString profiles = cid_;
    for (const auto& member : model.members)
        profiles += member.id + community::digest(member.name.toUtf8() + member.avatar);
    if (renderedProfiles_ != profiles) {
        renderedProfiles_ = profiles;
        const auto position = transcript_->verticalScrollBar()->value();
        const bool atBottom = position >= transcript_->verticalScrollBar()->maximum() - 4;
        render();
        if (!atBottom)
            transcript_->verticalScrollBar()->setValue(position);
    }
    QStringList mentionable = memberNames(cid_);
    if (model.isMember(node_->peerId()))
        mentionable.removeAll(model.members.value(node_->peerId()).name);
    mentions_->setMembers(mentionable);
    if (!model.id.isEmpty() && !model.channels.contains(channel_)) {
        channel_.clear();
        for (const auto& item : orderedChannels(model))
            if (item.type == "text") {
                channel_ = item.id;
                break;
            }
        if (!channel_.isEmpty()) {
            loadHistory(cid_, channel_);
            render();
        }
    }
    const auto channel = model.channels.value(channel_);
    const bool member = model.isMember(node_->peerId());
    const bool textChannel = member && channel.type == "text";
    heading_->setText(model.id.isEmpty()
                          ? "Your communities"
                          : (channel.type == "text"
                                 ? QString("<span style='color:%1;font-weight:400'>#</span>&nbsp;")
                                       .arg(theme::kDim)
                                 : QString()) +
                                channel.name.toHtmlEscaped());
    topic_->setText(model.id.isEmpty()
                        ? "Create a community or join with an invite link or QR image."
                        : !channel.topic.isEmpty()
                        ? channel.topic
                        : model.name + "  ·  " + QString::number(node_->online(cid_).size()) +
                              " online  ·  " + QString::number(model.members.size()) +
                              (model.members.size() == 1 ? " member" : " members") +
                              "  ·  hosted by " + memberName(cid_, node_->host(cid_)));
    composer_->setPlaceholderText(textChannel ? "Message #" + channel.name : "Message this channel");
    setFlag(composerFrame_, "inactive", !textChannel);
    const bool admin = model.canAdmin(node_->peerId());
    invite_->setEnabled(admin);
    manage_->setEnabled(admin);
    const bool canLeave = member && model.owner != node_->peerId();
    leaveAction_->setVisible(canLeave);
    leaveAction_->setEnabled(canLeave);
    composer_->setEnabled(textChannel);
    if (composerFocused && textChannel)
        composer_->setFocus(Qt::OtherFocusReason);
    send_->setEnabled(textChannel);
    image_->setEnabled(online_ && textChannel);
    gif_->setEnabled(online_ && textChannel);
    members_->clear();
    auto people = model.members.values();
    const auto onlineHere = online_ ? node_->online(cid_) : QSet<QString>{};
    std::sort(people.begin(), people.end(), [&](const auto& a, const auto& b) {
        const bool aOnline = onlineHere.contains(a.id), bOnline = onlineHere.contains(b.id);
        if (aOnline != bOnline)
            return aOnline;
        return QString::compare(a.name, b.name, Qt::CaseInsensitive) < 0;
    });
    const auto present = std::count_if(people.begin(), people.end(),
                                       [&](const auto& peer) { return onlineHere.contains(peer.id); });
    for (const auto& peer : people) {
        const bool here = onlineHere.contains(peer.id);
        auto* item = new QListWidgetItem(peer.name, members_);
        item->setIcon(QIcon(profileAvatar(peer, 26, devicePixelRatioF())));
        item->setData(Qt::UserRole, peer.id);
        item->setData(kPresentRole, here);
        item->setToolTip("View " + peer.name + "'s profile");
        if (members_->count() == 1 && here)
            item->setData(kSectionRole, QString("ONLINE — %1").arg(present));
        else if (!here && (members_->count() == 1 ||
                           members_->item(members_->count() - 2)->data(kPresentRole).toBool()))
            item->setData(kSectionRole, QString("OFFLINE — %1").arg(people.size() - present));
        if (peer.id == model.owner)
            item->setData(kBadgeRole, "OWNER");
        else if (peer.admin)
            item->setData(kBadgeRole, "ADMIN");
        const auto state = node_->presence(cid_, peer.id);
        item->setData(kStateColorRole,
                      here && state.sharing ? QColor("#ec97b7")
                      : here && !state.voiceChannel.isEmpty() ? QColor("#8fc7c2")
                                                              : QColor(theme::kMuted));
        item->setData(kStateRole,
                      !here ? "offline"
                      : state.sharing ? "sharing screen"
                      : !state.voiceChannel.isEmpty()
                          ? "in voice · " + model.channels.value(state.voiceChannel).name
                      : peer.id == node_->host(cid_) ? "online · hosting"
                                                     : "online");
        item->setForeground(peer.id == node_->peerId() ? chatColor(messageColor_)
                                                       : peerColors_.value(peer.id, chatColor("mint")));
    }
    const bool inVoice = !voiceCid_.isEmpty();
    voice_->setEnabled(inVoice);
    mute_->setEnabled(inVoice && !deafen_->isChecked());
    deafen_->setEnabled(inVoice);
    share_->setEnabled(inVoice);
    camera_->setEnabled(inVoice);
    camera_->setText(cameraPending_              ? "Cancel camera"
                     : cameraCapture_.running() ? "Turn camera off"
                                                : "Turn camera on");
    setFlag(camera_, "danger", cameraCapture_.running());
    share_->setText(capture_.running() ? "Stop sharing" : "Share screen");
    setFlag(share_, "primary", !capture_.running());
    setFlag(share_, "danger", capture_.running());
    if (inVoice) {
        const auto voiceModel = node_->communities().value(voiceCid_);
        if (!online_ || !voiceModel.isMember(node_->peerId()) ||
            !voiceModel.channels.contains(voiceChannel_) ||
            voiceModel.channels[voiceChannel_].type != "voice") {
            leaveVoice();
            return;
        }
        voiceStatus_->setText("Voice  ·  " + voiceModel.channels[voiceChannel_].name);
        QStringList sharers;
        for (const auto& member : voiceModel.members) {
            const auto state = node_->presence(voiceCid_, member.id);
            if (state.voiceChannel == voiceChannel_ && state.sharing &&
                node_->online(voiceCid_).contains(member.id))
                sharers.append(member.name);
        }
        const QString detail =
            sharers.isEmpty() ? QString()
                              : "|   " + sharers.join(", ") +
                                    (sharers.size() == 1 ? " is" : " are") + " sharing";
        voiceDetail_->setText(detail);
        voiceDetail_->setToolTip(detail);
        QSet<QString> active, streaming, cameras;
        // Presence is removed when the authenticated connection closes. A
        // metadata sync briefly clears readiness, but must not tear down video.
        for (const auto& peer : voiceModel.members.keys()) {
            const auto state = node_->presence(voiceCid_, peer);
            if (peer != node_->peerId() && state.voiceChannel == voiceChannel_) {
                active.insert(peer);
                if (state.muted) {
                    setSpeaking(peer, false);
                    quietAudioFrames_.remove(peer);
                }
                if (state.sharing)
                    streaming.insert(peer);
                if (state.camera) cameras.insert(peer);
            }
        }
        for (const auto& peer : audioPeers_ - active) {
            audio_.remove(peer);
            setSpeaking(peer, false);
            quietAudioFrames_.remove(peer);
            lastAudioAt_.remove(peer);
            if (watchers_.remove(peer))
                announceViewers();
            viewers_.remove(peer);
            cameraWatchers_.remove(peer);
            for (const auto& [_, session] : drawings_)
                session->peerChanged(peer, false);
        }
        for (const auto& peer : active - audioPeers_)
            applyVoiceSettings(peer);
        // Newcomers need the current list of who is watching this share.
        if (capture_.running() && !(active - audioPeers_).isEmpty())
            announceViewers();
        audioPeers_ = active;
        for (const auto& peer : streams_ - streaming) {
            stopDrawing(peer);
            stage_->removeStream(peer);
            screenAudio_.remove(peer);
            // Preserve watch intent while a peer is disconnected. Explicitly
            // ending a share or leaving voice still removes the subscription.
            if (node_->online(voiceCid_).contains(peer) || !voiceModel.isMember(peer))
                watching_.remove(peer);
            lastScreenFrames_.remove(peer);
            decoders_.erase(peer);
            frames_.remove(peer);
        }
        for (const auto& peer : streaming - streams_) {
            stage_->addStream(peer, false, memberName(voiceCid_, peer));
            const int volume = QSettings().value("community/streamVolume/" + peer, 100).toInt();
            stage_->setVolume(peer, volume);
            setStreamVolume(peer, volume);
            if (watching_.contains(peer))
                watch(peer, true);
        }
        for (const auto& peer : cameras_ - cameras) {
            const auto id = "camera/" + peer;
            stage_->setCameraFrame(peer, {});
            cameraWatching_.remove(peer);
            decoders_.erase(id);
            frames_.remove(id);
            lastScreenFrames_.remove(id);
        }
        for (const auto& peer : cameras - cameras_)
            watchCamera(peer);
        cameras_ = cameras;
        streams_ = streaming;
        for (const auto& peer : streams_)
            stage_->setDisplayName(peer, memberName(voiceCid_, peer));
        if (capture_.running())
            stage_->setDisplayName(node_->peerId(), memberName(voiceCid_, node_->peerId()));
        QHash<QString, QString> names;
        for (const auto& member : node_->communities().value(voiceCid_).members)
            names[member.id] = member.name;
        for (const auto& [peer, session] : drawings_)
            if (auto* drawing = stage_->drawing(peer)) drawing->setAuthorNames(names);
    }
    else
        voiceStatus_->setText("Voice disconnected");
    refreshAudience();
    openStage(stageOpen_);
}

void CommunityPanel::openStage(bool open) {
    // The call view shows the selected voice channel, or else your current one.
    if (open && selectedVoiceChannel_.isEmpty()) {
        selectedVoiceCid_ = voiceCid_;
        selectedVoiceChannel_ = voiceChannel_;
    }
    if (!open) {
        selectedVoiceCid_.clear();
        selectedVoiceChannel_.clear();
    }
    stageOpen_ = open && !selectedVoiceChannel_.isEmpty();
    views_->setCurrentIndex(stageOpen_ ? 1 : 0);
    // Name the community of whatever is on screen: the call or the chat.
    const auto shown = node_ ? node_->communities().value(stageOpen_ ? selectedVoiceCid_ : cid_)
                             : community::Model{};
    windowTitle_->setText(
        QString("<span style='color:%1;font-weight:600'>%2</span>")
            .arg(theme::kText, QApplication::applicationDisplayName().toHtmlEscaped()) +
        (shown.id.isEmpty() ? QString()
                            : QString("<span style='color:%1'>&nbsp; — &nbsp;%2</span>")
                                  .arg(theme::kMuted, shown.name.toHtmlEscaped())));
    QString sharer;
    for (const auto& peer : streams_) {
        sharer = memberName(voiceCid_, peer);
        break;
    }
    const bool viewingCall = stageOpen_ && selectedVoiceCid_ == voiceCid_ &&
                             selectedVoiceChannel_ == voiceChannel_;
    // Inside the call, the sidebar's text channels lead back to chat.
    stageToggle_->setText(!sharer.isEmpty() ? "Watch " + sharer : "Show call");
    stageToggle_->setVisible(!voiceCid_.isEmpty() && !viewingCall);
    setFlag(stageToggle_, "primary", !viewingCall && !streams_.isEmpty());
    refreshCall();
    refreshPip();
}

void CommunityPanel::showCall(bool open) {
    if (open) {
        // Show your own call rather than another channel you were looking at.
        selectedVoiceCid_ = voiceCid_;
        selectedVoiceChannel_ = voiceChannel_;
    }
    openStage(open);
    refresh();
}

void CommunityPanel::refreshCall() {
    if (!node_ || !stageOpen_)
        return;
    const auto model = node_->communities().value(selectedVoiceCid_);
    const auto channel = model.channels.value(selectedVoiceChannel_);
    const bool current = selectedVoiceCid_ == voiceCid_ && selectedVoiceChannel_ == voiceChannel_;
    QList<StageParticipant> people;
    const auto online = online_ ? node_->online(model.id) : QSet<QString>{};
    for (const auto& member : model.members) {
        const auto state = node_->presence(model.id, member.id);
        if (!online.contains(member.id) || state.voiceChannel != channel.id)
            continue;
        people.append({member.id,
                       member.id == node_->peerId() ? member.name + " (you)" : member.name,
                       profileAvatar(member, 96, devicePixelRatioF()), state.muted,
                       state.deafened, state.sharing});
    }
    std::sort(people.begin(), people.end(), [](const auto& a, const auto& b) {
        return QString::compare(a.name, b.name, Qt::CaseInsensitive) < 0;
    });
    stage_->setVisible(current);
    otherCall_->setVisible(!current);
    (current ? stage_ : otherCall_)->setParticipants(people);
    callTitle_->setText(channel.name);
    callSubtitle_->setText(model.name + "  ·  " + QString::number(people.size()) + " in voice");
    joinCall_->setText(voiceCid_.isEmpty() ? "Join voice" : "Switch to this channel");
    joinCall_->setVisible(online_ && model.isMember(node_->peerId()) && !current);
}

void CommunityPanel::announceViewers() {
    if (!node_ || voiceCid_.isEmpty() || !capture_.running())
        return;
    QJsonArray peers;
    for (const auto& peer : std::as_const(watchers_))
        peers.append(peer);
    attempt([&] { node_->sendPacket(voiceCid_, voiceChannel_, "viewers", {{"peers", peers}}); });
    refreshAudience();
}

void CommunityPanel::refreshAudience() {
    if (!node_ || voiceCid_.isEmpty())
        return;
    const auto model = node_->communities().value(voiceCid_);
    const auto online = node_->online(voiceCid_);
    const auto show = [&](const QString& stream, QSet<QString> peers) {
        QList<community::Member> people;
        for (const auto& peer : std::as_const(peers))
            if (peer != stream && model.isMember(peer) &&
                (peer == node_->peerId() || online.contains(peer)))
                people.append(model.members.value(peer));
        std::sort(people.begin(), people.end(), [](const auto& a, const auto& b) {
            return QString::compare(a.name, b.name, Qt::CaseInsensitive) < 0;
        });
        QList<QPixmap> avatars;
        QStringList names;
        for (const auto& person : people) {
            avatars.append(profileAvatar(person, 20, devicePixelRatioF()));
            names.append(person.id == node_->peerId() ? person.name + " (you)" : person.name);
        }
        stage_->setAudience(stream, avatars, names);
    };
    if (capture_.running())
        show(node_->peerId(), watchers_);
    for (const auto& stream : std::as_const(streams_)) {
        // Older clients never announce, so at least count yourself.
        auto peers = viewers_.value(stream);
        if (watching_.contains(stream))
            peers.insert(node_->peerId());
        else
            peers.remove(node_->peerId());
        show(stream, peers);
    }
}

void CommunityPanel::setCustomTitleBar(bool enabled) {
    titleBar_->setVisible(enabled);
    // The title bar carries the brand, so the top bar doesn't repeat it.
    for (QWidget* widget : std::initializer_list<QWidget*>{brandMark_, brandName_, brandGap_})
        widget->setVisible(!enabled);
}

void CommunityPanel::refreshPip() {
    // Prefer a stream you chose to watch, then your own share.
    QString peer;
    for (const auto& candidate : std::as_const(streams_))
        if (watching_.contains(candidate)) {
            peer = candidate;
            break;
        }
    if (peer.isEmpty() && capture_.running() && node_)
        peer = node_->peerId();
    if (peer != pipDismissed_)
        pipDismissed_.clear();
    const bool show = !stageOpen_ && !peer.isEmpty() && peer != pipDismissed_ &&
                      stage_->drawing(peer) != nullptr;
    if (show && peer != pipPeer_) {
        pipView_->setDrawing(stage_->drawing(peer));
        pipView_->setImage(stage_->image(peer));
    }
    pipPeer_ = show ? peer : QString();
    if (show)
        pipName_->setText(peer == node_->peerId() ? "Your screen" : memberName(voiceCid_, peer));
    pip_->setVisible(show);
    placePip();
}

void CommunityPanel::placePip() {
    if (!pip_ || !pip_->isVisibleTo(pip_->parentWidget()))
        return;
    const QRect area = transcript_->geometry();
    pip_->move(area.right() - pip_->width() - 14, area.bottom() - pip_->height() - 10);
    pip_->raise();
}

void CommunityPanel::showFrame(const QString& peer, const QImage& image, qsizetype encodedBytes) {
    stage_->showFrame(peer, image, encodedBytes);
    if (peer == pipPeer_ && pip_->isVisible())
        pipView_->setImage(image);
}

void CommunityPanel::openHistory() {
    history_.reset();
    historyLoaded_.clear();
    QSettings settings;
    if (!settings.value("messages/enabled", false).toBool())
        return;
    attempt([&] {
        const auto maximum = settings.value("messages/maximum").toString();
        const auto limit = maximum.isEmpty() ? std::nullopt
                                             : std::optional<std::int64_t>(maximum.toLongLong());
        history_ = std::make_unique<chat::RecentMessages>(
            settings.value("messages/file", QDir::homePath() + "/disquisition-messages.db")
                .toString()
                .toStdString(),
            limit);
        loadHistory(cid_, channel_);
        render();
    });
}

void CommunityPanel::loadHistory(const QString& cid, const QString& channel) {
    if (!history_ || cid.isEmpty() || channel.isEmpty() ||
        historyLoaded_.contains(key(cid, channel)))
        return;
    attempt([&] {
        const auto loaded = history_->recent(cid.toStdString(), channel.toStdString());
        auto& entries = entries_[key(cid, channel)];
        QSet<QString> existing;
        for (const auto& entry : entries)
            existing.insert(text(entry.message.messageId));
        QList<Entry> previous;
        for (const auto& message : loaded)
            if (!existing.contains(text(message.messageId)))
                previous.append({message, {}, false});
        previous.append(entries);
        entries = std::move(previous);
        historyLoaded_.insert(key(cid, channel));
    });
}

void CommunityPanel::append(const QString& cid, const QString& channel, Entry entry, bool persist) {
    entry.message.communityId = cid.toStdString();
    entry.message.channelId = channel.toStdString();
    if (persist && history_ && entry.image.isEmpty())
        attempt([&] { history_->append(entry.message); });
    auto& list = entries_[key(cid, channel)];
    if (list.isEmpty() && cid == cid_ && channel == channel_) {
        media_->clear();
        transcript_->clear();
    }
    if (!entry.image.isEmpty()) {
        entry.message.messageId = freshId().toStdString();
        cachedImageBytes_ += entry.image.size();
        cachedImages_.append({key(cid, channel), text(entry.message.messageId)});
    }
    list.append(entry);
    if (list.size() > 1000) {
        cachedImageBytes_ -= list.first().image.size();
        list.removeFirst();
    }
    bool rerender = false;
    while (cachedImages_.size() > 128 || cachedImageBytes_ > 24 * 1024 * 1024) {
        const auto oldest = cachedImages_.takeFirst();
        for (auto& cached : entries_[oldest.first]) {
            if (text(cached.message.messageId) != oldest.second || cached.image.isEmpty())
                continue;
            cachedImageBytes_ -= cached.image.size();
            cached.image.clear();
            cached.message.body = "[image no longer cached]";
            if (oldest.first == key(cid_, channel_))
                rerender = true;
            break;
        }
    }
    if (cid == cid_ && channel == channel_)
        showEntry(entry);
    else {
        if (!notifications_->settings().muted)
            ++unread_[key(cid, channel)];
        refresh();
    }
    if (rerender)
        render();
}

void CommunityPanel::showEntry(const Entry& entry) {
    if (entry.system) {
        transcript_->append(
            "<table width='100%' cellspacing='0' cellpadding='0' style='margin-top:12px'><tr>"
            "<td width='42'></td><td style='color:#b7c0c6'><span style='font-family:"
            "\"JetBrains Mono\";font-size:10px;color:#9dccca'>" +
            text(entry.message.sender).toUpper().toHtmlEscaped() + "</span>&nbsp;&nbsp;" +
            text(entry.message.body).toHtmlEscaped().replace('\n', "<br>") + "</td></tr></table>");
        return;
    }
    const auto when = QDateTime::fromSecsSinceEpoch(entry.message.timestamp).toString("HH:mm");
    const auto body = text(entry.message.body);
    const auto cid = text(entry.message.communityId);
    const auto peer = text(entry.message.senderId);
    const auto model = node_ ? node_->communities().value(cid) : community::Model{};
    auto member = model.members.value(peer);
    const bool hasProfile = model.isMember(peer);
    if (!hasProfile)
        member.name = text(entry.message.sender);
    const auto imageUrl = QUrl("profile-avatar:" +
                               community::digest(member.name.toUtf8() + member.avatar));
    transcript_->document()->addResource(QTextDocument::ImageResource, imageUrl,
                                         profileAvatar(member, 30, devicePixelRatioF()).toImage());
    const auto color = chatColor(text(entry.message.color)).name();
    const auto href = "profile://" + cid + '/' + peer;
    const auto link = [&](const QString& content) {
        return hasProfile ? "<a href='" + href.toHtmlEscaped() +
                                "' style='color:" + color + ";text-decoration:none'>" + content + "</a>"
                          : "<span style='color:" + color + "'>" + content + "</span>";
    };
    const bool showMedia = QSettings().value("gifs/showLinkedMedia", true).toBool();
    const auto links = showMedia ? findMediaLinks(body) : QList<MediaLink>{};
    const bool onlyMedia = links.size() == 1 && body.trimmed() == links.first().text;
    const auto mentions = findMentions(body, memberNames(cid));
    const bool mediaOnly = onlyMedia || (body.trimmed().isEmpty() && !entry.image.isEmpty());
    const bool mentioned = peer != node_->peerId() &&
        mentionOf(mentions, memberName(cid, node_->peerId())) != Mentioned::No;
    struct Span {
        qsizetype start;
        qsizetype length;
        bool url;
    };
    QList<Span> spans;
    static const QRegularExpression webLink(
        R"(\bhttps?://[^\s<>"']+)", QRegularExpression::CaseInsensitiveOption);
    auto matches = webLink.globalMatch(body);
    while (matches.hasNext()) {
        const auto match = matches.next();
        auto value = match.captured();
        // Leave sentence punctuation outside the link, but retain balanced
        // parentheses used in URLs such as Wikipedia article names.
        while (!value.isEmpty()) {
            const auto last = value.back();
            if (QString(".,!?;:").contains(last) ||
                (last == ')' && value.count(')') > value.count('(')) ||
                (last == ']' && value.count(']') > value.count('[')) ||
                (last == '}' && value.count('}') > value.count('{')))
                value.chop(1);
            else
                break;
        }
        const QUrl url(value);
        if (url.isValid() && !url.host().isEmpty())
            spans.append({match.capturedStart(), value.size(), true});
    }
    for (const auto& mention : mentions) {
        const bool overlapsLink = std::any_of(spans.cbegin(), spans.cend(), [&](const Span& span) {
            return mention.start < span.start + span.length &&
                   mention.start + mention.length > span.start;
        });
        if (!overlapsLink) spans.append({mention.start, mention.length, false});
    }
    std::stable_sort(spans.begin(), spans.end(), [](const Span& left, const Span& right) {
        return left.start < right.start;
    });
    QString html;
    qsizetype written = 0;
    for (const auto& span : spans) {
        if (span.start < written) continue;
        const auto content = body.mid(span.start, span.length).toHtmlEscaped();
        html += body.mid(written, span.start - written).toHtmlEscaped();
        if (span.url)
            html += "<a href=\"" + content + "\" style='color:#9dccca'>" + content + "</a>";
        else
            html += "<span style='background-color:#3e4372;color:#c9cdfb'>" + content + "</span>";
        written = span.start + span.length;
    }
    html += body.mid(written).toHtmlEscaped();
    if (onlyMedia) html.clear();
    transcript_->append(
        "<table width='100%' cellspacing='0' cellpadding='0' style='margin-top:12px'><tr>"
        "<td width='42' valign='top'" + QString(mentioned ? " bgcolor='#453d2c'" : "") + ">" +
        link("<img src='" + imageUrl.toString() + "' width='30' height='30'>") +
        "</td><td valign='top'" + (mentioned ? " bgcolor='#453d2c'" : "") +
        "><p style='margin:0 0 2px 0'>" +
        link("<span style='font-weight:600'>" + member.name.toHtmlEscaped() + "</span>") +
        "&nbsp;&nbsp;<span style='color:#6f7a84;font-family:\"JetBrains Mono\";font-size:10px'>" +
        when + "</span></p>" +
        (mediaOnly && !entry.undelivered
             ? QString()
             : "<p style='margin:0;color:" + QString(theme::kMessage) + "'>" +
                   html.replace('\n', "<br>") +
                   (entry.undelivered
                        ? "&nbsp;&nbsp;<span style='color:#d4ae77;font-size:12px'>not delivered · "
                          "no peers connected</span>"
                        : "") +
                   "</p>") +
        "</td></tr></table>");
    // Media sits in the message's text column, under the name or the text.
    const auto frames = transcript_->document()->rootFrame()->childFrames();
    auto* table = frames.isEmpty() ? nullptr : qobject_cast<QTextTable*>(frames.last());
    if (table)
        media_->setAnchor(table->cellAt(0, table->columns() - 1).lastCursorPosition());
    if (!entry.image.isEmpty())
        media_->appendImage(entry.image);
    for (const auto& link : links)
        media_->appendRemote(link.url);
    media_->setAnchor(QTextCursor());
}

void CommunityPanel::render() {
    media_->clear();
    transcript_->clear();
    const auto entries = entries_.value(key(cid_, channel_));
    if (entries.isEmpty()) {
        const bool voice = node_ &&
                           node_->communities().value(cid_).channels.value(channel_).type ==
                               "voice";
        transcript_->setPlainText(
            voice ? "Join this voice channel to talk or share your screen."
                  : (cid_.isEmpty()
                         ? "Your communities live on member devices. Create one to get started."
                         : "No messages on this device yet. Messages are delivered live."));
    }
    else
        for (const auto& entry : entries)
            showEntry(entry);
}

void CommunityPanel::sendChat() {
    const auto body = composer_->text().trimmed();
    if (body.isEmpty()) return;
    if (body.startsWith('/')) {
        runCommand(body);
        return;
    }
    if (!online_) { noteError("Connect before sending messages."); return; }
    attempt([&] {
        const auto body = composer_->text().trimmed();
        const auto id = freshId();
        const auto stamp = QDateTime::currentSecsSinceEpoch();
        const int count =
            node_->sendPacket(cid_, channel_, "chat",
                              {{"id", id}, {"body", body}, {"color", messageColor_},
                               {"timestamp", double(stamp * 1000)}});
        if (entries_[key(cid_, channel_)].isEmpty()) {
            media_->clear();
            transcript_->clear();
        }
        append(cid_, channel_,
               {{stamp, name_->text().trimmed().toStdString(), body.toStdString(),
                 messageColor_.toStdString(),
                 cid_.toStdString(), channel_.toStdString(), id.toStdString(),
                 node_->peerId().toStdString()},
                {},
                count == 0});
        penguins_->showMessage(node_->peerId(), body);
        penguins_->setColor(node_->peerId(), chatColor(messageColor_));
        composer_->clear();
    });
}

void CommunityPanel::prepareUpload(std::function<PreparedImage()> prepare) {
    const auto cid = cid_, channel = channel_;
    imagePool_.start([this, cid, channel, prepare = std::move(prepare)] {
        PreparedImage image;
        try {
            image = prepare();
        }
        catch (const std::exception& error) {
            image.error = QString::fromUtf8(error.what());
        }
        QMetaObject::invokeMethod(
            this,
            [this, cid, channel, image = std::move(image)] { sendImage(image, cid, channel); },
            Qt::QueuedConnection);
    });
}

void CommunityPanel::sendImage(const PreparedImage& image, const QString& cid,
                               const QString& channel) {
    if (!online_ || !image.error.isEmpty() || image.bytes.isEmpty()) {
        if (!image.error.isEmpty())
            noteError(image.error);
        return;
    }
    if (!node_->communities().value(cid).isMember(node_->peerId()) ||
        node_->communities().value(cid).channels.value(channel).type != "text")
        return;
    if (uploads_.size() >= 4) {
        note("Wait for the current image uploads to finish.");
        return;
    }
    if (cid == cid_) penguins_->noteActivity(node_->peerId());
    const auto transfer = QRandomGenerator::global()->generate();
    uploads_.append({cid, channel, transfer, splitImage(image.bytes), 0, messageColor_});
    imageTimer_.start();
    append(cid, channel,
           {{QDateTime::currentSecsSinceEpoch(),
             name_->text().trimmed().toStdString(),
             "",
             messageColor_.toStdString(),
             cid.toStdString(),
             channel.toStdString(),
             {},
             node_->peerId().toStdString()},
            image.bytes,
            node_->online(cid).size() <= 1},
           false);
}

void CommunityPanel::receive(const QString& cid, const QString& channel, const QString& peer,
                             const QString& type, const QJsonObject& data) {
    const auto sender = memberName(cid, peer);
    if (type == "sprite") {
        const auto bytes = QByteArray::fromBase64(data.value("bytes").toString().toLatin1());
        const auto count = data.value("count").toInt();
        if (count == 0) {
            spriteImages_.remove(key(cid, peer));
            peerSprites_.remove(peer);
            refreshPenguins();
            return;
        }
        const auto image = spriteImages_.add(key(cid, peer), quint32(data.value("transfer").toDouble()),
            data.value("index").toInt(), count, bytes, QDateTime::currentMSecsSinceEpoch());
        if (image && image->size() <= 256 * 1024) {
            const auto sprites = PenguinSprites::fromWireData(*image);
            if (sprites.complete()) {
                peerSprites_[peer] = {sprites.standing, sprites.walking};
                refreshPenguins();
            }
        }
        return;
    }
    if (type == "chat" || type == "image") {
        peerColors_[peer] = chatColor(packetColor(data));
        if (cid == cid_) {
            penguins_->setColor(peer, peerColors_[peer]);
            for (int i = 0; i < members_->count(); ++i)
                if (members_->item(i)->data(Qt::UserRole).toString() == peer)
                    members_->item(i)->setForeground(peerColors_[peer]);
            if (type == "chat") penguins_->showMessage(peer, data.value("body").toString());
        }
    }
    if (type == "chat") {
        const auto body = data.value("body").toString();
        const auto links = findMediaLinks(body);
        const bool onlyMedia = links.size() == 1 && body.trimmed() == links.first().text;
        notifications_->messageReceived(sender, onlyMedia ? QStringLiteral("sent a GIF") : body,
            mentionOf(findMentions(body, memberNames(cid)), memberName(cid, node_->peerId())));
        if (cid == cid_ && channel == channel_ && entries_[key(cid, channel)].isEmpty()) {
            media_->clear();
            transcript_->clear();
        }
        append(cid, channel,
               {{qint64(data.value("timestamp").toDouble()) / 1000, sender.toStdString(),
                 data.value("body").toString().toStdString(), packetColor(data).toStdString(),
                 cid.toStdString(),
                 channel.toStdString(), data.value("id").toString().toStdString(),
                 peer.toStdString()},
                {},
                false});
    }
    else if (type == "audio" && cid == voiceCid_ && channel == voiceChannel_) {
        const auto pcm = QByteArray::fromBase64(data.value("bytes").toString().toLatin1());
        audio_.receive(peer, pcm);
        if (pcm.size() == 640) {
            lastAudioAt_[peer] = QDateTime::currentMSecsSinceEpoch();
            const auto* samples = reinterpret_cast<const unsigned char*>(pcm.constData());
            double sum = 0;
            for (int index = 0; index < pcm.size(); index += 2) {
                const auto sample =
                    static_cast<qint16>(samples[index] | (samples[index + 1] << 8));
                sum += static_cast<double>(sample) * sample;
            }
            const bool loud = std::sqrt(sum / 320) > kVoiceActivityLevel;
            const int quiet =
                loud ? 0 : std::min(quietAudioFrames_.value(peer, kVoiceActivityHoldFrames) + 1,
                                    kVoiceActivityHoldFrames);
            quietAudioFrames_[peer] = quiet;
            setSpeaking(peer, quiet < kVoiceActivityHoldFrames);
        }
    }
    else if (type == "image") {
        const auto bytes = QByteArray::fromBase64(data.value("bytes").toString().toLatin1());
        const auto transfer = data.value("transfer").toDouble();
        const auto index = data.value("index").toInt(-1), count = data.value("count").toInt();
        if (transfer < 0 || transfer > UINT32_MAX || transfer != quint32(transfer) || index < 0 ||
            count <= 0 || count > int(chat::kMaxImageChunks) || index >= count ||
            bytes.size() > qsizetype(chat::kImageChunkSize))
            return;
        const auto image = images_.add(key(cid, channel) + '/' + peer, quint32(transfer), index,
                                       count, bytes, QDateTime::currentMSecsSinceEpoch());
        if (image && isDisplayableImage(*image)) {
            if (cid == cid_) penguins_->noteActivity(peer);
            notifications_->messageReceived(sender, "sent an image", Mentioned::No);
            append(cid, channel,
                   {{QDateTime::currentSecsSinceEpoch(),
                     sender.toStdString(),
                     "",
                     packetColor(data).toStdString(),
                     cid.toStdString(),
                     channel.toStdString(),
                     {},
                     peer.toStdString()},
                    *image,
                    false},
                   false);
        }
    }
    else if (type == "drawing" && cid == voiceCid_ && channel == voiceChannel_) {
        auto bytes = QByteArray::fromBase64(data.value("bytes").toString().toLatin1()).toStdString();
        chat::Message payload;
        if (chat::decode(bytes, payload) != chat::DecodeStatus::Ok || !bytes.empty() || payload.fields.empty())
            return;
        const auto session = drawings_.find(QString::fromStdString(payload.fields[0]));
        if (session != drawings_.end()) session->second->receive(peer, payload);
    }
    else if (type == "watch-camera" && cid == voiceCid_ && channel == voiceChannel_) {
        if (data.value("enabled").toBool() && cameraCapture_.running()) {
            cameraWatchers_.insert(peer);
            cameraCapture_.requestKeyframe();
        } else cameraWatchers_.remove(peer);
    }
    else if (type == "watch" && cid == voiceCid_ && channel == voiceChannel_) {
        const auto before = watchers_.size();
        if (data.value("enabled").toBool() && capture_.running()) {
            watchers_.insert(peer);
            capture_.requestKeyframe();
        }
        else
            watchers_.remove(peer);
        if (watchers_.size() != before)
            announceViewers();
        refreshAudience();
    }
    else if (type == "viewers" && cid == voiceCid_ && channel == voiceChannel_) {
        QSet<QString> watching;
        const auto model = node_->communities().value(cid);
        for (const auto& value : data.value("peers").toArray())
            if (model.isMember(value.toString()))
                watching.insert(value.toString());
        viewers_[peer] = watching;
        refreshAudience();
    }
    else if (type == "screen-audio" && watching_.contains(peer) && cid == voiceCid_ &&
             channel == voiceChannel_ && !deafen_->isChecked()) {
        screenAudio_.play(peer, QByteArray::fromBase64(data.value("bytes").toString().toLatin1()));
    }
    else if (((type == "screen" && watching_.contains(peer)) ||
              (type == "camera" && cameraWatching_.contains(peer))) && cid == voiceCid_ &&
             channel == voiceChannel_) {
        const auto frame = data.value("frame").toDouble(-1);
        const auto index = data.value("index").toInt(-1), count = data.value("count").toInt();
        const auto chunk = QByteArray::fromBase64(data.value("bytes").toString().toLatin1());
        if (frame < 0 || frame > UINT32_MAX || frame != quint32(frame) || index < 0 || count <= 0 ||
            count > kMaxScreenChunks || index >= count || chunk.size() > kScreenChunkBytes)
            return;
        QByteArray encoded;
        const auto id = type == "camera" ? "camera/" + peer : peer;
        if (frames_.add(id, quint32(frame), index, count, chunk, encoded)) {
            const auto image = decoders_[id].decode(quint32(frame),
                                                      data.value("keyframe").toBool(), encoded);
            if (!image.isNull())
                lastScreenFrames_[id] = QDateTime::currentMSecsSinceEpoch();
            if (!image.isNull())
                type == "camera" ? stage_->setCameraFrame(peer, image)
                                 : showFrame(peer, image, encoded.size());
        }
    }
}

void CommunityPanel::joinVoice() {
    if (!online_)
        return;
    const auto cid = selectedVoiceCid_, selectedChannel = selectedVoiceChannel_;
    if (node_->communities().value(cid).channels.value(selectedChannel).type != "voice")
        return;
    requestMicrophoneAccess(this, [this, cid, selectedChannel](bool granted) {
        if (!online_)
            return;
        if (!granted) {
            noteError("Microphone access is required to join voice.");
            return;
        }
        const auto model = node_->communities().value(cid);
        if (!model.isMember(node_->peerId()) ||
            model.channels.value(selectedChannel).type != "voice")
            return;
        leaveVoice();
        QSettings settings;
        if (!audio_.start(settings.value("community/input").toString(),
                          settings.value("community/output").toString())) {
            noteError("The selected audio devices are unavailable.");
            return;
        }
        screenAudio_.setOutputDevice(settings.value("community/output").toString());
        voiceCid_ = cid;
        voiceChannel_ = selectedChannel;
        updatePushToTalk();
        voiceState();
        refresh();
    });
}

void CommunityPanel::voiceState() {
    if (voiceCid_.isEmpty() || !node_)
        return;
    const bool silent = mute_->isChecked() || deafen_->isChecked() ||
        (QSettings().value("voice/pushToTalk", false).toBool() && !pushToTalk_.held());
    audio_.setMuted(silent);
    audio_.setDeafened(deafen_->isChecked());
    attempt([&] {
        node_->setVoice(voiceCid_, voiceChannel_, silent,
                        deafen_->isChecked(), capture_.running(), cameraCapture_.running());
    });
    refresh();
}

void CommunityPanel::leaveVoice() {
    const auto previous = voiceCid_;
    presenterOverlay_.reset();
    for (const auto& [_, session] : drawings_) session->stop();
    drawings_.clear();
    voiceCid_.clear();
    pushToTalk_.stop();
    voiceChannel_.clear();
    stopCamera(false);
    stopSharing();
    if (node_ && online_ && !previous.isEmpty())
        attempt([&] { node_->setVoice(previous, {}); });
    audio_.stop();
    audioPeers_.clear();
    speaking_.clear();
    quietAudioFrames_.clear();
    lastAudioAt_.clear();
    watchers_.clear();
    for (const auto& peer : streams_) {
        stage_->removeStream(peer);
        screenAudio_.remove(peer);
    }
    for (const auto& peer : cameras_)
        stage_->setCameraFrame(peer, {});
    cameras_.clear();
    cameraWatching_.clear();
    streams_.clear();
    watching_.clear();
    lastScreenFrames_.clear();
    streamRetry_.stop();
    decoders_.clear();
    frames_ = {};
    {
        QSignalBlocker a(mute_), b(deafen_);
        mute_->setChecked(false);
        deafen_->setChecked(false);
    }
    refresh();
}

void CommunityPanel::toggleCamera() {
    if (cameraCapture_.running() || cameraPending_) {
        stopCamera();
        return;
    }
    if (!online_ || voiceCid_.isEmpty()) return;
    cameraPending_ = true;
    const auto request = ++cameraRequest_;
    const auto cid = voiceCid_, channel = voiceChannel_;
    refresh();
    requestCameraAccess(this, [this, request, cid, channel](bool granted) {
        if (request != cameraRequest_) return;
        if (!granted) {
            cameraPending_ = false;
            refresh();
            QMessageBox dialog(QMessageBox::Warning, "Camera access",
                               "Camera access is disabled. Allow camera access in your system settings, then try again.",
                               QMessageBox::Cancel, this);
            dialog.setObjectName("cameraPermissionDenied");
#if defined(Q_OS_MACOS)
            auto* settings = dialog.addButton("Open Camera Settings", QMessageBox::ActionRole);
            dialog.exec();
            if (dialog.clickedButton() == settings)
                QDesktopServices::openUrl(QUrl("x-apple.systempreferences:com.apple.preference.security?Privacy_Camera"));
#else
            dialog.exec();
#endif
            return;
        }
        const auto devices = QMediaDevices::videoInputs();
        if (devices.isEmpty()) {
            cameraPending_ = false;
            refresh();
            noteError("No camera is available.");
            return;
        }
        auto device = QMediaDevices::defaultVideoInput();
        if (devices.size() > 1) {
            QStringList choices;
            int selected = 0;
            for (int i = 0; i < devices.size(); ++i) {
                choices.append(QString::number(i + 1) + ". " + devices[i].description());
                if (devices[i] == device) selected = i;
            }
            bool accepted = false;
            const auto choice = QInputDialog::getItem(this, "Camera", "Choose a camera", choices,
                                                      selected, false, &accepted);
            if (!accepted) {
                if (request == cameraRequest_) { cameraPending_ = false; refresh(); }
                return;
            }
            device = devices[choices.indexOf(choice)];
        }
        // A permission prompt or picker may outlive the voice session.
        if (request != cameraRequest_ || !online_ || voiceCid_ != cid || voiceChannel_ != channel)
            return;
        cameraPending_ = false;
        if (!cameraCapture_.startCamera(device)) {
            refresh();
            noteError("Could not start the camera.");
            return;
        }
        voiceState();
    });
}

void CommunityPanel::stopCamera(bool updatePresence) {
    ++cameraRequest_;
    cameraPending_ = false;
    const bool wasRunning = cameraCapture_.running();
    cameraCapture_.stop();
    cameraWatchers_.clear();
    if (node_) {
        stage_->setCameraFrame(node_->peerId(), {});
        decoders_.erase("camera/" + node_->peerId());
    }
    if (updatePresence) {
        if (wasRunning) voiceState();
        else refresh();
    }
}

// Cameras play in their owner's call tile, so everyone in the call gets them.
void CommunityPanel::watchCamera(const QString& peer) {
    if (voiceCid_.isEmpty()) return;
    attempt([&] {
        node_->sendPacket(voiceCid_, voiceChannel_, "watch-camera",
                          {{"target", peer}, {"enabled", true}});
        cameraWatching_.insert(peer);
        streamRetry_.start();
    });
}

void CommunityPanel::share() {
    if (capture_.running()) {
        stopSharing();
        return;
    }
    if (voiceCid_.isEmpty() || !requestScreenRecordingAccess())
        return;
    SharePicker dialog(this);
    QPointer<SharePicker> guarded(&dialog);
    listShareSources([guarded](QList<ShareSource> sources) {
        if (guarded)
            guarded->setSources(sources);
    });
    if (dialog.exec() != QDialog::Accepted || !dialog.chosen())
        return;
    if (!capture_.start(*dialog.chosen(), dialog.shareAudio())) {
        noteError("Could not start screen sharing.");
        return;
    }
    stage_->addStream(node_->peerId(), true, name_->text().trimmed());
    startDrawing(node_->peerId(), true);
    presenterOverlay_ = std::make_unique<PresenterOverlay>(*dialog.chosen(), stage_->drawing(node_->peerId()), this);
    voiceState();
    openStage(stageOpen_);
}

void CommunityPanel::stopSharing() {
    const bool wasSharing = capture_.running();
    presenterOverlay_.reset();
    if (node_) stopDrawing(node_->peerId());
    capture_.stop();
    watchers_.clear();
    if (node_) {
        stage_->removeStream(node_->peerId());
        decoders_.erase(node_->peerId());
    }
    if (wasSharing)
        voiceState();
    openStage(stageOpen_);
}

void CommunityPanel::watch(const QString& peer, bool enabled) {
    if (voiceCid_.isEmpty())
        return;
    attempt([&] {
        node_->sendPacket(voiceCid_, voiceChannel_, "watch",
                          {{"target", peer}, {"enabled", enabled}});
        if (enabled) {
            watching_.insert(peer);
            streamRetry_.start();
            startDrawing(peer, false);
        }
        else {
            stopDrawing(peer);
            watching_.remove(peer);
            lastScreenFrames_.remove(peer);
            decoders_.erase(peer);
            frames_.remove(peer);
            screenAudio_.remove(peer);
            if (watching_.isEmpty() && cameraWatching_.isEmpty())
                streamRetry_.stop();
        }
        stage_->setWatching(peer, enabled);
    });
    refreshAudience();
    refreshPip();
}

void CommunityPanel::setStreamVolume(const QString& peer, int percent) {
    screenAudio_.setVolume(peer, linearVolume(percent));
}

void CommunityPanel::applyVoiceSettings(const QString& peer) {
    QSettings settings;
    audio_.setPeerVolume(
        peer, linearVolume(settings.value("community/voiceVolume/" + peer, 100).toInt()));
    audio_.setPeerMuted(peer, settings.value("community/voiceMuted/" + peer, false).toBool());
}

void CommunityPanel::showVoiceMemberMenu(const QString& peer, const QPoint& at) {
    QSettings settings;
    // Parented to the panel: a presence update can rebuild the row while the
    // menu is open.
    QMenu menu(this);
    addVolumeSlider(menu, settings.value("community/voiceVolume/" + peer, 100).toInt(),
                    [this, peer](int percent) {
                        QSettings().setValue("community/voiceVolume/" + peer, percent);
                        audio_.setPeerVolume(peer, linearVolume(percent));
                    });
    menu.addSeparator();
    auto* mute = menu.addAction("Mute");
    mute->setCheckable(true);
    mute->setChecked(settings.value("community/voiceMuted/" + peer, false).toBool());
    connect(mute, &QAction::toggled, this, [this, peer](bool muted) {
        QSettings().setValue("community/voiceMuted/" + peer, muted);
        audio_.setPeerMuted(peer, muted);
        refresh();
    });
    menu.exec(at);
}

void CommunityPanel::dragEnterEvent(QDragEnterEvent* event) {
    if (online_ && composer_->isEnabled() &&
        (event->mimeData()->hasImage() || event->mimeData()->hasUrls()))
        event->acceptProposedAction();
}
void CommunityPanel::dropEvent(QDropEvent* event) {
    if (!online_ || !composer_->isEnabled())
        return;
    if (event->mimeData()->hasImage()) {
        const auto image = qvariant_cast<QImage>(event->mimeData()->imageData());
        prepareUpload([image] { return prepareImage(image); });
    }
    else {
        int count = 0;
        for (const auto& url : event->mimeData()->urls())
            if (url.isLocalFile() && count++ < 4) {
                const auto path = url.toLocalFile();
                prepareUpload([path] { return prepareImageFile(path); });
            }
    }
    event->acceptProposedAction();
}

bool CommunityPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == transcript_ && event->type() == QEvent::Resize)
        placePip();
    if (watched == composer_ &&
        (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut))
        setFlag(composerFrame_, "focused", event->type() == QEvent::FocusIn);
    if (watched == qApp && event->type() == QEvent::FileOpen) {
        const auto url = static_cast<QFileOpenEvent*>(event)->url();
        if (url.scheme() == "disquisition") {
            acceptInvite(url.toString());
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

QStringList CommunityPanel::memberNames(const QString& cid) const {
    QStringList result;
    if (node_)
        for (const auto& member : node_->communities().value(cid).members)
            result.append(member.name);
    return result;
}

void CommunityPanel::updatePushToTalk() {
    QSettings settings;
    if (!voiceCid_.isEmpty() && settings.value("voice/pushToTalk", false).toBool() &&
        settings.contains("voice/pushToTalkKey"))
        pushToTalk_.start(settings.value("voice/pushToTalkKey").toUInt());
    else
        pushToTalk_.stop();
}

void CommunityPanel::startDrawing(const QString& peer, bool owner) {
    stopDrawing(peer);
    auto* drawing = stage_->drawing(peer);
    if (!drawing) return;
    auto session = std::make_unique<ScreenDrawingSession>(node_->peerId(), peer, drawing, owner);
    QHash<QString, QString> names;
    for (const auto& member : node_->communities().value(voiceCid_).members)
        names[member.id] = member.name;
    drawing->setAuthorNames(names);
    drawing->setChatColor(chatColor(messageColor_));
    const auto cid = voiceCid_, channel = voiceChannel_;
    connect(session.get(), &ScreenDrawingSession::sendToPeer, this,
        [this, cid, channel](const QString& target, const chat::Message& payload) {
            if (!online_) return;
            attempt([&] {
                node_->sendPacket(cid, channel, "drawing",
                    {{"target", target}, {"bytes", QString::fromLatin1(
                        QByteArray::fromStdString(chat::encode(payload)).toBase64())}});
            });
        });
    auto* active = session.get();
    drawings_[peer] = std::move(session);
    if (!owner) active->start();
}

void CommunityPanel::stopDrawing(const QString& peer) {
    const auto session = drawings_.find(peer);
    if (session == drawings_.end()) return;
    session->second->stop();
    drawings_.erase(session);
}

void CommunityPanel::publishSprite(const QString& cid, const QString& target) {
    if (!online_ || !node_) return;
    // Sprites describe a community member and do not depend on its channels.
    const QString channel;
    QSettings settings;
    auto sprites = PenguinSprites::fromSettings(settings);
    if (!settings.value("penguins/unlocked", false).toBool()) sprites = {};
    const auto bytes = sprites.wireData();
    if (bytes.size() > 256 * 1024) return;
    const auto chunks = splitImage(bytes);
    const auto transfer = QRandomGenerator::global()->generate();
    attempt([&] {
        if (bytes.isEmpty()) {
            node_->sendPacket(cid, channel, "sprite", {{"target", target}, {"count", 0}});
            return;
        }
        for (int index = 0; index < chunks.size(); ++index)
            node_->sendPacket(cid, channel, "sprite", {{"target", target}, {"transfer", double(transfer)},
                {"index", index}, {"count", chunks.size()},
                {"bytes", QString::fromLatin1(chunks[index].toBase64())}});
    });
}

void CommunityPanel::refreshPenguins() {
    if (!penguins_ || !node_) return;
    if (!online_) {
        penguins_->setMembers({});
        spritePeers_.clear();
        peerSprites_.clear();
        peerColors_.clear();
        spriteImages_.clear();
        return;
    }
    QSet<QString> knownPeers;
    for (const auto& community : node_->communities()) {
        const auto online = node_->online(community.id);
        // Metadata reconciliation can briefly make a live link unready. Keep its
        // cached appearance while the authenticated identity is still a member.
        for (const auto& member : community.members) knownPeers.insert(member.id);
        for (const auto& peer : online - spritePeers_.value(community.id))
            if (peer != node_->peerId()) publishSprite(community.id, peer);
        spritePeers_[community.id] = online;
    }
    for (auto it = peerSprites_.begin(); it != peerSprites_.end();)
        if (!knownPeers.contains(it.key())) it = peerSprites_.erase(it); else ++it;
    for (auto it = peerColors_.begin(); it != peerColors_.end();)
        if (!knownPeers.contains(it.key())) it = peerColors_.erase(it); else ++it;
    const auto online = node_->online(cid_);
    penguins_->setMembers(online.values());
    QMap<QString, QString> names;
    for (const auto& peer : online) names[peer] = memberName(cid_, peer);
    penguins_->setDisplayNames(names);
    for (const auto& peer : online) {
        penguins_->setColor(peer, peer == node_->peerId() ? chatColor(messageColor_)
                                                        : peerColors_.value(peer, chatColor("mint")));
        const auto sprites = peerSprites_.value(peer);
        penguins_->setCustomSprites(peer, sprites.first, sprites.second);
    }
    QSettings settings;
    const auto sprites = PenguinSprites::fromSettings(settings);
    if (settings.value("penguins/unlocked", false).toBool() && sprites.enabled)
        penguins_->setCustomSprites(node_->peerId(), sprites.standing, sprites.walking);
}

void CommunityPanel::commandReply(const QString& title, const QString& body) {
    Entry entry;
    entry.system = true;
    entry.message.timestamp = QDateTime::currentSecsSinceEpoch();
    entry.message.sender = title.toStdString();
    entry.message.body = body.toStdString();
    append(cid_, channel_, std::move(entry), false);
}

void CommunityPanel::runCommand(const QString& command) {
    const auto parts = command.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    const auto name = parts.value(0).toLower();
    const auto argument = command.mid(parts.value(0).size()).trimmed();
    if (name == "/color") {
        if (argument.isEmpty()) {
            commandReply("Color", "Current color: " + messageColor_ +
                "\nColors: pink, mint, butter, periwinkle, lilac, aqua, peach. Use /color <name or 0-255>.");
        } else if (!validChatColor(argument.toLower())) {
            commandReply("Color", "Unknown or reserved color: " + argument + ". Try /color for available colors.");
            return;
        } else {
            messageColor_ = argument.toLower();
            QSettings().setValue("community/color", messageColor_);
            if (node_) penguins_->setColor(node_->peerId(), chatColor(messageColor_));
            for (const auto& [peer, session] : drawings_)
                if (auto* drawing = stage_->drawing(peer)) drawing->setChatColor(chatColor(messageColor_));
            refresh();
        }
    } else if (name == "/help") {
        commandReply("Help", "/users  list online members\n/color <name or 0-255>  set your name color\n"
            "/voice  join the selected voice channel\n/leavevoice  leave voice\n"
            "/mute, /unmute, /deafen, /undeafen  control voice\n/gif [search]  choose a GIF\n"
            "/clear  clear this channel on this device\n/save  open User Settings\n"
            "/dnd  mute or unmute notifications\n/leave  leave this community\n"
            "/quit, /exit  close the app\n@name, @everyone and @here highlight mentions");
    } else if (name == "/users") {
        QStringList names;
        if (node_ && online_)
            for (const auto& peer : node_->online(cid_)) names.append(memberName(cid_, peer));
        names.sort(Qt::CaseInsensitive);
        commandReply("Users", names.isEmpty() ? "Nobody is online." : names.join(", "));
    } else if (name == "/clear") {
        const auto scope = key(cid_, channel_);
        for (const auto& entry : entries_.value(scope)) cachedImageBytes_ -= entry.image.size();
        entries_.remove(scope);
        cachedImages_.removeIf([&](const auto& cached) { return cached.first == scope; });
        render();
    } else if (name == "/gif") {
        if (!online_) commandReply("GIF", "Connect before sending a GIF.");
        else if (gif_->isHidden()) commandReply("GIF", "GIF search is not available in this build.");
        else gifPicker_->popup(gif_, argument);
    } else if (name == "/save" || name == "/settings") {
        settings();
    } else if (name == "/voice") {
        if (selectedVoiceChannel_.isEmpty()) commandReply("Voice", "Select a voice channel first.");
        else joinVoice();
    } else if (name == "/leavevoice") {
        leaveVoice();
    } else if (name == "/mute" || name == "/unmute" || name == "/deafen" || name == "/undeafen") {
        if (voiceCid_.isEmpty()) commandReply("Voice", "Join voice first.");
        else if (name == "/mute") mute_->setChecked(true);
        else if (name == "/deafen") deafen_->setChecked(true);
        else if (name == "/undeafen") deafen_->setChecked(false);
        else if (deafen_->isChecked()) commandReply("Voice", "Undeafen before unmuting.");
        else mute_->setChecked(false);
    } else if (name == "/dnd") {
        notifications_->muteAction()->trigger();
        commandReply("Notifications", notifications_->settings().muted ? "Muted" : "On");
    } else if (name == "/waddleon") {
        QSettings().setValue("penguins/unlocked", true);
        penguinsAction_->setVisible(true);
        penguinsAction_->setChecked(true);
        refreshPenguins();
        commandReply("Penguins", "Waddle on! Desktop penguins are now available in User Settings.");
    } else if (name == "/leave") {
        leaveCommunity();
    } else if (name == "/quit" || name == "/exit") {
        window()->close();
    } else {
        commandReply("Command", "Unknown command: " + name + ". Try /help.");
    }
    composer_->clear();
}

void CommunityPanel::leaveCommunity(const QString& target) {
    const auto cid = target.isEmpty() ? cid_ : target;
    if (!node_ || !node_->communities().value(cid).isMember(node_->peerId())) return;
    const auto model = node_->communities().value(cid);
    const bool selected = cid == cid_;
    if (model.owner == node_->peerId()) {
        commandReply("Community", "You own this community. Open Manage > Community > Delete community to remove it for everyone.");
        return;
    }
    if (QMessageBox::question(this, "Leave community", "Leave " + model.name +
        "? You will need a new invite to rejoin.", QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel) != QMessageBox::Yes) return;
    attempt([&] {
        if (voiceCid_ == model.id) leaveVoice();
        node_->edit(model.id, "leave", {});
        refresh();
        if (selected)
            render();
        note("Left " + model.name + '.');
    });
}

bool CommunityPanel::deleteCommunity(const QString& cid) {
    if (!node_) return false;
    const auto model = node_->communities().value(cid);
    if (!model.isMember(node_->peerId()) || model.owner != node_->peerId()) return false;
    if (QMessageBox::question(this, "Delete community", "Delete " + model.name +
        " for all members? This cannot be undone.", QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel) != QMessageBox::Yes) return false;
    bool deleted = false;
    attempt([&] {
        if (voiceCid_ == model.id) leaveVoice();
        node_->edit(model.id, "delete", {});
        refresh();
        render();
        note("Deleted " + model.name + '.');
        deleted = true;
    });
    return deleted;
}
