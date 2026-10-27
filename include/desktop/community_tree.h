#pragma once

#include <QTreeWidget>

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

signals:
    void voiceMoveRequested(const QString& community, const QString& peer,
                            const QString& destinationChannel);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    bool canMoveTo(const QString& community, const QString& channel, const QString& peer,
                   QTreeWidgetItem* destination) const;
    QTreeWidgetItem* voiceChannel(QTreeWidgetItem* item) const;
    void armDrag(const QPoint& position);
    bool drag(const QPoint& position, Qt::MouseButtons buttons);
    QPoint dragStart_;
    QString dragCommunity_, dragChannel_, dragPeer_;
};
