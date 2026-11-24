#include "storage.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <set>
#include <stdexcept>
#include <sys/stat.h>

namespace wiiu {
namespace {
bool directory(const std::string& path) {
    if (::mkdir(path.c_str(), 0777) == 0) return true;
    struct stat info{};
    return errno == EEXIST && ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}
[[noreturn]] void fail(const std::string& operation, const std::string& name) {
    throw std::runtime_error("SD " + operation + " " + name + ": " + std::strerror(errno));
}
bool endsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
        text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}
}

bool createDirectories(const std::string& folder) {
    // The SD mount root itself cannot be created; start below it.
    const auto appRoot = folder.find("/wiiu/");
    const auto first = appRoot == std::string::npos ? std::size_t(1) : appRoot + 1;
    for (std::size_t end = folder.find('/', first); end != std::string::npos;
         end = folder.find('/', end + 1)) {
        if (!directory(folder.substr(0, end))) return false;
    }
    return directory(folder);
}

void SdStorage::prepare() {
    if (!ready_ && !createDirectories(directory_)) fail("create folder", directory_);
    ready_ = true;
}

bool SdStorage::read(const std::string& name, std::string& bytes, std::size_t limit) {
    // Only a missing file is "not found". Any other failure must not look like
    // a fresh install, or the community identity would be replaced.
    FILE* file = std::fopen(path(name).c_str(), "rb");
    if (!file && errno == ENOENT) {
        // An interrupted replacement leaves only the previous copy.
        file = std::fopen((path(name) + ".bak").c_str(), "rb");
        if (!file && errno == ENOENT) return false;
    }
    if (!file) fail("open", name);
    std::string result;
    char buffer[4096];
    std::size_t count;
    while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (result.size() + count > limit) {
            std::fclose(file);
            throw std::runtime_error("SD file " + name + " is too large.");
        }
        result.append(buffer, count);
    }
    const bool failed = std::ferror(file) != 0;
    if (std::fclose(file) != 0 || failed) fail("read", name);
    bytes = std::move(result);
    return true;
}

void SdStorage::write(const std::string& name, const std::string& bytes) {
    prepare();
    const auto target = path(name), temporary = target + ".tmp", backup = target + ".bak";
    FILE* file = std::fopen(temporary.c_str(), "wb");
    if (!file) fail("open", name);
    bool okay = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    if (okay) okay = std::fflush(file) == 0;
    if (std::fclose(file) != 0) okay = false;
    if (!okay) {
        const int error = errno;
        std::remove(temporary.c_str());
        errno = error;
        fail("write", name);
    }
    // Like settings.cfg, move the old copy aside rather than renaming over it.
    struct stat info{};
    const bool existing = ::stat(target.c_str(), &info) == 0;
    if (existing) {
        if (std::remove(backup.c_str()) != 0 && errno != ENOENT) fail("remove backup of", name);
        if (std::rename(target.c_str(), backup.c_str()) != 0) fail("back up", name);
    }
    if (std::rename(temporary.c_str(), target.c_str()) != 0) {
        const int error = errno;
        if (existing) std::rename(backup.c_str(), target.c_str());
        errno = error;
        fail("replace", name);
    }
    std::remove(backup.c_str());
}

void SdStorage::remove(const std::string& name) {
    std::remove((path(name) + ".tmp").c_str());
    if (std::remove((path(name) + ".bak").c_str()) != 0 && errno != ENOENT) fail("remove", name);
    if (std::remove(path(name).c_str()) != 0 && errno != ENOENT) fail("remove", name);
}

std::vector<std::string> SdStorage::list(const std::string& suffix) {
    prepare();
    DIR* folder = ::opendir(directory_.c_str());
    if (!folder) fail("list", directory_);
    std::set<std::string> names;
    while (const dirent* entry = ::readdir(folder)) {
        const std::string name = entry->d_name;
        if (endsWith(name, suffix)) names.insert(name);
        else if (endsWith(name, suffix + ".bak")) names.insert(name.substr(0, name.size() - 4));
    }
    ::closedir(folder);
    return {names.begin(), names.end()};
}

} // namespace wiiu
