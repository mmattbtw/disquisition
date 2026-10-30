#pragma once

class QWidget;

// Hides the title text macOS draws in a window's title bar, for windows that
// draw their own title under a transparent bar. The title still names the
// window in the Dock, the Window menu and Mission Control.
void hideNativeWindowTitle(QWidget* window);
