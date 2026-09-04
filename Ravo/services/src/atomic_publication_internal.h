#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace ravo::atomic_publication_internal
{

class FileDescriptor
{
public:
    FileDescriptor() = default;
    explicit FileDescriptor(int descriptor) noexcept;
    FileDescriptor(const FileDescriptor &) = delete;
    FileDescriptor &operator=(const FileDescriptor &) = delete;
    FileDescriptor(FileDescriptor &&other) noexcept;
    FileDescriptor &operator=(FileDescriptor &&other) noexcept;
    ~FileDescriptor();

    [[nodiscard]] int get() const noexcept;
    [[nodiscard]] std::error_code sync() noexcept;
    [[nodiscard]] std::error_code close() noexcept;
    [[nodiscard]] std::error_code finish() noexcept;

private:
    void close_ignoring_error() noexcept;

    int descriptor_ = -1;
};

class OwnedTemporaryPath
{
public:
    OwnedTemporaryPath() = default;
    OwnedTemporaryPath(const OwnedTemporaryPath &) = delete;
    OwnedTemporaryPath &operator=(const OwnedTemporaryPath &) = delete;
    ~OwnedTemporaryPath();

    void reset(std::filesystem::path path);
    [[nodiscard]] const std::filesystem::path &path() const noexcept;
    [[nodiscard]] std::error_code remove() noexcept;
    void release() noexcept;

private:
    void remove_ignoring_error() noexcept;

    std::filesystem::path path_;
};

class OwnedTemporaryDirectory
{
public:
    OwnedTemporaryDirectory() = default;
    OwnedTemporaryDirectory(const OwnedTemporaryDirectory &) = delete;
    OwnedTemporaryDirectory &operator=(const OwnedTemporaryDirectory &) = delete;
    ~OwnedTemporaryDirectory();

    void reset(std::filesystem::path path);
    [[nodiscard]] const std::filesystem::path &path() const noexcept;
    [[nodiscard]] std::error_code remove() noexcept;
    void release() noexcept;

private:
    void remove_ignoring_error() noexcept;

    std::filesystem::path path_;
};

[[nodiscard]] int open_temporary_descriptor(const std::filesystem::path &path) noexcept;
[[nodiscard]] int open_read_descriptor(const std::filesystem::path &path) noexcept;
[[nodiscard]] std::int64_t write_descriptor(int descriptor, const void *bytes,
                                            std::size_t size) noexcept;
[[nodiscard]] std::string path_utf8(const std::filesystem::path &path);
[[nodiscard]] std::string checkpoint_path_utf8(const std::filesystem::path &path);
[[nodiscard]] bool has_write_permission(std::filesystem::perms permissions) noexcept;
[[nodiscard]] std::filesystem::path temporary_candidate(const std::filesystem::path &output,
                                                        std::string_view owner);
[[nodiscard]] std::error_code publish_no_replace(const std::filesystem::path &temporary,
                                                 const std::filesystem::path &output) noexcept;

} // namespace ravo::atomic_publication_internal
