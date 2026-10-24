#include "desktop/chat_color.h"

#include <QHash>

namespace {

QColor xtermColor(int index) {
    static const int basic[][3] = {
        {0, 0, 0},       {205, 49, 49},   {13, 188, 121}, {229, 229, 16},
        {36, 114, 200},  {188, 63, 188},  {17, 168, 205}, {229, 229, 229},
        {102, 102, 102}, {241, 76, 76},   {35, 209, 139}, {245, 245, 67},
        {59, 142, 234},  {214, 112, 214}, {41, 184, 219}, {255, 255, 255}};
    if (index < 16) return QColor(basic[index][0], basic[index][1], basic[index][2]);
    if (index < 232) {
        const int value = index - 16;
        const int levels[] = {0, 95, 135, 175, 215, 255};
        return QColor(levels[value / 36], levels[(value / 6) % 6], levels[value % 6]);
    }
    const int gray = 8 + (index - 232) * 10;
    return QColor(gray, gray, gray);
}

}  // namespace

QColor chatColor(const QString& value) {
    static const QHash<QString, int> named {
        {"pink", 211}, {"mint", 121}, {"butter", 229}, {"periwinkle", 111},
        {"lilac", 183}, {"aqua", 159}, {"peach", 216}};
    if (named.contains(value)) return xtermColor(named.value(value));
    bool valid = false;
    const int index = value.toInt(&valid);
    return valid && index >= 0 && index <= 255 ? xtermColor(index) : xtermColor(121);
}
