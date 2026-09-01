#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "ravo/services/catalog_service.h"

namespace ravo::catalog_service_internal
{

[[nodiscard]] bool media_type_has_embedded_capture(std::string_view media_type) noexcept;
[[nodiscard]] bool is_common_raster_media(std::string_view media_type) noexcept;
void merge_engine_capture(CaptureMetadata &target, const EngineCaptureMetadata &source);
[[nodiscard]] bool should_try_raw_after_raster(const TaskError &error) noexcept;
[[nodiscard]] std::string utf8_string(const std::u8string &value);
[[nodiscard]] TaskError annotate_batch_export_error(TaskError error, std::size_t completed_count,
                                                    std::size_t total_count,
                                                    std::size_t failed_index,
                                                    std::string_view asset_id,
                                                    std::string_view output);

} // namespace ravo::catalog_service_internal
