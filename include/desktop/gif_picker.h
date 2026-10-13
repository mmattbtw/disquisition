#pragma once

#include <QFrame>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <memory>
#include <vector>

#include "desktop/gif_search.h"

class MediaFetcher;
class QLabel;
class QLineEdit;
class QListWidget;
class QMovie;
class QTabBar;

// A Discord-style GIF popup: trending GIFs until you type, then search
// results, with more loading as you scroll. Choosing one emits `chosen`.
class GifPicker final : public QFrame {
    Q_OBJECT

public:
    GifPicker(GifSearch* search, MediaFetcher* fetcher, QWidget* parent = nullptr);
    ~GifPicker() override;

    // Opens above `anchor`. An empty query shows trending GIFs.
    void popup(QWidget* anchor, const QString& query = {});

signals:
    void chosen(const QUrl& gif);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    // Returns true when the available providers changed.
    bool refreshProviders();
    GifProvider provider() const;
    void startSearch();
    void loadMore();
    void showPage(GifProvider provider, const QString& query, int page, const GifPage& results);
    void showFailure(GifProvider provider, const QString& query, int page, const QString& message);
    void setStatus(const QString& text);
    void choose(const QUrl& gif);

    GifSearch* search_;
    MediaFetcher* fetcher_;
    QTabBar* providers_ = nullptr;
    QLineEdit* query_ = nullptr;
    QLabel* status_ = nullptr;
    QListWidget* results_ = nullptr;
    QLabel* attribution_ = nullptr;
    QTimer debounce_;
    std::vector<std::unique_ptr<QMovie>> previews_;
    QString shownQuery_;
    int page_ = 0;          // the last page shown
    int requestedPage_ = 0; // the page in flight while loading_
    bool hasMore_ = false;
    bool loading_ = false;
    // Each search starts a new generation, so late previews from an
    // earlier one are dropped.
    quint64 generation_ = 0;
};
