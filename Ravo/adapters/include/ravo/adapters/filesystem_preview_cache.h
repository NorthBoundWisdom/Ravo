#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "ravo/domain/preview_cache.h"

namespace ravo
{

class FilesystemPreviewCache final : public PreviewCache
{
public:
    static constexpr std::uint64_t kDefaultMaxBytes = 512ULL * 1024ULL * 1024ULL;

    static Result<std::unique_ptr<FilesystemPreviewCache>>
    create(std::string_view cache_root, std::uint64_t max_bytes = kDefaultMaxBytes);

    [[nodiscard]] const std::string &root() const noexcept override;
    [[nodiscard]] std::string relative_png_path(std::string_view cache_key) const override;
    [[nodiscard]] std::string absolute_png_path(std::string_view cache_key) const override;
    [[nodiscard]] Result<std::optional<std::string>>
    existing_png(std::string_view cache_key) const override;
    [[nodiscard]] Result<std::string>
    commit_png_bytes(std::string_view cache_key,
                     const std::vector<std::uint8_t> &png_bytes) override;
    [[nodiscard]] Result<void> remove_png(std::string_view cache_key) override;
    [[nodiscard]] Result<void> remove_for_asset(std::string_view asset_id) override;

    [[nodiscard]] std::uint64_t max_bytes() const noexcept;
    [[nodiscard]] std::uint64_t used_bytes() const;
    [[nodiscard]] std::size_t entry_count() const;

private:
    struct Entry
    {
        std::uint64_t bytes = 0;
        std::uint64_t access_sequence = 0;
    };

    FilesystemPreviewCache(std::string root, std::uint64_t max_bytes);

    [[nodiscard]] Result<void> initialize_index();
    [[nodiscard]] Result<std::optional<std::string>>
    existing_png_locked(std::string_view cache_key) const;
    [[nodiscard]] Result<void> evict_to_fit_locked(std::uint64_t incoming_bytes) const;
    void forget_entry_locked(std::string_view cache_key) const;

    std::string root_;
    std::uint64_t max_bytes_ = 0;
    mutable std::mutex mutex_;
    mutable std::map<std::string, Entry, std::less<>> entries_;
    mutable std::uint64_t used_bytes_ = 0;
    mutable std::uint64_t access_sequence_ = 0;
};

} // namespace ravo
