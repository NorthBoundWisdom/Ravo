#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/color.h"
#include "ravo/foundation/error.h"
#include "ravo/recipe/operation.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

// Immutable scale metadata for a linear-working image.  It is the current
// working-pixel density divided by the original input-pixel density; it is not
// a UI value or an operation parameter.  An invalid value explicitly means
// that the creating boundary could not prove a proportional source geometry.
// Consumers which require scale must reject that state rather than guessing
// from the current dimensions.
class CanonicalRoiScale
{
public:
    constexpr CanonicalRoiScale() noexcept = default;

    [[nodiscard]] static CanonicalRoiScale
    from_scaled_dimensions(std::uint32_t current_width, std::uint32_t current_height,
                           std::uint32_t original_width, std::uint32_t original_height) noexcept
    {
        if (current_width == 0U || current_height == 0U || original_width == 0U ||
            original_height == 0U)
        {
            return {};
        }

        const bool landscape_or_square = original_width >= original_height;
        const std::uint32_t original_long = landscape_or_square ? original_width : original_height;
        const std::uint32_t original_short = landscape_or_square ? original_height : original_width;
        const std::uint32_t current_long = landscape_or_square ? current_width : current_height;
        const std::uint32_t current_short = landscape_or_square ? current_height : current_width;
        const auto expected_short = static_cast<std::uint32_t>(std::max<std::uint64_t>(
            1U, static_cast<std::uint64_t>(original_short) * current_long / original_long));
        if (current_short != expected_short)
        {
            return {};
        }

        const float scale = static_cast<float>(current_long) / static_cast<float>(original_long);
        return std::isfinite(scale) && scale > 0.0F ? CanonicalRoiScale(scale) :
                                                      CanonicalRoiScale{};
    }

    [[nodiscard]] bool valid() const noexcept
    {
        return std::isfinite(value_) && value_ > 0.0F;
    }

    [[nodiscard]] float value() const noexcept
    {
        return value_;
    }

private:
    explicit constexpr CanonicalRoiScale(const float value) noexcept
        : value_(value)
    {
    }

    float value_ = std::numeric_limits<float>::quiet_NaN();
};

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
    // Preview-only. Export callers must leave this empty.
    std::optional<std::string> overlay_mask_id;
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
    std::vector<float> mask_alpha;
};

enum class RenderSampleKind : std::uint8_t
{
    kRgb8 = 0,
    kRgb16 = 1,
    kRgbFloat = 2,
};

// Tagged export pixels. Exactly one alternative is valid. Preview/cache keep
// RenderedImage so high precision never enters QML or browse PNG paths.
struct RenderedExportImage
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    ColorProfileState color_profile;
    std::variant<std::vector<std::uint8_t>, std::vector<std::uint16_t>, std::vector<float>> samples;
};

[[nodiscard]] constexpr std::size_t
render_sample_bytes_per_pixel(const RenderSampleKind kind) noexcept
{
    switch (kind)
    {
    case RenderSampleKind::kRgb8:
        return 3U;
    case RenderSampleKind::kRgb16:
        return 6U;
    case RenderSampleKind::kRgbFloat:
        return 12U;
    }
    return 0U;
}

struct RasterBuffer
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // Oriented dimensions before the raster decoder's optional proportional
    // preview scaling.  Zero is explicit unknown geometry, never full size.
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
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
    // Assigned once at the RAW/raster creation boundary and propagated exactly
    // by every operation which creates a new working buffer.
    CanonicalRoiScale canonical_roi_scale{};
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

struct EngineCaptureDateTime
{
    std::string local_exif;
    std::optional<std::string> subsecond_digits;
    std::optional<std::int32_t> utc_offset_minutes;

    [[nodiscard]] bool operator==(const EngineCaptureDateTime &) const noexcept = default;
};

enum class EngineCaptureAltitudeReference : std::uint8_t
{
    kAboveSeaLevel = 0,
    kBelowSeaLevel = 1,
};

struct EngineCaptureAltitude
{
    std::uint32_t magnitude_mm = 0;
    EngineCaptureAltitudeReference reference = EngineCaptureAltitudeReference::kAboveSeaLevel;

    [[nodiscard]] bool operator==(const EngineCaptureAltitude &) const noexcept = default;
};

struct EngineCaptureLocation
{
    std::int32_t latitude_e6 = 0;
    std::int32_t longitude_e6 = 0;
    std::optional<EngineCaptureAltitude> altitude;

    [[nodiscard]] bool operator==(const EngineCaptureLocation &) const noexcept = default;
};

struct EngineCaptureMetadata
{
    std::optional<EngineCaptureDateTime> captured_datetime;
    std::optional<EngineCaptureLocation> location;

    [[nodiscard]] bool operator==(const EngineCaptureMetadata &) const noexcept = default;
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
                          const CancellationToken &cancellation,
                          std::optional<std::string> overlay_mask_id = {}) const;
    // Same recipe/output-colour stage as render_linear_working; packs the owned
    // ProfiledOutputBuffer to the requested sample kind. Preview callers stay on
    // the RGB8 APIs above.
    [[nodiscard]] Result<RenderedExportImage>
    render_linear_working_export(const LinearWorkingBuffer &working, const Recipe &recipe,
                                 RenderSampleKind sample_kind,
                                 const CancellationToken &cancellation) const;
    [[nodiscard]] Result<RenderedExportImage>
    render_to_export_image(const RenderRequest &request, RenderSampleKind sample_kind,
                           const RasterBuffer *raster = nullptr) const;
    [[nodiscard]] Result<std::vector<std::uint8_t>> encode_png(const RenderedImage &image) const;
    [[nodiscard]] Result<void>
    composite_preview_mask_overlay(std::vector<std::uint8_t> &rgb, std::uint32_t width,
                                   std::uint32_t height, const std::vector<float> &alpha,
                                   const CancellationToken &cancellation) const;
    // Engine-private Exiv2: embedded Exif only. No sidecar, XMP, or maker-note copy.
    // Returns one owned semantic capture value. Exiv2 types die inside the call.
    [[nodiscard]] Result<EngineCaptureMetadata>
    read_embedded_capture_metadata(std::string_view input_uri,
                                   const CancellationToken &cancellation) const;

private:
    explicit EngineFacade(OperationRegistry registry);

    OperationRegistry registry_;
};

} // namespace ravo
