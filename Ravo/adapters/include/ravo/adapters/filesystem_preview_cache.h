#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "ravo/domain/preview_cache.h"

namespace ravo
{

class FilesystemPreviewCache final : public PreviewCache
{
public:
    static Result<std::unique_ptr<FilesystemPreviewCache>> create(std::string_view cache_root);

    [[nodiscard]] const std::string &root() const noexcept override;
    [[nodiscard]] std::string relative_png_path(std::string_view cache_key) const override;
    [[nodiscard]] std::string absolute_png_path(std::string_view cache_key) const override;
    [[nodiscard]] Result<std::optional<std::string>>
    existing_png(std::string_view cache_key) const override;
    [[nodiscard]] Result<std::string>
    commit_png_bytes(std::string_view cache_key, const std::vector<std::uint8_t> &png_bytes) override;

private:
    explicit FilesystemPreviewCache(std::string root);

    std::string root_;
};

} // namespace ravo
