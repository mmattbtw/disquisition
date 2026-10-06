#pragma once

#include <QDialog>
#include <QList>

#include <optional>

#include "desktop/share_source.h"

class QListWidget;
class QPushButton;
class QTabWidget;

// Asks what to share, Discord style: an Applications tab listing windows and
// a Screens tab listing displays, then "Go Live".
class SharePicker final : public QDialog {
    Q_OBJECT
public:
    explicit SharePicker(QWidget* parent = nullptr);
    // Until this is called the lists say they are loading.
    void setSources(const QList<ShareSource>& sources);
    // The selection, once the dialog has been accepted.
    std::optional<ShareSource> chosen() const;

private:
    QListWidget* currentList() const;
    void updateGoLive();

    QTabWidget* tabs_ = nullptr;
    QListWidget* windows_ = nullptr;
    QListWidget* screens_ = nullptr;
    QPushButton* goLive_ = nullptr;
    QList<ShareSource> sources_;
};
