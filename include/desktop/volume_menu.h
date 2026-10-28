#pragma once

#include <functional>

class QMenu;

// Adds a "Volume NN%" slider to `menu`, calling `changed` as it moves.
void addVolumeSlider(QMenu& menu, int percent, std::function<void(int)> changed);
