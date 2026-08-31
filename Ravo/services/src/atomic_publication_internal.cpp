#include "atomic_publication_internal.h"

#include <cerrno>
#include <utility>

#include "ravo/domain/types.h"

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
#include <unistd.h>
#ifdef __APPLE__
#include <sys/stdio.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

namespace ravo::atomic_publication_internal
{

FileDescriptor::FileDescriptor(const int descriptor) noexcept
    : descriptor_(descriptor)
{
}

FileDescriptor::FileDescriptor(FileDescriptor &&other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1))
{
}

FileDescriptor &FileDescriptor::operator=(FileDescriptor &&other) noexcept
{
    if (this != &other)
    {
        close_ignoring_error();
        descriptor_ = std::exchange(other.descriptor_, -1);
    }
    return *this;
}

FileDescriptor::~FileDescriptor()
{
    close_ignoring_error();
}

int FileDescriptor::get() const noexcept
{
    return descriptor_;
}

std::error_code FileDescriptor::sync() noexcept
{
    if (descriptor_ < 0)
    {
        return std::make_error_code(std::errc::bad_file_descriptor);
    }
#ifdef _WIN32
    if (::_commit(descriptor_) != 0)
#else
    int result = -1;
    do
    {
        result = ::fsync(descriptor_);
    } while (result != 0 && errno == EINTR);
    if (result != 0)
#endif
    {
        return {errno, std::generic_category()};
    }
    return {};
}

std::error_code FileDescriptor::close() noexcept
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
        return {errno, std::generic_category()};
    }
    return {};
}

std::error_code FileDescriptor::finish() noexcept
{
    const auto sync_error = sync();
    if (sync_error)
    {
        close_ignoring_error();
        return sync_error;
    }
    return close();
}

void FileDescriptor::close_ignoring_error() noexcept
{
    static_cast<void>(close());
}

OwnedTemporaryPath::~OwnedTemporaryPath()
{
    remove_ignoring_error();
}

void OwnedTemporaryPath::reset(std::filesystem::path path)
{
    remove_ignoring_error();
    path_ = std::move(path);
}

const std::filesystem::path &OwnedTemporaryPath::path() const noexcept
{
    return path_;
}

std::error_code OwnedTemporaryPath::remove() noexcept
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

void OwnedTemporaryPath::release() noexcept
{
    path_.clear();
}

void OwnedTemporaryPath::remove_ignoring_error() noexcept
{
    static_cast<void>(remove());
}

OwnedTemporaryDirectory::~OwnedTemporaryDirectory()
{
    remove_ignoring_error();
}

void OwnedTemporaryDirectory::reset(std::filesystem::path path)
{
    remove_ignoring_error();
    path_ = std::move(path);
}

const std::filesystem::path &OwnedTemporaryDirectory::path() const noexcept
{
    return path_;
}

std::error_code OwnedTemporaryDirectory::remove() noexcept
{
    if (path_.empty())
        return {};
    std::error_code error;
    std::filesystem::remove_all(path_, error);
    if (!error)
        path_.clear();
    return error;
}

void OwnedTemporaryDirectory::release() noexcept
{
    path_.clear();
}

void OwnedTemporaryDirectory::remove_ignoring_error() noexcept
{
    static_cast<void>(remove());
}

int open_temporary_descriptor(const std::filesystem::path &path) noexcept
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

std::int64_t write_descriptor(const int descriptor, const void *const bytes,
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

std::string path_utf8(const std::filesystem::path &path)
{
    const auto encoded = path.generic_u8string();
    return {reinterpret_cast<const char *>(encoded.data()), encoded.size()};
}

std::string checkpoint_path_utf8(const std::filesystem::path &path)
{
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    return path_utf8(error ? path : canonical);
}

bool has_write_permission(const std::filesystem::perms permissions) noexcept
{
#ifdef _WIN32
    static_cast<void>(permissions);
    return true;
#else
    constexpr auto write_permissions = std::filesystem::perms::owner_write |
                                       std::filesystem::perms::group_write |
                                       std::filesystem::perms::others_write;
    return (permissions & write_permissions) != std::filesystem::perms::none;
#endif
}

std::filesystem::path temporary_candidate(const std::filesystem::path &output,
                                          const std::string_view owner)
{
    const auto parent =
        output.parent_path().empty() ? std::filesystem::path(".") : output.parent_path();
    const auto filename =
        path_utf8(".ravo-" + std::string(owner) + "-" + generate_catalog_id() + ".tmp");
    return parent / filename;
}

std::error_code publish_no_replace(const std::filesystem::path &temporary,
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

} // namespace ravo::atomic_publication_internal
