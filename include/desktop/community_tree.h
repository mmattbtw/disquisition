#pragma once

#include <QIcon>
#include <QPixmap>
#include <QPointer>
#include <QTreeWidget>

class QDragLeaveEvent;

// A compact avatar-and-name pill that follows the cursor while a voice member
// is dragged to another channel.
QPixmap voiceDragPreview(const QIcon& avatar, const QString& name);

// Voice moves are requests to the authenticated community node. The tree never
// reparents a participant locally; presence updates determine their position.
class CommunityTree final : public QTreeWidget {
    Q_OBJECT
public:
    explicit CommunityTree(QWidget* parent = nullptr);
    void configureVoiceChannel(QTreeWidgetItem* item, const QString& community,
                               const QString& channel, bool canMove);
    void configureVoiceParticipant(QTreeWidgetItem* item, QWidget* row,
                                   const QString& community, const QString& channel,
                                   const QString& peer, bool canMove);
    bool requestVoiceMove(const QString& community, const QString& channel,
                          const QString& peer, QTreeWidgetItem* destination);
    // Paints a drop highlight around the channel, independent of selection.
    void showDropTarget(const QString& community, const QString& channel);
    QString dropHighlight() const { return dropChannel_; }

signals:
    void voiceMoveRequested(const QString& community, const QString& peer,
                            const QString& destinationChannel);
    void voiceChannelDoubleClicked(const QString& community, const QString& channel);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    bool canMoveTo(const QString& community, const QString& channel, const QString& peer,
                   QTreeWidgetItem* destination) const;
    QTreeWidgetItem* voiceChannel(QTreeWidgetItem* item) const;
    QTreeWidgetItem* channelItem(const QString& community, const QString& channel) const;
    void armDrag(const QPoint& position);
    bool drag(const QPoint& position, Qt::MouseButtons buttons);
    QPoint dragStart_;
    QString dragCommunity_, dragChannel_, dragPeer_;
    QIcon dragAvatar_;
    QString dragName_;
    QPointer<QWidget> dragRow_;
    QString dropCommunity_, dropChannel_;
};
