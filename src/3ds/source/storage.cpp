#include "storage.h"

#include <3ds.h>
#include <algorithm>
#include <cstdio>
#include <set>
#include <stdexcept>

namespace handheld {
namespace {
constexpr u32 kChunk = 256 * 1024;

FS_Path path(const std::string& value) { return fsMakePath(PATH_ASCII, value.c_str()); }
std::string failure(const std::string& operation, Result result) {
    char code[16];
    std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(static_cast<u32>(result)));
    return "SD " + operation + ": " + code;
}
[[noreturn]] void fail(const std::string& operation, Result result) {
    throw std::runtime_error(failure(operation, result));
}
bool missing(Result result) {
    return R_FAILED(result) && (R_SUMMARY(result) == RS_NOTFOUND || R_DESCRIPTION(result) == RD_NOT_FOUND);
}
bool endsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
        text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}
const Result kShort = static_cast<Result>(MAKERESULT(RL_PERMANENT, RS_INTERNAL, RM_APPLICATION, RD_INVALID_SIZE));

struct Archive {
    FS_Archive handle = 0;
    Archive() {
        const Result result = FSUSER_OpenArchive(&handle, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
        if (R_FAILED(result)) fail("open card", result);
    }
    ~Archive() { FSUSER_CloseArchive(handle); }
    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;
};

void directories(FS_Archive archive, const std::string& directory) {
    for (std::size_t end = directory.find('/', 1);; end = directory.find('/', end + 1)) {
        const auto name = directory.substr(0, end);
        if (R_FAILED(FSUSER_CreateDirectory(archive, path(name), 0))) {
            // Different firmware returns different "already exists" descriptions.
            // Opening it proves that it exists and is a directory, rather than a file.
            Handle handle = 0;
            const auto check = FSUSER_OpenDirectory(&handle, archive, path(name));
            if (R_FAILED(check)) fail(name, check);
            FSDIR_Close(handle);
        }
        if (end == std::string::npos) return;
    }
}

bool exists(FS_Archive archive, const std::string& file) {
    Handle handle = 0;
    if (R_FAILED(FSUSER_OpenFile(&handle, archive, path(file), FS_OPEN_READ, 0))) return false;
    FSFILE_Close(handle);
    return true;
}

Result load(FS_Archive archive, const std::string& file, std::string& bytes, std::size_t limit) {
    Handle handle = 0;
    Result result = FSUSER_OpenFile(&handle, archive, path(file), FS_OPEN_READ, 0);
    if (R_FAILED(result)) return result;
    u64 size = 0;
    result = FSFILE_GetSize(handle, &size);
    if (R_SUCCEEDED(result) && size > limit) {
        FSFILE_Close(handle);
        throw std::runtime_error(file + " is larger than " + std::to_string(limit) + " bytes");
    }
    std::string text;
    if (R_SUCCEEDED(result)) text.resize(static_cast<std::size_t>(size));
    for (u64 offset = 0; R_SUCCEEDED(result) && offset < size;) {
        const u32 chunk = static_cast<u32>(std::min<u64>(kChunk, size - offset));
        u32 count = 0;
        result = FSFILE_Read(handle, &count, offset, &text[static_cast<std::size_t>(offset)], chunk);
        if (R_SUCCEEDED(result) && count != chunk) result = kShort;
        offset += chunk;
    }
    const auto closed = FSFILE_Close(handle);
    if (R_SUCCEEDED(result)) result = closed;
    if (R_SUCCEEDED(result)) bytes.swap(text);
    return result;
}

} // namespace

bool SdStorage::read(const std::string& name, std::string& bytes, std::size_t limit) {
    Archive archive;
    const auto file = directory_ + "/" + name;
    Result result = load(archive.handle, file, bytes, limit);
    if (R_SUCCEEDED(result)) return true;
    if (!missing(result)) fail("read " + name, result);
    // Recover the previous file if the console lost power during replacement.
    result = load(archive.handle, file + ".bak", bytes, limit);
    if (R_SUCCEEDED(result)) return true;
    if (!missing(result)) fail("read " + name + ".bak", result);
    return false;
}

void SdStorage::write(const std::string& name, const std::string& bytes) {
    Archive archive;
    if (!created_) {
        directories(archive.handle, directory_);
        created_ = true;
    }
    const auto file = directory_ + "/" + name, temporary = file + ".tmp", backup = file + ".bak";
    Handle handle = 0;
    auto result = FSUSER_OpenFile(&handle, archive.handle, path(temporary), FS_OPEN_CREATE | FS_OPEN_WRITE, 0);
    if (R_FAILED(result)) fail("open " + name + ".tmp", result);
    result = FSFILE_SetSize(handle, bytes.size());
    for (std::size_t offset = 0; R_SUCCEEDED(result) && offset < bytes.size();) {
        const u32 chunk = static_cast<u32>(std::min<std::size_t>(kChunk, bytes.size() - offset));
        const bool last = offset + chunk == bytes.size();
        u32 written = 0;
        result = FSFILE_Write(handle, &written, offset, bytes.data() + offset, chunk,
                              last ? FS_WRITE_FLUSH : 0);
        if (R_SUCCEEDED(result) && written != chunk) result = kShort;
        offset += chunk;
    }
    const auto closed = FSFILE_Close(handle);
    if (R_FAILED(result)) fail("write " + name, result);
    if (R_FAILED(closed)) fail("close " + name, closed);

    result = FSUSER_RenameFile(archive.handle, path(temporary), archive.handle, path(file));
    if (R_SUCCEEDED(result)) return;

    // FSUSER_RenameFile cannot overwrite an existing destination. Preserve the
    // old file in a backup and roll back if publishing the new file fails.
    if (!exists(archive.handle, file)) fail("publish " + name, result);
    const auto deleted = FSUSER_DeleteFile(archive.handle, path(backup));
    if (R_FAILED(deleted) && !missing(deleted)) fail("remove old " + name + ".bak", deleted);
    result = FSUSER_RenameFile(archive.handle, path(file), archive.handle, path(backup));
    if (R_FAILED(result)) fail("back up " + name, result);
    result = FSUSER_RenameFile(archive.handle, path(temporary), archive.handle, path(file));
    if (R_FAILED(result)) {
        auto error = failure("publish " + name, result);
        const auto restored = FSUSER_RenameFile(archive.handle, path(backup), archive.handle, path(file));
        if (R_FAILED(restored)) error += "; " + failure("restore " + name + ".bak", restored);
        throw std::runtime_error(error);
    }
    // Keep the previous valid file as the backup for recovery.
}

void SdStorage::remove(const std::string& name) {
    Archive archive;
    // The backup goes first so that a failure never leaves only a backup to recover.
    for (const auto& file : {name + ".bak", name, name + ".tmp"}) {
        const auto result = FSUSER_DeleteFile(archive.handle, path(directory_ + "/" + file));
        if (R_FAILED(result) && !missing(result)) fail("remove " + file, result);
    }
}

std::vector<std::string> SdStorage::list(const std::string& suffix) {
    Archive archive;
    Handle handle = 0;
    const auto opened = FSUSER_OpenDirectory(&handle, archive.handle, path(directory_));
    if (missing(opened)) return {};
    if (R_FAILED(opened)) fail("open " + directory_, opened);
    std::set<std::string> names;
    std::vector<FS_DirectoryEntry> entries(16);
    for (;;) {
        u32 count = 0;
        const auto result = FSDIR_Read(handle, &count, entries.size(), entries.data());
        if (R_FAILED(result)) {
            FSDIR_Close(handle);
            fail("list " + directory_, result);
        }
        if (count == 0) break;
        for (u32 i = 0; i < count; ++i) {
            if (entries[i].attributes & FS_ATTRIBUTE_DIRECTORY) continue;
            std::string name;
            bool ascii = true;
            for (const u16 code : entries[i].name) {
                if (code == 0) break;
                ascii = ascii && code < 0x80;
                name += static_cast<char>(code);
            }
            if (!ascii) continue;
            // A file whose replacement was interrupted survives as its backup.
            if (endsWith(name, ".bak")) name.resize(name.size() - 4);
            if (endsWith(name, suffix)) names.insert(name);
        }
    }
    FSDIR_Close(handle);
    return {names.begin(), names.end()};
}

} // namespace handheld
