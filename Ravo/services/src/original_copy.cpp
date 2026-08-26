#include "catalog_internal.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/stdio.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

namespace ravo
{

namespace
{

constexpr std::size_t kOriginalCopyChunkBytes = 64U * 1024U;
constexpr int kOriginalCopyTemporaryAttempts = 32;

class FileDescriptor
{
public:
    FileDescriptor() = default;
    explicit FileDescriptor(const int descriptor) noexcept
        : descriptor_(descriptor)
    {
    }

    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;
    FileDescriptor(FileDescriptor &&other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1))
    {
    }
    FileDescriptor &operator=(FileDescriptor &&other) noexcept
    {
        if (this != &other)
        {
            close_ignoring_error();
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }

    ~FileDescriptor()
    {
        close_ignoring_error();
    }

    [[nodiscard]] int get() const noexcept
    {
        return descriptor_;
    }

    [[nodiscard]] std::error_code finish() noexcept
    {
        if (descriptor_ < 0)
        {
            return std::make_error_code(std::errc::bad_file_descriptor);
        }
#ifdef _WIN32
        if (::_commit(descriptor_) != 0)
#else
        int sync_result = -1;
        do
        {
            sync_result = ::fsync(descriptor_);
        } while (sync_result != 0 && errno == EINTR);
        if (sync_result != 0)
#endif
        {
            const std::error_code error(errno, std::generic_category());
            close_ignoring_error();
            return error;
        }
        return close();
    }

    [[nodiscard]] std::error_code close() noexcept
    {
        if (descriptor_ < 0)
        {
            return {};
        }
        const int descriptor = std::exchange(descriptor_, -1);
#ifdef _WIN32
        if (::_close(descriptor) != 0)
#else
        if (::close(descriptor) != 0)
#endif
        {
            return std::error_code(errno, std::generic_category());
        }
        return {};
    }

private:
    void close_ignoring_error() noexcept
    {
        static_cast<void>(close());
    }

    int descriptor_ = -1;
};

class OwnedTemporaryPath
{
public:
    OwnedTemporaryPath() = default;
    OwnedTemporaryPath(const OwnedTemporaryPath &) = delete;
    OwnedTemporaryPath &operator=(const OwnedTemporaryPath &) = delete;

    ~OwnedTemporaryPath()
    {
        remove_ignoring_error();
    }

    void reset(std::filesystem::path path)
    {
        remove_ignoring_error();
        path_ = std::move(path);
    }

    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return path_;
    }

    [[nodiscard]] std::error_code remove() noexcept
    {
        if (path_.empty())
        {
            return {};
        }
        std::error_code error;
        std::filesystem::remove(path_, error);
        if (!error)
        {
            path_.clear();
        }
        return error;
    }

    void release() noexcept
    {
        path_.clear();
    }

private:
    void remove_ignoring_error() noexcept
    {
        static_cast<void>(remove());
    }

    std::filesystem::path path_;
};

[[nodiscard]] int open_source_descriptor(const std::filesystem::path &path) noexcept
{
#ifdef _WIN32
    int descriptor = -1;
    const errno_t result =
        ::_wsopen_s(&descriptor, path.c_str(), _O_BINARY | _O_RDONLY | _O_NOINHERIT, _SH_DENYNO, 0);
    if (result != 0)
    {
        errno = result;
        return -1;
    }
    return descriptor;
#else
    return ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
#endif
}

[[nodiscard]] int open_temporary_descriptor(const std::filesystem::path &path) noexcept
{
#ifdef _WIN32
    int descriptor = -1;
    const errno_t result = ::_wsopen_s(&descriptor, path.c_str(),
                                       _O_BINARY | _O_WRONLY | _O_CREAT | _O_EXCL | _O_NOINHERIT,
                                       _SH_DENYRW, _S_IREAD | _S_IWRITE);
    if (result != 0)
    {
        errno = result;
        return -1;
    }
    return descriptor;
#else
    return ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
#endif
}

[[nodiscard]] std::int64_t read_descriptor(const int descriptor, void *const bytes,
                                           const std::size_t size) noexcept
{
#ifdef _WIN32
    return ::_read(descriptor, bytes, static_cast<unsigned int>(size));
#else
    std::int64_t result = -1;
    do
    {
        result = ::read(descriptor, bytes, size);
    } while (result < 0 && errno == EINTR);
    return result;
#endif
}

[[nodiscard]] std::int64_t write_descriptor(const int descriptor, const void *const bytes,
                                            const std::size_t size) noexcept
{
#ifdef _WIN32
    return ::_write(descriptor, bytes, static_cast<unsigned int>(size));
#else
    std::int64_t result = -1;
    do
    {
        result = ::write(descriptor, bytes, size);
    } while (result < 0 && errno == EINTR);
    return result;
#endif
}

[[nodiscard]] std::string path_utf8(const std::filesystem::path &path)
{
    const auto encoded = path.generic_u8string();
    return {reinterpret_cast<const char *>(encoded.data()), encoded.size()};
}

[[nodiscard]] std::string checkpoint_path_utf8(const std::filesystem::path &path)
{
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    return path_utf8(error ? path : canonical);
}

[[nodiscard]] TaskError original_copy_error(const ErrorCode code, std::string message,
                                            const std::string_view reason,
                                            const std::string_view source,
                                            const std::string_view output,
                                            const std::error_code &error = {})
{
    std::map<std::string, std::string, std::less<>> context{
        {"output", std::string(output)},
        {"reason", std::string(reason)},
        {"source", std::string(source)},
    };
    if (error)
    {
        context.emplace("detail", error.message());
        if (is_disk_full(error))
        {
            context.emplace("disk_full", "true");
        }
    }
    return make_error(code, std::move(message), std::move(context));
}

[[nodiscard]] Result<void> check_original_copy_cancellation(const CancellationToken &cancellation,
                                                            const std::string_view source,
                                                            const std::string_view output)
{
    auto active = cancellation.check();
    if (active)
    {
        return {};
    }
    auto error = std::move(active).error();
    error.context.insert_or_assign("source", std::string(source));
    error.context.insert_or_assign("output", std::string(output));
    return error;
}

[[nodiscard]] std::error_code
invoke_original_copy_hook(const OriginalCopyCheckpointHook &hook,
                          const OriginalCopyCheckpoint checkpoint, const std::string_view path,
                          const std::uint64_t bytes_processed) noexcept
{
    return hook.callback == nullptr ?
               std::error_code{} :
               hook.callback(hook.context, checkpoint, path, bytes_processed);
}

#ifndef _WIN32
[[nodiscard]] bool has_write_permission(const std::filesystem::perms permissions) noexcept
{
    constexpr auto write_permissions = std::filesystem::perms::owner_write |
                                       std::filesystem::perms::group_write |
                                       std::filesystem::perms::others_write;
    return (permissions & write_permissions) != std::filesystem::perms::none;
}
#endif

[[nodiscard]] bool paths_are_same(const std::filesystem::path &source,
                                  const std::filesystem::path &output) noexcept
{
    if (source.lexically_normal() == output.lexically_normal())
    {
        return true;
    }
    std::error_code error;
    return std::filesystem::exists(output, error) && !error &&
           std::filesystem::equivalent(source, output, error) && !error;
}

[[nodiscard]] std::filesystem::path temporary_candidate(const std::filesystem::path &output)
{
    const auto parent =
        output.parent_path().empty() ? std::filesystem::path(".") : output.parent_path();
    auto filename = output.filename();
    filename += utf8_path(".ravo-original-copy-" + generate_catalog_id() + ".tmp");
    return parent / filename;
}

[[nodiscard]] std::error_code publish_no_replace(const std::filesystem::path &temporary,
                                                 const std::filesystem::path &output) noexcept
{
#ifdef _WIN32
    if (::MoveFileExW(temporary.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH) != 0)
    {
        return {};
    }
    return {static_cast<int>(::GetLastError()), std::system_category()};
#elif defined(__APPLE__)
    if (::renamex_np(temporary.c_str(), output.c_str(), RENAME_EXCL) == 0)
    {
        return {};
    }
    return {errno, std::generic_category()};
#elif defined(__linux__) && defined(SYS_renameat2)
    constexpr unsigned int kRenameNoReplace = 1U;
    if (::syscall(SYS_renameat2, AT_FDCWD, temporary.c_str(), AT_FDCWD, output.c_str(),
                  kRenameNoReplace) == 0)
    {
        return {};
    }
    return {errno, std::generic_category()};
#else
    static_cast<void>(temporary);
    static_cast<void>(output);
    return std::make_error_code(std::errc::operation_not_supported);
#endif
}

[[nodiscard]] TaskError hook_stage_error(const OriginalCopyCheckpoint checkpoint,
                                         const std::string_view source,
                                         const std::string_view output,
                                         const std::error_code &error)
{
    switch (checkpoint)
    {
    case OriginalCopyCheckpoint::kSourceOpened:
        return original_copy_error(ErrorCode::kIo, "Unable to open original file",
                                   "original_copy_source_open_failed", source, output, error);
    case OriginalCopyCheckpoint::kBeforeSourceRead:
    case OriginalCopyCheckpoint::kSourceChunkRead:
        return original_copy_error(ErrorCode::kIo, "Unable to read original file",
                                   "original_copy_source_read_failed", source, output, error);
    case OriginalCopyCheckpoint::kBeforeTemporaryOpen:
    case OriginalCopyCheckpoint::kTemporaryCreated:
        return original_copy_error(ErrorCode::kIo, "Unable to open temporary export file",
                                   "original_copy_temporary_open_failed", source, output, error);
    case OriginalCopyCheckpoint::kBeforeTemporaryWrite:
    case OriginalCopyCheckpoint::kTemporaryChunkWritten:
        return original_copy_error(ErrorCode::kIo, "Unable to write temporary export file",
                                   "original_copy_temporary_write_failed", source, output, error);
    case OriginalCopyCheckpoint::kBeforeTemporaryFinish:
        return original_copy_error(ErrorCode::kIo, "Unable to finish temporary export file",
                                   "original_copy_temporary_finish_failed", source, output, error);
    case OriginalCopyCheckpoint::kBeforePublish:
        return original_copy_error(ErrorCode::kIo, "Unable to publish original copy",
                                   "original_copy_publish_failed", source, output, error);
    }
    return original_copy_error(ErrorCode::kIo, "Unable to copy original file",
                               "original_copy_internal_error", source, output, error);
}

} // namespace

[[nodiscard]] Result<std::uint64_t> copy_file_atomically(const std::string_view source_utf8,
                                                         const std::string_view dest_utf8,
                                                         const CancellationToken &cancellation)
{
    return copy_file_atomically(source_utf8, dest_utf8, cancellation, {});
}

[[nodiscard]] Result<std::uint64_t>
copy_file_atomically(const std::string_view source_utf8, const std::string_view dest_utf8,
                     const CancellationToken &cancellation,
                     const OriginalCopyCheckpointHook checkpoint_hook)
{
    auto active = check_original_copy_cancellation(cancellation, source_utf8, dest_utf8);
    if (!active)
    {
        return active.error();
    }

    const auto source = utf8_path(source_utf8);
    const auto output = utf8_path(dest_utf8);
    if (paths_are_same(source, output))
    {
        return original_copy_error(ErrorCode::kConflict, "Original and output paths are the same",
                                   "original_copy_source_equals_output", source_utf8, dest_utf8);
    }

    std::error_code error;
    const auto source_status = std::filesystem::status(source, error);
    if (error == std::errc::no_such_file_or_directory ||
        source_status.type() == std::filesystem::file_type::not_found)
    {
        return original_copy_error(ErrorCode::kNotFound, "Original file is missing",
                                   "original_copy_source_missing", source_utf8, dest_utf8);
    }
    if (error)
    {
        return original_copy_error(ErrorCode::kIo, "Unable to inspect original file",
                                   "original_copy_source_open_failed", source_utf8, dest_utf8,
                                   error);
    }
    if (!std::filesystem::is_regular_file(source_status))
    {
        return original_copy_error(ErrorCode::kIo, "Original source is not a regular file",
                                   "original_copy_source_not_regular", source_utf8, dest_utf8);
    }
    const auto source_size = std::filesystem::file_size(source, error);
    if (error)
    {
        return original_copy_error(ErrorCode::kIo, "Unable to inspect original file",
                                   "original_copy_source_open_failed", source_utf8, dest_utf8,
                                   error);
    }
    const auto source_mtime = std::filesystem::last_write_time(source, error);
    if (error)
    {
        return original_copy_error(ErrorCode::kIo, "Unable to inspect original file",
                                   "original_copy_source_open_failed", source_utf8, dest_utf8,
                                   error);
    }

    const auto output_status = std::filesystem::symlink_status(output, error);
    if (!error && output_status.type() != std::filesystem::file_type::not_found)
    {
        return original_copy_error(ErrorCode::kConflict, "Export output already exists",
                                   "original_copy_output_exists", source_utf8, dest_utf8);
    }
    if (error != std::errc::no_such_file_or_directory && error != std::errc::not_a_directory &&
        error)
    {
        return original_copy_error(ErrorCode::kIo, "Unable to inspect export output path",
                                   "original_copy_output_inspect_failed", source_utf8, dest_utf8,
                                   error);
    }
    error.clear();

    const auto parent =
        output.parent_path().empty() ? std::filesystem::path(".") : output.parent_path();
    const auto parent_status = std::filesystem::status(parent, error);
    if (error == std::errc::no_such_file_or_directory ||
        parent_status.type() == std::filesystem::file_type::not_found)
    {
        return original_copy_error(ErrorCode::kIo, "Export directory does not exist",
                                   "original_copy_output_parent_missing", source_utf8, dest_utf8);
    }
    if (error)
    {
        return original_copy_error(ErrorCode::kIo, "Unable to inspect export directory",
                                   "original_copy_output_parent_not_directory", source_utf8,
                                   dest_utf8, error);
    }
    if (!std::filesystem::is_directory(parent_status))
    {
        return original_copy_error(ErrorCode::kIo, "Export parent is not a directory",
                                   "original_copy_output_parent_not_directory", source_utf8,
                                   dest_utf8);
    }
#ifndef _WIN32
    if (!has_write_permission(parent_status.permissions()))
    {
        return original_copy_error(ErrorCode::kIo, "Export directory is not writable",
                                   "original_copy_output_parent_unwritable", source_utf8,
                                   dest_utf8);
    }
#endif

    FileDescriptor input(open_source_descriptor(source));
    if (input.get() < 0)
    {
        const std::error_code open_error(errno, std::generic_category());
        if (open_error == std::errc::no_such_file_or_directory)
        {
            return original_copy_error(ErrorCode::kNotFound, "Original file is missing",
                                       "original_copy_source_missing", source_utf8, dest_utf8);
        }
        return original_copy_error(ErrorCode::kIo, "Unable to open original file",
                                   "original_copy_source_open_failed", source_utf8, dest_utf8,
                                   open_error);
    }
    const std::string source_checkpoint_path = checkpoint_path_utf8(source);
    auto hook_error = invoke_original_copy_hook(
        checkpoint_hook, OriginalCopyCheckpoint::kSourceOpened, source_checkpoint_path, 0U);
    active = check_original_copy_cancellation(cancellation, source_utf8, dest_utf8);
    if (!active)
    {
        return active.error();
    }
    if (hook_error)
    {
        return hook_stage_error(OriginalCopyCheckpoint::kSourceOpened, source_utf8, dest_utf8,
                                hook_error);
    }

    OwnedTemporaryPath owned_temporary;
    FileDescriptor temporary_file;
    std::string temporary_utf8;
    for (int attempt = 0; attempt < kOriginalCopyTemporaryAttempts; ++attempt)
    {
        const auto candidate = temporary_candidate(output);
        temporary_utf8 = checkpoint_path_utf8(candidate);
        hook_error = invoke_original_copy_hook(
            checkpoint_hook, OriginalCopyCheckpoint::kBeforeTemporaryOpen, temporary_utf8, 0U);
        active = check_original_copy_cancellation(cancellation, source_utf8, dest_utf8);
        if (!active)
        {
            return active.error();
        }
        if (hook_error)
        {
            return hook_stage_error(OriginalCopyCheckpoint::kBeforeTemporaryOpen, source_utf8,
                                    dest_utf8, hook_error);
        }

        const int descriptor = open_temporary_descriptor(candidate);
        if (descriptor >= 0)
        {
            temporary_file = FileDescriptor(descriptor);
            owned_temporary.reset(candidate);
            break;
        }
        const std::error_code open_error(errno, std::generic_category());
        if (open_error == std::errc::file_exists)
        {
            continue;
        }
        return original_copy_error(ErrorCode::kIo, "Unable to open temporary export file",
                                   "original_copy_temporary_open_failed", source_utf8, dest_utf8,
                                   open_error);
    }
    if (temporary_file.get() < 0)
    {
        return original_copy_error(ErrorCode::kIo, "Unable to create unique temporary export file",
                                   "original_copy_temporary_open_failed", source_utf8, dest_utf8,
                                   std::make_error_code(std::errc::file_exists));
    }

    hook_error = invoke_original_copy_hook(
        checkpoint_hook, OriginalCopyCheckpoint::kTemporaryCreated, temporary_utf8, 0U);
    active = check_original_copy_cancellation(cancellation, source_utf8, dest_utf8);
    if (!active)
    {
        return active.error();
    }
    if (hook_error)
    {
        return hook_stage_error(OriginalCopyCheckpoint::kTemporaryCreated, source_utf8, dest_utf8,
                                hook_error);
    }

    std::array<std::uint8_t, kOriginalCopyChunkBytes> buffer{};
    std::uint64_t bytes_copied = 0U;
    for (;;)
    {
        active = check_original_copy_cancellation(cancellation, source_utf8, dest_utf8);
        if (!active)
        {
            return active.error();
        }
        hook_error =
            invoke_original_copy_hook(checkpoint_hook, OriginalCopyCheckpoint::kBeforeSourceRead,
                                      source_checkpoint_path, bytes_copied);
        active = check_original_copy_cancellation(cancellation, source_utf8, dest_utf8);
        if (!active)
        {
            return active.error();
        }
        if (hook_error)
        {
            return hook_stage_error(OriginalCopyCheckpoint::kBeforeSourceRead, source_utf8,
                                    dest_utf8, hook_error);
        }

        const auto count = read_descriptor(input.get(), buffer.data(), buffer.size());
        if (count < 0)
        {
            const std::error_code read_error(errno, std::generic_category());
            active = check_original_copy_cancellation(cancellation, source_utf8, dest_utf8);
            if (!active)
            {
                return active.error();
            }
            return original_copy_error(ErrorCode::kIo, "Unable to read original file",
                                       "original_copy_source_read_failed", source_utf8, dest_utf8,
                                       read_error);
        }
        if (count == 0)
        {
            break;
        }
        const auto chunk_size = static_cast<std::size_t>(count);
        if (bytes_copied > std::numeric_limits<std::uint64_t>::max() - chunk_size)
        {
            return original_copy_error(ErrorCode::kIo, "Original file is too large to copy",
                                       "original_copy_source_size_overflow", source_utf8,
                                       dest_utf8);
        }
        const auto chunk_end = bytes_copied + chunk_size;
        hook_error =
            invoke_original_copy_hook(checkpoint_hook, OriginalCopyCheckpoint::kSourceChunkRead,
                                      source_checkpoint_path, chunk_end);
        active = check_original_copy_cancellation(cancellation, source_utf8, dest_utf8);
        if (!active)
        {
            return active.error();
        }
        if (hook_error)
        {
            return hook_stage_error(OriginalCopyCheckpoint::kSourceChunkRead, source_utf8,
                                    dest_utf8, hook_error);
        }

        hook_error = invoke_original_copy_hook(checkpoint_hook,
                                               OriginalCopyCheckpoint::kBeforeTemporaryWrite,
                                               temporary_utf8, bytes_copied);
        active = check_original_copy_cancellation(cancellation, source_utf8, dest_utf8);
        if (!active)
        {
            return active.error();
        }
        if (hook_error)
        {
            return hook_stage_error(OriginalCopyCheckpoint::kBeforeTemporaryWrite, source_utf8,
                                    dest_utf8, hook_error);
        }

        std::size_t offset = 0U;
        while (offset < chunk_size)
        {
            const auto written =
                write_descriptor(temporary_file.get(), buffer.data() + offset, chunk_size - offset);
            if (written <= 0)
            {
                const std::error_code write_error =
                    written < 0 ? std::error_code(errno, std::generic_category()) :
                                  std::make_error_code(std::errc::io_error);
                active = check_original_copy_cancellation(cancellation, source_utf8, dest_utf8);
                if (!active)
                {
                    return active.error();
                }
                return hook_stage_error(OriginalCopyCheckpoint::kBeforeTemporaryWrite, source_utf8,
                                        dest_utf8, write_error);
            }
            offset += static_cast<std::size_t>(written);
        }
        bytes_copied = chunk_end;
        hook_error = invoke_original_copy_hook(checkpoint_hook,
                                               OriginalCopyCheckpoint::kTemporaryChunkWritten,
                                               temporary_utf8, bytes_copied);
        active = check_original_copy_cancellation(cancellation, source_utf8, dest_utf8);
        if (!active)
        {
            return active.error();
        }
        if (hook_error)
        {
            return hook_stage_error(OriginalCopyCheckpoint::kTemporaryChunkWritten, source_utf8,
                                    dest_utf8, hook_error);
        }
    }

    const auto final_size = std::filesystem::file_size(source, error);
    if (error || source_size != final_size || source_size != bytes_copied ||
        std::filesystem::last_write_time(source, error) != source_mtime || error)
    {
        return original_copy_error(ErrorCode::kIo, "Original file changed while copying",
                                   "original_copy_source_changed", source_utf8, dest_utf8);
    }

    hook_error =
        invoke_original_copy_hook(checkpoint_hook, OriginalCopyCheckpoint::kBeforeTemporaryFinish,
                                  temporary_utf8, bytes_copied);
    active = check_original_copy_cancellation(cancellation, source_utf8, dest_utf8);
    if (!active)
    {
        return active.error();
    }
    if (hook_error)
    {
        return hook_stage_error(OriginalCopyCheckpoint::kBeforeTemporaryFinish, source_utf8,
                                dest_utf8, hook_error);
    }
    const auto finish_error = temporary_file.finish();
    if (finish_error)
    {
        active = check_original_copy_cancellation(cancellation, source_utf8, dest_utf8);
        if (!active)
        {
            return active.error();
        }
        return hook_stage_error(OriginalCopyCheckpoint::kBeforeTemporaryFinish, source_utf8,
                                dest_utf8, finish_error);
    }

    hook_error = invoke_original_copy_hook(checkpoint_hook, OriginalCopyCheckpoint::kBeforePublish,
                                           temporary_utf8, bytes_copied);
    active = check_original_copy_cancellation(cancellation, source_utf8, dest_utf8);
    if (!active)
    {
        return active.error();
    }
    if (hook_error)
    {
        return hook_stage_error(OriginalCopyCheckpoint::kBeforePublish, source_utf8, dest_utf8,
                                hook_error);
    }

    error = publish_no_replace(owned_temporary.path(), output);
    if (error)
    {
        std::error_code inspect_error;
        const auto status = std::filesystem::symlink_status(output, inspect_error);
        if (!inspect_error && status.type() != std::filesystem::file_type::not_found)
        {
            return original_copy_error(ErrorCode::kConflict, "Export output already exists",
                                       "original_copy_output_exists", source_utf8, dest_utf8);
        }
        return hook_stage_error(OriginalCopyCheckpoint::kBeforePublish, source_utf8, dest_utf8,
                                error);
    }
    owned_temporary.release();
    return bytes_copied;
}

} // namespace ravo
