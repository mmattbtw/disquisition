#include "check.h"
#include "settings_store.h"
#include <3ds.h>

#include <cstring>
#include <map>
#include <set>

namespace {
constexpr const char* primary = "/3ds/disquisition/settings.cfg";
constexpr const char* backup = "/3ds/disquisition/settings.cfg.bak";
const Result notFound = static_cast<Result>(MAKERESULT(27, RS_NOTFOUND, 17, RD_NOT_FOUND));
const Result exists = static_cast<Result>(0xC82044BEu);
const Result denied = static_cast<Result>(0xC8804471u);
std::map<std::string, std::string> files;
std::set<std::string> directories{"/"};
std::map<Handle, std::string> handles;
Handle nextHandle = 1;
int openArchives = 0;
bool cardUnavailable = false, shortWrite = false, writeDenied = false, publishDenied = false;
std::string name(FS_Path path) { return static_cast<const char*>(path.data); }
}

FS_Path fsMakePath(int type, const void* value) { return {type, 0, value}; }
Result FSUSER_OpenArchive(FS_Archive* archive, int, FS_Path) {
    if (cardUnavailable) return denied;
    *archive = 1; ++openArchives; return 0;
}
Result FSUSER_CloseArchive(FS_Archive) { --openArchives; return 0; }
Result FSUSER_CreateDirectory(FS_Archive, FS_Path path, u32) {
    if (files.count(name(path)) || directories.count(name(path))) return exists;
    directories.insert(name(path)); return 0;
}
Result FSUSER_OpenDirectory(Handle* handle, FS_Archive, FS_Path path) {
    if (!directories.count(name(path))) return notFound;
    *handle = nextHandle++; handles[*handle] = name(path); return 0;
}
Result FSDIR_Close(Handle handle) { CHECK(handles.erase(handle) == 1); return 0; }
Result FSUSER_OpenFile(Handle* handle, FS_Archive, FS_Path path, u32 flags, u32) {
    if (!files.count(name(path))) {
        if (!(flags & FS_OPEN_CREATE)) return notFound;
        files[name(path)] = "";
    }
    *handle = nextHandle++; handles[*handle] = name(path); return 0;
}
Result FSFILE_GetSize(Handle handle, u64* size) { *size = files.at(handles.at(handle)).size(); return 0; }
Result FSFILE_SetSize(Handle handle, u64 size) { files.at(handles.at(handle)).resize(size); return 0; }
Result FSFILE_Read(Handle handle, u32* count, u64, void* data, u32 size) {
    const auto& text = files.at(handles.at(handle));
    CHECK(size == text.size()); std::memcpy(data, text.data(), size); *count = size; return 0;
}
Result FSFILE_Write(Handle handle, u32* count, u64, const void* data, u32 size, u32 flags) {
    CHECK(flags & FS_WRITE_FLUSH);
    if (writeDenied) return denied;
    *count = shortWrite ? size - 1 : size;
    files.at(handles.at(handle)).assign(static_cast<const char*>(data), *count); return 0;
}
Result FSFILE_Close(Handle handle) { CHECK(handles.erase(handle) == 1); return 0; }
Result FSUSER_RenameFile(FS_Archive, FS_Path from, FS_Archive, FS_Path to) {
    if (files.count(name(to))) return exists;
    if (!files.count(name(from))) return notFound;
    if (publishDenied && name(to) == primary && name(from).find(".tmp") != std::string::npos) {
        publishDenied = false; return denied;
    }
    files[name(to)] = files.at(name(from)); files.erase(name(from)); return 0;
}
Result FSUSER_DeleteFile(FS_Archive, FS_Path path) { return files.erase(name(path)) ? 0 : notFound; }

int main() {
    std::string error;
    CHECK(handheld::loadSettings(error).port == 3333 && error.empty());
    handheld::Settings settings;
    settings.host = "relay.mmatt.net"; settings.name = "device";
    CHECK(handheld::saveSettings(settings, error) && error.empty());
    CHECK(handheld::loadSettings(error).host == settings.host);
    const auto original = files.at(primary);
    settings.name = "changed";
    CHECK(handheld::saveSettings(settings, error));
    CHECK(files.at(backup) == original);
    CHECK(handheld::loadSettings(error).name == "changed");
    const auto saved = files.at(primary);
    settings.name = "unsaved";
    writeDenied = true;
    CHECK(!handheld::saveSettings(settings, error));
    CHECK(error == "SD write settings: 0xC8804471" && files.at(primary) == saved);
    writeDenied = false; shortWrite = true;
    CHECK(!handheld::saveSettings(settings, error));
    CHECK(error == "SD write settings: incomplete write" && files.at(primary) == saved);
    shortWrite = false; publishDenied = true;
    CHECK(!handheld::saveSettings(settings, error));
    CHECK(error == "SD publish settings: 0xC8804471" && files.at(primary) == saved);
    // Simulate power loss between backing up the old file and publishing new.
    files[backup] = saved; files.erase(primary);
    CHECK(handheld::loadSettings(error).name == "changed" && error.empty());
    cardUnavailable = true;
    CHECK(!handheld::saveSettings(settings, error));
    CHECK(error == "SD open card: 0xC8804471");
    cardUnavailable = false;
    files["/3ds/disquisition"] = "a file cannot be used as a directory";
    directories.erase("/3ds/disquisition");
    CHECK(!handheld::saveSettings(settings, error));
    CHECK(error.find("SD /3ds/disquisition:") == 0);
    CHECK(handles.empty() && openArchives == 0);
    std::puts("handheld_settings_store_test: ok");
}
