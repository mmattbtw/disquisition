#include "settings_store.h"

#include <3ds.h>
#include <cstdio>

namespace handheld {
namespace {
constexpr const char* kFile = "/3ds/disquisition/settings.cfg";
constexpr const char* kTemporary = "/3ds/disquisition/settings.cfg.tmp";
constexpr const char* kBackup = "/3ds/disquisition/settings.cfg.bak";

FS_Path path(const char* value) { return fsMakePath(PATH_ASCII, value); }
std::string failure(const char* operation, Result result) {
    char code[16];
    std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(static_cast<u32>(result)));
    return std::string("SD ") + operation + ": " + code;
}
bool missing(Result result) {
    return R_FAILED(result) && (R_SUMMARY(result) == RS_NOTFOUND || R_DESCRIPTION(result) == RD_NOT_FOUND);
}

struct Archive {
    FS_Archive handle = 0;
    bool opened = false;
    ~Archive() { if (opened) FSUSER_CloseArchive(handle); }
    bool open(std::string& error) {
        const Result result = FSUSER_OpenArchive(&handle, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
        opened = R_SUCCEEDED(result);
        if (!opened) error = failure("open card", result);
        return opened;
    }
};

bool directory(FS_Archive archive, const char* name, std::string& error) {
    const auto result = FSUSER_CreateDirectory(archive, path(name), 0);
    if (R_SUCCEEDED(result)) return true;
    // Different firmware returns different "already exists" descriptions.
    // Opening it proves that it exists and is a directory, rather than a file.
    Handle handle = 0;
    const auto check = FSUSER_OpenDirectory(&handle, archive, path(name));
    if (R_SUCCEEDED(check)) { FSDIR_Close(handle); return true; }
    error = failure(name, check);
    return false;
}

bool read(FS_Archive archive, const char* name, std::string& text, Result& error) {
    Handle handle = 0;
    error = FSUSER_OpenFile(&handle, archive, path(name), FS_OPEN_READ, 0);
    if (R_FAILED(error)) return false;
    u64 size = 0;
    error = FSFILE_GetSize(handle, &size);
    if (R_SUCCEEDED(error) && size <= 4096) {
        text.resize(static_cast<std::size_t>(size));
        u32 count = 0;
        if (size != 0) error = FSFILE_Read(handle, &count, 0, text.data(), static_cast<u32>(size));
        if (R_SUCCEEDED(error) && count != size)
            error = static_cast<Result>(MAKERESULT(RL_PERMANENT, RS_INTERNAL, RM_APPLICATION, RD_INVALID_SIZE));
    } else if (R_SUCCEEDED(error)) {
        error = static_cast<Result>(MAKERESULT(RL_PERMANENT, RS_INVALIDARG, RM_APPLICATION, RD_INVALID_SIZE));
    }
    const auto closed = FSFILE_Close(handle);
    if (R_SUCCEEDED(error)) error = closed;
    return R_SUCCEEDED(error);
}

} // namespace

Settings loadSettings(std::string& error) {
    error.clear();
    Archive archive;
    if (!archive.open(error)) return {};
    std::string text;
    Result result = 0;
    if (read(archive.handle, kFile, text, result)) return parseSettings(text);
    if (!missing(result)) { error = failure("read settings", result); return {}; }
    // Recover the previous file if the console lost power during replacement.
    if (read(archive.handle, kBackup, text, result)) return parseSettings(text);
    if (!missing(result)) error = failure("read backup", result);
    return {};
}

bool saveSettings(const Settings& settings, std::string& error) {
    error.clear();
    Archive archive;
    if (!archive.open(error) || !directory(archive.handle, "/3ds", error) ||
        !directory(archive.handle, "/3ds/disquisition", error)) return false;

    const auto text = encodeSettings(settings);
    Handle handle = 0;
    auto result = FSUSER_OpenFile(&handle, archive.handle, path(kTemporary), FS_OPEN_CREATE | FS_OPEN_WRITE, 0);
    if (R_FAILED(result)) { error = failure("open temporary file", result); return false; }
    result = FSFILE_SetSize(handle, text.size());
    u32 written = 0;
    if (R_SUCCEEDED(result)) result = FSFILE_Write(handle, &written, 0, text.data(), text.size(), FS_WRITE_FLUSH);
    const auto closed = FSFILE_Close(handle);
    if (R_FAILED(result)) { error = failure("write settings", result); return false; }
    if (written != text.size()) { error = "SD write settings: incomplete write"; return false; }
    if (R_FAILED(closed)) { error = failure("close settings", closed); return false; }

    result = FSUSER_RenameFile(archive.handle, path(kTemporary), archive.handle, path(kFile));
    if (R_SUCCEEDED(result)) return true;

    // FSUSER_RenameFile cannot overwrite an existing destination. Preserve the
    // old settings in a backup and roll back if publishing the new file fails.
    std::string previous;
    Result readError = 0;
    if (!read(archive.handle, kFile, previous, readError)) {
        error = failure("publish settings", result);
        return false;
    }
    const auto deleted = FSUSER_DeleteFile(archive.handle, path(kBackup));
    if (R_FAILED(deleted) && !missing(deleted)) { error = failure("remove old backup", deleted); return false; }
    result = FSUSER_RenameFile(archive.handle, path(kFile), archive.handle, path(kBackup));
    if (R_FAILED(result)) { error = failure("backup settings", result); return false; }
    result = FSUSER_RenameFile(archive.handle, path(kTemporary), archive.handle, path(kFile));
    if (R_FAILED(result)) {
        error = failure("publish settings", result);
        const auto restored = FSUSER_RenameFile(archive.handle, path(kBackup), archive.handle, path(kFile));
        if (R_FAILED(restored)) error += "; " + failure("restore backup", restored);
        return false;
    }
    // Keep the previous valid backup for recovery; do not delete user settings
    // if the final publish or any preceding filesystem operation fails.
    return true;
}

} // namespace handheld
