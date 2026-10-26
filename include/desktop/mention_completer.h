#pragma once

#include <QObject>
#include <QStringList>

class QAbstractItemView;
class QCompleter;
class QEvent;
class QLineEdit;
class QStringListModel;

class MentionCompleter final : public QObject {
    Q_OBJECT

public:
    explicit MentionCompleter(QLineEdit* editor);

    void setMembers(const QStringList& members);
    QAbstractItemView* popup() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void update();
    void insert(const QString& name);

    QLineEdit* editor_;
    QStringListModel* model_;
    QCompleter* completer_;
};
