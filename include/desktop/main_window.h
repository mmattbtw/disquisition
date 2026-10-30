#pragma once

#include <QMainWindow>

class QLabel;
class QProgressBar;
class QPushButton;
class Updater;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void checkForUpdates();
    void showUpdateState();
    void offerUpdateRestart();

    QLabel* updateLabel_ = nullptr;
    QProgressBar* updateProgress_ = nullptr;
    QPushButton* updateButton_ = nullptr;
    Updater* updater_ = nullptr;
    bool updateCheckRequested_ = false;
};
