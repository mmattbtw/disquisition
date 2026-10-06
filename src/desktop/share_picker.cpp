#include "desktop/share_picker.h"

#include <QDialogButtonBox>
#include <QListWidget>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

namespace {

constexpr int kSourceIndexRole = Qt::UserRole;

// Replaces the list with one line of text that cannot be selected.
void showNotice(QListWidget* list, const QString& text) {
    list->clear();
    auto* item = new QListWidgetItem(text, list);
    item->setFlags(Qt::NoItemFlags);
}

} // namespace

SharePicker::SharePicker(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Share your screen");
    resize(560, 440);

    windows_ = new QListWidget(this);
    screens_ = new QListWidget(this);
    tabs_ = new QTabWidget(this);
    tabs_->addTab(windows_, "Applications");
    tabs_->addTab(screens_, "Screens");
    showNotice(windows_, "loading...");
    showNotice(screens_, "loading...");

    auto* buttons = new QDialogButtonBox(this);
    goLive_ = buttons->addButton("Go Live", QDialogButtonBox::AcceptRole);
    goLive_->setObjectName("primary");
    buttons->addButton(QDialogButtonBox::Cancel);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(tabs_, 1);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(tabs_, &QTabWidget::currentChanged, this, &SharePicker::updateGoLive);
    for (QListWidget* list : {windows_, screens_}) {
        connect(list, &QListWidget::currentItemChanged, this, &SharePicker::updateGoLive);
        connect(list, &QListWidget::itemDoubleClicked, this, [this] {
            if (chosen()) accept();
        });
    }
    updateGoLive();
}

void SharePicker::setSources(const QList<ShareSource>& sources) {
    sources_ = sources;
    windows_->clear();
    screens_->clear();
    for (qsizetype i = 0; i < sources_.size(); ++i) {
        const ShareSource& source = sources_[i];
        QListWidget* list = source.kind == ShareSource::Kind::Window ? windows_ : screens_;
        const QString text =
            source.detail.isEmpty() ? source.title : source.title + "  -  " + source.detail;
        auto* item = new QListWidgetItem(text, list);
        item->setData(kSourceIndexRole, static_cast<int>(i));
        item->setToolTip(text);
    }

    for (QListWidget* list : {windows_, screens_}) {
        if (list->count() > 0) {
            list->setCurrentRow(0);
        }
    }
    if (windows_->count() == 0) {
        showNotice(windows_, "no windows can be shared");
    }
    if (screens_->count() == 0) {
        showNotice(screens_, "no screens can be shared");
    }
    // Some systems, such as Wayland desktops, offer screens but no windows.
    if (windows_->item(0)->flags() == Qt::NoItemFlags &&
        screens_->item(0)->flags() != Qt::NoItemFlags) {
        tabs_->setCurrentWidget(screens_);
    }
    updateGoLive();
}

std::optional<ShareSource> SharePicker::chosen() const {
    const QListWidgetItem* item = currentList()->currentItem();
    if (!item || !(item->flags() & Qt::ItemIsSelectable)) {
        return std::nullopt;
    }
    return sources_.at(item->data(kSourceIndexRole).toInt());
}

QListWidget* SharePicker::currentList() const {
    return tabs_->currentWidget() == windows_ ? windows_ : screens_;
}

void SharePicker::updateGoLive() {
    goLive_->setEnabled(chosen().has_value());
}
