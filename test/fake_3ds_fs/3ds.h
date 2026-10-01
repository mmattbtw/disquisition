#pragma once

// Only the FS calls used by settings_store.cpp. The real devkitPro build
// checks these declarations against libctru; host tests simulate SD failures.
#include <cstdint>
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using Result = std::int32_t;
using Handle = u32;
using FS_Archive = u64;
struct FS_Path { int type; u32 size; const void* data; };
constexpr int PATH_ASCII = 3, PATH_EMPTY = 1, ARCHIVE_SDMC = 9;
constexpr int FS_OPEN_READ = 1, FS_OPEN_WRITE = 2, FS_OPEN_CREATE = 4, FS_WRITE_FLUSH = 1;
constexpr int RL_PERMANENT = 27, RS_INTERNAL = 11, RS_INVALIDARG = 7, RS_NOTFOUND = 4;
constexpr int RM_APPLICATION = 254, RD_INVALID_SIZE = 1004, RD_NOT_FOUND = 1018;
#define R_SUCCEEDED(result) ((result) >= 0)
#define R_FAILED(result) ((result) < 0)
#define R_SUMMARY(result) (((u32)(result) >> 21) & 0x3F)
#define R_DESCRIPTION(result) ((u32)(result) & 0x3FF)
#define MAKERESULT(level, summary, module, description) \
    ((u32(level) << 27) | (u32(summary) << 21) | (u32(module) << 10) | u32(description))
FS_Path fsMakePath(int type, const void* path);
Result FSUSER_OpenArchive(FS_Archive*, int, FS_Path);
Result FSUSER_CloseArchive(FS_Archive);
Result FSUSER_CreateDirectory(FS_Archive, FS_Path, u32);
Result FSUSER_OpenDirectory(Handle*, FS_Archive, FS_Path);
Result FSDIR_Close(Handle);
Result FSUSER_OpenFile(Handle*, FS_Archive, FS_Path, u32, u32);
Result FSFILE_GetSize(Handle, u64*);
Result FSFILE_SetSize(Handle, u64);
Result FSFILE_Read(Handle, u32*, u64, void*, u32);
Result FSFILE_Write(Handle, u32*, u64, const void*, u32, u32);
Result FSFILE_Close(Handle);
Result FSUSER_RenameFile(FS_Archive, FS_Path, FS_Archive, FS_Path);
Result FSUSER_DeleteFile(FS_Archive, FS_Path);
