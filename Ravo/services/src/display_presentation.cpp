#include "ravo/services/display_presentation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <lcms2.h>

#if defined(__APPLE__)
#include <CoreGraphics/CoreGraphics.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ravo
{
namespace
{

constexpr std::size_t kMaxIccBytes = 8U * 1024U * 1024U;

// Apple CI's libc++ does not expose jthread. Own the same bounded worker set
// explicitly: partial thread creation must join before any captured ICC state
// or output buffer can be destroyed. No thread is detached or reused.
struct DisplayConversionWorkers final
{
    std::array<std::thread, 7> threads;

    ~DisplayConversionWorkers()
    {
        for (auto &thread : threads)
            if (thread.joinable())
                thread.join();
    }
};

class CmsContext final
{
public:
    CmsContext()
        : handle_(cmsCreateContext(nullptr, nullptr))
    {
    }
    ~CmsContext()
    {
        if (handle_ != nullptr)
        {
            cmsDeleteContext(handle_);
        }
    }
    CmsContext(const CmsContext &) = delete;
    CmsContext &operator=(const CmsContext &) = delete;
    [[nodiscard]] cmsContext get() const noexcept
    {
        return handle_;
    }

private:
    cmsContext handle_ = nullptr;
};

class CmsProfile final
{
public:
    CmsProfile() = default;
    explicit CmsProfile(cmsHPROFILE handle)
        : handle_(handle)
    {
    }
    ~CmsProfile()
    {
        if (handle_ != nullptr)
        {
            cmsCloseProfile(handle_);
        }
    }
    CmsProfile(const CmsProfile &) = delete;
    CmsProfile &operator=(const CmsProfile &) = delete;
    CmsProfile(CmsProfile &&other) noexcept
        : handle_(other.handle_)
    {
        other.handle_ = nullptr;
    }
    CmsProfile &operator=(CmsProfile &&other) noexcept
    {
        if (this != &other)
        {
            if (handle_ != nullptr)
            {
                cmsCloseProfile(handle_);
            }
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }
    [[nodiscard]] cmsHPROFILE get() const noexcept
    {
        return handle_;
    }

private:
    cmsHPROFILE handle_ = nullptr;
};

class CmsTransform final
{
public:
    explicit CmsTransform(cmsHTRANSFORM handle)
        : handle_(handle)
    {
    }
    ~CmsTransform()
    {
        if (handle_ != nullptr)
        {
            cmsDeleteTransform(handle_);
        }
    }
    CmsTransform(const CmsTransform &) = delete;
    CmsTransform &operator=(const CmsTransform &) = delete;
    [[nodiscard]] cmsHTRANSFORM get() const noexcept
    {
        return handle_;
    }

private:
    cmsHTRANSFORM handle_ = nullptr;
};

[[nodiscard]] Result<ColorProfileState> make_srgb_profile_state()
{
    CmsContext context;
    if (context.get() == nullptr)
    {
        return make_error(ErrorCode::kInternal, "LittleCMS could not allocate an sRGB context");
    }
    CmsProfile profile(cmsCreate_sRGBProfileTHR(context.get()));
    if (profile.get() == nullptr)
    {
        return make_error(ErrorCode::kInternal, "LittleCMS could not create an sRGB profile");
    }
    cmsUInt32Number bytes_needed = 0;
    if (!cmsSaveProfileToMem(profile.get(), nullptr, &bytes_needed) || bytes_needed == 0 ||
        bytes_needed > kMaxIccBytes)
    {
        return make_error(ErrorCode::kInternal, "LittleCMS could not size an sRGB ICC blob");
    }
    ColorProfileState state;
    state.kind = ColorProfileKind::kBuiltin;
    state.model = ColorModel::kRgb;
    state.identifier = "srgb";
    state.icc_bytes.resize(bytes_needed);
    cmsUInt32Number written = bytes_needed;
    if (!cmsSaveProfileToMem(profile.get(), state.icc_bytes.data(), &written) || written == 0)
    {
        return make_error(ErrorCode::kInternal, "LittleCMS could not export an sRGB ICC blob");
    }
    state.icc_bytes.resize(written);
    return state;
}

[[nodiscard]] Result<ColorProfileState> profile_from_icc_bytes(std::vector<std::uint8_t> bytes,
                                                               std::string identifier)
{
    if (bytes.empty() || bytes.size() > kMaxIccBytes)
    {
        return make_error(ErrorCode::kValidation, "Monitor ICC bytes are empty or oversized",
                          {{"reason", "invalid_monitor_icc_bytes"}});
    }
    CmsProfile parsed(
        cmsOpenProfileFromMem(bytes.data(), static_cast<cmsUInt32Number>(bytes.size())));
    if (parsed.get() == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Monitor ICC profile is corrupt or unsupported",
                          {{"reason", "corrupt_monitor_icc"}});
    }
    const auto color_space = cmsGetColorSpace(parsed.get());
    if (color_space != cmsSigRgbData)
    {
        return make_error(ErrorCode::kUnsupported, "Monitor ICC must be an RGB profile",
                          {{"reason", "non_rgb_monitor_icc"}});
    }
    ColorProfileState state;
    state.kind = ColorProfileKind::kIcc;
    state.model = ColorModel::kRgb;
    state.identifier = std::move(identifier);
    state.icc_bytes = std::move(bytes);
    return state;
}

[[nodiscard]] DisplayPresentationState
fallback_srgb_state(std::string screen_token, std::string reason, ColorProfileState profile)
{
    DisplayPresentationState state;
    state.screen_token = std::move(screen_token);
    state.source = DisplayProfileSource::kFallbackSrgb;
    state.reason = std::move(reason);
    state.monitor_profile = std::move(profile);
    state.profile_fingerprint = color_profile_fingerprint(state.monitor_profile);
    state.valid = true;
    return state;
}

[[nodiscard]] Result<DisplayPresentationState> finalize_state(std::string screen_token,
                                                              DisplayProfileSource source,
                                                              std::string reason,
                                                              ColorProfileState profile)
{
    DisplayPresentationState state;
    state.screen_token = std::move(screen_token);
    state.source = source;
    state.reason = std::move(reason);
    state.monitor_profile = std::move(profile);
    state.profile_fingerprint = color_profile_fingerprint(state.monitor_profile);
    state.valid = !state.profile_fingerprint.empty();
    if (!state.valid)
    {
        return make_error(ErrorCode::kInternal, "Display presentation fingerprint is empty");
    }
    return state;
}

#if defined(__APPLE__)
[[nodiscard]] Result<std::vector<std::uint8_t>>
copy_display_icc_bytes(const CGDirectDisplayID display)
{
    CGColorSpaceRef space = CGDisplayCopyColorSpace(display);
    if (space == nullptr)
    {
        return make_error(ErrorCode::kNotFound, "macOS display colour space is unavailable",
                          {{"reason", "macos_display_colorspace_missing"}});
    }
    CFDataRef data = CGColorSpaceCopyICCData(space);
    CGColorSpaceRelease(space);
    if (data == nullptr)
    {
        return make_error(ErrorCode::kNotFound, "macOS display ICC data is unavailable",
                          {{"reason", "macos_display_icc_missing"}});
    }
    const auto length = static_cast<std::size_t>(CFDataGetLength(data));
    if (length == 0 || length > kMaxIccBytes)
    {
        CFRelease(data);
        return make_error(ErrorCode::kValidation, "macOS display ICC data is empty or oversized",
                          {{"reason", "macos_display_icc_invalid_size"}});
    }
    std::vector<std::uint8_t> bytes(length);
    std::memcpy(bytes.data(), CFDataGetBytePtr(data), length);
    CFRelease(data);
    return bytes;
}

[[nodiscard]] CGDirectDisplayID resolve_macos_display_id(const std::string_view screen_token)
{
    if (const auto parsed = parse_macos_cg_screen_token(screen_token))
    {
        return static_cast<CGDirectDisplayID>(*parsed);
    }
    return CGMainDisplayID();
}
#endif

[[nodiscard]] Result<CmsProfile> open_profile(const ColorProfileState &state, CmsContext &context)
{
    if (state.icc_bytes.empty() || state.icc_bytes.size() > kMaxIccBytes)
    {
        return make_error(ErrorCode::kValidation,
                          "Colour profile ICC bytes are missing or exceed the presentation limit",
                          {{"identifier", state.identifier}});
    }
    CmsProfile profile(
        cmsOpenProfileFromMemTHR(context.get(), state.icc_bytes.data(),
                                 static_cast<cmsUInt32Number>(state.icc_bytes.size())));
    if (profile.get() == nullptr)
    {
        return make_error(ErrorCode::kValidation, "LittleCMS could not open a presentation profile",
                          {{"identifier", state.identifier}});
    }
    return profile;
}

[[nodiscard]] Result<DisplayPresentationRgb8>
transform_icc_rgb8(const std::vector<std::uint8_t> &source_rgb8, std::uint32_t width,
                   std::uint32_t height, const ColorProfileState &source_profile,
                   const ColorProfileState &monitor_profile, const CancellationToken &cancellation,
                   DisplayConversionObserver *observer)
try
{
    const std::size_t expected =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U;
    if (width == 0 || height == 0 || source_rgb8.size() != expected)
    {
        return make_error(ErrorCode::kValidation,
                          "Display presentation RGB8 dimensions are invalid");
    }
    CmsContext context;
    if (context.get() == nullptr)
    {
        return make_error(ErrorCode::kInternal,
                          "LittleCMS could not allocate a presentation context");
    }
    auto source = open_profile(source_profile, context);
    if (!source)
    {
        return source.error();
    }
    auto dest = open_profile(monitor_profile, context);
    if (!dest)
    {
        return dest.error();
    }
    CmsTransform transform(cmsCreateTransformTHR(
        context.get(), source.value().get(), TYPE_RGB_8, dest.value().get(), TYPE_RGB_8,
        INTENT_RELATIVE_COLORIMETRIC,
        cmsFLAGS_NOCACHE | cmsFLAGS_NOOPTIMIZE | cmsFLAGS_BLACKPOINTCOMPENSATION));
    if (transform.get() == nullptr)
    {
        return make_error(ErrorCode::kUnsupported,
                          "LittleCMS could not create a monitor presentation transform");
    }
    DisplayPresentationRgb8 output;
    output.width = width;
    output.height = height;
    output.rgb8.resize(expected);
    output.color_profile = monitor_profile;
    if (auto active = cancellation.check(); !active)
        return active.error();
    // NOCACHE makes the transform read-only during evaluation. Each worker owns
    // disjoint complete rows; retain NOOPTIMIZE and the exact RGB8 evaluator.
    // Small thumbnails stay serial. Scope-owned workers always join before the
    // transform, profiles or output are destroyed, including thread-start errors.
    const auto workers =
        expected < 512U * 1024U ?
            1U :
            std::min({8U, std::max(1U, std::thread::hardware_concurrency()), height});
    const auto rows = [&](const unsigned worker) noexcept
    {
        struct Observation
        {
            DisplayConversionObserver *sink;
            ~Observation()
            {
                if (sink)
                    sink->worker_stopped();
            }
        } observation{observer};
        if (observer)
            observer->worker_started(worker != 0);
        for (std::uint32_t y = worker; y < height; y += workers)
        {
            if (cancellation.is_cancellation_requested())
                return;
            const std::size_t row = static_cast<std::size_t>(y) * width * 3U;
            cmsDoTransform(transform.get(), source_rgb8.data() + row, output.rgb8.data() + row,
                           width);
        }
    };
    try
    {
        DisplayConversionWorkers owned;
        for (unsigned worker = 1U; worker < workers; ++worker)
            owned.threads[worker - 1U] = std::thread(rows, worker);
        rows(0U);
    }
    catch (const std::system_error &error)
    {
        return make_error(
            ErrorCode::kIo, "Unable to start display conversion worker",
            {{"reason", "display_worker_start_failed"}, {"detail", error.code().message()}});
    }
    if (auto active = cancellation.check(); !active)
        return active.error();
    return output;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Display conversion allocation failed",
                      {{"reason", "allocation_failed"}});
}

[[nodiscard]] Result<DisplayPresentationRgb8>
apply_synthetic_matrix(const std::vector<std::uint8_t> &source_rgb8, std::uint32_t width,
                       std::uint32_t height, const ColorProfileState &monitor_profile,
                       const CancellationToken &cancellation)
{
    const std::size_t expected =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U;
    if (width == 0 || height == 0 || source_rgb8.size() != expected || !monitor_profile.has_matrix)
    {
        return make_error(ErrorCode::kValidation, "Synthetic matrix presentation state is invalid");
    }
    DisplayPresentationRgb8 output;
    output.width = width;
    output.height = height;
    output.rgb8.resize(expected);
    output.color_profile = monitor_profile;
    const auto &m = monitor_profile.matrix_to_xyz_d50;
    for (std::uint32_t y = 0; y < height; ++y)
    {
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 3U;
            const float r = static_cast<float>(source_rgb8[offset]) / 255.0F;
            const float g = static_cast<float>(source_rgb8[offset + 1U]) / 255.0F;
            const float b = static_cast<float>(source_rgb8[offset + 2U]) / 255.0F;
            const float out_r = m[0] * r + m[1] * g + m[2] * b;
            const float out_g = m[3] * r + m[4] * g + m[5] * b;
            const float out_b = m[6] * r + m[7] * g + m[8] * b;
            const auto encode = [](float value) -> std::uint8_t
            {
                if (!std::isfinite(value))
                {
                    return 0U;
                }
                value = std::clamp(value, 0.0F, 1.0F);
                return static_cast<std::uint8_t>(std::lround(value * 255.0F));
            };
            output.rgb8[offset] = encode(out_r);
            output.rgb8[offset + 1U] = encode(out_g);
            output.rgb8[offset + 2U] = encode(out_b);
        }
    }
    return output;
}

} // namespace

struct DisplayPresentationCache::Impl
{
    struct Entry
    {
        ColorProfileState source;
        ColorProfileState monitor;
        std::vector<std::uint32_t> pixels;
    };
    mutable std::mutex mutex;
    std::mutex preparation_mutex;
    std::uint64_t generation = 0;
    std::shared_ptr<const Entry> entry;
};

DisplayPresentationCache::DisplayPresentationCache() noexcept = default;

DisplayPresentationCache::~DisplayPresentationCache() = default;

void DisplayPresentationCache::clear()
{
    const std::lock_guard initialization(impl_mutex_);
    if (!impl_)
        return;
    const std::lock_guard lock(impl_->mutex);
    ++impl_->generation;
    impl_->entry.reset();
}

std::size_t DisplayPresentationCache::owned_bytes() const
{
    const std::lock_guard initialization(impl_mutex_);
    if (!impl_)
        return 0U;
    const std::lock_guard lock(impl_->mutex);
    return impl_->entry ? impl_->entry->pixels.size() * sizeof(std::uint32_t) : 0U;
}

Result<void> DisplayPresentationCache::prepare(const ColorProfileState &source_profile,
                                               const DisplayPresentationState &presentation,
                                               const CancellationToken &cancellation,
                                               DisplayConversionObserver *observer)
try
{
    if (auto active = cancellation.check(); !active)
        return active.error();
    if (!presentation.valid)
        return make_error(ErrorCode::kValidation, "Display presentation state is not valid");
    if (presentation.source == DisplayProfileSource::kSyntheticMatrix ||
        presentation.source == DisplayProfileSource::kSyntheticLut)
        return {};
    std::uint64_t generation;
    {
        const std::lock_guard initialization(impl_mutex_);
        // Lazy allocation stays inside this Result boundary. Construction and
        // cache misses do not allocate an implementation or hide allocation errors.
        if (!impl_)
            impl_ = std::make_unique<Impl>();
        const std::lock_guard lock(impl_->mutex);
        generation = impl_->generation;
    }
    auto source = source_profile.icc_bytes.empty() ? make_srgb_color_profile() :
                                                     Result<ColorProfileState>{source_profile};
    if (!source)
        return source.error();
    if (source.value() == presentation.monitor_profile)
        return {};
    const std::lock_guard preparation_lock(impl_->preparation_mutex);
    {
        const std::lock_guard lock(impl_->mutex);
        if (generation != impl_->generation)
            return make_error(ErrorCode::kCancelled,
                              "Display conversion preparation was superseded");
        if (impl_->entry && impl_->entry->source == source.value() &&
            impl_->entry->monitor == presentation.monitor_profile)
            return cancellation.check();
    }
    CmsContext context;
    if (context.get() == nullptr)
        return make_error(ErrorCode::kInternal,
                          "LittleCMS could not allocate a presentation context");
    auto from = open_profile(source.value(), context);
    if (!from)
        return from.error();
    auto to = open_profile(presentation.monitor_profile, context);
    if (!to)
        return to.error();
    CmsTransform transform(cmsCreateTransformTHR(
        context.get(), from.value().get(), TYPE_RGB_8, to.value().get(), TYPE_RGB_8,
        INTENT_RELATIVE_COLORIMETRIC,
        cmsFLAGS_NOCACHE | cmsFLAGS_NOOPTIMIZE | cmsFLAGS_BLACKPOINTCOMPENSATION));
    if (transform.get() == nullptr)
        return make_error(ErrorCode::kUnsupported,
                          "LittleCMS could not create a monitor presentation transform");
    constexpr std::uint32_t colors = 1U << 24U;
    constexpr std::uint32_t batch = 4096U;
    const auto workers = std::min(8U, std::max(1U, std::thread::hardware_concurrency()));
    auto entry = std::make_shared<Impl::Entry>();
    entry->source = std::move(source).value();
    entry->monitor = presentation.monitor_profile;
    entry->pixels.resize(colors);
    // All allocations precede thread startup; worker bodies cannot throw.
    std::array<std::vector<std::uint8_t>, 8> input, output;
    for (unsigned k = 0; k < workers; ++k)
    {
        input[k].resize(batch * 3U);
        output[k].resize(batch * 3U);
    }
    const auto fill = [&](unsigned k) noexcept
    {
        struct Observation
        {
            DisplayConversionObserver *sink;
            ~Observation()
            {
                if (sink)
                    sink->worker_stopped();
            }
        } observation{observer};
        if (observer)
            observer->worker_started(k != 0U);
        for (std::uint32_t base = k * batch; base < colors; base += workers * batch)
        {
            if (cancellation.is_cancellation_requested())
                return;
            // clear()/profile changes invalidate preparation without waiting for
            // a full enumeration. Hold the metadata lock for this check only.
            {
                const std::lock_guard lock(impl_->mutex);
                if (generation != impl_->generation)
                    return;
            }
            auto &in = input[k];
            auto &out = output[k];
            for (std::uint32_t i = 0; i < batch; ++i)
            {
                const auto rgb = base + i;
                in[i * 3U] = static_cast<std::uint8_t>(rgb >> 16U);
                in[i * 3U + 1U] = static_cast<std::uint8_t>(rgb >> 8U);
                in[i * 3U + 2U] = static_cast<std::uint8_t>(rgb);
            }
            cmsDoTransform(transform.get(), in.data(), out.data(), batch);
            for (std::uint32_t i = 0; i < batch; ++i)
                entry->pixels[base + i] = static_cast<std::uint32_t>(out[i * 3U]) |
                                          (static_cast<std::uint32_t>(out[i * 3U + 1U]) << 8U) |
                                          (static_cast<std::uint32_t>(out[i * 3U + 2U]) << 16U);
        }
    };
    {
        DisplayConversionWorkers owned;
        for (unsigned k = 1; k < workers; ++k)
            owned.threads[k - 1U] = std::thread(fill, k);
        fill(0U);
    }
    if (auto active = cancellation.check(); !active)
        return active.error();
    const std::lock_guard lock(impl_->mutex);
    if (generation != impl_->generation)
        return make_error(ErrorCode::kCancelled, "Display conversion preparation was superseded");
    impl_->entry = std::move(entry);
    return {};
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Display conversion cache allocation failed",
                      {{"reason", "allocation_failed"}});
}
catch (const std::system_error &error)
{
    return make_error(
        ErrorCode::kIo, "Unable to prepare display conversion cache",
        {{"reason", "display_cache_worker_failed"}, {"detail", error.code().message()}});
}

Result<std::optional<DisplayPresentationRgb8>> DisplayPresentationCache::apply_if_prepared(
    const std::vector<std::uint8_t> &source_rgb8, std::uint32_t width, std::uint32_t height,
    const ColorProfileState &source_profile, const DisplayPresentationState &presentation,
    const CancellationToken &cancellation, DisplayConversionObserver *observer) const
try
{
    if (auto active = cancellation.check(); !active)
        return active.error();
    if (!presentation.valid || width == 0U || height == 0U ||
        width > std::numeric_limits<std::size_t>::max() / 3U / height ||
        source_rgb8.size() != static_cast<std::size_t>(width) * height * 3U)
        return make_error(ErrorCode::kValidation, "Display presentation RGB8 buffer is invalid");
    std::shared_ptr<const Impl::Entry> entry;
    {
        const std::lock_guard initialization(impl_mutex_);
        if (!impl_)
            return std::optional<DisplayPresentationRgb8>{};
        const std::lock_guard lock(impl_->mutex);
        entry = impl_->entry;
    }
    if (!entry)
        return std::optional<DisplayPresentationRgb8>{};
    auto source = source_profile.icc_bytes.empty() ? make_srgb_color_profile() :
                                                     Result<ColorProfileState>{source_profile};
    if (!source)
        return source.error();
    if (entry->source != source.value() || entry->monitor != presentation.monitor_profile ||
        presentation.source == DisplayProfileSource::kSyntheticMatrix ||
        presentation.source == DisplayProfileSource::kSyntheticLut)
        return std::optional<DisplayPresentationRgb8>{};
    DisplayPresentationRgb8 result;
    result.width = width;
    result.height = height;
    result.color_profile = presentation.monitor_profile;
    result.rgb8.resize(source_rgb8.size());
    const auto workers =
        source_rgb8.size() < 512U * 1024U ?
            1U :
            std::min({8U, std::max(1U, std::thread::hardware_concurrency()), height});
    const auto rows = [&](unsigned worker) noexcept
    {
        struct Observation
        {
            DisplayConversionObserver *sink;
            ~Observation()
            {
                if (sink)
                    sink->worker_stopped();
            }
        } observation{observer};
        if (observer)
            observer->worker_started(worker != 0U);
        for (std::uint32_t y = worker; y < height; y += workers)
        {
            if (cancellation.is_cancellation_requested())
                return;
            const auto row_bytes = static_cast<std::size_t>(width) * 3U;
            const auto start = static_cast<std::size_t>(y) * row_bytes;
            for (std::size_t i = start; i < start + row_bytes; i += 3U)
            {
                const auto index = (static_cast<std::uint32_t>(source_rgb8[i]) << 16U) |
                                   (static_cast<std::uint32_t>(source_rgb8[i + 1U]) << 8U) |
                                   source_rgb8[i + 2U];
                const auto pixel = entry->pixels[index];
                result.rgb8[i] = static_cast<std::uint8_t>(pixel);
                result.rgb8[i + 1U] = static_cast<std::uint8_t>(pixel >> 8U);
                result.rgb8[i + 2U] = static_cast<std::uint8_t>(pixel >> 16U);
            }
        }
    };
    {
        DisplayConversionWorkers owned;
        for (unsigned worker = 1U; worker < workers; ++worker)
            owned.threads[worker - 1U] = std::thread(rows, worker);
        rows(0U);
    }
    if (auto active = cancellation.check(); !active)
        return active.error();
    return std::optional<DisplayPresentationRgb8>{std::move(result)};
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Display conversion allocation failed",
                      {{"reason", "allocation_failed"}});
}

catch (const std::system_error &error)
{
    return make_error(
        ErrorCode::kIo, "Unable to start display cache lookup worker",
        {{"reason", "display_worker_start_failed"}, {"detail", error.code().message()}});
}

Result<ColorProfileState> make_srgb_color_profile()
{
    static const Result<ColorProfileState> profile = make_srgb_profile_state();
    return profile;
}

std::string make_macos_cg_screen_token(const std::uint32_t display_id)
{
    return "cg:" + std::to_string(display_id);
}

std::optional<std::uint32_t> parse_macos_cg_screen_token(const std::string_view screen_token)
{
    constexpr std::string_view kPrefix = "cg:";
    if (!screen_token.starts_with(kPrefix))
    {
        return std::nullopt;
    }
    const auto digits = screen_token.substr(kPrefix.size());
    if (digits.empty())
    {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char ch : digits)
    {
        if (ch < '0' || ch > '9')
        {
            return std::nullopt;
        }
        value = value * 10U + static_cast<std::uint64_t>(ch - '0');
        if (value > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()))
        {
            return std::nullopt;
        }
    }
    return static_cast<std::uint32_t>(value);
}

std::string macos_display_screen_token_for_point(const double global_x, const double global_y)
{
#if defined(__APPLE__)
    const CGPoint point = CGPointMake(global_x, global_y);
    CGDirectDisplayID matched = kCGNullDirectDisplay;
    uint32_t count = 0;
    if (CGGetDisplaysWithPoint(point, 1, &matched, &count) == kCGErrorSuccess && count > 0 &&
        matched != kCGNullDirectDisplay)
    {
        return make_macos_cg_screen_token(static_cast<std::uint32_t>(matched));
    }
    return make_macos_cg_screen_token(static_cast<std::uint32_t>(CGMainDisplayID()));
#else
    static_cast<void>(global_x);
    static_cast<void>(global_y);
    return "primary";
#endif
}

Result<DisplayPresentationState> discover_monitor_presentation(const std::string_view screen_token)
{
    auto srgb = make_srgb_profile_state();
    if (!srgb)
    {
        return srgb.error();
    }
#if defined(__APPLE__)
    const CGDirectDisplayID display = resolve_macos_display_id(screen_token);
    const std::string resolved_token =
        parse_macos_cg_screen_token(screen_token) ?
            std::string(screen_token) :
            make_macos_cg_screen_token(static_cast<std::uint32_t>(display));
    auto bytes = copy_display_icc_bytes(display);
    if (!bytes)
    {
        return fallback_srgb_state(resolved_token,
                                   bytes.error().context.count("reason") ?
                                       bytes.error().context.at("reason") :
                                       "macos_display_icc_unavailable",
                                   std::move(srgb).value());
    }
    auto profile =
        profile_from_icc_bytes(std::move(bytes).value(), "macos.display." + resolved_token);
    if (!profile)
    {
        return fallback_srgb_state(resolved_token,
                                   profile.error().context.count("reason") ?
                                       profile.error().context.at("reason") :
                                       "corrupt_monitor_icc",
                                   std::move(srgb).value());
    }
    return finalize_state(resolved_token, DisplayProfileSource::kSystemMonitor,
                          "macos_coregraphics_display_icc", std::move(profile).value());
#elif defined(_WIN32)
    // Best-effort MSCMS/GDI ICM path (no new deps). Fail closed to explicit
    // fallback_srgb with a machine-visible reason when ICM is missing/corrupt.
    {
        HDC hdc = GetDC(nullptr);
        if (hdc == nullptr)
        {
            return fallback_srgb_state(std::string(screen_token), "windows_display_dc_unavailable",
                                       std::move(srgb).value());
        }
        DWORD chars_needed = 0;
        if (!GetICMProfileW(hdc, &chars_needed, nullptr) &&
            GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        {
            ReleaseDC(nullptr, hdc);
            return fallback_srgb_state(std::string(screen_token), "windows_icm_profile_unavailable",
                                       std::move(srgb).value());
        }
        if (chars_needed == 0 || chars_needed > 32768U)
        {
            ReleaseDC(nullptr, hdc);
            return fallback_srgb_state(std::string(screen_token),
                                       "windows_icm_profile_path_invalid", std::move(srgb).value());
        }
        std::wstring path(chars_needed, L'\0');
        DWORD path_chars = chars_needed;
        if (!GetICMProfileW(hdc, &path_chars, path.data()))
        {
            ReleaseDC(nullptr, hdc);
            return fallback_srgb_state(std::string(screen_token), "windows_icm_profile_unreadable",
                                       std::move(srgb).value());
        }
        ReleaseDC(nullptr, hdc);
        while (!path.empty() && path.back() == L'\0')
        {
            path.pop_back();
        }
        if (path.empty())
        {
            return fallback_srgb_state(std::string(screen_token), "windows_icm_profile_path_empty",
                                       std::move(srgb).value());
        }
        std::ifstream input(std::filesystem::path(path), std::ios::binary);
        if (!input)
        {
            return fallback_srgb_state(std::string(screen_token),
                                       "windows_icm_profile_file_unreadable",
                                       std::move(srgb).value());
        }
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                        std::istreambuf_iterator<char>());
        auto profile = profile_from_icc_bytes(std::move(bytes), "windows.display.primary");
        if (!profile)
        {
            return fallback_srgb_state(std::string(screen_token),
                                       profile.error().context.count("reason") ?
                                           profile.error().context.at("reason") :
                                           "corrupt_monitor_icc",
                                       std::move(srgb).value());
        }
        static_cast<void>(screen_token);
        return finalize_state(std::string(screen_token.empty() ? "primary" : screen_token),
                              DisplayProfileSource::kSystemMonitor, "windows_icm_display_icc",
                              std::move(profile).value());
    }
#elif defined(__linux__)
    // No packaged colord/X11 dependency: keep explicit sRGB fallback. C2 host
    // discovery remains macOS-primary with cross-platform fallback_srgb.
    return fallback_srgb_state(std::string(screen_token), "linux_monitor_discovery_unavailable",
                               std::move(srgb).value());
#else
    return fallback_srgb_state(std::string(screen_token), "host_monitor_discovery_unavailable",
                               std::move(srgb).value());
#endif
}

Result<DisplayPresentationState>
inject_monitor_presentation_from_icc_path(const std::string_view path,
                                          const std::string_view screen_token)
{
    auto srgb = make_srgb_profile_state();
    if (!srgb)
    {
        return srgb.error();
    }
    std::ifstream input(std::string(path), std::ios::binary);
    if (!input)
    {
        return fallback_srgb_state(std::string(screen_token), "injected_icc_unreadable",
                                   std::move(srgb).value());
    }
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                    std::istreambuf_iterator<char>());
    auto profile = profile_from_icc_bytes(std::move(bytes), std::string(path));
    if (!profile)
    {
        return fallback_srgb_state(std::string(screen_token),
                                   profile.error().context.count("reason") ?
                                       profile.error().context.at("reason") :
                                       "corrupt_monitor_icc",
                                   std::move(srgb).value());
    }
    return finalize_state(std::string(screen_token), DisplayProfileSource::kInjectedPath,
                          "injected_icc_path", std::move(profile).value());
}

Result<DisplayPresentationState>
make_synthetic_matrix_monitor_presentation(const std::array<float, 9> &matrix_rgb,
                                           const std::string_view screen_token)
{
    for (const float value : matrix_rgb)
    {
        if (!std::isfinite(value))
        {
            return make_error(ErrorCode::kValidation, "Synthetic matrix contains non-finite values",
                              {{"reason", "nonfinite_synthetic_matrix"}});
        }
    }
    ColorProfileState profile;
    profile.kind = ColorProfileKind::kMatrix;
    profile.model = ColorModel::kRgb;
    profile.identifier = "ravo.display.synthetic.matrix";
    profile.has_matrix = true;
    profile.matrix_to_xyz_d50 = matrix_rgb;
    return finalize_state(std::string(screen_token), DisplayProfileSource::kSyntheticMatrix,
                          "synthetic_matrix_test_profile", std::move(profile));
}

Result<DisplayPresentationState>
make_synthetic_lut_monitor_presentation(const std::uint32_t lut_size,
                                        const std::string_view screen_token)
{
    if (lut_size < 2U || lut_size > 256U)
    {
        return make_error(
            ErrorCode::kValidation, "Synthetic LUT size is out of range",
            {{"reason", "invalid_synthetic_lut_size"}, {"lut_size", std::to_string(lut_size)}});
    }
    auto srgb = make_srgb_profile_state();
    if (!srgb)
    {
        return srgb.error();
    }
    ColorProfileState profile = std::move(srgb).value();
    profile.identifier = "ravo.display.synthetic.lut." + std::to_string(lut_size);
    return finalize_state(std::string(screen_token), DisplayProfileSource::kSyntheticLut,
                          "synthetic_identity_lut_test_profile", std::move(profile));
}

Result<DisplayPresentationState>
refresh_monitor_presentation(const DisplayPresentationState &previous,
                             const std::string_view new_screen_token)
{
    // Injected/synthetic presentation stays owned by the caller; only system /
    // fallback states re-discover on screen change.
    if (previous.source == DisplayProfileSource::kInjectedPath ||
        previous.source == DisplayProfileSource::kSyntheticMatrix ||
        previous.source == DisplayProfileSource::kSyntheticLut)
    {
        auto refreshed = previous;
        refreshed.screen_token = std::string(new_screen_token);
        refreshed.reason = "presentation_screen_token_updated_without_rediscovery";
        return refreshed;
    }
    return discover_monitor_presentation(new_screen_token);
}

Result<DisplayPresentationRgb8> apply_display_presentation_rgb8(
    const std::vector<std::uint8_t> &source_rgb8, const std::uint32_t width,
    const std::uint32_t height, const ColorProfileState &source_profile,
    const DisplayPresentationState &presentation, const CancellationToken &cancellation,
    DisplayConversionObserver *observer, const DisplayPresentationCache *cache)
{
    if (!presentation.valid)
    {
        return make_error(ErrorCode::kValidation, "Display presentation state is not valid",
                          {{"reason", presentation.reason}});
    }
    const std::size_t expected =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U;
    if (width == 0 || height == 0 || source_rgb8.size() != expected)
    {
        return make_error(ErrorCode::kValidation, "Display presentation RGB8 buffer is invalid");
    }
    if (cache)
    {
        auto cached = cache->apply_if_prepared(source_rgb8, width, height, source_profile,
                                               presentation, cancellation, observer);
        if (!cached)
            return cached.error();
        if (cached.value())
            return std::move(*cached.value());
    }
    if (presentation.source == DisplayProfileSource::kSyntheticMatrix)
    {
        return apply_synthetic_matrix(source_rgb8, width, height, presentation.monitor_profile,
                                      cancellation);
    }
    if (presentation.source == DisplayProfileSource::kSyntheticLut)
    {
        DisplayPresentationRgb8 output;
        output.width = width;
        output.height = height;
        output.rgb8 = source_rgb8;
        output.color_profile = presentation.monitor_profile;
        return output;
    }
    if (source_profile.icc_bytes.empty())
    {
        auto srgb = make_srgb_profile_state();
        if (!srgb)
        {
            return srgb.error();
        }
        return transform_icc_rgb8(source_rgb8, width, height, srgb.value(),
                                  presentation.monitor_profile, cancellation, observer);
    }
    if (color_profile_fingerprint(source_profile) == presentation.profile_fingerprint)
    {
        DisplayPresentationRgb8 output;
        output.width = width;
        output.height = height;
        output.rgb8 = source_rgb8;
        output.color_profile = presentation.monitor_profile;
        return output;
    }
    return transform_icc_rgb8(source_rgb8, width, height, source_profile,
                              presentation.monitor_profile, cancellation, observer);
}

JsonValue display_presentation_state_to_json(const DisplayPresentationState &state)
{
    return JsonValue::Object{
        {"contract_version", state.contract_version},
        {"screen_token", state.screen_token},
        {"source", std::string(display_profile_source_name(state.source))},
        {"reason", state.reason},
        {"profile_identifier", state.monitor_profile.identifier},
        {"profile_fingerprint", state.profile_fingerprint},
        {"icc_byte_count",
         JsonValue::number(std::to_string(state.monitor_profile.icc_bytes.size()))},
        {"valid", state.valid},
    };
}

std::vector<DisplayViewPixelContract> display_presentation_view_contracts()
{
    return {
        {"gallery_thumbnail", DisplayViewPixelKind::kDisplayTransformed,
         "after_soft_proof_display_only",
         "Gallery thumbs apply presentation after soft-proof/output; scopes keep base."},
        {"loupe_preview", DisplayViewPixelKind::kDisplayTransformed,
         "after_soft_proof_display_only",
         "Loupe CPU preview applies presentation after soft-proof/output."},
        {"develop_preview", DisplayViewPixelKind::kDisplayTransformed,
         "after_soft_proof_display_only",
         "Develop CPU preview applies presentation after soft-proof/output."},
        {"before_after", DisplayViewPixelKind::kDisplayTransformed, "after_soft_proof_display_only",
         "Before/After uses the same display-transformed preview path."},
        {"comparison", DisplayViewPixelKind::kDisplayTransformed, "after_soft_proof_display_only",
         "Comparison before/after plates are display-transformed."},
        {"magnifier", DisplayViewPixelKind::kDisplayTransformed, "after_soft_proof_display_only",
         "Magnifier zooms the active Loupe/Develop display-transformed pixels."},
        {"scopes", DisplayViewPixelKind::kAnalysisDiagnostic, "after_soft_proof_display_only",
         "Scopes analyze output-referred pixels; not monitor-converted."},
        {"gpu_native_preview", DisplayViewPixelKind::kDisplayTransformed,
         "after_soft_proof_display_only",
         "Studio publishes C++-presented IOSurface RGB8 via apply_display_presentation_rgb8; "
         "Engine GPU token is never QML-exposed. Linux/non-Metal publish residual."},
    };
}

JsonValue display_presentation_view_contracts_to_json()
{
    JsonValue::Array views;
    for (const auto &entry : display_presentation_view_contracts())
    {
        views.push_back(JsonValue::Object{
            {"view_id", entry.view_id},
            {"pixel_kind", std::string(display_view_pixel_kind_name(entry.pixel_kind))},
            {"soft_proof_interaction", entry.soft_proof_interaction},
            {"notes", entry.notes},
        });
    }
    return JsonValue::Object{
        {"contract_version", std::string(kDisplayPresentationContractVersion)},
        {"supported_host_discovery",
#if defined(__APPLE__)
         "macos_coregraphics"
#elif defined(_WIN32)
         "windows_icm_best_effort"
#else
         "fallback_srgb_only"
#endif
        },
        {"views", std::move(views)},
    };
}

} // namespace ravo
