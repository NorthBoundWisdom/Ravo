#pragma once

#include <string_view>

#include <QString>

namespace ravo::raw_pipeline_internal
{
void configure_exiv2_diagnostics();
[[nodiscard]] QString local_path(std::string_view uri);
} // namespace ravo::raw_pipeline_internal
