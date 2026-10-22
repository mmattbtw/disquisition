#include "desktop/main_window.h"

#include <QAction>
#include <QMenuBar>

#include "desktop/community_panel.h"

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("disquisition");
    resize(1080, 740);
    auto* communities = new CommunityPanel(this);
    setCentralWidget(communities);
    auto* appMenu = menuBar()->addMenu("disquisition");
    auto* preferences = appMenu->addAction("Preferences…");
    preferences->setMenuRole(QAction::PreferencesRole);
    preferences->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));
    connect(preferences, &QAction::triggered, communities, &CommunityPanel::showSettings);
    setStyleSheet(R"(
        QWidget { background: #282c34; color: #c5c8c6; font-family: Menlo, Monaco, "Courier New"; font-size: 14px; }
        QLabel#title { background: #9dccca; color: #20262c; font-size: 15px; font-weight: 700; padding: 3px 5px; }
        QLineEdit, QSpinBox, QComboBox, QTextBrowser, QListWidget, QTreeWidget { background: #282c34; color: #c5c8c6; border: 1px solid #545b66; border-radius: 0; padding: 5px; selection-background-color: #9dccca; selection-color: #20262c; }
        QLineEdit[invalid="true"] { background: #3b2529; color: #ffb3b3; border: 2px solid #e05252; }
        QComboBox QAbstractItemView { background: #282c34; color: #c5c8c6; selection-background-color: #9dccca; selection-color: #20262c; }
        QTextBrowser { border: 0; padding: 0; }
        QListWidget { border-left: 1px solid #545b66; border-top: 0; border-right: 0; border-bottom: 0; }
        QPushButton { background: #333842; color: #d5d7d6; border: 1px solid #777f89; border-radius: 0; padding: 6px 12px; }
        QPushButton:hover { background: #9dccca; color: #20262c; }
        QPushButton#primary { background: #9dccca; color: #20262c; border-color: #9dccca; }
        QPushButton:checked { background: #e05252; color: #fff3f3; border-color: #ff8a8a; }
        QPushButton:disabled { color: #70757c; background: #2d3139; }
        QTreeWidget { border: 0; background: #242830; padding: 6px; }
        QTreeWidget::item { padding: 6px 2px; }
        QTreeWidget::item:selected { background: #39484b; color: #d5e9e6; }
        QListWidget#communityManageCategories { background: #242830; border: 0; padding: 8px; }
        QListWidget#communityManageCategories::item { padding: 10px 12px; margin-bottom: 3px; }
        QListWidget#communityManageCategories::item:selected { background: #39484b; color: #d5e9e6; }
        QLabel#settingsHeading { font-size: 19px; font-weight: 700; margin-bottom: 12px; }
        QWidget#communityVoiceControls { background: #242f32; }
        QPushButton#communityJoinVoice { color: #9dccca; border-color: #596f70; padding: 4px 8px; }
        QSplitter::handle { width: 1px; background: #545b66; }
        QSplitter::handle:vertical { height: 1px; }
        QFrame#streamTile { border: 1px solid #545b66; }
        QLabel#streamName { font-weight: 700; }
        QLabel#live { background: #e05252; color: #fff3f3; font-size: 11px; font-weight: 700; padding: 1px 5px; }
        QLabel#streamStats { color: #8a9099; font-size: 12px; }
        QScrollBar:vertical { background: #282c34; width: 10px; }
        QScrollBar::handle:vertical { background: #68717c; min-height: 24px; }
        QFrame#gifPicker { border: 1px solid #777f89; }
        QFrame#gifPicker QListWidget { border: 0; padding: 0; }
        QFrame#gifPicker QListWidget::item:hover, QFrame#gifPicker QListWidget::item:selected { background: #9dccca; }
        QLabel#gifStatus, QLabel#gifAttribution { color: #8a9099; font-size: 12px; }
        QLabel#dropOverlay { background: rgba(32, 38, 44, 225); color: #9dccca; border: 2px dashed #9dccca; font-size: 16px; font-weight: 700; }
    )");
}
