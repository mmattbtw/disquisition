#include "desktop/community_tree.h"

#include <QApplication>
#include <QAbstractButton>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QMouseEvent>
#include <QPointer>
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

CommunityTree::CommunityTree(QWidget* parent) : QTreeWidget(parent) {
    setAcceptDrops(true);
    setDropIndicatorShown(true);
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
    const auto* item = itemAt(position);
    if (!item || !item->data(0, kCanMove).toBool() ||
        item->data(0, kSourceChannel).toString().isEmpty())
        return;
    dragStart_ = position;
    dragCommunity_ = item->data(0, Qt::UserRole).toString();
    dragChannel_ = item->data(0, kSourceChannel).toString();
    dragPeer_ = item->data(0, Qt::UserRole + 2).toString();
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
    operation.exec(Qt::MoveAction);
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

void CommunityTree::dragEnterEvent(QDragEnterEvent* event) {
    if (event->source() == this && event->mimeData()->hasFormat(kMime))
        event->acceptProposedAction();
    else
        event->ignore();
}

void CommunityTree::dragMoveEvent(QDragMoveEvent* event) {
    const auto data = payload(event->mimeData());
    if (event->source() == this &&
        canMoveTo(data.value("community").toString(), data.value("channel").toString(),
                  data.value("peer").toString(), itemAt(event->position().toPoint())))
        event->acceptProposedAction();
    else
        event->ignore();
}

void CommunityTree::dropEvent(QDropEvent* event) {
    const auto data = payload(event->mimeData());
    if (event->source() == this &&
        requestVoiceMove(data.value("community").toString(), data.value("channel").toString(),
                         data.value("peer").toString(), itemAt(event->position().toPoint())))
        event->acceptProposedAction();
    else
        event->ignore();
}
