#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/engine/engine.h"

namespace ravo
{

struct LegacyExposureMetadataTags
{
    std::optional<double> photo_exposure_bias_ev;
    std::optional<double> image_exposure_bias_ev;
    std::optional<std::vector<std::int64_t>> canon_lighting_opt;
    std::optional<std::vector<std::int64_t>> canon_auto_lighting_optimizer;
    std::optional<std::int64_t> fuji_development_dynamic_range;
    std::optional<std::int64_t> fuji_auto_dynamic_range;
    std::optional<std::int64_t> nikon_color_space;
    std::optional<std::int64_t> nikon_active_d_lighting;
    std::optional<std::vector<std::int64_t>> olympus_camera_settings_gradation;
    std::optional<std::vector<std::int64_t>> olympus_raw_development_gradation;
    std::optional<std::vector<std::uint8_t>> pentax_dynamic_range_expansion;
};

struct RawDefaultCropMetadata
{
    std::string ifd_group;
    std::optional<std::vector<std::int64_t>> origin;
    std::optional<std::vector<std::int64_t>> size;
};

struct RawDecodeRegion
{
    std::uint32_t left = 0;
    std::uint32_t top = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t relative_left = 0;
    std::uint32_t relative_top = 0;
};

[[nodiscard]] Result<InspectionResult> identify_raw(std::string_view input_uri);
[[nodiscard]] Result<EmbeddedPreview> extract_libraw_preview(std::string_view input_uri,
                                                             std::uint32_t max_edge,
                                                             const CancellationToken &cancellation);
[[nodiscard]] Result<RawInspectPreview>
inspect_raw_with_embedded_preview(std::string_view input_uri, std::uint32_t max_edge,
                                  const CancellationToken &cancellation);
[[nodiscard]] Result<RawExposureMetadata>
resolve_legacy_exposure_metadata(const LegacyExposureMetadataTags &tags);
[[nodiscard]] Result<RawDecodeRegion>
resolve_raw_decode_region(const std::optional<RawDefaultCropMetadata> &metadata,
                          std::uint32_t libraw_left, std::uint32_t libraw_top,
                          std::uint32_t libraw_width, std::uint32_t libraw_height,
                          std::uint32_t raw_width, std::uint32_t raw_height);
[[nodiscard]] Result<DecodedRaw> decode_raw(std::string_view input_uri,
                                            const CancellationToken &cancellation);
[[nodiscard]] Result<std::shared_ptr<const ExposureAnalysisContext>>
build_exposure_analysis_context(const DecodedRaw &raw, const CancellationToken &cancellation);
[[nodiscard]] std::uint64_t
estimate_raw_render_memory(const DecodedRaw &raw, const Recipe &recipe, std::uint32_t width,
                           std::uint32_t height, std::size_t output_bytes_per_pixel = 3U) noexcept;
[[nodiscard]] Result<void> write_png_atomically(std::string_view output_uri,
                                                const RenderedImage &image);
[[nodiscard]] Result<EngineCaptureMetadata>
read_embedded_capture_metadata(std::string_view input_uri, const CancellationToken &cancellation);

} // namespace ravo
