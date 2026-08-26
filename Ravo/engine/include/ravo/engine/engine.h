#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/color.h"
#include "ravo/foundation/error.h"
#include "ravo/recipe/operation.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

enum class RenderBackend
{
    kCpu,
};

struct RenderRequest
{
    AssetDescriptor asset;
    Recipe recipe;
    std::string output_uri;
    std::optional<std::uint32_t> output_width;
    std::optional<std::uint32_t> output_height;
    std::uint64_t memory_budget_bytes = 0;
    std::uint32_t worker_count = 1;
    bool deterministic = true;
    RenderBackend backend = RenderBackend::kCpu;
    CancellationToken cancellation;
    std::string correlation_id;
};

struct RenderResult
{
    std::string correlation_id;
    std::string output_uri;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

struct RenderedImage
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgb;
    ColorProfileState color_profile;
};

struct RasterBuffer
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // Encoded RGB8 samples in color_profile, not necessarily sRGB.
    std::vector<std::uint8_t> srgb;
    ColorProfileState color_profile;
};

inline constexpr std::size_t kExposureRawHistogramBins = 1U << 16U;

enum class RawExposureMetadataStatus
{
    kUnavailable,
    kReady,
    kReadFailed,
};

struct RawExposureMetadata
{
    RawExposureMetadataStatus status = RawExposureMetadataStatus::kUnavailable;
    double exposure_bias_ev = 0.0;
    double highlight_preservation_ev = 0.0;
    std::string failure_detail;
};

struct ExposureAnalysisContext
{
    std::vector<std::uint32_t> raw_histogram;
    std::uint64_t raw_pixel_count = 0;
    std::uint32_t raw_black_level = 0;
    std::uint32_t raw_white_level = 0;
    RawExposureMetadata metadata;
};

struct LinearWorkingBuffer
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> rgb;
    ColorProfileState color_profile;
    // RAW-only analysis is immutable and may be shared by live-preview/cache copies.
    std::shared_ptr<const ExposureAnalysisContext> exposure_analysis;
};

inline constexpr std::uint32_t kRgbHistogramBins = 256;
inline constexpr std::uint32_t kWaveformTones = 160;
inline constexpr std::uint32_t kWaveformMaxBins = 360;

struct RgbHistogram
{
    std::array<std::uint32_t, kRgbHistogramBins> red{};
    std::array<std::uint32_t, kRgbHistogramBins> green{};
    std::array<std::uint32_t, kRgbHistogramBins> blue{};
    std::uint32_t max_count = 0;
};

struct RgbParade
{
    std::uint32_t bins = 0;
    std::uint32_t tones = kWaveformTones;
    std::vector<std::uint8_t> rgb;
};

[[nodiscard]] Result<RgbHistogram> collect_rgb_histogram(const RasterBuffer &raster);
[[nodiscard]] Result<RgbParade> collect_rgb_parade(const RasterBuffer &raster);

[[nodiscard]] inline int normalized_rotate_quarters(const int quarters) noexcept
{
    return ((quarters % 4) + 4) % 4;
}

inline void apply_display_rotation_to_size(std::uint32_t &width, std::uint32_t &height,
                                           const int rotate_quarters) noexcept
{
    if (normalized_rotate_quarters(rotate_quarters) % 2 != 0)
    {
        const auto swapped = width;
        width = height;
        height = swapped;
    }
}

struct DecodedRaw
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int rotate_quarters = 0;
    std::uint32_t cfa_width = 0;
    std::uint32_t cfa_height = 0;
    std::int32_t black_level = 0;
    std::uint32_t white_level = 65535;
    std::string make;
    std::string model;
    std::array<float, 4> as_shot_white_balance{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 4> camera_reference_white_balance{1.0F, 1.0F, 1.0F, 1.0F};
    bool has_as_shot_white_balance = false;
    bool has_camera_reference_white_balance = false;
    std::uint32_t exposure_deflicker_black_level = 0;
    std::uint32_t exposure_deflicker_white_level = 0;
    RawExposureMetadata exposure_metadata;
    ColorProfileState color_profile;
    std::vector<std::uint8_t> cfa_channels;
    std::vector<std::uint16_t> pixels;
};

struct InspectionResult
{
    std::string input_uri;
    std::string format;
    std::string make;
    std::string model;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool is_raw = false;
    std::optional<double> iso;
    std::optional<double> aperture;
    std::optional<double> focal_length_mm;
    std::optional<double> shutter_s;
    std::optional<std::int64_t> captured_unix_s;
};

struct EmbeddedPreview
{
    std::string mime_type;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    int rotate_quarters = 0;
    std::vector<std::uint8_t> bytes;
    ColorProfileState color_profile;
};

struct RawInspectPreview
{
    InspectionResult inspection;
    std::optional<EmbeddedPreview> embedded_preview;
};

struct ProgressEvent
{
    std::string correlation_id;
    std::string stage;
    std::uint64_t completed_units = 0;
    std::uint64_t total_units = 0;
};

class ProgressSink
{
public:
    virtual ~ProgressSink() = default;
    virtual void on_progress(const ProgressEvent &event) = 0;
};

class EngineFacade
{
public:
    [[nodiscard]] static Result<EngineFacade> create_phase1();

    // The facade does not retain input_uri or token after this synchronous call.
    [[nodiscard]] Result<InspectionResult> inspect(std::string_view input_uri,
                                                   const CancellationToken &cancellation) const;
    [[nodiscard]] Result<EmbeddedPreview>
    extract_embedded_preview(std::string_view input_uri, std::uint32_t max_edge,
                             const CancellationToken &cancellation) const;
    // One LibRaw open: metadata plus optional JPEG thumbnail. Missing thumbs are
    // not an inspection failure; `embedded_preview` stays empty.
    [[nodiscard]] Result<RawInspectPreview>
    inspect_with_embedded_preview(std::string_view input_uri, std::uint32_t max_edge,
                                  const CancellationToken &cancellation) const;
    [[nodiscard]] const std::vector<OperationDescriptor> &operations() const noexcept;
    [[nodiscard]] Result<Recipe> upgrade(Recipe recipe) const;
    [[nodiscard]] Result<void> validate(const Recipe &recipe) const;
    [[nodiscard]] Result<std::string> input_color_cache_fingerprint(const Recipe &recipe) const;
    [[nodiscard]] Result<std::string> output_color_cache_fingerprint(const Recipe &recipe) const;

    // The sink is borrowed only for the duration of this synchronous call.
    [[nodiscard]] Result<RenderResult> render(const RenderRequest &request,
                                              ProgressSink *progress_sink = nullptr) const;
    [[nodiscard]] Result<RenderedImage> render_to_image(const RenderRequest &request,
                                                        const RasterBuffer *raster = nullptr) const;
    [[nodiscard]] Result<DecodedRaw> decode_raw_frame(std::string_view input_uri,
                                                      const CancellationToken &cancellation) const;
    [[nodiscard]] Result<LinearWorkingBuffer>
    linear_working_from_raw(const DecodedRaw &raw, const Recipe &recipe, std::uint32_t width,
                            std::uint32_t height, const CancellationToken &cancellation) const;
    [[nodiscard]] Result<LinearWorkingBuffer>
    linear_working_from_raster(const RasterBuffer &raster, const Recipe &recipe,
                               const CancellationToken &cancellation) const;
    // Applies RGB recipe ops onto a copy of `working`. Callers that already baked
    // `ravo.raw.highlights` into the buffer must disable that operation before calling.
    [[nodiscard]] Result<RenderedImage>
    render_linear_working(const LinearWorkingBuffer &working, const Recipe &recipe,
                          const CancellationToken &cancellation) const;
    [[nodiscard]] Result<std::vector<std::uint8_t>> encode_png(const RenderedImage &image) const;

private:
    explicit EngineFacade(OperationRegistry registry);

    OperationRegistry registry_;
};

} // namespace ravo
