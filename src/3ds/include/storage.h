#pragma once

#include "console/client.h"

#include <utility>

namespace handheld {

constexpr const char* kAppDirectory = "/3ds/disquisition";
constexpr const char* kCommunityDirectory = "/3ds/disquisition/community";

// Files in one SD card directory, through the native SD archive API rather than
// std::fstream/devoptab. Failures name the operation and its Result code.
// Each write keeps the file it replaces as <name>.bak, which reads fall back to
// if the console lost power while replacing it.
class SdStorage final : public console::Storage {
public:
    explicit SdStorage(std::string directory) : directory_(std::move(directory)) {}
    bool read(const std::string& name, std::string& bytes, std::size_t limit) override;
    void write(const std::string& name, const std::string& bytes) override;
    void remove(const std::string& name) override;
    std::vector<std::string> list(const std::string& suffix) override;

private:
    std::string directory_;
    bool created_ = false;
};

} // namespace handheld
