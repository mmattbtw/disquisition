#include "microphone_frames.h"

namespace wiiu {

bool MicrophoneFrames::push(std::int16_t sample, handheld::Samples& frame) {
    if (!paired_) { previous_ = sample; paired_ = true; return false; }
    paired_ = false;
    // Average before decimating, using a wider type to avoid signed overflow.
    pending_[count_++] = static_cast<std::int16_t>((int(previous_) + int(sample)) / 2);
    if (count_ != handheld::kSamples) return false;
    frame = pending_;
    count_ = 0;
    return true;
}

} // namespace wiiu
