#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/foundation/error.h"

namespace ravo
{

class PreviewCache
{
public:
    virtual ~PreviewCache() = default;

    [[nodiscard]] virtual const std::string &root() const noexcept = 0;
    [[nodiscard]] virtual std::string relative_png_path(std::string_view cache_key) const = 0;
    [[nodiscard]] virtual std::string absolute_png_path(std::string_view cache_key) const = 0;
    [[nodiscard]] virtual Result<std::optional<std::string>>
    existing_png(std::string_view cache_key) const = 0;
    [[nodiscard]] virtual Result<std::string>
    commit_png_bytes(std::string_view cache_key, const std::vector<std::uint8_t> &png_bytes) = 0;
};

} // namespace ravo
