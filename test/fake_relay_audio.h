#pragma once

#include <initializer_list>

namespace testAudio {
// Each startup replaces the current audio, just like the hardware backend.
void setStartResults(std::initializer_list<bool> results);
}
