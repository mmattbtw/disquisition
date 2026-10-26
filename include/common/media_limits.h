#pragma once

#include <cstddef>
#include <cstdint>

namespace chat {

// Shared image transfer limits, independent of message framing.
constexpr std::size_t kImageChunkSize = 12 * 1024;
constexpr std::size_t kMaxImageBytes = 4 * 1024 * 1024;
constexpr std::uint32_t kMaxImageChunks = static_cast<std::uint32_t>(
    (kMaxImageBytes + kImageChunkSize - 1) / kImageChunkSize);

} // namespace chat
