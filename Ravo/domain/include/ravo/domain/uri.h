#pragma once

#include <string>
#include <string_view>

#include "ravo/domain/types.h"
#include "ravo/foundation/error.h"

namespace ravo
{

struct NormalizedLocation
{
    std::string path;
    std::string uri;
};

[[nodiscard]] Result<NormalizedLocation> normalize_local_input(std::string_view input);
[[nodiscard]] Result<FileIdentity> read_file_identity(std::string_view path);
[[nodiscard]] std::string uri_parent(std::string_view uri);
[[nodiscard]] std::string uri_display_name(std::string_view uri);

} // namespace ravo
