#pragma once
#include "session.h"

namespace wiiu {

// Streaming 32 kHz GamePad PCM -> 16 kHz relay PCM. Pairs survive poll chunks.
class MicrophoneFrames {
public:
    void reset() { paired_ = false; count_ = 0; }
    bool push(std::int16_t sample, handheld::Samples& frame);
private:
    bool paired_ = false;
    std::int16_t previous_ = 0;
    std::size_t count_ = 0;
    handheld::Samples pending_{};
};

} // namespace wiiu
