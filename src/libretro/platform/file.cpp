/*
    Copyright 2023 Jesse Talavera-Greenberg

    melonDS DS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS DS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS DS. If not, see http://www.gnu.org/licenses/.
*/

#include "file.hpp"
#define SKIP_STDIO_REDEFINES

#include <cerrno>
#include <memory>
#include <system_error>
#include <unordered_map>
#include <unistd.h>
#include <vector>

#include <file/file_path.h>
#include <Platform.h>
#include <streams/file_stream.h>
#include <streams/file_stream_transforms.h>
#include <retro_assert.h>
#include <compat/strl.h>
#include <vfs/vfs.h>
#include <string/stdstring.h>

#include "config.hpp"
#include "environment.hpp"
#include "tracy.hpp"
#include "utils.hpp"

using std::unique_ptr;
using std::unordered_map;

static unique_ptr<unordered_map<Platform::FileHandle*, int>> flushTimers;

namespace Platform {
    constexpr unsigned GetRetroVfsFileAccessFlags(FileMode mode) noexcept {
        unsigned retro_mode = 0;
        if (mode & FileMode::Read)
            retro_mode |= RETRO_VFS_FILE_ACCESS_READ;

        if (mode & FileMode::Write)
            retro_mode |= RETRO_VFS_FILE_ACCESS_WRITE;

        if (mode & FileMode::Preserve)
            retro_mode |= RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING;

        return retro_mode;
    }

    unsigned GetRetroVfsFileAccessHints(const std::string& path) noexcept {
        // TODO: If this path matches an SD card path, NAND path, or firmware path,
        // then we should return RETRO_VFS_FILE_ACCESS_HINT_FREQUENT_ACCESS

        if (string_ends_with(path.c_str(), ".bin")) {
            // TODO: When I submit that PR with those new Platform calls,
            // use them instead of relying on a .bin file extension
            return RETRO_VFS_FILE_ACCESS_HINT_FREQUENT_ACCESS;
        }

        return RETRO_VFS_FILE_ACCESS_HINT_NONE;
    }

    constexpr unsigned GetRetroVfsFileSeekOrigin(FileSeekOrigin origin) noexcept {
        switch (origin) {
            case FileSeekOrigin::Start:
                return RETRO_VFS_SEEK_POSITION_START;
            case FileSeekOrigin::Current:
                return RETRO_VFS_SEEK_POSITION_CURRENT;
            case FileSeekOrigin::End:
                return RETRO_VFS_SEEK_POSITION_END;
            default:
                return 0;
        }
    }
}


Platform::FileHandle *Platform::OpenFile(const std::string& path, FileMode mode) {
    if ((mode & FileMode::ReadWrite) == FileMode::None)
    { // If we aren't reading or writing, then we can't open the file
        Log(LogLevel::Error, "Attempted to open \"%s\" in neither read nor write mode (FileMode 0x%x)\n", path.c_str(), mode);
        return nullptr;
    }

    bool file_exists = path_is_valid(path.c_str());

    if (!file_exists && (mode & FileMode::NoCreate)) {
        // If the file doesn't exist, and we're not allowed to create it...
        retro::warn("Attempted to open \"%s\" in FileMode 0x%x, but the file doesn't exist and FileMode::NoCreate is set\n", path.c_str(), mode);
        return nullptr;
    }

    Platform::FileHandle *handle = new Platform::FileHandle;
    handle->hints = GetRetroVfsFileAccessHints(path);
    handle->file = filestream_open(path.c_str(), GetRetroVfsFileAccessFlags(mode), handle->hints);

    if (!handle->file) {
        retro::error("Attempted to open \"%s\" in FileMode 0x%x, but failed", path.c_str(), mode);
        delete handle;
        return nullptr;
    }

    retro::debug("Opened \"%s\" in FileMode 0x%x", path.c_str(), mode);

    return handle;
}

Platform::FileHandle *Platform::OpenLocalFile(const std::string& path, FileMode mode) {
    if (path_is_absolute(path.c_str())) {
        return OpenFile(path, mode);
    }

    std::string sysdir = retro::get_system_directory().value_or("");
    char fullpath[PATH_MAX];
    size_t pathLength = fill_pathname_join_special(fullpath, sysdir.c_str(), path.c_str(), sizeof(fullpath));
    pathname_make_slashes_portable(fullpath);

    if (pathLength >= sizeof(fullpath)) {
        Log(LogLevel::Warn, "Path \"%s\" is too long to be joined with system directory \"%s\"", path.c_str(), sysdir.c_str());
    }

    return OpenFile(fullpath, mode);
}

bool Platform::FileExists(const std::string& name)
{
    return path_is_valid(name.c_str());
}

bool Platform::LocalFileExists(const std::string& name)
{
    if (name.empty()) {
        return false;
    }

    if (path_is_absolute(name.c_str())) {
        return path_is_valid(name.c_str());
    }

    std::string sysdir = retro::get_system_directory().value_or("");
    char fullpath[PATH_MAX];
    size_t pathLength = fill_pathname_join_special(fullpath, sysdir.c_str(), name.c_str(), sizeof(fullpath));
    pathname_make_slashes_portable(fullpath);

    if (pathLength >= sizeof(fullpath)) {
        Log(LogLevel::Warn, "Path \"%s\" is too long to be joined with system directory \"%s\"", name.c_str(), sysdir.c_str());
    }

    return path_is_valid(fullpath);
}

/// Close a file opened with \c OpenFile.
/// @returns \c true if the file was closed successfully, false otherwise.
bool Platform::CloseFile(FileHandle* file)
{
    if (!file) {
        return false;
    }

    flushTimers->erase(file);

    char path[PATH_MAX];
    strlcpy(path, filestream_get_path(file->file), sizeof(path));
    retro::debug("Closing \"%s\"", path);
    bool ok = (filestream_close(file->file) == 0);

    if (!ok) {
        retro::error("Failed to close \"%s\"", path);
    }
    delete file;

    return ok;
}

/// Returns true if there is no more data left to read in this file.
bool Platform::IsEndOfFile(FileHandle* file)
{
    if (!file)
        return false;

    return filestream_eof(file->file) == EOF;
}

bool Platform::FileReadLine(char* str, int count, FileHandle* file)
{
    if (!file || !str)
        return false;

    return filestream_gets(file->file, str, count);
}

bool Platform::FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin)
{
    if (!file)
        return false;

    return filestream_seek(file->file, offset, GetRetroVfsFileSeekOrigin(origin)) == 0;
}

void Platform::FileRewind(FileHandle* file)
{
    if (file)
        filestream_rewind(file->file);
}

u64 Platform::FileRead(void* data, u64 size, u64 count, FileHandle* file)
{
    if (!file || !data)
        return 0;

    return filestream_read(file->file, data, size * count);
}

bool Platform::FileFlush(FileHandle* file)
{
    if (!file)
        return false;

    return filestream_flush(file->file) == 0;
}

u64 Platform::FileWrite(const void* data, u64 size, u64 count, FileHandle* file)
{
    if (!file || !data)
        return 0;

    u64 result = filestream_write(file->file, data, size * count);
    if (file->hints & RETRO_VFS_FILE_ACCESS_HINT_FREQUENT_ACCESS) {
        (*flushTimers)[file] = melonds::config::save::FlushDelay();
    }

    return result;
}

u64 Platform::FileWriteFormatted(FileHandle* file, const char* fmt, ...)
{
    if (!file || !fmt)
        return 0;

    va_list args;
    va_start(args, fmt);
    u64 ret = filestream_vprintf(file->file, fmt, args);
    va_end(args);
    return ret;
}

u64 Platform::FileLength(FileHandle* file)
{
    if (!file)
        return 0;

    return filestream_get_size(file->file);
}

void melonds::file::init() {
    retro_assert(flushTimers == nullptr);
    flushTimers = std::make_unique<unordered_map<Platform::FileHandle*, int>>();
}

void melonds::file::deinit() {
    retro_assert(flushTimers != nullptr);
    for (auto& [file, timeUntilFlush] : *flushTimers) {
        Platform::CloseFile(file);
    }
    flushTimers.reset();
}

retro::task::TaskSpec melonds::file::FlushTask() noexcept {
    retro::task::TaskSpec task([](retro::task::TaskHandle &task) {
        ZoneScopedN("melonds::fat::FlushTask");
        using namespace melonds::file;
        if (task.IsCancelled()) {
            // If it's time to stop...
            task.Finish();
            return;
        }

        std::vector<Platform::FileHandle*> filesToRemove;

        for (auto& [file, timeUntilFlush] : *flushTimers) {
            timeUntilFlush--;
            if (timeUntilFlush == 0) {
                // If the timer has reached 0, flush the file and remove it from the map

                retro_assert(file != nullptr);
                retro_assert(file->file != nullptr);

                libretro_vfs_implementation_file* handle = filestream_get_vfs_handle(file->file);
                const char* original_path = filestream_get_path(file->file);
                retro_assert(handle != nullptr);
                retro_assert(handle->fd >= 0);
                retro_assert(original_path != nullptr);
                // If the above conditions were false, the file should never have left Platform::OpenFile

#ifdef _WIN32
                int syncResult = _commit(handle->fd);
#else
                int syncResult = fsync(handle->fd);
#endif

                if (syncResult == 0) {
                    retro::debug("Flushed file \"%s\" to host disk", original_path);
                } else {
                    int error = errno;
                    if (error == EBADF) {
                        retro::info("File \"%s\" was closed behind our backs, no need to flush it to disk.", original_path);
                    }
                    else {
                        retro::error("Failed to flush \"%s\" to host disk: %s (%x)", original_path, strerror(error), error);
                    }
                }

                // If the file is not open, then it was closed before the flush timer reached 0.
                // In that case it will have been flushed to disk anyway, so we don't need to do anything.

                filesToRemove.push_back(file);
            }
        }

        for (Platform::FileHandle* file : filesToRemove) {
            int removed = flushTimers->erase(file);
            retro_assert(removed == 1);
        }
    });

    return task;
}