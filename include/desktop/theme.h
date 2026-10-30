#pragma once

#include <QColor>
#include <QIcon>
#include <QPalette>
#include <QPixmap>
#include <QString>

// Colors, type and the application stylesheet, matched to disquisition.app.
namespace theme {
inline constexpr auto kTeal = "#9dccca";
inline constexpr auto kInk = "#12171a"; // Text on teal.
inline constexpr auto kText = "#e6eae9";
inline constexpr auto kMuted = "#8b959e";
inline constexpr auto kDim = "#6f7a84";
inline constexpr auto kChrome = "#1f2228";
inline constexpr auto kSidebar = "#24272e";
inline constexpr auto kCanvas = "#292c34";
inline constexpr auto kRaised = "#2f343c";
inline constexpr auto kLine = "#33373f";
inline constexpr auto kLineStrong = "#424852";
inline constexpr auto kSelected = "#36464a";
inline constexpr auto kOnline = "#7ed957";
inline constexpr auto kMessage = "#d7dbde";

// Registers the bundled Inter, Cal Sans and JetBrains Mono faces once.
void loadFonts();
QString styleSheet();
// Dark palette for anything the stylesheet does not paint, such as scroll area viewports.
QPalette palette();
// A stable pastel from the site palette, so a name always gets the same tile.
QColor avatarColor(const QString& name);
// Rounded-square initial tile used when someone has no profile picture.
QPixmap initialAvatar(const QString& name, int size, qreal scale);
QPixmap brandMark(int size, qreal scale);
QIcon gearIcon(qreal scale);
} // namespace theme
