#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <QByteArray>
#include <QColor>
#include <QColorSpace>
#include <QCoreApplication>
#include <QImage>
#include <QProcess>
#include <QXmlStreamReader>
#include <gtest/gtest.h>
#include <png.h>

#include "ravo/adapters/legacy_xmp.h"
#include "ravo/adapters/text_file.h"
#include "ravo/cli/application.h"
#include "ravo/domain/types.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/json.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/color_checker.h"
#include "ravo/recipe/color_contrast.h"
#include "ravo/recipe/color_correction.h"
#include "ravo/recipe/color_harmonizer.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"
#include "ravo/recipe/profile_gamma.h"
#include "ravo/recipe/primaries.h"
#include "ravo/recipe/recipe.h"
#include "ravo/recipe/style.h"

#include "capture_metadata_test_support.h"
#include "cli_test_support.h"
#include "test_support.h"

namespace ravo
{
namespace
{


[[nodiscard]] std::string mire1_path()
{
    const auto path =
        std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests" / "images" / "mire1.cr2";
    const auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

[[nodiscard]] std::string mire1_xtrans_path()
{
    const auto path = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests" / "images" /
                      "mire1-xtrans.raf";
    const auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

struct SourceFileSnapshot
{
    std::uintmax_t size = 0U;
    std::filesystem::file_time_type modified;
    std::uint64_t content_hash = 1469598103934665603ULL;

    [[nodiscard]] bool operator==(const SourceFileSnapshot &) const = default;
};

[[nodiscard]] std::optional<SourceFileSnapshot> source_file_snapshot(const std::string &path)
{
    std::error_code error;
    SourceFileSnapshot result;
    result.size = std::filesystem::file_size(path, error);
    if (error)
    {
        return std::nullopt;
    }
    result.modified = std::filesystem::last_write_time(path, error);
    if (error)
    {
        return std::nullopt;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        return std::nullopt;
    }
    std::array<char, 64U * 1024U> block{};
    while (input)
    {
        input.read(block.data(), static_cast<std::streamsize>(block.size()));
        const auto read = input.gcount();
        for (std::streamsize index = 0; index < read; ++index)
        {
            result.content_hash ^=
                static_cast<std::uint8_t>(block[static_cast<std::size_t>(index)]);
            result.content_hash *= 1099511628211ULL;
        }
    }
    if (!input.eof())
    {
        return std::nullopt;
    }
    return result;
}

[[nodiscard]] std::uint64_t pixel_hash(const std::vector<std::uint8_t> &pixels) noexcept
{
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto value : pixels)
    {
        hash ^= value;
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline constexpr std::string_view kLegacyPrimariesPayload =
    "cc56143f4c37093e22c9293ed9ce573f448d12be7b149e3f1e206a3e85eb513f";
inline constexpr std::string_view kLegacyPrimariesDefaultBlend =
    "gz09eJxjYGBgYAFiCQYYOOHEgAZY0QVwggZ7CB6pfOygYtaVAyCMi48L/AcCEA0AmawnoA==";

struct FrozenLegacyGammaBlendTuple
{
    std::string_view version;
    std::string_view parameters;
    std::size_t fixture_count;
};

inline constexpr std::string_view kLegacyGammaBlendV9 =
    "gz11eJxjYGBgkGAAgRNODGiAEV0AJ2iwh+CRyscOAAdeGQQ=";
inline constexpr std::string_view kLegacyGammaBlendGz14GuideOne =
    "gz14eJxjYIAACQYYOOHEgAYY0QVwggZ7CB6pfNoAAEkgGQQ=";
inline constexpr std::string_view kLegacyGammaBlendGz14GuideFive =
    "gz14eJxjYIAACQYYOOHEgAZY0QVwggZ7CB6pfNoAAE8gGQg=";
inline constexpr std::string_view kLegacyGammaBlendGz12GuideOne =
    "gz12eJxjYIAACQYYOOHEgAYY0QVwggZ7CB6pfOqC/0AAogFjBh0A";
inline constexpr std::string_view kLegacyGammaBlendGz12GuideFive =
    "gz12eJxjYIAACQYYOOHEgAZY0QVwggZ7CB6pfOqC/0AAogFpBh0E";
inline constexpr std::string_view kLegacyGammaBlendGz11FeatherV1 =
    "gz11eJxjYIAACQYYOOHEgAZY0QWAgBGLGANDgz0Ej1Q+dcF/IADRAGpyHQU=";
inline constexpr std::string_view kLegacyGammaBlendV11UncompressedGuideFive =
    "000000000000000018000000000000000000c84200000000000000000000000000000000050000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000";

inline constexpr std::array kFrozenLegacyGammaBlendTuples{
    FrozenLegacyGammaBlendTuple{"9", kLegacyGammaBlendV9, 37U},
    FrozenLegacyGammaBlendTuple{"10", kLegacyGammaBlendGz14GuideOne, 37U},
    FrozenLegacyGammaBlendTuple{"11", kLegacyGammaBlendV11UncompressedGuideFive, 1U},
    FrozenLegacyGammaBlendTuple{"11", kLegacyGammaBlendGz14GuideOne, 19U},
    FrozenLegacyGammaBlendTuple{"11", kLegacyGammaBlendGz14GuideFive, 25U},
    FrozenLegacyGammaBlendTuple{"12", kLegacyGammaBlendGz12GuideFive, 2U},
    FrozenLegacyGammaBlendTuple{"12", kLegacyGammaBlendGz14GuideOne, 5U},
    FrozenLegacyGammaBlendTuple{"12", kLegacyGammaBlendGz14GuideFive, 4U},
    FrozenLegacyGammaBlendTuple{"13", kLegacyGammaBlendGz11FeatherV1, 14U},
    FrozenLegacyGammaBlendTuple{"13", kLegacyGammaBlendGz12GuideOne, 1U},
    FrozenLegacyGammaBlendTuple{"13", kLegacyGammaBlendGz12GuideFive, 6U},
    FrozenLegacyGammaBlendTuple{"14", kLegacyGammaBlendGz11FeatherV1, 7U},
};

struct LegacyGammaXmpOptions
{
    std::optional<std::string_view> version = "1";
    std::optional<std::string_view> enabled = "1";
    std::optional<std::string_view> parameters = "0000000000000000";
    std::optional<std::string_view> blend_version = "9";
    std::optional<std::string_view> blend_parameters = kLegacyGammaBlendV9;
    std::optional<std::string_view> multi_priority = "0";
    std::optional<std::string_view> multi_name = "";
    std::optional<std::string_view> multi_name_hand_edited;
    std::string_view extra_attributes;
    std::size_t instances = 1U;
};

[[nodiscard]] std::string legacy_gamma_xmp(const LegacyGammaXmpOptions &options = {})
{
    std::string document = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq>)";
    const auto append_attribute =
        [&](const std::string_view name, const std::optional<std::string_view> value)
    {
        if (value)
        {
            document += " darktable:";
            document += name;
            document += "=\"";
            document += *value;
            document += '"';
        }
    };
    for (std::size_t index = 0; index < options.instances; ++index)
    {
        document += R"(<rdf:li darktable:num=")";
        document += std::to_string(index + 40U);
        document += R"(" darktable:operation="gamma")";
        append_attribute("modversion", options.version);
        append_attribute("enabled", options.enabled);
        append_attribute("params", options.parameters);
        append_attribute("multi_name", options.multi_name);
        append_attribute("multi_priority", options.multi_priority);
        append_attribute("multi_name_hand_edited", options.multi_name_hand_edited);
        append_attribute("blendop_version", options.blend_version);
        append_attribute("blendop_params", options.blend_parameters);
        document += options.extra_attributes;
        document += "/>";
    }
    document += R"(</rdf:Seq></darktable:history></rdf:Description>
</rdf:RDF>)";
    return document;
}

[[nodiscard]] std::string legacy_flip_xmp(const std::string_view parameters,
                                          const std::string_view blend = kLegacyGammaBlendV9)
{
    std::string document = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq>
<rdf:li darktable:num="7" darktable:operation="flip" darktable:modversion="2" darktable:enabled="1" darktable:params=")";
    document += parameters;
    document +=
        R"(" darktable:multi_name="" darktable:multi_priority="0" darktable:blendop_version="9" darktable:blendop_params=")";
    document += blend;
    document += R"("/>
</rdf:Seq></darktable:history></rdf:Description>
</rdf:RDF>)";
    return document;
}

[[nodiscard]] std::string legacy_crop_xmp(const std::string_view parameters,
                                          const std::string_view version = "3",
                                          const std::string_view blend = kLegacyGammaBlendV9)
{
    std::string document = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq>
<rdf:li darktable:num="11" darktable:operation="crop" darktable:modversion=")";
    document += version;
    document += R"(" darktable:enabled="1" darktable:params=")";
    document += parameters;
    document +=
        R"(" darktable:multi_name="" darktable:multi_priority="0" darktable:blendop_version="9" darktable:blendop_params=")";
    document += blend;
    document += R"("/>
</rdf:Seq></darktable:history></rdf:Description>
</rdf:RDF>)";
    return document;
}

inline constexpr std::string_view kLegacyExposureV5ManualOne =
    "00000000000000000000803f00004842000080c0";
inline constexpr std::string_view kLegacyExposureV6ManualOneBias =
    "00000000000000000000803f00004842000080c001000000";
inline constexpr std::string_view kLegacyExposureV7ManualOneBothCompensations =
    "00000000000000000000803f00004842000080c00100000001000000";
inline constexpr std::string_view kLegacyColorBalanceV3FixturePayload =
    "010000000000803f0000803f0000803f0000803ffeff7f3f0000803f0000803f0000803f"
    "0000803f0000803f0000803f0000803f0000803f8de4aa3f024ee1410000803f";
inline constexpr std::string_view kLegacyColorCheckerV2FixturePayload =
    "gz04eJw7/FXcqTW72Sloh7tTTL+O05KCGCDudbq+uMqpWkTB6WGVD5D9yTGGf6KTx8N+p0LbZ462XLFOaWkaTodLFzv1H/"
    "J3+qbhA1R70Em+dZGTbVcrUI83UA+fk8i6xY4MdACHv0YA3TTR8ZvGnAOzZgYeXFLA6zhrJuuhde4CQD+pOR7+6uDEdX2"
    "7o8i6vQc9Hs5y9HiY5KgZI3lIvjXUaZ17o8Phr+5OtlxPD37TuLOP67ryfo+HVvtnzZTcZ8t1HchfvJc+fkhwfFjV57ik"
    "YP3Bde7rHY2NTx88e8ZnX1paCtDt5ocYGJocg3asOWjLlQqMq3YnY2PPQ4e//nKUb33oeH3xfKcdcpkHd8g9PfgmcIZ9t"
    "ci6fUAj9xfadu1bUjB3/5vAiv308QPutPSvfidJaenQ14nwtNR/yHRQpCU5w8NOpKSlK4v1nQZbWqpcvNSJlLSkb8wPVA9"
    "JS68Dc53olZYkgBgAYTUMzA==";
inline constexpr std::string_view kLegacyColorCheckerDefaultBlend =
    "gz13eJxjYGBgYAJiCQYYOOHEgAZY0QVwggZ7CB6pfNoAAFJgGQo=";

struct LegacyColorCheckerXmpOptions
{
    std::optional<std::string_view> history_position = "8";
    std::optional<std::string_view> version = "2";
    std::optional<std::string_view> enabled = "1";
    std::optional<std::string_view> parameters = kLegacyColorCheckerV2FixturePayload;
    std::optional<std::string_view> blend_version = "11";
    std::optional<std::string_view> blend_parameters = kLegacyColorCheckerDefaultBlend;
    std::optional<std::string_view> multi_priority = "0";
    std::optional<std::string_view> multi_name = "";
    std::optional<std::string_view> multi_name_hand_edited;
    std::string_view extra_attributes;
};

[[nodiscard]] std::string
legacy_color_checker_xmp(const std::vector<LegacyColorCheckerXmpOptions> &entries)
{
    std::string document = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq>)";
    const auto append_attribute =
        [&](const std::string_view name, const std::optional<std::string_view> value)
    {
        if (value)
        {
            document += " darktable:";
            document += name;
            document += "=\"";
            document += *value;
            document += '"';
        }
    };
    for (const auto &entry : entries)
    {
        document += R"(<rdf:li darktable:operation="colorchecker")";
        append_attribute("num", entry.history_position);
        append_attribute("modversion", entry.version);
        append_attribute("enabled", entry.enabled);
        append_attribute("params", entry.parameters);
        append_attribute("multi_name", entry.multi_name);
        append_attribute("multi_priority", entry.multi_priority);
        append_attribute("multi_name_hand_edited", entry.multi_name_hand_edited);
        append_attribute("blendop_version", entry.blend_version);
        append_attribute("blendop_params", entry.blend_parameters);
        document += entry.extra_attributes;
        document += "/>";
    }
    document += R"(</rdf:Seq></darktable:history></rdf:Description>
</rdf:RDF>)";
    return document;
}

[[nodiscard]] std::string legacy_color_checker_xmp(const LegacyColorCheckerXmpOptions &options = {})
{
    return legacy_color_checker_xmp(std::vector<LegacyColorCheckerXmpOptions>{options});
}

inline constexpr std::string_view kLegacyColorContrastV2Parameters =
    "6666264000000000000020400000000001000000";
inline constexpr std::string_view kLegacyColorContrastV1Parameters =
    "66662640000000000000204000000000";
inline constexpr std::string_view kLegacyColorContrastDefaultBlend =
    "gz13eJxjYGBgYAJiCQYYOOHEgAYY0QVwggZ7CB6pfNoAAExgGQY=";

struct LegacyColorContrastXmpOptions
{
    std::optional<std::string_view> history_position = "9";
    std::optional<std::string_view> version = "2";
    std::optional<std::string_view> enabled = "1";
    std::optional<std::string_view> parameters = kLegacyColorContrastV2Parameters;
    std::optional<std::string_view> multi_name = "";
    std::optional<std::string_view> multi_priority = "0";
    std::optional<std::string_view> multi_name_hand_edited;
    std::optional<std::string_view> blend_version = "10";
    std::optional<std::string_view> blend_parameters = kLegacyColorContrastDefaultBlend;
    std::string_view extra_attributes;
};

[[nodiscard]] std::string
legacy_color_contrast_xmp(const std::vector<LegacyColorContrastXmpOptions> &entries)
{
    std::string document = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq>)";
    const auto append_attribute =
        [&](const std::string_view name, const std::optional<std::string_view> value)
    {
        if (value)
        {
            document += " darktable:";
            document += name;
            document += "=\"";
            document += *value;
            document += '"';
        }
    };
    for (const auto &entry : entries)
    {
        document += R"(<rdf:li darktable:operation="colorcontrast")";
        append_attribute("num", entry.history_position);
        append_attribute("modversion", entry.version);
        append_attribute("enabled", entry.enabled);
        append_attribute("params", entry.parameters);
        append_attribute("multi_name", entry.multi_name);
        append_attribute("multi_priority", entry.multi_priority);
        append_attribute("multi_name_hand_edited", entry.multi_name_hand_edited);
        append_attribute("blendop_version", entry.blend_version);
        append_attribute("blendop_params", entry.blend_parameters);
        document += entry.extra_attributes;
        document += "/>";
    }
    document += R"(</rdf:Seq></darktable:history></rdf:Description>
</rdf:RDF>)";
    return document;
}

[[nodiscard]] std::string
legacy_color_contrast_xmp(const LegacyColorContrastXmpOptions &options = {})
{
    return legacy_color_contrast_xmp(std::vector<LegacyColorContrastXmpOptions>{options});
}

void append_u32_le_hex(std::string &output, const std::uint32_t word)
{
    constexpr std::string_view digits = "0123456789abcdef";
    for (std::size_t byte = 0U; byte < sizeof(word); ++byte)
    {
        const std::uint8_t value = static_cast<std::uint8_t>(word >> (byte * 8U));
        output.push_back(digits[value >> 4U]);
        output.push_back(digits[value & 0x0fU]);
    }
}

[[nodiscard]] std::string
legacy_color_checker_v2_hex(const ColorCheckerParams &params, const std::int32_t count,
                            const std::optional<std::size_t> dirty_tail = std::nullopt)
{
    constexpr std::size_t stride = kColorCheckerMaxPatchCount;
    std::array<std::uint32_t, 6U * stride + 1U> words{};
    const std::size_t active = std::min(params.patches.size(), stride);
    for (std::size_t patch = 0U; patch < active; ++patch)
    {
        for (std::size_t channel = 0U; channel < 3U; ++channel)
        {
            words[channel * stride + patch] = std::bit_cast<std::uint32_t>(
                static_cast<float>(params.patches[patch].source_lab[channel]));
            words[(channel + 3U) * stride + patch] = std::bit_cast<std::uint32_t>(
                static_cast<float>(params.patches[patch].target_lab[channel]));
        }
    }
    if (dirty_tail)
    {
        words[*dirty_tail] = 0x7fc00000U;
    }
    words.back() = std::bit_cast<std::uint32_t>(count);
    std::string output;
    output.reserve(words.size() * sizeof(std::uint32_t) * 2U);
    for (const auto word : words)
    {
        append_u32_le_hex(output, word);
    }
    return output;
}

[[nodiscard]] std::string legacy_color_checker_v1_hex(const ColorCheckerParams &params)
{
    std::string output;
    output.reserve(kColorCheckerDefaultPatchCount * 3U * sizeof(std::uint32_t) * 2U);
    for (std::size_t channel = 0U; channel < 3U; ++channel)
    {
        for (std::size_t patch = 0U; patch < kColorCheckerDefaultPatchCount; ++patch)
        {
            append_u32_le_hex(output, std::bit_cast<std::uint32_t>(static_cast<float>(
                                          params.patches[patch].target_lab[channel])));
        }
    }
    return output;
}

struct LegacyExposureXmpOptions
{
    std::optional<std::string_view> history_position = "8";
    std::optional<std::string_view> version = "5";
    std::optional<std::string_view> enabled = "1";
    std::optional<std::string_view> parameters = kLegacyExposureV5ManualOne;
    std::optional<std::string_view> blend_version = "9";
    std::optional<std::string_view> blend_parameters = kLegacyGammaBlendV9;
    std::optional<std::string_view> multi_priority = "0";
    std::optional<std::string_view> multi_name = "";
    std::optional<std::string_view> multi_name_hand_edited;
    std::string_view extra_attributes;
};

[[nodiscard]] std::string legacy_exposure_xmp(const std::vector<LegacyExposureXmpOptions> &entries)
{
    std::string document = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq>)";
    const auto append_attribute =
        [&](const std::string_view name, const std::optional<std::string_view> value)
    {
        if (value)
        {
            document += " darktable:";
            document += name;
            document += "=\"";
            document += *value;
            document += '"';
        }
    };
    for (const auto &entry : entries)
    {
        document += R"(<rdf:li darktable:operation="exposure")";
        append_attribute("num", entry.history_position);
        append_attribute("modversion", entry.version);
        append_attribute("enabled", entry.enabled);
        append_attribute("params", entry.parameters);
        append_attribute("multi_name", entry.multi_name);
        append_attribute("multi_priority", entry.multi_priority);
        append_attribute("multi_name_hand_edited", entry.multi_name_hand_edited);
        append_attribute("blendop_version", entry.blend_version);
        append_attribute("blendop_params", entry.blend_parameters);
        document += entry.extra_attributes;
        document += "/>";
    }
    document += R"(</rdf:Seq></darktable:history></rdf:Description>
</rdf:RDF>)";
    return document;
}

[[nodiscard]] std::string legacy_exposure_xmp(const LegacyExposureXmpOptions &options = {})
{
    return legacy_exposure_xmp(std::vector<LegacyExposureXmpOptions>{options});
}

struct LegacyColorBalanceXmpOptions
{
    std::optional<std::string_view> history_position = "15";
    std::optional<std::string_view> version = "3";
    std::optional<std::string_view> enabled = "1";
    std::optional<std::string_view> parameters = kLegacyColorBalanceV3FixturePayload;
    std::optional<std::string_view> blend_version = "9";
    std::optional<std::string_view> blend_parameters = kLegacyGammaBlendV9;
    std::optional<std::string_view> multi_priority = "0";
    std::optional<std::string_view> multi_name = "";
    std::optional<std::string_view> multi_name_hand_edited;
    std::string_view extra_attributes;
};

[[nodiscard]] std::string
legacy_color_balance_xmp(const std::vector<LegacyColorBalanceXmpOptions> &entries)
{
    std::string document = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq>)";
    const auto append_attribute =
        [&](const std::string_view name, const std::optional<std::string_view> value)
    {
        if (value)
        {
            document += " darktable:";
            document += name;
            document += "=\"";
            document += *value;
            document += '"';
        }
    };
    for (const auto &entry : entries)
    {
        document += R"(<rdf:li darktable:operation="colorbalance")";
        append_attribute("num", entry.history_position);
        append_attribute("modversion", entry.version);
        append_attribute("enabled", entry.enabled);
        append_attribute("params", entry.parameters);
        append_attribute("multi_name", entry.multi_name);
        append_attribute("multi_priority", entry.multi_priority);
        append_attribute("multi_name_hand_edited", entry.multi_name_hand_edited);
        append_attribute("blendop_version", entry.blend_version);
        append_attribute("blendop_params", entry.blend_parameters);
        document += entry.extra_attributes;
        document += "/>";
    }
    document += R"(</rdf:Seq></darktable:history></rdf:Description>
</rdf:RDF>)";
    return document;
}

[[nodiscard]] std::string legacy_color_balance_xmp(const LegacyColorBalanceXmpOptions &options = {})
{
    return legacy_color_balance_xmp(std::vector<LegacyColorBalanceXmpOptions>{options});
}

[[nodiscard]] std::optional<std::string> xml_attribute_value(const QXmlStreamAttributes &attributes,
                                                             const QStringView name)
{
    for (const auto &attribute : attributes)
    {
        if (attribute.name() == name)
        {
            return attribute.value().toString().toStdString();
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::string
legacy_primaries_xmp(const std::string_view version = "1", const std::string_view enabled = "1",
                     const std::string_view parameters = kLegacyPrimariesPayload,
                     const std::string_view blend = kLegacyPrimariesDefaultBlend,
                     const std::size_t instances = 1U, const std::string_view blend_version = "13",
                     const std::string_view extra_attributes = {})
{
    std::string document = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq>)";
    for (std::size_t index = 0; index < instances; ++index)
    {
        document += R"(<rdf:li darktable:operation="primaries" darktable:modversion=")";
        document += version;
        document += R"(" darktable:enabled=")";
        document += enabled;
        document += R"(" darktable:params=")";
        document += parameters;
        document += R"(" darktable:blendop_version=")";
        document += blend_version;
        document += R"(" darktable:blendop_params=")";
        document += blend;
        document += '"';
        document += extra_attributes;
        document += "/>";
    }
    document += R"(</rdf:Seq></darktable:history></rdf:Description>
</rdf:RDF>)";
    return document;
}

[[nodiscard]] bool png_has_chunk(const std::filesystem::path &path, const std::array<char, 4> &type)
{
    std::ifstream stream(path, std::ios::binary);
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(stream),
                                          std::istreambuf_iterator<char>()};
    if (bytes.size() < 8U)
    {
        return false;
    }
    std::size_t offset = 8U;
    while (offset + 12U <= bytes.size())
    {
        const std::uint32_t size = (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
                                   (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
                                   (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
                                   static_cast<std::uint32_t>(bytes[offset + 3U]);
        if (size > bytes.size() - offset - 12U)
        {
            return false;
        }
        if (std::equal(type.begin(), type.end(),
                       bytes.begin() + static_cast<std::ptrdiff_t>(offset + 4U)))
        {
            return true;
        }
        offset += static_cast<std::size_t>(size) + 12U;
    }
    return false;
}

[[nodiscard]] std::vector<std::uint8_t> read_png_rgb(const std::filesystem::path &path)
{
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (png_image_begin_read_from_file(&image, path.string().c_str()) == 0)
    {
        return {};
    }
    image.format = PNG_FORMAT_RGB;
    std::vector<std::uint8_t> pixels(PNG_IMAGE_SIZE(image));
    if (png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr) == 0)
    {
        png_image_free(&image);
        return {};
    }
    png_image_free(&image);
    return pixels;
}

[[nodiscard]] bool write_perspective_grid_png(const std::filesystem::path &path)
{
    constexpr std::uint32_t width = 320U;
    constexpr std::uint32_t height = 240U;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 3U, 24U);
    const auto paint = [&](const std::uint32_t x, const std::uint32_t y)
    {
        const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 3U;
        pixels[offset] = pixels[offset + 1U] = pixels[offset + 2U] = 235U;
    };
    for (const std::uint32_t x : {55U, 155U, 265U})
        for (std::uint32_t y = 18U; y < 222U; ++y)
            for (std::uint32_t dx = 0U; dx < 3U; ++dx)
                paint(x + dx - 1U, y);
    for (const std::uint32_t y : {45U, 118U, 198U})
        for (std::uint32_t x = 18U; x < 302U; ++x)
            for (std::uint32_t dy = 0U; dy < 3U; ++dy)
                paint(x, y + dy - 1U);
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    image.width = width;
    image.height = height;
    image.format = PNG_FORMAT_RGB;
    return png_image_write_to_file(&image, path.string().c_str(), 0, pixels.data(), 0, nullptr) !=
           0;
}

TEST_F(CliTest, CatalogDevelopProbeIsReadOnlyAndReportsDeterministicPixelStatistics)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-cli-probe-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    const auto catalog = (root / "library.sqlite").string();
    const auto png = (std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests" /
                      "0000-nop" / "expected.png")
                         .generic_u8string();
    const std::string png_path(png.begin(), png.end());

    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    ASSERT_EQ(application.run(
                  std::vector<std::string_view>{"catalog", "create", "--path", catalog, "--json"}),
              0)
        << stdout_stream.str();

    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "import", "--catalog", catalog, "--input", png_path, "--json"}),
              0)
        << stdout_stream.str();
    auto imported = parse_json(stdout_stream.str());
    ASSERT_TRUE(imported) << imported.error().message;
    const auto *data = imported.value().find("data");
    ASSERT_NE(data, nullptr);
    const auto *items = data->find("items");
    ASSERT_NE(items, nullptr);
    ASSERT_NE(items->array_if(), nullptr);
    ASSERT_EQ(items->array_if()->size(), 1U);
    const auto *asset = items->array_if()->front().find("asset");
    ASSERT_NE(asset, nullptr);
    const auto *asset_id = asset->find("id");
    ASSERT_NE(asset_id, nullptr);
    ASSERT_NE(asset_id->string_if(), nullptr);
    const auto id = *asset_id->string_if();

    const auto run_probe = [&](const std::optional<std::string_view> override,
                               const bool baseline) -> Result<JsonValue>
    {
        stdout_stream.str({});
        stdout_stream.clear();
        std::vector<std::string_view> arguments{"catalog",    "probe", "--catalog",  catalog,
                                                "--asset-id", id,      "--max-edge", "64"};
        if (baseline)
        {
            arguments.push_back("--baseline");
        }
        if (override)
        {
            arguments.push_back("--set");
            arguments.push_back(*override);
        }
        arguments.push_back("--json");
        if (application.run(arguments) != 0)
        {
            return make_error(ErrorCode::kIo, "Develop probe command failed",
                              {{"stdout", stdout_stream.str()}});
        }
        return parse_json(stdout_stream.str());
    };
    const auto luma_mean = [](const JsonValue &response) -> std::optional<double>
    {
        const auto *data = response.find("data");
        const auto *statistics = data == nullptr ? nullptr : data->find("statistics");
        const auto *luma = statistics == nullptr ? nullptr : statistics->find("display_luma_mean");
        if (luma == nullptr || luma->number_if() == nullptr)
        {
            return std::nullopt;
        }
        return std::stod(luma->number_if()->text);
    };

    auto baseline = run_probe(std::nullopt, true);
    ASSERT_TRUE(baseline) << baseline.error().message;
    auto minus_one = run_probe("exposure=-1", true);
    ASSERT_TRUE(minus_one) << minus_one.error().message;
    auto plus_one = run_probe("exposure=1", true);
    ASSERT_TRUE(plus_one) << plus_one.error().message;
    auto texture = run_probe("texture=0.75", true);
    ASSERT_TRUE(texture) << texture.error().message;
    const auto baseline_luma = luma_mean(baseline.value());
    const auto minus_one_luma = luma_mean(minus_one.value());
    const auto plus_one_luma = luma_mean(plus_one.value());
    ASSERT_TRUE(baseline_luma);
    ASSERT_TRUE(minus_one_luma);
    ASSERT_TRUE(plus_one_luma);
    EXPECT_GT(*minus_one_luma, 8.0);
    EXPECT_LT(*minus_one_luma, *baseline_luma);
    EXPECT_GT(*plus_one_luma, *baseline_luma);
    const auto *minus_data = minus_one.value().find("data");
    ASSERT_NE(minus_data, nullptr);
    const auto *unchanged = minus_data->find("recipe_unchanged");
    ASSERT_NE(unchanged, nullptr);
    ASSERT_NE(unchanged->boolean_if(), nullptr);
    EXPECT_TRUE(*unchanged->boolean_if());
    const auto *preview_records_unchanged = minus_data->find("preview_records_unchanged");
    ASSERT_NE(preview_records_unchanged, nullptr);
    ASSERT_NE(preview_records_unchanged->boolean_if(), nullptr);
    EXPECT_TRUE(*preview_records_unchanged->boolean_if());

    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "develop", "--catalog", catalog, "--asset-id", id, "--set",
                  "exposure=1", "--set", "demosaicModeIndex=1", "--json"}),
              0)
        << stdout_stream.str();
    auto current = run_probe(std::nullopt, false);
    ASSERT_TRUE(current) << current.error().message;
    auto baseline_after_save = run_probe(std::nullopt, true);
    ASSERT_TRUE(baseline_after_save) << baseline_after_save.error().message;
    ASSERT_TRUE(luma_mean(current.value()));
    ASSERT_TRUE(luma_mean(baseline_after_save.value()));
    EXPECT_DOUBLE_EQ(*luma_mean(baseline_after_save.value()), *baseline_luma);
    EXPECT_GT(*luma_mean(current.value()), *baseline_luma);

    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{"develop-fields", "--json"}), 0)
        << stdout_stream.str();
    auto field_list = parse_json(stdout_stream.str());
    ASSERT_TRUE(field_list) << field_list.error().message;
    const auto *field_data = field_list.value().find("data");
    ASSERT_NE(field_data, nullptr);
    const auto *listed = field_data->find("fields");
    ASSERT_NE(listed, nullptr);
    ASSERT_NE(listed->array_if(), nullptr);
    EXPECT_GE(listed->array_if()->size(), 50U);
    bool listed_exposure = false;
    bool listed_demosaic_mode = false;
    bool listed_raw_denoise = false;
    bool listed_texture = false;
    bool listed_texture_iterations = false;
    for (const auto &field : *listed->array_if())
    {
        const auto *name = field.find("name");
        if (name != nullptr && name->string_if() != nullptr && *name->string_if() == "exposure")
        {
            listed_exposure = true;
        }
        if (name != nullptr && name->string_if() != nullptr &&
            *name->string_if() == "demosaicModeIndex")
        {
            listed_demosaic_mode = true;
            const auto *kind = field.find("kind");
            ASSERT_NE(kind, nullptr);
            ASSERT_NE(kind->string_if(), nullptr);
            EXPECT_EQ(*kind->string_if(), "integer");
            const auto *minimum = field.find("minimum");
            const auto *maximum = field.find("maximum");
            ASSERT_NE(minimum, nullptr);
            ASSERT_NE(maximum, nullptr);
            EXPECT_DOUBLE_EQ(std::stod(minimum->number_if()->text), 0.0);
            EXPECT_DOUBLE_EQ(std::stod(maximum->number_if()->text), 3.0);
        }
        if (name != nullptr && name->string_if() != nullptr &&
            *name->string_if() == "rawDenoiseThreshold")
        {
            listed_raw_denoise = true;
            const auto *minimum = field.find("minimum");
            const auto *maximum = field.find("maximum");
            ASSERT_NE(minimum, nullptr);
            ASSERT_NE(maximum, nullptr);
            EXPECT_DOUBLE_EQ(std::stod(minimum->number_if()->text), 0.0);
            EXPECT_DOUBLE_EQ(std::stod(maximum->number_if()->text), 1.0);
        }
        if (name != nullptr && name->string_if() != nullptr && *name->string_if() == "texture")
        {
            listed_texture = true;
            EXPECT_DOUBLE_EQ(std::stod(field.find("minimum")->number_if()->text), -2.0);
            EXPECT_DOUBLE_EQ(std::stod(field.find("maximum")->number_if()->text), 2.0);
        }
        if (name != nullptr && name->string_if() != nullptr &&
            *name->string_if() == "textureIterations")
        {
            listed_texture_iterations = true;
            ASSERT_NE(field.find("kind"), nullptr);
            EXPECT_EQ(*field.find("kind")->string_if(), "integer");
            EXPECT_DOUBLE_EQ(std::stod(field.find("minimum")->number_if()->text), 1.0);
            EXPECT_DOUBLE_EQ(std::stod(field.find("maximum")->number_if()->text), 5.0);
        }
    }
    EXPECT_TRUE(listed_exposure);
    EXPECT_TRUE(listed_demosaic_mode);
    EXPECT_TRUE(listed_raw_denoise);
    EXPECT_TRUE(listed_texture);
    EXPECT_TRUE(listed_texture_iterations);
    const auto *prefixes = field_data->find("prefixes");
    ASSERT_NE(prefixes, nullptr);
    ASSERT_NE(prefixes->array_if(), nullptr);
    EXPECT_GE(prefixes->array_if()->size(), 2U);

    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{"catalog", "fields", "--json"}), 0)
        << stdout_stream.str();
    auto catalog_fields = parse_json(stdout_stream.str());
    ASSERT_TRUE(catalog_fields) << catalog_fields.error().message;
    const auto *catalog_field_data = catalog_fields.value().find("data");
    ASSERT_NE(catalog_field_data, nullptr);
    const auto *catalog_listed = catalog_field_data->find("fields");
    ASSERT_NE(catalog_listed, nullptr);
    ASSERT_NE(catalog_listed->array_if(), nullptr);
    EXPECT_EQ(catalog_listed->array_if()->size(), listed->array_if()->size());

    const auto probe_png = (root / "probe.png").generic_string();
    stdout_stream.str({});
    stdout_stream.clear();
    ASSERT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "probe", "--catalog", catalog, "--asset-id", id, "--baseline", "--set",
                  "exposure=1", "--max-edge", "64", "--output", probe_png, "--json"}),
              0)
        << stdout_stream.str();
    auto probed_file = parse_json(stdout_stream.str());
    ASSERT_TRUE(probed_file) << probed_file.error().message;
    const auto *probe_data = probed_file.value().find("data");
    ASSERT_NE(probe_data, nullptr);
    const auto *output = probe_data->find("output");
    ASSERT_NE(output, nullptr);
    ASSERT_NE(output->string_if(), nullptr);
    EXPECT_EQ(*output->string_if(), probe_png);
    EXPECT_TRUE(std::filesystem::exists(probe_png));
    EXPECT_GT(std::filesystem::file_size(probe_png), 8U);
    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "probe", "--catalog", catalog, "--asset-id", id, "--output",
                  (root / "probe.jpg").generic_string(), "--json"}),
              2);
    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{"catalog", "probe", "--catalog",
                                                            catalog, "--asset-id", id, "--output",
                                                            probe_png, "--json"}),
              6);

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{"catalog", "probe", "--catalog",
                                                            catalog, "--asset-id", id, "--baseline",
                                                            "--set", "exposure=19", "--json"}),
              2);
    auto rejected = parse_json(stdout_stream.str());
    ASSERT_TRUE(rejected) << rejected.error().message;
    const auto *error = rejected.value().find("error");
    ASSERT_NE(error, nullptr);
    const auto *code = error->find("code");
    ASSERT_NE(code, nullptr);
    ASSERT_NE(code->string_if(), nullptr);
    EXPECT_EQ(*code->string_if(), "invalid_argument");

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{"catalog", "develop", "--catalog",
                                                            catalog, "--asset-id", id, "--set",
                                                            "exposure=19", "--json"}),
              2);
    auto rejected_save = parse_json(stdout_stream.str());
    ASSERT_TRUE(rejected_save) << rejected_save.error().message;
    error = rejected_save.value().find("error");
    ASSERT_NE(error, nullptr);
    code = error->find("code");
    ASSERT_NE(code, nullptr);
    ASSERT_NE(code->string_if(), nullptr);
    EXPECT_EQ(*code->string_if(), "invalid_argument");

    auto current_after_reject = run_probe(std::nullopt, false);
    ASSERT_TRUE(current_after_reject) << current_after_reject.error().message;
    ASSERT_TRUE(luma_mean(current_after_reject.value()));
    EXPECT_DOUBLE_EQ(*luma_mean(current_after_reject.value()), *luma_mean(current.value()));
    EXPECT_TRUE(stderr_stream.str().empty());

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_F(CliTest, RealCliColorHarmonizerDevelopSetPersistsAndRejectsInvalidInput)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-cli-harmonizer-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    const auto catalog = (root / "library.sqlite").string();
    const auto png = (std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests" /
                      "0000-nop" / "expected.png")
                         .generic_u8string();
    const QString catalog_q = QString::fromStdString(catalog);
    const QString png_q = QString::fromStdString(std::string(png.begin(), png.end()));
    struct CliRun
    {
        int exit_code = 1;
        QByteArray stdout_bytes;
        QByteArray stderr_bytes;
    };
    const auto run = [&](const QStringList &arguments) -> CliRun
    {
        QProcess process;
        process.start(QStringLiteral(RAVO_CLI_EXECUTABLE), arguments);
        EXPECT_TRUE(process.waitForStarted());
        EXPECT_TRUE(process.waitForFinished());
        return {process.exitCode(), process.readAllStandardOutput(),
                process.readAllStandardError()};
    };
    const auto recipe_json = [&](const QByteArray &stdout_bytes) -> Result<JsonValue>
    {
        auto parsed = parse_json(stdout_bytes.toStdString());
        if (!parsed)
        {
            return parsed.error();
        }
        const auto *data = parsed.value().find("data");
        const auto *recipe = data == nullptr ? nullptr : data->find("recipe");
        if (recipe == nullptr)
        {
            return make_error(ErrorCode::kValidation, "CLI recipe payload is missing");
        }
        return *recipe;
    };

    const auto created = run({QStringLiteral("catalog"), QStringLiteral("create"),
                              QStringLiteral("--path"), catalog_q, QStringLiteral("--json")});
    ASSERT_EQ(created.exit_code, 0) << created.stdout_bytes.constData();
    EXPECT_TRUE(created.stderr_bytes.isEmpty());
    const auto imported =
        run({QStringLiteral("catalog"), QStringLiteral("import"), QStringLiteral("--catalog"),
             catalog_q, QStringLiteral("--input"), png_q, QStringLiteral("--json")});
    ASSERT_EQ(imported.exit_code, 0) << imported.stdout_bytes.constData();
    const auto imported_json = parse_json(imported.stdout_bytes.toStdString());
    ASSERT_TRUE(imported_json) << imported_json.error().message;
    const auto *data = imported_json.value().find("data");
    ASSERT_NE(data, nullptr);
    const auto *items = data->find("items");
    ASSERT_NE(items, nullptr);
    ASSERT_NE(items->array_if(), nullptr);
    const auto *asset = items->array_if()->front().find("asset");
    ASSERT_NE(asset, nullptr);
    const auto *asset_id = asset->find("id");
    ASSERT_NE(asset_id, nullptr);
    ASSERT_NE(asset_id->string_if(), nullptr);
    const QString id = QString::fromStdString(*asset_id->string_if());

    const auto baseline =
        run({QStringLiteral("catalog"), QStringLiteral("recipe"), QStringLiteral("--catalog"),
             catalog_q, QStringLiteral("--asset-id"), id, QStringLiteral("--json")});
    ASSERT_EQ(baseline.exit_code, 0) << baseline.stdout_bytes.constData();
    auto baseline_recipe = recipe_json(baseline.stdout_bytes);
    ASSERT_TRUE(baseline_recipe) << baseline_recipe.error().message;
    const auto serialized_baseline = serialize_json(baseline_recipe.value());

    const auto noop =
        run({QStringLiteral("catalog"), QStringLiteral("develop"), QStringLiteral("--catalog"),
             catalog_q, QStringLiteral("--asset-id"), id, QStringLiteral("--json")});
    ASSERT_EQ(noop.exit_code, 0) << noop.stdout_bytes.constData();
    const auto after_noop =
        run({QStringLiteral("catalog"), QStringLiteral("recipe"), QStringLiteral("--catalog"),
             catalog_q, QStringLiteral("--asset-id"), id, QStringLiteral("--json")});
    ASSERT_EQ(after_noop.exit_code, 0) << after_noop.stdout_bytes.constData();
    auto after_noop_recipe = recipe_json(after_noop.stdout_bytes);
    ASSERT_TRUE(after_noop_recipe) << after_noop_recipe.error().message;
    EXPECT_EQ(serialize_json(after_noop_recipe.value()), serialized_baseline);

    const auto enabled =
        run({QStringLiteral("catalog"), QStringLiteral("develop"), QStringLiteral("--catalog"),
             catalog_q, QStringLiteral("--asset-id"), id, QStringLiteral("--set"),
             QStringLiteral("colorHarmonizerEnabled=1"), QStringLiteral("--json")});
    ASSERT_EQ(enabled.exit_code, 0) << enabled.stdout_bytes.constData();
    const auto enabled_recipe_run =
        run({QStringLiteral("catalog"), QStringLiteral("recipe"), QStringLiteral("--catalog"),
             catalog_q, QStringLiteral("--asset-id"), id, QStringLiteral("--json")});
    ASSERT_EQ(enabled_recipe_run.exit_code, 0) << enabled_recipe_run.stdout_bytes.constData();
    auto enabled_recipe = recipe_json(enabled_recipe_run.stdout_bytes);
    ASSERT_TRUE(enabled_recipe) << enabled_recipe.error().message;
    const auto enabled_text = serialize_json(enabled_recipe.value());
    EXPECT_NE(enabled_text.find("ravo.color.colorharmonizer"), std::string::npos);
    EXPECT_NE(enabled_text.find("\"pull_strength\""), std::string::npos);
    const auto enabled_again =
        run({QStringLiteral("catalog"), QStringLiteral("recipe"), QStringLiteral("--catalog"),
             catalog_q, QStringLiteral("--asset-id"), id, QStringLiteral("--json")});
    EXPECT_EQ(enabled_again.stdout_bytes, enabled_recipe_run.stdout_bytes);

    const auto edited =
        run({QStringLiteral("catalog"),    QStringLiteral("develop"),
             QStringLiteral("--catalog"),  catalog_q,
             QStringLiteral("--asset-id"), id,
             QStringLiteral("--set"),      QStringLiteral("colorHarmonizerRuleIndex=4"),
             QStringLiteral("--set"),      QStringLiteral("colorHarmonizerAnchorHueDegrees=198"),
             QStringLiteral("--set"),      QStringLiteral("colorHarmonizerPullStrength=0.82"),
             QStringLiteral("--set"),      QStringLiteral("colorHarmonizerPullWidth=1.84"),
             QStringLiteral("--set"),      QStringLiteral("colorHarmonizerSmoothing=0.5"),
             QStringLiteral("--set"),      QStringLiteral("colorHarmonizerNodeSaturation0=1.26"),
             QStringLiteral("--set"),      QStringLiteral("colorHarmonizerNodeSaturation1=0.18"),
             QStringLiteral("--set"),      QStringLiteral("colorHarmonizerNodeSaturation2=1.52"),
             QStringLiteral("--json")});
    ASSERT_EQ(edited.exit_code, 0) << edited.stdout_bytes.constData();

    const auto expect_fail = [&](const QStringList &extra)
    {
        QStringList arguments{QStringLiteral("catalog"),    QStringLiteral("develop"),
                              QStringLiteral("--catalog"),  catalog_q,
                              QStringLiteral("--asset-id"), id};
        arguments.append(extra);
        arguments.push_back(QStringLiteral("--json"));
        const auto failed = run(arguments);
        EXPECT_NE(failed.exit_code, 0) << extra.join(' ').toStdString();
        EXPECT_TRUE(failed.stderr_bytes.isEmpty()) << failed.stderr_bytes.constData();
        const auto parsed = parse_json(failed.stdout_bytes.toStdString());
        ASSERT_TRUE(parsed) << parsed.error().message;
        const auto *error = parsed.value().find("error");
        ASSERT_NE(error, nullptr);
        const auto *code = error->find("code");
        ASSERT_NE(code, nullptr);
        ASSERT_NE(code->string_if(), nullptr);
        EXPECT_EQ(*code->string_if(), "invalid_argument");
    };
    expect_fail({QStringLiteral("--set"), QStringLiteral("colorHarmonizerEnabled=1"),
                 QStringLiteral("--set"), QStringLiteral("colorHarmonizerEnabled=1")});
    expect_fail({QStringLiteral("--set"), QStringLiteral("colorHarmonizerRuleIndex=3.5")});
    expect_fail({QStringLiteral("--set"), QStringLiteral("colorHarmonizerEnabled=0.5")});
    expect_fail({QStringLiteral("--set"), QStringLiteral("colorHarmonizerPullStrength=nan")});
    expect_fail({QStringLiteral("--set"), QStringLiteral("colorHarmonizerPullStrength=inf")});
    expect_fail({QStringLiteral("--set"), QStringLiteral("colorHarmonizerPullStrength=1.5")});
    expect_fail({QStringLiteral("--set"), QStringLiteral("colorHarmonizerSmoothing=-0.01")});
    expect_fail({QStringLiteral("--set"), QStringLiteral("colorHarmonizerSmoothing=2.01")});
    expect_fail({QStringLiteral("--set"), QStringLiteral("unknownHarmonizer=1")});

    const auto after_fail =
        run({QStringLiteral("catalog"), QStringLiteral("recipe"), QStringLiteral("--catalog"),
             catalog_q, QStringLiteral("--asset-id"), id, QStringLiteral("--json")});
    ASSERT_EQ(after_fail.exit_code, 0) << after_fail.stdout_bytes.constData();
    auto after_fail_recipe = recipe_json(after_fail.stdout_bytes);
    ASSERT_TRUE(after_fail_recipe) << after_fail_recipe.error().message;
    EXPECT_NE(serialize_json(after_fail_recipe.value()).find("split_complementary"),
              std::string::npos);
    EXPECT_NE(serialize_json(after_fail_recipe.value()).find("\"smoothing\":0.5"),
              std::string::npos);

    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);
}

TEST_F(CliTest, RealCliLut3dPersistsProbesExportsAndRejectsChangedCorruptSource)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-cli-lut3d-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    const auto catalog = root / "library.sqlite";
    const auto cube = root / "red-compression.cube";
    const auto source = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests" /
                        "0000-nop" / "expected.png";
    const auto source_before = source_file_snapshot(source.string());
    ASSERT_TRUE(source_before);
    {
        std::ofstream output(cube, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output);
        output << "TITLE \"red compression\"\n"
                  "LUT_3D_SIZE 2\n"
                  "DOMAIN_MIN 0 0 0\n"
                  "DOMAIN_MAX 1 1 1\n"
                  "0 0 0\n0.2 0 0\n0 1 0\n0.2 1 0\n"
                  "0 0 1\n0.2 0 1\n0 1 1\n0.2 1 1\n";
        ASSERT_TRUE(output);
    }

    struct CliRun
    {
        int exit_code = 1;
        QByteArray stdout_bytes;
        QByteArray stderr_bytes;
    };
    const auto run = [&](const QStringList &arguments) -> CliRun
    {
        QProcess process;
        process.start(QStringLiteral(RAVO_CLI_EXECUTABLE), arguments);
        EXPECT_TRUE(process.waitForStarted());
        EXPECT_TRUE(process.waitForFinished());
        return {process.exitCode(), process.readAllStandardOutput(),
                process.readAllStandardError()};
    };
    const auto parse_data = [](const QByteArray &stdout_bytes) -> Result<JsonValue>
    {
        auto response = parse_json(stdout_bytes.toStdString());
        if (!response)
            return response.error();
        const auto *data = response.value().find("data");
        if (data == nullptr)
            return make_error(ErrorCode::kValidation, "CLI response is missing data");
        return *data;
    };
    const auto recipe_text = [&](const QString &asset_id) -> Result<std::string>
    {
        const auto response =
            run({QStringLiteral("catalog"), QStringLiteral("recipe"), QStringLiteral("--catalog"),
                 QString::fromStdString(catalog.string()), QStringLiteral("--asset-id"), asset_id,
                 QStringLiteral("--json")});
        if (response.exit_code != 0)
            return make_error(ErrorCode::kIo, "CLI recipe command failed");
        auto data = parse_data(response.stdout_bytes);
        if (!data)
            return data.error();
        const auto *recipe = data.value().find("recipe");
        if (recipe == nullptr)
            return make_error(ErrorCode::kValidation, "CLI recipe payload is missing recipe");
        return serialize_json(*recipe);
    };
    const auto probe_luma = [&](const QStringList &extra) -> Result<double>
    {
        QStringList arguments{QStringLiteral("catalog"), QStringLiteral("probe"),
                              QStringLiteral("--catalog"), QString::fromStdString(catalog.string()),
                              QStringLiteral("--asset-id")};
        arguments.append(extra);
        arguments.append(
            {QStringLiteral("--max-edge"), QStringLiteral("64"), QStringLiteral("--json")});
        const auto response = run(arguments);
        if (response.exit_code != 0)
            return make_error(ErrorCode::kIo, "CLI probe command failed",
                              {{"stdout", response.stdout_bytes.toStdString()}});
        auto data = parse_data(response.stdout_bytes);
        if (!data)
            return data.error();
        const auto *statistics = data.value().find("statistics");
        const auto *luma = statistics == nullptr ? nullptr : statistics->find("display_luma_mean");
        if (luma == nullptr || luma->number_if() == nullptr)
            return make_error(ErrorCode::kValidation, "CLI probe payload is missing luma");
        return std::stod(luma->number_if()->text);
    };

    const auto created =
        run({QStringLiteral("catalog"), QStringLiteral("create"), QStringLiteral("--path"),
             QString::fromStdString(catalog.string()), QStringLiteral("--json")});
    ASSERT_EQ(created.exit_code, 0) << created.stdout_bytes.constData();
    const auto imported =
        run({QStringLiteral("catalog"), QStringLiteral("import"), QStringLiteral("--catalog"),
             QString::fromStdString(catalog.string()), QStringLiteral("--input"),
             QString::fromStdString(source.string()), QStringLiteral("--json")});
    ASSERT_EQ(imported.exit_code, 0) << imported.stdout_bytes.constData();
    auto import_data = parse_data(imported.stdout_bytes);
    ASSERT_TRUE(import_data) << import_data.error().message;
    const auto *items = import_data.value().find("items");
    ASSERT_NE(items, nullptr);
    ASSERT_NE(items->array_if(), nullptr);
    ASSERT_EQ(items->array_if()->size(), 1U);
    const auto *asset = items->array_if()->front().find("asset");
    const auto *asset_id = asset == nullptr ? nullptr : asset->find("id");
    ASSERT_NE(asset_id, nullptr);
    ASSERT_NE(asset_id->string_if(), nullptr);
    const QString id = QString::fromStdString(*asset_id->string_if());

    auto baseline_luma = probe_luma({id, QStringLiteral("--baseline")});
    ASSERT_TRUE(baseline_luma) << baseline_luma.error().message;
    const auto developed =
        run({QStringLiteral("catalog"), QStringLiteral("develop"), QStringLiteral("--catalog"),
             QString::fromStdString(catalog.string()), QStringLiteral("--asset-id"), id,
             QStringLiteral("--set-text"),
             QStringLiteral("lut3dFile=") + QString::fromStdString(cube.string()),
             QStringLiteral("--set"), QStringLiteral("lut3dInputSpaceIndex=3"),
             QStringLiteral("--set"), QStringLiteral("lut3dOutputSpaceIndex=3"),
             QStringLiteral("--set"), QStringLiteral("lut3dInterpolationIndex=0"),
             QStringLiteral("--set"), QStringLiteral("lut3dStrength=1"), QStringLiteral("--json")});
    ASSERT_EQ(developed.exit_code, 0) << developed.stdout_bytes.constData();
    EXPECT_TRUE(developed.stderr_bytes.isEmpty());

    auto stored = recipe_text(id);
    ASSERT_TRUE(stored) << stored.error().message;
    EXPECT_NE(stored.value().find("ravo.color.lut3d"), std::string::npos);
    auto stored_recipe = parse_recipe_json(stored.value());
    ASSERT_TRUE(stored_recipe) << stored_recipe.error().message;
    auto stored_develop = develop_from_recipe(stored_recipe.value());
    ASSERT_TRUE(stored_develop) << stored_develop.error().message;
    EXPECT_EQ(stored_develop.value().lut3d.file_path, cube.string());
    EXPECT_NE(stored.value().find("linear_rec709"), std::string::npos);
    auto developed_luma = probe_luma({id});
    ASSERT_TRUE(developed_luma) << developed_luma.error().message;
    EXPECT_LT(developed_luma.value(), baseline_luma.value());

    auto zero_strength_luma =
        probe_luma({id, QStringLiteral("--set"), QStringLiteral("lut3dStrength=0")});
    ASSERT_TRUE(zero_strength_luma) << zero_strength_luma.error().message;
    EXPECT_NEAR(zero_strength_luma.value(), baseline_luma.value(), 0.01);
    auto after_probe = recipe_text(id);
    ASSERT_TRUE(after_probe) << after_probe.error().message;
    EXPECT_EQ(after_probe.value(), stored.value());

    const auto exported_path = root / "lut-export.png";
    const auto exported =
        run({QStringLiteral("catalog"), QStringLiteral("export"), QStringLiteral("--catalog"),
             QString::fromStdString(catalog.string()), QStringLiteral("--asset-id"), id,
             QStringLiteral("--output"), QString::fromStdString(exported_path.string()),
             QStringLiteral("--format"), QStringLiteral("png"), QStringLiteral("--json")});
    ASSERT_EQ(exported.exit_code, 0) << exported.stdout_bytes.constData();
    EXPECT_TRUE(std::filesystem::exists(exported_path));
    EXPECT_GT(std::filesystem::file_size(exported_path), 8U);

    {
        std::ofstream output(cube, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(output);
        output << "LUT_3D_SIZE 2\n0 0 0\n";
    }
    const auto corrupt =
        run({QStringLiteral("catalog"), QStringLiteral("probe"), QStringLiteral("--catalog"),
             QString::fromStdString(catalog.string()), QStringLiteral("--asset-id"), id,
             QStringLiteral("--max-edge"), QStringLiteral("64"), QStringLiteral("--json")});
    EXPECT_NE(corrupt.exit_code, 0);
    auto corrupt_json = parse_json(corrupt.stdout_bytes.toStdString());
    ASSERT_TRUE(corrupt_json) << corrupt_json.error().message;
    EXPECT_NE(corrupt_json.value().find("error"), nullptr);
    auto after_corrupt = recipe_text(id);
    ASSERT_TRUE(after_corrupt) << after_corrupt.error().message;
    EXPECT_EQ(after_corrupt.value(), stored.value());
    EXPECT_EQ(source_file_snapshot(source.string()), source_before);

    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);
}

} // namespace
} // namespace ravo
