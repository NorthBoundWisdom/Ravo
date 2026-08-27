#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

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

[[nodiscard]] Result<InspectionResult> identify_raw(std::string_view input_uri);
[[nodiscard]] Result<EmbeddedPreview> extract_libraw_preview(std::string_view input_uri,
                                                             std::uint32_t max_edge,
                                                             const CancellationToken &cancellation);
[[nodiscard]] Result<RawInspectPreview>
inspect_raw_with_embedded_preview(std::string_view input_uri, std::uint32_t max_edge,
                                  const CancellationToken &cancellation);
[[nodiscard]] Result<RawExposureMetadata>
resolve_legacy_exposure_metadata(const LegacyExposureMetadataTags &tags);
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
