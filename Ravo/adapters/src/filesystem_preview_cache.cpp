#include "ravo/adapters/filesystem_preview_cache.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <system_error>
#include <utility>
#include <vector>

#include <QtCore/QByteArray>
#include <QtCore/QFile>
#include <QtCore/QIODevice>
#include <QtCore/QSaveFile>
#include <QtCore/QString>

namespace ravo
{
namespace
{

constexpr char kPngSignature[] = {'\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n'};

[[nodiscard]] QString qstring_from_utf8(const std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

[[nodiscard]] std::string from_u8(const std::u8string &value)
{
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}

[[nodiscard]] std::filesystem::path path_from_utf8(const std::string_view value)
{
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

[[nodiscard]] bool key_is_safe(const std::string_view cache_key)
{
    if (cache_key.empty() || cache_key.size() > 180)
    {
        return false;
    }
    return std::all_of(cache_key.begin(), cache_key.end(),
                       [](const char character)
                       {
                           return (character >= 'a' && character <= 'z') ||
                                  (character >= 'A' && character <= 'Z') ||
                                  (character >= '0' && character <= '9') || character == '_' ||
                                  character == '-' || character == '.';
                       });
}

[[nodiscard]] bool has_png_signature(QFile &file)
{
    const QByteArray magic = file.read(8);
    return magic.size() == 8 && magic == QByteArray::fromRawData(kPngSignature, 8);
}

[[nodiscard]] bool has_png_signature(const std::vector<std::uint8_t> &bytes)
{
    return bytes.size() >= 8U && std::equal(bytes.begin(), bytes.begin() + 8,
                                            reinterpret_cast<const unsigned char *>(kPngSignature));
}

[[nodiscard]] TaskError filesystem_error(const std::string &message,
                                         const std::filesystem::path &path,
                                         const std::error_code &error)
{
    return make_error(ErrorCode::kIo, message,
                      {{"path", from_u8(path.generic_u8string())}, {"detail", error.message()}});
}

} // namespace

FilesystemPreviewCache::FilesystemPreviewCache(std::string root, const std::uint64_t max_bytes)
    : root_(std::move(root))
    , max_bytes_(max_bytes)
{
}

Result<std::unique_ptr<FilesystemPreviewCache>>
FilesystemPreviewCache::create(const std::string_view cache_root, const std::uint64_t max_bytes)
{
    if (cache_root.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "Preview cache root must not be empty");
    }
    if (max_bytes == 0)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Preview cache byte budget must be greater than zero");
    }
    const auto root_path = path_from_utf8(cache_root);
    std::error_code error;
    std::filesystem::create_directories(root_path, error);
    if (error)
    {
        return filesystem_error("Unable to create preview cache directory", root_path, error);
    }
    if (!std::filesystem::is_directory(root_path, error) || error)
    {
        if (!error)
            error = std::make_error_code(std::errc::not_a_directory);
        return filesystem_error("Preview cache root is not a directory", root_path, error);
    }

    auto cache = std::unique_ptr<FilesystemPreviewCache>(
        new FilesystemPreviewCache(std::string(cache_root), max_bytes));
    auto initialized = cache->initialize_index();
    if (!initialized)
    {
        return initialized.error();
    }
    return cache;
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
    auto path = path_from_utf8(root_);
    path /= path_from_utf8(relative_png_path(cache_key));
    return from_u8(path.generic_u8string());
}

Result<void> FilesystemPreviewCache::initialize_index()
{
    struct Candidate
    {
        std::string key;
        std::uint64_t bytes = 0;
        std::filesystem::file_time_type last_access;
    };

    std::scoped_lock lock(mutex_);
    entries_.clear();
    used_bytes_ = 0;
    access_sequence_ = 0;
    std::vector<Candidate> candidates;
    const auto root_path = path_from_utf8(root_);
    std::error_code iterator_error;
    std::filesystem::directory_iterator iterator(root_path, iterator_error);
    if (iterator_error)
    {
        return filesystem_error("Unable to inspect preview cache directory", root_path,
                                iterator_error);
    }
    const std::filesystem::directory_iterator end;
    while (iterator != end)
    {
        const auto path = iterator->path();
        std::error_code status_error;
        const auto status = iterator->symlink_status(status_error);
        if (status_error)
        {
            return filesystem_error("Unable to inspect preview cache entry", path, status_error);
        }
        const auto name = from_u8(path.filename().u8string());
        const auto key = from_u8(path.stem().u8string());
        if (std::filesystem::is_regular_file(status) && path.extension() == ".png" &&
            name == key + ".png" && key_is_safe(key))
        {
            QFile file(qstring_from_utf8(from_u8(path.generic_u8string())));
            if (!file.open(QIODevice::ReadOnly))
            {
                return make_error(ErrorCode::kIo, "Unable to open preview cache file",
                                  {{"path", from_u8(path.generic_u8string())},
                                   {"qt_error", file.errorString().toUtf8().toStdString()}});
            }
            if (!has_png_signature(file))
            {
                file.close();
                std::error_code remove_error;
                std::filesystem::remove(path, remove_error);
                if (remove_error)
                {
                    return filesystem_error("Unable to remove corrupt preview cache file", path,
                                            remove_error);
                }
            }
            else
            {
                std::error_code size_error;
                const auto file_bytes = std::filesystem::file_size(path, size_error);
                if (size_error)
                {
                    return filesystem_error("Unable to measure preview cache file", path,
                                            size_error);
                }
                if (file_bytes > std::numeric_limits<std::uint64_t>::max())
                {
                    return make_error(ErrorCode::kValidation, "Preview cache file is too large",
                                      {{"path", from_u8(path.generic_u8string())}});
                }
                std::error_code time_error;
                const auto last_access = std::filesystem::last_write_time(path, time_error);
                if (time_error)
                {
                    return filesystem_error("Unable to read preview cache access time", path,
                                            time_error);
                }
                candidates.push_back({key, static_cast<std::uint64_t>(file_bytes), last_access});
            }
        }
        iterator.increment(iterator_error);
        if (iterator_error)
        {
            return filesystem_error("Unable to iterate preview cache directory", root_path,
                                    iterator_error);
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &left, const Candidate &right)
              {
                  if (left.last_access != right.last_access)
                      return left.last_access < right.last_access;
                  return left.key < right.key;
              });
    for (const auto &candidate : candidates)
    {
        if (candidate.bytes > std::numeric_limits<std::uint64_t>::max() - used_bytes_)
        {
            return make_error(ErrorCode::kValidation, "Preview cache size overflows accounting",
                              {{"path", root_}});
        }
        used_bytes_ += candidate.bytes;
        entries_.emplace(candidate.key, Entry{candidate.bytes, ++access_sequence_});
    }
    return evict_to_fit_locked(0);
}

void FilesystemPreviewCache::forget_entry_locked(const std::string_view cache_key) const
{
    const auto found = entries_.find(cache_key);
    if (found == entries_.end())
        return;
    if (used_bytes_ >= found->second.bytes)
        used_bytes_ -= found->second.bytes;
    else
        used_bytes_ = 0;
    entries_.erase(found);
}

Result<void> FilesystemPreviewCache::evict_to_fit_locked(const std::uint64_t incoming_bytes) const
{
    if (incoming_bytes > max_bytes_)
    {
        return make_error(ErrorCode::kValidation, "Preview PNG exceeds cache byte budget",
                          {{"required_bytes", std::to_string(incoming_bytes)},
                           {"max_bytes", std::to_string(max_bytes_)}});
    }
    while (used_bytes_ > max_bytes_ - incoming_bytes)
    {
        if (entries_.empty())
        {
            return make_error(ErrorCode::kInternal, "Preview cache accounting is inconsistent",
                              {{"used_bytes", std::to_string(used_bytes_)},
                               {"max_bytes", std::to_string(max_bytes_)}});
        }
        const auto victim = std::min_element(
            entries_.begin(), entries_.end(),
            [](const auto &left, const auto &right)
            {
                if (left.second.access_sequence != right.second.access_sequence)
                    return left.second.access_sequence < right.second.access_sequence;
                return left.first < right.first;
            });
        const auto path = path_from_utf8(absolute_png_path(victim->first));
        std::error_code remove_error;
        std::filesystem::remove(path, remove_error);
        if (remove_error)
        {
            return filesystem_error("Unable to evict preview cache file", path, remove_error);
        }
        used_bytes_ -= victim->second.bytes;
        entries_.erase(victim);
    }
    return {};
}

Result<std::optional<std::string>>
FilesystemPreviewCache::existing_png_locked(const std::string_view cache_key) const
{
    const auto path_text = absolute_png_path(cache_key);
    const auto path = path_from_utf8(path_text);
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(path, status_error);
    if (status_error)
    {
        if (status_error == std::errc::no_such_file_or_directory)
        {
            forget_entry_locked(cache_key);
            return std::optional<std::string>{};
        }
        return filesystem_error("Unable to inspect preview cache file", path, status_error);
    }
    if (!std::filesystem::exists(status))
    {
        forget_entry_locked(cache_key);
        return std::optional<std::string>{};
    }
    if (!std::filesystem::is_regular_file(status))
    {
        return make_error(ErrorCode::kIo, "Preview cache entry is not a regular file",
                          {{"path", path_text}});
    }

    QFile file(qstring_from_utf8(path_text));
    if (!file.open(QIODevice::ReadOnly))
    {
        return make_error(
            ErrorCode::kIo, "Unable to open preview cache file",
            {{"path", path_text}, {"qt_error", file.errorString().toUtf8().toStdString()}});
    }
    if (!has_png_signature(file))
    {
        file.close();
        std::error_code remove_error;
        std::filesystem::remove(path, remove_error);
        if (remove_error)
        {
            return filesystem_error("Unable to remove corrupt preview cache file", path,
                                    remove_error);
        }
        forget_entry_locked(cache_key);
        return std::optional<std::string>{};
    }
    file.close();

    std::error_code size_error;
    const auto file_bytes = std::filesystem::file_size(path, size_error);
    if (size_error)
    {
        return filesystem_error("Unable to measure preview cache file", path, size_error);
    }
    if (file_bytes > max_bytes_)
    {
        std::error_code remove_error;
        std::filesystem::remove(path, remove_error);
        if (remove_error)
        {
            return filesystem_error("Unable to evict oversized preview cache file", path,
                                    remove_error);
        }
        forget_entry_locked(cache_key);
        return std::optional<std::string>{};
    }

    std::error_code time_error;
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(),
                                     time_error);
    if (time_error)
    {
        return filesystem_error("Unable to update preview cache access time", path, time_error);
    }
    forget_entry_locked(cache_key);
    const auto bytes = static_cast<std::uint64_t>(file_bytes);
    auto evicted = evict_to_fit_locked(bytes);
    if (!evicted)
        return evicted.error();
    used_bytes_ += bytes;
    entries_.emplace(std::string(cache_key), Entry{bytes, ++access_sequence_});
    return std::optional<std::string>{path_text};
}

Result<std::optional<std::string>>
FilesystemPreviewCache::existing_png(const std::string_view cache_key) const
{
    if (!key_is_safe(cache_key))
    {
        return make_error(ErrorCode::kValidation, "Preview cache key is invalid",
                          {{"cache_key", std::string(cache_key)}});
    }
    std::scoped_lock lock(mutex_);
    return existing_png_locked(cache_key);
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
    if (!has_png_signature(png_bytes))
    {
        return make_error(ErrorCode::kValidation, "Preview cache payload is not a PNG");
    }
    if (png_bytes.size() > static_cast<std::size_t>(std::numeric_limits<qint64>::max()))
    {
        return make_error(ErrorCode::kValidation, "Preview PNG is too large");
    }
    const auto incoming_bytes = static_cast<std::uint64_t>(png_bytes.size());
    if (incoming_bytes > max_bytes_)
    {
        return make_error(ErrorCode::kValidation, "Preview PNG exceeds cache byte budget",
                          {{"required_bytes", std::to_string(incoming_bytes)},
                           {"max_bytes", std::to_string(max_bytes_)}});
    }

    std::scoped_lock lock(mutex_);
    auto existing = existing_png_locked(cache_key);
    if (!existing)
        return existing.error();
    if (existing.value())
        return *existing.value();

    auto evicted = evict_to_fit_locked(incoming_bytes);
    if (!evicted)
        return evicted.error();

    const auto path = absolute_png_path(cache_key);
    QSaveFile output(qstring_from_utf8(path));
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly))
    {
        return make_error(
            ErrorCode::kIo, "Unable to open preview cache file",
            {{"path", path}, {"qt_error", output.errorString().toUtf8().toStdString()}});
    }
    const auto written = output.write(reinterpret_cast<const char *>(png_bytes.data()),
                                      static_cast<qint64>(png_bytes.size()));
    if (written != static_cast<qint64>(png_bytes.size()) || !output.commit())
    {
        return make_error(
            ErrorCode::kIo, "Unable to commit preview cache file",
            {{"path", path}, {"qt_error", output.errorString().toUtf8().toStdString()}});
    }
    forget_entry_locked(cache_key);
    used_bytes_ += incoming_bytes;
    entries_.emplace(std::string(cache_key), Entry{incoming_bytes, ++access_sequence_});
    return path;
}

Result<void> FilesystemPreviewCache::remove_png(const std::string_view cache_key)
{
    if (!key_is_safe(cache_key))
    {
        return make_error(ErrorCode::kValidation, "Preview cache key is invalid",
                          {{"cache_key", std::string(cache_key)}});
    }
    std::scoped_lock lock(mutex_);
    const auto path = path_from_utf8(absolute_png_path(cache_key));
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(path, status_error);
    if (status_error)
    {
        if (status_error == std::errc::no_such_file_or_directory)
        {
            forget_entry_locked(cache_key);
            return {};
        }
        return filesystem_error("Unable to inspect preview cache file", path, status_error);
    }
    if (!std::filesystem::exists(status))
    {
        forget_entry_locked(cache_key);
        return {};
    }
    if (!std::filesystem::is_regular_file(status))
    {
        return make_error(ErrorCode::kIo, "Preview cache entry is not a regular file",
                          {{"path", from_u8(path.generic_u8string())}});
    }
    std::error_code remove_error;
    std::filesystem::remove(path, remove_error);
    if (remove_error)
        return filesystem_error("Unable to remove preview cache file", path, remove_error);
    forget_entry_locked(cache_key);
    return {};
}

Result<void> FilesystemPreviewCache::remove_for_asset(const std::string_view asset_id)
{
    if (asset_id.empty() || asset_id.find('/') != std::string_view::npos ||
        asset_id.find('\\') != std::string_view::npos)
    {
        return make_error(ErrorCode::kValidation, "Preview cache asset id is invalid",
                          {{"asset_id", std::string(asset_id)}});
    }
    const auto needle = "_" + std::string(asset_id) + "_";
    std::scoped_lock lock(mutex_);
    const auto root_path = path_from_utf8(root_);
    std::error_code iterator_error;
    std::filesystem::directory_iterator iterator(root_path, iterator_error);
    if (iterator_error)
        return filesystem_error("Unable to inspect preview cache directory", root_path,
                                iterator_error);
    std::vector<std::pair<std::string, std::filesystem::path>> victims;
    const std::filesystem::directory_iterator end;
    while (iterator != end)
    {
        const auto path = iterator->path();
        std::error_code status_error;
        const auto status = iterator->symlink_status(status_error);
        if (status_error)
            return filesystem_error("Unable to inspect preview cache entry", path, status_error);
        const auto key = from_u8(path.stem().u8string());
        if (std::filesystem::is_regular_file(status) && path.extension() == ".png" &&
            key_is_safe(key) && key.find(needle) != std::string::npos)
        {
            victims.emplace_back(key, path);
        }
        iterator.increment(iterator_error);
        if (iterator_error)
            return filesystem_error("Unable to iterate preview cache directory", root_path,
                                    iterator_error);
    }
    for (const auto &[key, path] : victims)
    {
        std::error_code remove_error;
        std::filesystem::remove(path, remove_error);
        if (remove_error)
            return filesystem_error("Unable to remove preview cache file", path, remove_error);
        forget_entry_locked(key);
    }
    return {};
}

std::uint64_t FilesystemPreviewCache::max_bytes() const noexcept
{
    return max_bytes_;
}

std::uint64_t FilesystemPreviewCache::used_bytes() const
{
    std::scoped_lock lock(mutex_);
    return used_bytes_;
}

std::size_t FilesystemPreviewCache::entry_count() const
{
    std::scoped_lock lock(mutex_);
    return entries_.size();
}

} // namespace ravo
