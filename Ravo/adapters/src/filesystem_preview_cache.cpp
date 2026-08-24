#include "ravo/adapters/filesystem_preview_cache.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <system_error>

#include <QtCore/QFileInfo>
#include <QtCore/QIODevice>
#include <QtCore/QSaveFile>
#include <QtCore/QString>

namespace ravo
{
namespace
{

[[nodiscard]] QString qstring_from_utf8(const std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

[[nodiscard]] std::string from_u8(const std::u8string &value)
{
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}

[[nodiscard]] bool key_is_safe(const std::string_view cache_key)
{
    if (cache_key.empty() || cache_key.size() > 180)
    {
        return false;
    }
    return std::all_of(cache_key.begin(), cache_key.end(), [](const char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '_' || character == '-' ||
               character == '.' || character == '-';
    });
}

} // namespace

FilesystemPreviewCache::FilesystemPreviewCache(std::string root)
    : root_(std::move(root))
{
}

Result<std::unique_ptr<FilesystemPreviewCache>>
FilesystemPreviewCache::create(const std::string_view cache_root)
{
    if (cache_root.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "Preview cache root must not be empty");
    }
    std::error_code error;
    std::filesystem::create_directories(
        std::filesystem::path(std::u8string(cache_root.begin(), cache_root.end())), error);
    if (error)
    {
        return make_error(ErrorCode::kIo, "Unable to create preview cache directory",
                          {{"path", std::string(cache_root)}, {"detail", error.message()}});
    }
    return std::unique_ptr<FilesystemPreviewCache>(
        new FilesystemPreviewCache(std::string(cache_root)));
}

const std::string &FilesystemPreviewCache::root() const noexcept
{
    return root_;
}

std::string FilesystemPreviewCache::relative_png_path(const std::string_view cache_key) const
{
    return std::string(cache_key) + ".png";
}

std::string FilesystemPreviewCache::absolute_png_path(const std::string_view cache_key) const
{
    std::filesystem::path root(std::u8string(root_.begin(), root_.end()));
    root /= std::filesystem::path(relative_png_path(cache_key));
    return from_u8(root.generic_u8string());
}

Result<std::optional<std::string>>
FilesystemPreviewCache::existing_png(const std::string_view cache_key) const
{
    if (!key_is_safe(cache_key))
    {
        return make_error(ErrorCode::kValidation, "Preview cache key is invalid",
                          {{"cache_key", std::string(cache_key)}});
    }
    const auto path = absolute_png_path(cache_key);
    if (!QFileInfo::exists(qstring_from_utf8(path)))
    {
        return std::optional<std::string>{};
    }
    return std::optional<std::string>{path};
}

Result<std::string>
FilesystemPreviewCache::commit_png_bytes(const std::string_view cache_key,
                                         const std::vector<std::uint8_t> &png_bytes)
{
    if (!key_is_safe(cache_key))
    {
        return make_error(ErrorCode::kValidation, "Preview cache key is invalid",
                          {{"cache_key", std::string(cache_key)}});
    }
    if (png_bytes.empty())
    {
        return make_error(ErrorCode::kValidation, "Preview PNG bytes must not be empty");
    }
    auto existing = existing_png(cache_key);
    if (!existing)
    {
        return existing.error();
    }
    if (existing.value())
    {
        return existing.value().value();
    }

    const auto path = absolute_png_path(cache_key);
    if (png_bytes.size() > static_cast<std::size_t>(std::numeric_limits<qsizetype>::max()))
    {
        return make_error(ErrorCode::kValidation, "Preview PNG is too large");
    }
    QSaveFile output(qstring_from_utf8(path));
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly))
    {
        return make_error(ErrorCode::kIo, "Unable to open preview cache file",
                          {{"path", path}, {"qt_error", output.errorString().toUtf8().toStdString()}});
    }
    const auto written =
        output.write(reinterpret_cast<const char *>(png_bytes.data()),
                     static_cast<qint64>(png_bytes.size()));
    if (written != static_cast<qint64>(png_bytes.size()) || !output.commit())
    {
        return make_error(ErrorCode::kIo, "Unable to commit preview cache file",
                          {{"path", path}, {"qt_error", output.errorString().toUtf8().toStdString()}});
    }
    return path;
}

} // namespace ravo
