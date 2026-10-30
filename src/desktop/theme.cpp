#include "desktop/theme.h"

#include <QFont>
#include <QFontDatabase>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QtMath>
#include <iterator>
#include <utility>

namespace theme {

void loadFonts() {
    static const bool loaded = [] {
        for (const char* font : {":/fonts/Inter-Regular.ttf", ":/fonts/Inter-SemiBold.ttf",
                                 ":/fonts/Inter-Bold.ttf", ":/fonts/CalSans-Regular.ttf",
                                 ":/fonts/JetBrainsMono-Regular.ttf"})
            QFontDatabase::addApplicationFont(font);
        return true;
    }();
    Q_UNUSED(loaded);
}

QString styleSheet() {
    return QStringLiteral(R"(
* { outline: 0; }
QWidget { color: #e6eae9; font-family: Inter, "Segoe UI", "Helvetica Neue", sans-serif; font-size: 14px; }
QMainWindow { background: #292c34; }
QMenuBar { background: #1f2228; color: #8b959e; }
QDialog { background: #24272e; }
QToolTip { background: #1f2228; color: #e6eae9; border: 1px solid #424852; border-radius: 6px; padding: 5px 8px; font-size: 12px; }
QMenu { background: #1f2228; border: 1px solid #424852; border-radius: 10px; padding: 6px; }
QMenu::item { background: transparent; padding: 7px 16px; border-radius: 6px; }
QMenu::item:selected { background: #36464a; color: #e2f1ef; }
QMenu::item:disabled { color: #6f7a84; }
QMenu::separator { height: 1px; background: #33373f; margin: 5px 6px; }

QLineEdit, QPlainTextEdit, QSpinBox, QComboBox {
    background: #1f2228; color: #e6eae9; border: 1px solid #3a3f48; border-radius: 8px;
    padding: 7px 10px; selection-background-color: #9dccca; selection-color: #12171a; }
QLineEdit:hover, QPlainTextEdit:hover, QSpinBox:hover, QComboBox:hover { border-color: #4a515c; }
QLineEdit:focus, QPlainTextEdit:focus, QSpinBox:focus, QComboBox:focus { border-color: #9dccca; }
QLineEdit:disabled, QPlainTextEdit:disabled, QSpinBox:disabled, QComboBox:disabled {
    color: #8b959e; background: #24272e; border-color: #30343b; }
QLineEdit[invalid="true"] { background: #3b2529; color: #ffb3b3; border: 1px solid #e05252; }
QComboBox { padding-right: 28px; }
QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: center right; width: 26px; border: 0; }
QComboBox::down-arrow { image: url(:/ui/chevron-down.png); width: 12px; height: 12px; }
QComboBox QAbstractItemView { background: #1f2228; color: #e6eae9; border: 1px solid #424852;
    padding: 4px; selection-background-color: #36464a; selection-color: #e2f1ef; outline: 0; }
QSpinBox { padding-right: 24px; }
QSpinBox::up-button, QSpinBox::down-button { subcontrol-origin: border; width: 22px; border: 0; background: transparent; }
QSpinBox::up-button { subcontrol-position: top right; }
QSpinBox::down-button { subcontrol-position: bottom right; }
QSpinBox::up-arrow { image: url(:/ui/chevron-up.png); width: 12px; height: 12px; }
QSpinBox::down-arrow { image: url(:/ui/chevron-down.png); width: 12px; height: 12px; }

QPushButton { background: #2f343c; color: #e6eae9; border: 1px solid #424852; border-radius: 8px;
    padding: 7px 14px; font-size: 14px; font-weight: 600; }
QPushButton:hover { background: #373d46; border-color: #535b68; }
QPushButton:pressed { background: #2a2f36; }
QPushButton:disabled { background: #272a31; color: #5f6872; border-color: #31353c; }
QPushButton:checked { background: #3d2529; color: #ffb3b3; border-color: #a8434a; }
QPushButton:checked:hover { background: #4a2a2f; border-color: #e05252; }
QPushButton#primary, QPushButton[primary="true"] { background: #9dccca; color: #12171a; border-color: #9dccca; }
QPushButton#primary:hover, QPushButton[primary="true"]:hover { background: #c4efeb; border-color: #c4efeb; }
QPushButton#primary:disabled, QPushButton[primary="true"]:disabled { background: #34413f; color: #6c7f7d; border-color: #34413f; }
QPushButton[danger="true"] { color: #ff9b9b; background: #2f2a2f; border-color: #5c3438; }
QPushButton[danger="true"]:hover { background: #3d2a2e; border-color: #e05252; }
QPushButton[quiet="true"] { background: transparent; border-color: transparent; color: #8b959e; padding: 6px; }
QPushButton[quiet="true"]:hover { background: #33373f; color: #e6eae9; }
QPushButton[compact="true"] { padding: 5px 11px; font-size: 13px; border-radius: 7px; }

QCheckBox { spacing: 10px; background: transparent; }
QCheckBox::indicator { width: 18px; height: 18px; border-radius: 5px; border: 1px solid #515967; background: #1f2228; }
QCheckBox::indicator:hover { border-color: #9dccca; }
QCheckBox::indicator:checked { background: #9dccca; border-color: #9dccca; image: url(:/ui/check.png); }
QCheckBox:disabled { color: #6f7a84; }

QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle { background: #3e444e; border-radius: 3px; }
QScrollBar::handle:vertical { min-height: 28px; }
QScrollBar::handle:horizontal { min-width: 28px; }
QScrollBar::handle:hover { background: #56606c; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QScrollArea { background: transparent; border: 0; }
QSplitter::handle { background: #33373f; }
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }
QProgressBar { background: #33373f; border: 0; border-radius: 2px; }
QProgressBar::chunk { background: #9dccca; border-radius: 2px; }
QTextBrowser { background: transparent; border: 0; }
QListWidget, QTreeWidget { background: transparent; border: 0; }

QWidget#topBar { background: #1f2228; border-bottom: 1px solid #33373f; }
QLabel#brandName { font-family: "Cal Sans", Inter, sans-serif; font-size: 19px; color: #edf0ef; }
QLineEdit#communityName { padding: 6px 10px; }
QLabel#connectionStatus { color: #8b959e; font-size: 13px; }

QWidget#sidebar { background: #24272e; }
QTreeWidget#communityTree { padding: 2px 8px; show-decoration-selected: 0; selection-background-color: transparent; }
QTreeWidget#communityTree::item { padding: 5px 4px; border-radius: 7px; margin: 1px 0; }
QTreeWidget#communityTree::item:hover { background: #2c3037; }
QTreeWidget#communityTree::item:selected { background: #36464a; }
QTreeWidget#communityTree::branch { background: transparent; }
QTreeWidget#communityTree::branch:selected, QTreeWidget#communityTree::branch:hover { background: #24272e; }
QTreeWidget#communityTree::branch:has-children:closed { image: url(:/ui/chevron-right.png); }
QTreeWidget#communityTree::branch:has-children:open { image: url(:/ui/chevron-down.png); }
QPushButton#voiceMemberProfile { background: transparent; border: 0; padding: 0 4px; text-align: left;
    color: #c9d0d4; font-size: 14px; font-weight: 400; }
QPushButton#voiceMemberProfile:hover { color: #edf0ef; }
QPushButton#communityJoinVoice { background: #9dccca; color: #12171a; border-color: #9dccca; padding: 5px 10px; font-size: 13px; border-radius: 7px; }
QPushButton#communityJoinVoice:hover { background: #c4efeb; border-color: #c4efeb; }
QLabel#communityJoiningLabel { color: #9dccca; font-size: 13px; }
QWidget#userCard { background: #1f2228; border-top: 1px solid #33373f; }
QLabel#userCardName { font-weight: 600; color: #edf0ef; }
QLabel#userCardState { color: #8b959e; font-size: 12px; }

QWidget#conversation { background: #292c34; }
QWidget#channelHeader { border-bottom: 1px solid #33373f; }
QLabel#channelHeading { font-family: "Cal Sans", Inter, sans-serif; font-size: 22px; color: #edf0ef; }
QLabel#channelTopic { color: #8b959e; font-size: 13px; }
QTextBrowser#communityTranscript { padding: 0 14px; }
QListWidget#communityMembers { background: #262930; border-left: 1px solid #33373f; padding: 12px 8px; }
QFrame#composerFrame { background: #1f2228; border: 1px solid #3a3f48; border-radius: 10px; }
QFrame#composerFrame[focused="true"] { border-color: #9dccca; }
QFrame#composerFrame[inactive="true"] { background: #24272e; border-color: #30343b; }
QLineEdit#communityComposer { background: transparent; border: 0; padding: 9px 2px; }
QLineEdit#communityComposer:disabled { background: transparent; }
QLabel#composerPrefix { color: #6f7a84; font-size: 17px; font-weight: 600; }
QLabel#communityStatus { color: #ffb3b3; font-size: 13px; }

QWidget#communityVoiceControls { background: #1f2628; border-top: 1px solid #2d3a3b; }
QLabel#communityVoiceStatus { font-weight: 600; color: #edf0ef; }
QLabel#voiceDetail { color: #8b959e; font-size: 13px; }

QLabel#dialogTitle { font-family: "Cal Sans", Inter, sans-serif; font-size: 30px; color: #edf0ef; }
QLabel#dialogSubtitle { color: #8b959e; }
QLabel#fieldLabel { font-family: "JetBrains Mono", monospace; font-size: 11px; color: #8b959e; }
QLabel#fieldHint { color: #8b959e; font-size: 13px; }
QFrame#dialogRule { background: #33373f; border: 0; min-height: 1px; max-height: 1px; }
QWidget#dialogFooter { background: #1f2228; border-top: 1px solid #33373f; }
QLabel#footerNote { color: #6f7a84; font-size: 12px; }
QFrame#inviteQr { background: #f4efe4; border-radius: 12px; }
QFrame#inviteQr QLabel { background: transparent; color: #3b4148; font-family: "JetBrains Mono", monospace; font-size: 11px; }
QLabel#settingsDescription { color: #8b959e; }
QLabel#settingsHeading { font-family: "Cal Sans", Inter, sans-serif; font-size: 24px; color: #edf0ef; margin-bottom: 8px; }
QListWidget#userSettingsCategories, QListWidget#communityManageCategories {
    background: #1f2228; border-right: 1px solid #33373f; padding: 16px 10px; }
QListWidget#userSettingsCategories::item, QListWidget#communityManageCategories::item {
    padding: 9px 12px; margin-bottom: 2px; border-radius: 7px; color: #b7c0c6; }
QListWidget#userSettingsCategories::item:hover, QListWidget#communityManageCategories::item:hover { background: #2a2e35; }
QListWidget#userSettingsCategories::item:selected, QListWidget#communityManageCategories::item:selected {
    background: #36464a; color: #e2f1ef; }

QDialog#userProfileCard { background: #1f2228; border: 1px solid #424852; }
QDialog#userProfileCard QLabel, QDialog#userProfileCard QScrollArea, QDialog#userProfileCard QScrollArea QWidget { background: transparent; }
QLabel#profileDisplayName { font-family: "Cal Sans", Inter, sans-serif; color: #edf0ef; font-size: 24px; }
QLabel#profileBio { color: #c9d0d4; }

QPushButton#update { background: #9dccca; color: #12171a; border-color: #9dccca; padding: 1px 10px; font-size: 12px; border-radius: 6px; }
QLabel#updateStatus { color: #8b959e; font-size: 12px; }
QStatusBar { background: #1f2228; border-top: 1px solid #33373f; }

QFrame#streamTile { background: #1a1d22; border: 1px solid #33373f; border-radius: 12px; }
QFrame#streamTile QLabel { background: transparent; }
QLabel#streamName { font-weight: 600; font-size: 14px; color: #edf0ef; }
QFrame#streamTile QLabel#live { background: #ff3131; color: #ffffff; font-family: "JetBrains Mono", monospace; font-size: 10px; font-weight: 700; padding: 2px 6px; border-radius: 4px; }
QLabel#streamStats { color: #8b959e; font-family: "JetBrains Mono", monospace; font-size: 11px; }
QFrame#streamTile QPushButton { padding: 4px 10px; font-size: 12px; border-radius: 7px; }
QFrame#streamTile QPushButton#primary { padding: 7px 16px; font-size: 14px; border-radius: 8px; }
QFrame#streamTile QWidget#drawingControls { background: #16191f; border: 1px solid #3a424c; border-radius: 10px; }
QWidget#drawingControls QPushButton { padding: 5px 10px; font-size: 12px; background: transparent; border: 1px solid transparent; color: #dfe4e4; }
QWidget#drawingControls QPushButton:hover { background: #33373f; }
QWidget#drawingControls QPushButton:checked { background: #2f5549; border-color: #67e8a5; color: #effff8; }
QWidget#drawingControls QComboBox { padding: 3px 26px 3px 8px; font-size: 12px; border-radius: 7px; }
QWidget#drawingControls QLabel { color: #8b959e; font-size: 12px; }
QSlider#streamVolume::groove:horizontal { background: #33373f; height: 4px; border-radius: 2px; }
QSlider#streamVolume::sub-page:horizontal { background: #9dccca; border-radius: 2px; }
QSlider#streamVolume::handle:horizontal { background: #e6eae9; width: 12px; margin: -4px 0; border-radius: 6px; }
QSlider#streamVolume::handle:horizontal:hover { background: #9dccca; }

QFrame#gifPicker { background: #1f2228; border: 1px solid #424852; border-radius: 12px; }
QFrame#gifPicker QListWidget { border: 0; padding: 0; }
QFrame#gifPicker QListWidget::item { border-radius: 6px; }
QFrame#gifPicker QListWidget::item:hover, QFrame#gifPicker QListWidget::item:selected { background: #36464a; }
QLabel#gifStatus, QLabel#gifAttribution { color: #8b959e; font-size: 12px; }
QLabel#dropOverlay { background: rgba(31, 34, 40, 230); color: #9dccca; border: 2px dashed #9dccca; border-radius: 14px;
    font-family: "Cal Sans", Inter, sans-serif; font-size: 20px; }
)");
}

QPalette palette() {
    QPalette result;
    const QColor text(kText), canvas(kSidebar), base(kChrome);
    for (const auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
        const bool disabled = group == QPalette::Disabled;
        result.setColor(group, QPalette::Window, canvas);
        result.setColor(group, QPalette::WindowText, disabled ? QColor(kDim) : text);
        result.setColor(group, QPalette::Base, base);
        result.setColor(group, QPalette::AlternateBase, QColor(kCanvas));
        result.setColor(group, QPalette::Text, disabled ? QColor(kDim) : text);
        result.setColor(group, QPalette::Button, QColor(kRaised));
        result.setColor(group, QPalette::ButtonText, disabled ? QColor(kDim) : text);
        result.setColor(group, QPalette::Highlight, QColor(kSelected));
        result.setColor(group, QPalette::HighlightedText, QColor("#e2f1ef"));
        result.setColor(group, QPalette::PlaceholderText, QColor(kDim));
        result.setColor(group, QPalette::ToolTipBase, base);
        result.setColor(group, QPalette::ToolTipText, text);
        result.setColor(group, QPalette::Link, QColor(kTeal));
        result.setColor(group, QPalette::Light, QColor(kLineStrong));
        result.setColor(group, QPalette::Midlight, QColor(kLine));
        result.setColor(group, QPalette::Mid, QColor(kLine));
        result.setColor(group, QPalette::Dark, QColor(kChrome));
        result.setColor(group, QPalette::Shadow, QColor("#101215"));
    }
    return result;
}

QColor avatarColor(const QString& name) {
    // Softened versions of the brand mark's wedges.
    static const QColor palette[]{QColor("#a5dfdc"), QColor("#ffd666"), QColor("#ff8fcf"),
                                  QColor("#94e070"), QColor("#b49cff"), QColor("#ff9f63"),
                                  QColor("#5fd3ea")};
    uint hash = 0;
    for (const auto character : name.trimmed().toLower())
        hash = hash * 31 + character.unicode();
    return palette[hash % std::size(palette)];
}

QPixmap initialAvatar(const QString& name, int size, qreal scale) {
    QPixmap result(QSize(size, size) * scale);
    result.setDevicePixelRatio(scale);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(avatarColor(name));
    painter.drawRoundedRect(QRectF(0, 0, size, size), size * 0.26, size * 0.26);
    QFont font("Inter");
    font.setPixelSize(qMax(8, qRound(size * 0.42)));
    font.setWeight(QFont::Bold);
    painter.setFont(font);
    painter.setPen(QColor("#1a1d22"));
    painter.drawText(QRectF(0, 0, size, size), Qt::AlignCenter, name.trimmed().left(1).toUpper());
    return result;
}

QPixmap brandMark(int size, qreal scale) {
    QPixmap source(":/brand/disquisition-mark.png");
    if (source.isNull())
        return {};
    auto result = source.scaled(QSize(size, size) * scale, Qt::KeepAspectRatio,
                                Qt::SmoothTransformation);
    result.setDevicePixelRatio(scale);
    return result;
}

QIcon gearIcon(qreal scale) {
    QIcon icon;
    for (const auto& [mode, color] : {std::pair{QIcon::Normal, QColor("#8b959e")},
                                      std::pair{QIcon::Active, QColor("#e6eae9")},
                                      std::pair{QIcon::Disabled, QColor("#4f5761")}}) {
        QPixmap image(QSize(18, 18) * scale);
        image.setDevicePixelRatio(scale);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        const QPointF center(9, 9);
        QPainterPath teeth;
        for (int i = 0; i < 8; ++i) {
            const qreal angle = qDegreesToRadians(i * 45.0);
            QTransform rotation;
            rotation.translate(center.x(), center.y());
            rotation.rotateRadians(angle);
            QPainterPath tooth;
            tooth.addRoundedRect(QRectF(-1.6, -8.2, 3.2, 4), 0.8, 0.8);
            teeth.addPath(rotation.map(tooth));
        }
        QPainterPath ring;
        ring.addEllipse(center, 5.6, 5.6);
        QPainterPath hole;
        hole.addEllipse(center, 2.3, 2.3);
        painter.fillPath(teeth.united(ring).subtracted(hole), color);
        painter.end();
        icon.addPixmap(image, mode);
    }
    return icon;
}

} // namespace theme
