#pragma once

#include "settings.h"

namespace handheld {

// Native SD archive operations, independent of std::fstream/devoptab mounting.
// Failures identify both the filesystem operation and its actual Result code.
Settings loadSettings(std::string& error);
bool saveSettings(const Settings& settings, std::string& error);

} // namespace handheld
