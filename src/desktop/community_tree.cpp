#include "desktop/community_tree.h"

#include <QApplication>
#include <QAbstractButton>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFontMetrics>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QTreeWidgetItemIterator>

namespace {
constexpr int kVoiceChannel = Qt::UserRole + 10;
constexpr int kCanMove = Qt::UserRole + 11;
constexpr int kSourceChannel = Qt::UserRole + 12;
constexpr auto kMime = "application/x-disquisition-voice-member";
QJsonObject payload(const QMimeData* mime) {
    return QJsonDocument::fromJson(mime->data(kMime)).object();
}
} // namespace

QPixmap voiceDragPreview(const QIcon& avatar, const QString& name) {
    constexpr int kAvatarSide = 24;
    constexpr int kGap = 8;
    constexpr int kPadding = 10;
    constexpr int kHeight = 36;
    QFont font = QApplication::font();
    font.setPixelSize(14);
    const QFontMetrics metrics(font);
    const QString label = QString(name).replace("&&", "&");
    const int width = kPadding + kAvatarSide + kGap + metrics.horizontalAdvance(label) + kPadding;
    const qreal ratio = QGuiApplication::primaryScreen()
                            ? QGuiApplication::primaryScreen()->devicePixelRatio()
                            : 1.0;
    QPixmap preview(QSize(width, kHeight) * ratio);
    preview.setDevicePixelRatio(ratio);
    preview.fill(Qt::transparent);

    QPainter painter(&preview);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setPen(QPen(QColor("#9dccca"), 1));
    painter.setBrush(QColor(0x2f, 0x36, 0x3f, 235));
    painter.drawRoundedRect(QRectF(0.5, 0.5, width - 1, kHeight - 1), 10, 10);

    const QRectF avatarRect(kPadding, (kHeight - kAvatarSide) / 2.0, kAvatarSide, kAvatarSide);
    const QPixmap picture = avatar.pixmap(kAvatarSide, kAvatarSide);
    if (!picture.isNull()) {
        QPainterPath clip;
        clip.addEllipse(avatarRect);
        painter.save();
        painter.setClipPath(clip);
        painter.drawPixmap(avatarRect.topLeft(), picture);
        painter.restore();
    }
    painter.setFont(font);
    painter.setPen(QColor("#e2f1ef"));
    painter.drawText(QRectF(kPadding + kAvatarSide + kGap, 0,
                            width - kPadding - kAvatarSide - kGap - kPadding, kHeight),
                     Qt::AlignVCenter | Qt::AlignLeft, label);
    return preview;
}

CommunityTree::CommunityTree(QWidget* parent) : QTreeWidget(parent) {
    setAcceptDrops(true);
    // The destination channel itself is highlighted while dragging.
    setDropIndicatorShown(false);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDragEnabled(false); // Only participant drags use our control protocol.
    setDefaultDropAction(Qt::MoveAction);
}

void CommunityTree::configureVoiceChannel(QTreeWidgetItem* item, const QString& community,
                                           const QString& channel, bool canMove) {
    item->setData(0, Qt::UserRole, community);
    item->setData(0, Qt::UserRole + 1, channel);
    item->setData(0, kVoiceChannel, true);
    item->setData(0, kCanMove, canMove);
    item->setFlags(canMove ? item->flags() | Qt::ItemIsDropEnabled
                           : item->flags() & ~Qt::ItemIsDropEnabled);
}

void CommunityTree::configureVoiceParticipant(QTreeWidgetItem* item, QWidget* row,
                                               const QString& community, const QString& channel,
                                               const QString& peer, bool canMove) {
    item->setData(0, Qt::UserRole, community);
    item->setData(0, Qt::UserRole + 2, peer);
    item->setData(0, kSourceChannel, channel);
    item->setData(0, kCanMove, canMove);
    // Profile buttons still handle ordinary clicks. Observe movement on every
    // child too so dragging works on the participant's name and avatar.
    row->installEventFilter(this);
    for (auto* widget : row->findChildren<QWidget*>())
        widget->installEventFilter(this);
    if (canMove)
        row->setToolTip("Drag to another voice channel to move this member");
}

QTreeWidgetItem* CommunityTree::voiceChannel(QTreeWidgetItem* item) const {
    while (item && !item->data(0, kVoiceChannel).toBool())
        item = item->parent();
    return item;
}

bool CommunityTree::canMoveTo(const QString& community, const QString& channel,
                              const QString& peer, QTreeWidgetItem* destination) const {
    const auto* target = voiceChannel(destination);
    if (!target || community.isEmpty() || channel.isEmpty() || peer.isEmpty() ||
        !target->data(0, kCanMove).toBool() ||
        target->data(0, Qt::UserRole).toString() != community ||
        target->data(0, Qt::UserRole + 1).toString() == channel)
        return false;
    // A metadata/presence update can rebuild the tree during the drag. Reject
    // stale participants and revoked permissions against the current rows.
    for (QTreeWidgetItemIterator it(const_cast<CommunityTree*>(this)); *it; ++it) {
        const auto* item = *it;
        if (item->data(0, Qt::UserRole).toString() == community &&
            item->data(0, Qt::UserRole + 2).toString() == peer &&
            item->data(0, kSourceChannel).toString() == channel &&
            item->data(0, kCanMove).toBool())
            return true;
    }
    return false;
}

bool CommunityTree::requestVoiceMove(const QString& community, const QString& channel,
                                     const QString& peer, QTreeWidgetItem* destination) {
    if (!canMoveTo(community, channel, peer, destination))
        return false;
    emit voiceMoveRequested(community, peer,
                            voiceChannel(destination)->data(0, Qt::UserRole + 1).toString());
    return true;
}

void CommunityTree::armDrag(const QPoint& position) {
    dragCommunity_.clear();
    dragChannel_.clear();
    dragPeer_.clear();
    dragAvatar_ = {};
    dragName_.clear();
    dragRow_ = nullptr;
    auto* item = itemAt(position);
    if (!item || !item->data(0, kCanMove).toBool() ||
        item->data(0, kSourceChannel).toString().isEmpty())
        return;
    dragStart_ = position;
    dragCommunity_ = item->data(0, Qt::UserRole).toString();
    dragChannel_ = item->data(0, kSourceChannel).toString();
    dragPeer_ = item->data(0, Qt::UserRole + 2).toString();
    dragRow_ = itemWidget(item, 0);
    if (dragRow_) {
        if (auto* profile = dragRow_->findChild<QPushButton*>("voiceMemberProfile")) {
            dragAvatar_ = profile->icon();
            dragName_ = profile->text();
        }
    }
}

bool CommunityTree::drag(const QPoint& position, Qt::MouseButtons buttons) {
    if (!(buttons & Qt::LeftButton) || dragPeer_.isEmpty() ||
        (position - dragStart_).manhattanLength() < QApplication::startDragDistance())
        return false;
    auto* mime = new QMimeData;
    mime->setData(kMime, QJsonDocument(QJsonObject{{"community", dragCommunity_},
                                                {"channel", dragChannel_},
                                                {"peer", dragPeer_}})
                            .toJson(QJsonDocument::Compact));
    dragPeer_.clear();
    QDrag operation(this);
    operation.setMimeData(mime);
    QPixmap preview;
    if (!dragName_.isEmpty())
        preview = voiceDragPreview(dragAvatar_, dragName_);
    else if (dragRow_)
        preview = dragRow_->grab();
    if (!preview.isNull()) {
        operation.setPixmap(preview);
        const QSizeF logical = preview.deviceIndependentSize();
        operation.setHotSpot(QPoint(qRound(logical.width() / 2), qRound(logical.height() / 2)));
    }
    operation.exec(Qt::MoveAction);
    showDropTarget({}, {});
    return true;
}

bool CommunityTree::eventFilter(QObject* watched, QEvent* event) {
    auto* widget = qobject_cast<QWidget*>(watched);
    if (!widget)
        return QTreeWidget::eventFilter(watched, event);
    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseMove ||
        event->type() == QEvent::MouseButtonRelease) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        const auto position = viewport()->mapFromGlobal(mouse->globalPosition().toPoint());
        if (event->type() == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton)
            armDrag(position);
        else if (event->type() == QEvent::MouseMove) {
            QPointer<QAbstractButton> button = qobject_cast<QAbstractButton*>(widget);
            if (drag(position, mouse->buttons())) {
                // Presence updates may delete the row while the drag runs.
                if (button)
                    button->setDown(false);
                return true;
            }
        }
        else if (event->type() == QEvent::MouseButtonRelease)
            dragPeer_.clear();
    }
    return QTreeWidget::eventFilter(watched, event);
}

void CommunityTree::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton)
        armDrag(event->position().toPoint());
    QTreeWidget::mousePressEvent(event);
}

void CommunityTree::mouseMoveEvent(QMouseEvent* event) {
    if (!drag(event->position().toPoint(), event->buttons()))
        QTreeWidget::mouseMoveEvent(event);
}

void CommunityTree::mouseReleaseEvent(QMouseEvent* event) {
    dragPeer_.clear();
    QTreeWidget::mouseReleaseEvent(event);
}

QTreeWidgetItem* CommunityTree::channelItem(const QString& community,
                                           const QString& channel) const {
    if (community.isEmpty() || channel.isEmpty())
        return nullptr;
    for (int index = 0; index < topLevelItemCount(); ++index) {
        auto* root = topLevelItem(index);
        if (root->data(0, Qt::UserRole).toString() != community)
            continue;
        for (int child = 0; child < root->childCount(); ++child) {
            auto* item = root->child(child);
            if (item->data(0, Qt::UserRole + 1).toString() == channel)
                return item;
        }
    }
    return nullptr;
}

void CommunityTree::showDropTarget(const QString& community, const QString& channel) {
    if (dropCommunity_ == community && dropChannel_ == channel)
        return;
    dropCommunity_ = community;
    dropChannel_ = channel;
    viewport()->update();
}

void CommunityTree::paintEvent(QPaintEvent* event) {
    QTreeWidget::paintEvent(event);
    if (dropChannel_.isEmpty())
        return;
    const auto* target = channelItem(dropCommunity_, dropChannel_);
    if (!target)
        return;
    const QRect rect = visualItemRect(target);
    if (!rect.isValid())
        return;
    QPainter painter(viewport());
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(0x9d, 0xcc, 0xca, 220), 1));
    painter.setBrush(QColor(0x9d, 0xcc, 0xca, 48));
    painter.drawRoundedRect(rect.adjusted(1, 1, -1, -1), 6, 6);
}

void CommunityTree::dragEnterEvent(QDragEnterEvent* event) {
    if (event->source() == this && event->mimeData()->hasFormat(kMime))
        event->acceptProposedAction();
    else
        event->ignore();
}

void CommunityTree::dragMoveEvent(QDragMoveEvent* event) {
    const auto data = payload(event->mimeData());
    auto* destination = itemAt(event->position().toPoint());
    auto* target = voiceChannel(destination);
    if (event->source() == this && target &&
        canMoveTo(data.value("community").toString(), data.value("channel").toString(),
                  data.value("peer").toString(), destination)) {
        event->acceptProposedAction();
        showDropTarget(target->data(0, Qt::UserRole).toString(),
                       target->data(0, Qt::UserRole + 1).toString());
    }
    else {
        event->ignore();
        showDropTarget({}, {});
    }
}

void CommunityTree::dragLeaveEvent(QDragLeaveEvent* event) {
    showDropTarget({}, {});
    QTreeWidget::dragLeaveEvent(event);
}

void CommunityTree::dropEvent(QDropEvent* event) {
    showDropTarget({}, {});
    const auto data = payload(event->mimeData());
    if (event->source() == this &&
        requestVoiceMove(data.value("community").toString(), data.value("channel").toString(),
                         data.value("peer").toString(), itemAt(event->position().toPoint())))
        event->acceptProposedAction();
    else
        event->ignore();
}
