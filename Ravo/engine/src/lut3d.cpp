#include "lut3d.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <list>
#include <locale>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "input_color.h"
#include "ravo/recipe/color_input.h"

namespace ravo
{
namespace
{
constexpr std::size_t kCacheEntries = 8U;
constexpr float kCubeValueLimit = 65536.0F;

struct LoadedCubeFile
{
    std::string canonical_path;
    std::string fingerprint;
    std::string bytes;
};

[[nodiscard]] TaskError cube_error(const ErrorCode code, const std::string_view message,
                                   const std::string_view reason,
                                   const std::string_view path = {},
                                   const std::size_t line = 0U)
{
    std::map<std::string, std::string, std::less<>> context{{"reason", std::string(reason)}};
    if (!path.empty())
        context.emplace("path", path);
    if (line != 0U)
        context.emplace("line", std::to_string(line));
    return make_error(code, std::string(message), std::move(context));
}

[[nodiscard]] std::string trim_copy(const std::string_view value)
{
    const auto first = std::find_if_not(value.begin(), value.end(), [](const unsigned char c)
                                        { return std::isspace(c) != 0; });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](const unsigned char c)
                                       { return std::isspace(c) != 0; })
                          .base();
    return first < last ? std::string(first, last) : std::string{};
}

[[nodiscard]] bool starts_with_word(const std::string_view line,
                                    const std::string_view word) noexcept
{
    return line == word ||
           (line.size() > word.size() && line.starts_with(word) &&
            std::isspace(static_cast<unsigned char>(line[word.size()])) != 0);
}

[[nodiscard]] bool parse_three_floats(const std::string_view text, std::array<float, 3> &values)
{
    std::istringstream stream{std::string(text)};
    stream.imbue(std::locale::classic());
    stream >> std::noskipws;
    for (std::size_t index = 0U; index < values.size(); ++index)
    {
        stream >> std::ws;
        double value = 0.0;
        if (!(stream >> value) || !std::isfinite(value) ||
            !std::isfinite(static_cast<float>(value)) || std::abs(value) > kCubeValueLimit)
            return false;
        values[index] = static_cast<float>(value);
    }
    stream >> std::ws;
    return stream.peek() == std::char_traits<char>::eof();
}

[[nodiscard]] std::string fnv1a64(const std::string_view bytes)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char source_byte : bytes)
    {
        const auto byte = static_cast<unsigned char>(source_byte);
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream stream;
    stream << "fnv1a64:" << std::hex << std::setfill('0') << std::setw(16) << hash << ':'
           << std::dec << bytes.size();
    return stream.str();
}

[[nodiscard]] Result<LoadedCubeFile>
read_cube_file(const std::string_view path, const CancellationToken &cancellation)
try
{
    auto active = cancellation.check();
    if (!active)
        return active.error();
    if (path.empty() || path.size() > kLut3dPathMaximumBytes)
        return cube_error(ErrorCode::kInvalidArgument, "3D LUT path is empty or too long",
                          "invalid_lut_path", path);

    std::error_code error;
    auto absolute = std::filesystem::absolute(std::filesystem::path(path), error);
    if (error)
        return cube_error(ErrorCode::kIo, "3D LUT path could not be resolved",
                          "lut_path_resolution_failed", path);
    absolute = absolute.lexically_normal();
    std::string extension = absolute.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](const unsigned char c)
                   { return static_cast<char>(std::tolower(c)); });
    if (extension != ".cube")
        return cube_error(ErrorCode::kUnsupported, "Only .cube 3D LUT files are supported",
                          "unsupported_lut_format", absolute.string());
    const auto size = std::filesystem::file_size(absolute, error);
    if (error)
        return cube_error(error == std::errc::no_such_file_or_directory ? ErrorCode::kNotFound :
                                                                       ErrorCode::kIo,
                          "3D LUT file is unavailable", "lut_file_unavailable",
                          absolute.string());
    if (size == 0U || size > kCubeLutMaximumFileBytes ||
        size > static_cast<std::uintmax_t>(std::string{}.max_size()))
        return cube_error(ErrorCode::kValidation, "3D LUT file size is invalid",
                          "invalid_lut_file_size", absolute.string());

    std::ifstream file(absolute, std::ios::binary);
    if (!file)
        return cube_error(ErrorCode::kIo, "3D LUT file could not be opened",
                          "lut_open_failed", absolute.string());
    std::string bytes;
    bytes.reserve(static_cast<std::size_t>(size));
    std::array<char, 64U * 1024U> chunk{};
    while (file)
    {
        active = cancellation.check();
        if (!active)
            return active.error();
        file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto count = file.gcount();
        if (count > 0)
            bytes.append(chunk.data(), static_cast<std::size_t>(count));
        if (bytes.size() > kCubeLutMaximumFileBytes)
            return cube_error(ErrorCode::kValidation, "3D LUT file exceeds the size limit",
                              "lut_file_too_large", absolute.string());
    }
    if (!file.eof() || bytes.size() != size)
        return cube_error(ErrorCode::kIo, "3D LUT file changed or failed while reading",
                          "lut_read_failed", absolute.string());

    auto canonical = std::filesystem::weakly_canonical(absolute, error);
    if (error)
        canonical = absolute;
    return LoadedCubeFile{canonical.string(), fnv1a64(bytes), std::move(bytes)};
}
catch (const std::bad_alloc &)
{
    return cube_error(ErrorCode::kIo, "3D LUT file allocation failed", "allocation_failed",
                      path);
}

[[nodiscard]] Result<std::shared_ptr<const CubeLut>>
parse_cube(LoadedCubeFile file, const CancellationToken &cancellation)
try
{
    auto lut = std::make_shared<CubeLut>();
    lut->canonical_path = std::move(file.canonical_path);
    lut->fingerprint = std::move(file.fingerprint);
    bool has_size = false;
    bool has_domain_min = false;
    bool has_domain_max = false;
    bool has_title = false;
    bool data_started = false;
    std::size_t line_number = 0U;
    std::size_t offset = 0U;
    while (offset <= file.bytes.size())
    {
        if ((line_number & 0xffU) == 0U)
        {
            auto active = cancellation.check();
            if (!active)
                return active.error();
        }
        const auto end = file.bytes.find('\n', offset);
        const auto length = (end == std::string::npos ? file.bytes.size() : end) - offset;
        ++line_number;
        if (length > kCubeLutMaximumLineBytes)
            return cube_error(ErrorCode::kValidation, "3D LUT line exceeds the length limit",
                              "lut_line_too_long", lut->canonical_path, line_number);
        std::string_view raw(file.bytes.data() + offset, length);
        if (!raw.empty() && raw.back() == '\r')
            raw.remove_suffix(1U);
        const auto comment = raw.find('#');
        if (comment != std::string_view::npos)
            raw = raw.substr(0U, comment);
        const std::string line = trim_copy(raw);
        if (!line.empty())
        {
            if (starts_with_word(line, "LUT_1D_SIZE"))
                return cube_error(ErrorCode::kUnsupported,
                                  "One-dimensional .cube LUTs are unsupported",
                                  "unsupported_1d_lut", lut->canonical_path, line_number);
            if (starts_with_word(line, "TITLE"))
            {
                if (data_started || has_title)
                    return cube_error(ErrorCode::kValidation, "3D LUT TITLE is misplaced or repeated",
                                      "invalid_lut_title", lut->canonical_path, line_number);
                std::string title = trim_copy(std::string_view(line).substr(5U));
                if (title.size() >= 2U && title.front() == '"' && title.back() == '"')
                    title = title.substr(1U, title.size() - 2U);
                if (title.size() > 256U)
                    return cube_error(ErrorCode::kValidation, "3D LUT title is too long",
                                      "invalid_lut_title", lut->canonical_path, line_number);
                lut->title = std::move(title);
                has_title = true;
            }
            else if (starts_with_word(line, "LUT_3D_SIZE"))
            {
                if (data_started || has_size)
                    return cube_error(ErrorCode::kValidation,
                                      "3D LUT size is misplaced or repeated",
                                      "invalid_lut_size", lut->canonical_path, line_number);
                std::istringstream stream{trim_copy(std::string_view(line).substr(11U))};
                stream.imbue(std::locale::classic());
                std::uint64_t size = 0U;
                std::string trailing;
                if (!(stream >> size) || (stream >> trailing) || size < kCubeLutMinimumSize ||
                    size > kCubeLutMaximumSize)
                    return cube_error(ErrorCode::kValidation, "3D LUT size is unsupported",
                                      "invalid_lut_size", lut->canonical_path, line_number);
                lut->size = static_cast<std::uint32_t>(size);
                const auto count = size * size * size;
                lut->values.reserve(static_cast<std::size_t>(count));
                has_size = true;
            }
            else if (starts_with_word(line, "DOMAIN_MIN") ||
                     starts_with_word(line, "DOMAIN_MAX"))
            {
                const bool minimum = line.starts_with("DOMAIN_MIN");
                bool &seen = minimum ? has_domain_min : has_domain_max;
                if (data_started || seen)
                    return cube_error(ErrorCode::kValidation,
                                      "3D LUT domain is misplaced or repeated",
                                      "invalid_lut_domain", lut->canonical_path, line_number);
                auto &domain = minimum ? lut->domain_min : lut->domain_max;
                if (!parse_three_floats(std::string_view(line).substr(10U), domain))
                    return cube_error(ErrorCode::kValidation, "3D LUT domain is malformed",
                                      "invalid_lut_domain", lut->canonical_path, line_number);
                seen = true;
            }
            else
            {
                if (!has_size)
                    return cube_error(ErrorCode::kValidation,
                                      "3D LUT data appears before LUT_3D_SIZE",
                                      "lut_size_missing", lut->canonical_path, line_number);
                std::array<float, 3> value{};
                if (!parse_three_floats(line, value))
                    return cube_error(ErrorCode::kValidation,
                                      "3D LUT contains an unknown directive or malformed sample",
                                      "invalid_lut_sample", lut->canonical_path, line_number);
                data_started = true;
                const std::uint64_t expected = static_cast<std::uint64_t>(lut->size) * lut->size *
                                               lut->size;
                if (lut->values.size() >= expected)
                    return cube_error(ErrorCode::kValidation, "3D LUT contains too many samples",
                                      "invalid_lut_sample_count", lut->canonical_path,
                                      line_number);
                lut->values.push_back(value);
            }
        }
        if (end == std::string::npos)
            break;
        offset = end + 1U;
    }
    if (!has_size)
        return cube_error(ErrorCode::kValidation, "3D LUT size is missing", "lut_size_missing",
                          lut->canonical_path);
    for (std::size_t channel = 0U; channel < 3U; ++channel)
        if (!(lut->domain_min[channel] < lut->domain_max[channel]))
            return cube_error(ErrorCode::kValidation,
                              "3D LUT domain minimum must be below its maximum",
                              "invalid_lut_domain", lut->canonical_path);
    const std::uint64_t expected = static_cast<std::uint64_t>(lut->size) * lut->size * lut->size;
    if (lut->values.size() != expected)
        return cube_error(ErrorCode::kValidation, "3D LUT sample count does not match its size",
                          "invalid_lut_sample_count", lut->canonical_path);
    auto active = cancellation.check();
    return active ? Result<std::shared_ptr<const CubeLut>>{std::move(lut)} : active.error();
}
catch (const std::bad_alloc &)
{
    return cube_error(ErrorCode::kIo, "3D LUT parse allocation failed", "allocation_failed",
                      file.canonical_path);
}

[[nodiscard]] std::string linear_profile_for(const std::string_view space)
{
    if (space == kLut3dSpaceLinearRec2020)
        return std::string(kInputProfileLinearRec2020);
    if (space == kLut3dSpaceLinearProPhoto)
        return std::string(kInputProfileProPhotoRgb);
    if (space == kLut3dSpaceAdobeRgb)
        return std::string(kInputProfileAdobeRgb);
    return std::string(kInputProfileLinearRec709);
}

[[nodiscard]] float signed_transfer(const float value,
                                    float (*positive)(float) noexcept) noexcept
{
    return std::signbit(value) ? -positive(-value) : positive(value);
}

[[nodiscard]] float srgb_encode_positive(const float value) noexcept
{
    return value <= 0.0031308F ? 12.92F * value :
                                1.055F * std::pow(value, 1.0F / 2.4F) - 0.055F;
}
[[nodiscard]] float srgb_decode_positive(const float value) noexcept
{
    return value <= 0.04045F ? value / 12.92F :
                               std::pow((value + 0.055F) / 1.055F, 2.4F);
}
[[nodiscard]] float rec709_encode_positive(const float value) noexcept
{
    return value < 0.018F ? 4.5F * value : 1.099F * std::pow(value, 0.45F) - 0.099F;
}
[[nodiscard]] float rec709_decode_positive(const float value) noexcept
{
    return value < 0.081F ? value / 4.5F : std::pow((value + 0.099F) / 1.099F, 1.0F / 0.45F);
}
[[nodiscard]] float adobe_encode_positive(const float value) noexcept
{
    return std::pow(value, 256.0F / 563.0F);
}
[[nodiscard]] float adobe_decode_positive(const float value) noexcept
{
    return std::pow(value, 563.0F / 256.0F);
}

[[nodiscard]] float transfer_encode(const float value, const std::string_view space) noexcept
{
    if (space == kLut3dSpaceSrgb)
        return signed_transfer(value, srgb_encode_positive);
    if (space == kLut3dSpaceRec709)
        return signed_transfer(value, rec709_encode_positive);
    if (space == kLut3dSpaceAdobeRgb)
        return signed_transfer(value, adobe_encode_positive);
    return value;
}

[[nodiscard]] float transfer_decode(const float value, const std::string_view space) noexcept
{
    if (space == kLut3dSpaceSrgb)
        return signed_transfer(value, srgb_decode_positive);
    if (space == kLut3dSpaceRec709)
        return signed_transfer(value, rec709_decode_positive);
    if (space == kLut3dSpaceAdobeRgb)
        return signed_transfer(value, adobe_decode_positive);
    return value;
}

[[nodiscard]] const std::array<float, 3> &sample(const CubeLut &lut, const std::uint32_t red,
                                                 const std::uint32_t green,
                                                 const std::uint32_t blue) noexcept
{
    const auto index = static_cast<std::size_t>(red) +
                       static_cast<std::size_t>(lut.size) * green +
                       static_cast<std::size_t>(lut.size) * lut.size * blue;
    return lut.values[index];
}

[[nodiscard]] std::array<float, 3> interpolate_trilinear(const CubeLut &lut,
                                                         const std::array<float, 3> position)
{
    std::array<std::uint32_t, 3> low{};
    std::array<std::uint32_t, 3> high{};
    std::array<float, 3> fraction{};
    for (std::size_t channel = 0U; channel < 3U; ++channel)
    {
        low[channel] = static_cast<std::uint32_t>(std::floor(position[channel]));
        high[channel] = std::min(low[channel] + 1U, lut.size - 1U);
        fraction[channel] = position[channel] - static_cast<float>(low[channel]);
    }
    std::array<float, 3> result{};
    for (std::uint32_t blue = 0U; blue < 2U; ++blue)
        for (std::uint32_t green = 0U; green < 2U; ++green)
            for (std::uint32_t red = 0U; red < 2U; ++red)
            {
                const float weight = (red ? fraction[0] : 1.0F - fraction[0]) *
                                     (green ? fraction[1] : 1.0F - fraction[1]) *
                                     (blue ? fraction[2] : 1.0F - fraction[2]);
                const auto &corner = sample(lut, red ? high[0] : low[0],
                                            green ? high[1] : low[1], blue ? high[2] : low[2]);
                for (std::size_t channel = 0U; channel < 3U; ++channel)
                    result[channel] += weight * corner[channel];
            }
    return result;
}

[[nodiscard]] std::array<float, 3> interpolate_tetrahedral(const CubeLut &lut,
                                                           const std::array<float, 3> position)
{
    std::array<std::uint32_t, 3> low{};
    std::array<std::uint32_t, 3> high{};
    std::array<float, 3> f{};
    for (std::size_t channel = 0U; channel < 3U; ++channel)
    {
        low[channel] = static_cast<std::uint32_t>(std::floor(position[channel]));
        high[channel] = std::min(low[channel] + 1U, lut.size - 1U);
        f[channel] = position[channel] - static_cast<float>(low[channel]);
    }
    const auto &c000 = sample(lut, low[0], low[1], low[2]);
    const auto &c100 = sample(lut, high[0], low[1], low[2]);
    const auto &c010 = sample(lut, low[0], high[1], low[2]);
    const auto &c001 = sample(lut, low[0], low[1], high[2]);
    const auto &c110 = sample(lut, high[0], high[1], low[2]);
    const auto &c101 = sample(lut, high[0], low[1], high[2]);
    const auto &c011 = sample(lut, low[0], high[1], high[2]);
    const auto &c111 = sample(lut, high[0], high[1], high[2]);
    std::array<float, 3> result{};
    for (std::size_t channel = 0U; channel < 3U; ++channel)
    {
        if (f[0] >= f[1])
        {
            if (f[1] >= f[2])
                result[channel] = c000[channel] + f[0] * (c100[channel] - c000[channel]) +
                                  f[1] * (c110[channel] - c100[channel]) +
                                  f[2] * (c111[channel] - c110[channel]);
            else if (f[0] >= f[2])
                result[channel] = c000[channel] + f[0] * (c100[channel] - c000[channel]) +
                                  f[2] * (c101[channel] - c100[channel]) +
                                  f[1] * (c111[channel] - c101[channel]);
            else
                result[channel] = c000[channel] + f[2] * (c001[channel] - c000[channel]) +
                                  f[0] * (c101[channel] - c001[channel]) +
                                  f[1] * (c111[channel] - c101[channel]);
        }
        else if (f[2] >= f[1])
            result[channel] = c000[channel] + f[2] * (c001[channel] - c000[channel]) +
                              f[1] * (c011[channel] - c001[channel]) +
                              f[0] * (c111[channel] - c011[channel]);
        else if (f[2] >= f[0])
            result[channel] = c000[channel] + f[1] * (c010[channel] - c000[channel]) +
                              f[2] * (c011[channel] - c010[channel]) +
                              f[0] * (c111[channel] - c011[channel]);
        else
            result[channel] = c000[channel] + f[1] * (c010[channel] - c000[channel]) +
                              f[0] * (c110[channel] - c010[channel]) +
                              f[2] * (c111[channel] - c110[channel]);
    }
    return result;
}
} // namespace

struct Lut3dCache::Impl
{
    struct Entry
    {
        std::string path;
        std::string fingerprint;
        std::shared_ptr<const CubeLut> lut;
    };
    std::mutex mutex;
    std::list<Entry> entries;
};

Result<std::shared_ptr<const CubeLut>>
Lut3dCache::load(const std::string_view path, const CancellationToken &cancellation)
{
    auto file = read_cube_file(path, cancellation);
    if (!file)
        return file.error();
    if (!impl_)
    {
        try
        {
            std::call_once(initialize_once_, [this] { impl_ = std::make_shared<Impl>(); });
        }
        catch (const std::bad_alloc &)
        {
            return cube_error(ErrorCode::kIo, "3D LUT cache allocation failed",
                              "allocation_failed", path);
        }
    }
    {
        std::lock_guard lock(impl_->mutex);
        const auto found = std::find_if(impl_->entries.begin(), impl_->entries.end(),
                                        [&](const Impl::Entry &entry)
                                        {
                                            return entry.path == file.value().canonical_path &&
                                                   entry.fingerprint == file.value().fingerprint;
                                        });
        if (found != impl_->entries.end())
        {
            auto lut = found->lut;
            impl_->entries.splice(impl_->entries.begin(), impl_->entries, found);
            return lut;
        }
    }
    auto parsed = parse_cube(std::move(file).value(), cancellation);
    if (!parsed)
        return parsed.error();
    {
        std::lock_guard lock(impl_->mutex);
        const auto duplicate = std::find_if(impl_->entries.begin(), impl_->entries.end(),
                                             [&](const Impl::Entry &entry)
                                             {
                                                 return entry.path == parsed.value()->canonical_path &&
                                                        entry.fingerprint == parsed.value()->fingerprint;
                                             });
        if (duplicate != impl_->entries.end())
        {
            auto lut = duplicate->lut;
            impl_->entries.splice(impl_->entries.begin(), impl_->entries, duplicate);
            return lut;
        }
        impl_->entries.push_front(
            {parsed.value()->canonical_path, parsed.value()->fingerprint, parsed.value()});
        while (impl_->entries.size() > kCacheEntries)
            impl_->entries.pop_back();
    }
    return parsed;
}

Lut3dCache &process_lut3d_cache()
{
    static Lut3dCache cache;
    return cache;
}

Result<WorkingImage> apply_lut3d(WorkingImage input, const Lut3dParams &params, Lut3dCache &cache,
                                 const CancellationToken &cancellation)
try
{
    auto active = cancellation.check();
    if (!active)
        return active.error();
    auto canonical = lut3d_to_parameters(params);
    if (!canonical)
        return canonical.error();
    const std::uint64_t pixels = static_cast<std::uint64_t>(input.width) * input.height;
    if (input.width == 0U || input.height == 0U ||
        pixels > std::vector<float>{}.max_size() / 3U || input.rgb.size() != pixels * 3U ||
        input.color_profile.model != ColorModel::kRgb || !input.color_profile.has_matrix ||
        input.color_profile.identifier != kInputProfileLinearRec709)
        return cube_error(ErrorCode::kValidation, "3D LUT input buffer is invalid",
                          "invalid_lut_input");
    auto lut = cache.load(params.file_path, cancellation);
    if (!lut)
        return lut.error();
    if (params.strength == 0.0)
        return input;

    const std::string input_profile = linear_profile_for(params.input_space);
    auto lut_input = input.color_profile.identifier == input_profile ?
                         Result<LinearWorkingBuffer>{input} :
                         convert_working_profile(input, input_profile, cancellation);
    if (!lut_input)
        return lut_input.error();
    const std::string output_profile = linear_profile_for(params.output_space);
    auto output_state = builtin_linear_working_profile_state(output_profile);
    if (!output_state)
        return output_state.error();

    WorkingImage transformed;
    transformed.width = input.width;
    transformed.height = input.height;
    transformed.rgb.resize(input.rgb.size());
    transformed.color_profile = std::move(output_state).value();
    transformed.exposure_analysis = input.exposure_analysis;
    transformed.canonical_roi_scale = input.canonical_roi_scale;
    transformed.mask_attached_frame = input.mask_attached_frame;
    const float maximum_index = static_cast<float>(lut.value()->size - 1U);
    for (std::uint32_t row = 0U; row < input.height; ++row)
    {
        active = cancellation.check();
        if (!active)
            return active.error();
        for (std::uint32_t column = 0U; column < input.width; ++column)
        {
            const std::size_t offset =
                (static_cast<std::size_t>(row) * input.width + column) * 3U;
            std::array<float, 3> position{};
            for (std::size_t channel = 0U; channel < 3U; ++channel)
            {
                const float encoded =
                    transfer_encode(lut_input.value().rgb[offset + channel], params.input_space);
                const float normalized =
                    (encoded - lut.value()->domain_min[channel]) /
                    (lut.value()->domain_max[channel] - lut.value()->domain_min[channel]);
                position[channel] = std::clamp(normalized, 0.0F, 1.0F) * maximum_index;
            }
            const auto mapped = params.interpolation == kLut3dInterpolationTrilinear ?
                                    interpolate_trilinear(*lut.value(), position) :
                                    interpolate_tetrahedral(*lut.value(), position);
            for (std::size_t channel = 0U; channel < 3U; ++channel)
            {
                const float decoded = transfer_decode(mapped[channel], params.output_space);
                if (!std::isfinite(decoded))
                    return cube_error(ErrorCode::kValidation,
                                      "3D LUT produced a non-finite output",
                                      "nonfinite_lut_output", lut.value()->canonical_path);
                transformed.rgb[offset + channel] = decoded;
            }
        }
    }
    auto canonical_output = output_profile == kInputProfileLinearRec709 ?
                                Result<LinearWorkingBuffer>{std::move(transformed)} :
                                convert_working_profile(transformed, kInputProfileLinearRec709,
                                                        cancellation);
    if (!canonical_output)
        return canonical_output.error();
    const float strength = static_cast<float>(params.strength);
    for (std::size_t index = 0U; index < input.rgb.size(); ++index)
        canonical_output.value().rgb[index] =
            input.rgb[index] + strength * (canonical_output.value().rgb[index] - input.rgb[index]);
    active = cancellation.check();
    return active ? Result<WorkingImage>{std::move(canonical_output).value()} : active.error();
}
catch (const std::bad_alloc &)
{
    return cube_error(ErrorCode::kIo, "3D LUT processing allocation failed", "allocation_failed",
                      params.file_path);
}

Result<WorkingImage> apply_lut3d(WorkingImage input, const OperationInstance &operation,
                                 Lut3dCache &cache, const CancellationToken &cancellation)
{
    if (operation.id != kLut3dOperationId)
        return make_error(ErrorCode::kInvalidArgument, "Operation is not a 3D LUT");
    auto params = lut3d_from_parameters(operation.parameters);
    return params ? apply_lut3d(std::move(input), params.value(), cache, cancellation) :
                    Result<WorkingImage>{params.error()};
}

Result<std::string> lut3d_recipe_cache_fingerprint(const Recipe &recipe, Lut3dCache &cache,
                                                   const CancellationToken &cancellation)
try
{
    std::string combined = "ravo-lut3d-cache-v1";
    bool found = false;
    for (const auto &operation : recipe.operations)
    {
        if (!operation.enabled || operation.id != kLut3dOperationId)
            continue;
        auto params = lut3d_from_parameters(operation.parameters);
        if (!params)
            return params.error();
        auto lut = cache.load(params.value().file_path, cancellation);
        if (!lut)
            return lut.error();
        found = true;
        combined.append("|").append(operation.instance_id).append("|").append(
            lut.value()->fingerprint);
    }
    return found ? fnv1a64(combined) : std::string("none");
}
catch (const std::bad_alloc &)
{
    return cube_error(ErrorCode::kIo, "3D LUT fingerprint allocation failed",
                      "allocation_failed");
}

} // namespace ravo
