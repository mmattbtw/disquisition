#pragma once

#include <QObject>
#include <QSet>

class GlobalHotkeys final : public QObject {
    Q_OBJECT
public:
    explicit GlobalHotkeys(QObject* parent = nullptr);
    ~GlobalHotkeys() override;
    bool set(int id, bool enabled, Qt::KeyboardModifiers modifiers, Qt::Key key);
    bool registered(int id) const { return registered_.contains(id); }

signals:
    void activated(int id);

private:
    QSet<int> registered_;
    void* window_ = nullptr;
};
