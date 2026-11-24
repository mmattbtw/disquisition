#pragma once

#include "console/client.h"

#include <string>
#include <utility>
#include <vector>

namespace wiiu {

// Creates each folder of path below its /wiiu/ component on the mounted SD card.
bool createDirectories(const std::string& path);

// Community identity and metadata files in one SD card folder.
class SdStorage final : public console::Storage {
public:
    explicit SdStorage(std::string directory) : directory_(std::move(directory)) {}
    bool read(const std::string& name, std::string& bytes, std::size_t limit) override;
    void write(const std::string& name, const std::string& bytes) override;
    void remove(const std::string& name) override;
    std::vector<std::string> list(const std::string& suffix) override;

private:
    std::string directory_;
    bool ready_ = false;
    std::string path(const std::string& name) const { return directory_ + "/" + name; }
    void prepare();
};

} // namespace wiiu
