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
        std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" / "frozen" / "images" / "mire1.cr2";
    const auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

[[nodiscard]] std::string mire1_xtrans_path()
{
    const auto path = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" / "frozen" / "images" /
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

TEST_F(CliTest, LegacyXmpExposureFixtureCensusPinsRevisionsSingletonsMasksAndBlendStates)
{
    const auto fixture_root = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" / "frozen";
    std::vector<std::filesystem::path> xmp_paths;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(fixture_root))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".xmp")
        {
            xmp_paths.push_back(entry.path());
        }
    }
    std::sort(xmp_paths.begin(), xmp_paths.end());
    ASSERT_EQ(xmp_paths.size(), 158U);

    struct CensusEntry
    {
        std::uint64_t history_position = 0U;
        std::string version;
        std::string enabled;
        std::string multi_priority;
        std::string multi_name;
        std::string multi_name_hand_edited;
        std::string blend_version;
        std::string blend_parameters;
        bool frozen_blend = false;
    };
    std::map<std::string, std::size_t, std::less<>> versions;
    std::map<std::string, std::size_t, std::less<>> enabled_states;
    std::map<std::string, std::size_t, std::less<>> priorities;
    std::map<std::string, std::size_t, std::less<>> names;
    std::map<std::string, std::size_t, std::less<>> hand_edited_states;
    std::map<std::string, std::size_t, std::less<>> blend_versions;
    std::map<std::uint64_t, std::size_t> history_positions;
    std::array<std::size_t, 6> records_per_file{};
    std::set<std::pair<std::string, std::string>> distinct_blends;
    std::set<std::pair<std::string, std::string>> frozen_blends;
    std::size_t exposure_records = 0U;
    std::size_t frozen_blend_records = 0U;
    std::size_t explicit_mask_attributes = 0U;
    std::size_t strictly_increasing_revision_files = 0U;
    std::size_t final_singleton_frozen_files = 0U;
    std::map<std::string, std::size_t, std::less<>> final_versions;
    std::map<std::string, std::size_t, std::less<>> final_enabled_states;

    for (const auto &path : xmp_paths)
    {
        SCOPED_TRACE(path.generic_string());
        const auto content = read_utf8_text_file(path.generic_string());
        ASSERT_TRUE(content) << content.error().message;
        const QByteArray bytes(content.value().data(),
                               static_cast<qsizetype>(content.value().size()));
        QXmlStreamReader reader(bytes);
        std::vector<CensusEntry> file_entries;
        while (!reader.atEnd())
        {
            reader.readNext();
            if (!reader.isStartElement() || reader.name() != u"li" ||
                xml_attribute_value(reader.attributes(), u"operation") != "exposure")
            {
                continue;
            }
            for (const auto &attribute : reader.attributes())
            {
                explicit_mask_attributes += attribute.name().contains(u"mask") ? 1U : 0U;
                const auto name = attribute.name();
                EXPECT_TRUE(name == u"num" || name == u"operation" || name == u"enabled" ||
                            name == u"modversion" || name == u"params" || name == u"multi_name" ||
                            name == u"multi_priority" || name == u"multi_name_hand_edited" ||
                            name == u"blendop_version" || name == u"blendop_params")
                    << name.toString().toStdString();
            }

            const auto position = xml_attribute_value(reader.attributes(), u"num");
            const auto version = xml_attribute_value(reader.attributes(), u"modversion");
            const auto enabled = xml_attribute_value(reader.attributes(), u"enabled");
            const auto priority = xml_attribute_value(reader.attributes(), u"multi_priority");
            const auto name = xml_attribute_value(reader.attributes(), u"multi_name");
            const auto hand_edited =
                xml_attribute_value(reader.attributes(), u"multi_name_hand_edited");
            const auto blend_version = xml_attribute_value(reader.attributes(), u"blendop_version");
            const auto blend_parameters =
                xml_attribute_value(reader.attributes(), u"blendop_params");
            ASSERT_TRUE(position);
            ASSERT_TRUE(version);
            ASSERT_TRUE(enabled);
            ASSERT_TRUE(priority);
            ASSERT_TRUE(name);
            ASSERT_TRUE(blend_version);
            ASSERT_TRUE(blend_parameters);
            const auto parsed_position = static_cast<std::uint64_t>(std::stoull(*position));

            LegacyExposureXmpOptions blend_probe;
            blend_probe.blend_version = *blend_version;
            blend_probe.blend_parameters = *blend_parameters;
            const auto probed =
                import_legacy_xmp({legacy_exposure_xmp(blend_probe),
                                   {"blend-census", "file:///fixture.raw", std::nullopt}});
            const bool frozen_blend = static_cast<bool>(probed);
            if (!frozen_blend)
            {
                EXPECT_EQ(probed.error().code, ErrorCode::kUnsupported);
                EXPECT_EQ(probed.error().context.at("reason"), "unsupported_legacy_exposure_blend");
            }

            file_entries.push_back({parsed_position, *version, *enabled, *priority, *name,
                                    hand_edited.value_or("<missing>"), *blend_version,
                                    *blend_parameters, frozen_blend});
            ++exposure_records;
            ++versions[*version];
            ++enabled_states[*enabled];
            ++priorities[*priority];
            ++names[*name];
            ++hand_edited_states[hand_edited.value_or("<missing>")];
            ++blend_versions[*blend_version];
            ++history_positions[parsed_position];
            distinct_blends.emplace(*blend_version, *blend_parameters);
            if (frozen_blend)
            {
                ++frozen_blend_records;
                frozen_blends.emplace(*blend_version, *blend_parameters);
            }
        }
        ASSERT_FALSE(reader.hasError()) << reader.errorString().toStdString();
        ASSERT_LT(file_entries.size(), records_per_file.size());
        ++records_per_file[file_entries.size()];
        if (file_entries.empty())
        {
            continue;
        }
        const bool increasing =
            std::adjacent_find(file_entries.begin(), file_entries.end(),
                               [](const CensusEntry &left, const CensusEntry &right)
                               { return left.history_position >= right.history_position; }) ==
            file_entries.end();
        strictly_increasing_revision_files += increasing ? 1U : 0U;
        const auto final =
            std::max_element(file_entries.begin(), file_entries.end(),
                             [](const CensusEntry &left, const CensusEntry &right)
                             { return left.history_position < right.history_position; });
        ++final_versions[final->version];
        ++final_enabled_states[final->enabled];
        const bool singleton =
            final->multi_priority == "0" && final->multi_name.empty() &&
            (final->multi_name_hand_edited == "<missing>" || final->multi_name_hand_edited == "0");
        final_singleton_frozen_files += singleton && final->frozen_blend ? 1U : 0U;
    }

    const decltype(versions) expected_versions{{"5", 5U}, {"6", 102U}, {"7", 3U}};
    const decltype(enabled_states) expected_enabled{{"0", 1U}, {"1", 109U}};
    const decltype(priorities) expected_priorities{
        {"0", 77U}, {"1", 9U}, {"2", 8U}, {"3", 8U}, {"4", 8U}};
    const decltype(names) expected_names{{"", 47U},
                                         {"1", 9U},
                                         {"2", 8U},
                                         {"3", 8U},
                                         {"4", 8U},
                                         {"Défaut pour « relatif à la scène »", 16U},
                                         {"_builtin_scene-referred default", 7U},
                                         {"scene-referred default", 7U}};
    const decltype(hand_edited_states) expected_hand_edited{{"0", 38U}, {"<missing>", 72U}};
    const decltype(blend_versions) expected_blend_versions{{"9", 19U}, {"10", 4U},  {"11", 56U},
                                                           {"12", 6U}, {"13", 18U}, {"14", 7U}};
    const decltype(history_positions) expected_positions{{6U, 1U},  {7U, 6U},   {8U, 43U},
                                                         {9U, 24U}, {10U, 13U}, {11U, 10U},
                                                         {12U, 8U}, {13U, 3U},  {14U, 2U}};
    const decltype(final_versions) expected_final_versions{{"5", 4U}, {"6", 66U}, {"7", 3U}};
    const decltype(final_enabled_states) expected_final_enabled{{"0", 1U}, {"1", 72U}};
    EXPECT_EQ(exposure_records, 110U);
    EXPECT_EQ(records_per_file, (std::array<std::size_t, 6>{85U, 61U, 3U, 1U, 0U, 8U}));
    EXPECT_EQ(versions, expected_versions);
    EXPECT_EQ(enabled_states, expected_enabled);
    EXPECT_EQ(priorities, expected_priorities);
    EXPECT_EQ(names, expected_names);
    EXPECT_EQ(hand_edited_states, expected_hand_edited);
    EXPECT_EQ(blend_versions, expected_blend_versions);
    EXPECT_EQ(history_positions, expected_positions);
    EXPECT_EQ(final_versions, expected_final_versions);
    EXPECT_EQ(final_enabled_states, expected_final_enabled);
    EXPECT_EQ(distinct_blends.size(), 50U);
    EXPECT_EQ(frozen_blends.size(), 11U);
    EXPECT_EQ(frozen_blend_records, 69U);
    EXPECT_EQ(explicit_mask_attributes, 0U);
    EXPECT_EQ(strictly_increasing_revision_files, 73U);
    EXPECT_EQ(final_singleton_frozen_files, 29U);
}

TEST_F(CliTest, LegacyXmpImportsTheFrozenRealExposureFixture)
{
    const auto path = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" / "frozen" /
                      "0001-exposure" / "exposure.xmp";
    const auto source = read_utf8_text_file(path.string());
    ASSERT_TRUE(source) << source.error().message;

    const auto imported =
        import_legacy_xmp({source.value(), {"asset-1", "file:///fixture.raw", std::nullopt}});

    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_EQ(imported.value().operations.size(), 3U);
    const auto &operation = imported.value().operations[1];
    EXPECT_EQ(operation.id, kExposureOperationId);
    EXPECT_EQ(operation.schema_version, kExposureOperationSchemaVersion);
    auto params = exposure_from_parameters(operation.parameters);
    ASSERT_TRUE(params) << params.error().message;
    EXPECT_EQ(params.value().mode, kExposureModeManual);
    EXPECT_DOUBLE_EQ(params.value().black, 0.0);
    EXPECT_DOUBLE_EQ(params.value().exposure_ev, 1.0);
}

TEST_F(CliTest, LegacyXmpImportCommandWritesTheProvenManualExposureRecipe)
{
    const auto directory = std::filesystem::temp_directory_path();
    const auto xmp_path = directory / "ravo-manual-exposure-v5.xmp";
    const auto recipe_path = directory / "ravo-manual-exposure-v5.recipe.json";
    std::error_code ignored;
    std::filesystem::remove(xmp_path, ignored);
    std::filesystem::remove(recipe_path, ignored);
    {
        std::ofstream output(xmp_path, std::ios::binary);
        ASSERT_TRUE(output);
        output << legacy_exposure_xmp();
    }

    const auto xmp_u8 = xmp_path.generic_u8string();
    const std::string xmp_argument(xmp_u8.begin(), xmp_u8.end());
    const auto recipe_u8 = recipe_path.generic_u8string();
    const std::string recipe_argument(recipe_u8.begin(), recipe_u8.end());
    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    const std::vector<std::string_view> arguments{
        "recipe",  "import-xmp",          xmp_argument, "--asset-id",    "asset-1",
        "--input", "file:///fixture.raw", "--output",   recipe_argument, "--json"};

    const int exit_code = application.run(std::span{arguments});
    const auto recipe = read_utf8_text_file(recipe_argument);
    std::filesystem::remove(xmp_path, ignored);
    std::filesystem::remove(recipe_path, ignored);

    EXPECT_EQ(exit_code, 0);
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto parsed = parse_recipe_json(recipe.value());
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_EQ(parsed.value().operations.size(), 3U);
    EXPECT_EQ(parsed.value().operations.front().id, "ravo.color.input");
    EXPECT_EQ(parsed.value().operations[1].id, "ravo.core.exposure");
    EXPECT_EQ(parsed.value().operations.back().id, "ravo.color.output");
    EXPECT_TRUE(stderr_stream.str().empty());
}

TEST_F(CliTest, LegacyXmpPreservesDeflickerAsAnExplicitDeferredContract)
{
    LegacyExposureXmpOptions options;
    options.parameters = "01000000000000000000803f00004842000080c0";
    const auto xmp = legacy_exposure_xmp(options);
    const LegacyXmpImportRequest request{xmp, {"asset-1", "file:///fixture.raw", std::nullopt}};

    const auto imported = import_legacy_xmp(request);

    ASSERT_TRUE(imported) << imported.error().message;
    auto params = exposure_from_parameters(imported.value().operations[1].parameters);
    ASSERT_TRUE(params) << params.error().message;
    EXPECT_EQ(params.value().mode, kExposureModeDeflicker);
    EXPECT_DOUBLE_EQ(params.value().deflicker_percentile, 50.0);
    EXPECT_DOUBLE_EQ(params.value().deflicker_target_ev, -4.0);
}

TEST_F(CliTest, LegacyXmpExposureRejectsMalformedVersionedPayloads)
{
    const auto expect_rejected = [&](const LegacyExposureXmpOptions &options, const ErrorCode code,
                                     const std::string_view reason)
    {
        const auto imported = import_legacy_xmp(
            {legacy_exposure_xmp(options), {"asset-1", "file:///fixture.raw", std::nullopt}});
        ASSERT_FALSE(imported);
        EXPECT_EQ(imported.error().code, code);
        EXPECT_EQ(imported.error().context.at("reason"), reason);
    };

    LegacyExposureXmpOptions unsupported_version;
    unsupported_version.version = "8";
    expect_rejected(unsupported_version, ErrorCode::kUnsupported,
                    "unsupported_legacy_exposure_version");

    LegacyExposureXmpOptions wrong_length;
    wrong_length.parameters = "00000000";
    expect_rejected(wrong_length, ErrorCode::kValidation, "invalid_legacy_exposure_parameters");

    LegacyExposureXmpOptions invalid_mode;
    invalid_mode.parameters = "02000000000000000000803f00004842000080c0";
    expect_rejected(invalid_mode, ErrorCode::kValidation, "invalid_legacy_exposure_parameters");

    LegacyExposureXmpOptions invalid_boolean;
    invalid_boolean.version = "6";
    invalid_boolean.parameters = "00000000000000000000803f00004842000080c002000000";
    expect_rejected(invalid_boolean, ErrorCode::kValidation, "invalid_legacy_exposure_parameters");

    LegacyExposureXmpOptions non_finite;
    non_finite.parameters = "00000000000000000000c07f00004842000080c0";
    expect_rejected(non_finite, ErrorCode::kValidation, "invalid_legacy_exposure_parameters");
}

TEST_F(CliTest, LegacyXmpExposureAcceptsOnlyFrozenUnmaskedSingletonState)
{
    const auto expect_reason =
        [&](const LegacyExposureXmpOptions &options, const std::string_view reason)
    {
        const auto imported = import_legacy_xmp(
            {legacy_exposure_xmp(options), {"asset-1", "file:///fixture.raw", std::nullopt}});
        ASSERT_FALSE(imported);
        EXPECT_EQ(imported.error().code, ErrorCode::kUnsupported);
        EXPECT_EQ(imported.error().context.at("reason"), reason);
    };

    LegacyExposureXmpOptions blend;
    blend.blend_parameters = "legacy-blend";
    expect_reason(blend, "unsupported_legacy_exposure_blend");

    LegacyExposureXmpOptions mask;
    mask.extra_attributes = R"( darktable:mask_id="42")";
    expect_reason(mask, "unsupported_legacy_exposure_mask");

    LegacyExposureXmpOptions priority;
    priority.multi_priority = "1";
    expect_reason(priority, "unsupported_legacy_exposure_multi_state");

    LegacyExposureXmpOptions noncanonical_priority;
    noncanonical_priority.multi_priority = "00";
    expect_reason(noncanonical_priority, "unsupported_legacy_exposure_multi_state");

    LegacyExposureXmpOptions named;
    named.multi_name = "second";
    expect_reason(named, "unsupported_legacy_exposure_multi_state");

    LegacyExposureXmpOptions unknown;
    unknown.extra_attributes = R"( darktable:unproven="1")";
    expect_reason(unknown, "unsupported_legacy_exposure_attribute");
}

TEST_F(CliTest, LegacyXmpExposureFoldsRevisionsByNumWithoutUsingDocumentOrder)
{
    LegacyExposureXmpOptions final;
    final.history_position = "11";
    final.parameters = kLegacyExposureV5ManualOne;
    LegacyExposureXmpOptions superseded;
    superseded.history_position = "9";
    superseded.parameters = "00000000000000000000003f00004842000080c0";
    const auto xmp = legacy_exposure_xmp({final, superseded});
    const LegacyXmpImportRequest request{xmp, {"asset-1", "file:///fixture.raw", std::nullopt}};

    const auto imported = import_legacy_xmp(request);

    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_EQ(imported.value().operations.size(), 3U);
    const auto &operation = imported.value().operations[1];
    EXPECT_EQ(operation.instance_id, "legacy-exposure-11");
    auto params = exposure_from_parameters(operation.parameters);
    ASSERT_TRUE(params) << params.error().message;
    EXPECT_DOUBLE_EQ(params.value().exposure_ev, 1.0);

    superseded.history_position = "11";
    const auto duplicate = import_legacy_xmp({legacy_exposure_xmp({final, superseded}),
                                              {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, ErrorCode::kConflict);
    EXPECT_EQ(duplicate.error().context.at("reason"), "duplicate_legacy_exposure_revision");
}

TEST_F(CliTest, LegacyXmpMapsSyntheticColorBalanceV3AndV4IntoTheIndependentCanonicalSchema)
{
    for (const std::string_view version : {"3", "4"})
    {
        SCOPED_TRACE(version);
        LegacyColorBalanceXmpOptions options;
        options.version = version;
        options.enabled = version == "3" ? "1" : "0";
        auto imported = import_legacy_xmp(
            {legacy_color_balance_xmp(options), {"asset-1", "file:///fixture.raw", std::nullopt}});
        ASSERT_TRUE(imported) << imported.error().message;
        ASSERT_EQ(imported.value().operations.size(), 3U);
        EXPECT_EQ(imported.value().operations.front().id, "ravo.color.input");
        EXPECT_EQ(imported.value().operations.back().id, "ravo.color.output");
        const auto &operation = imported.value().operations[1];
        EXPECT_EQ(operation.id, kColorBalanceOperationId);
        EXPECT_EQ(operation.schema_version, kColorBalanceOperationSchemaVersion);
        EXPECT_EQ(operation.instance_id, "legacy-colorbalance-15");
        EXPECT_EQ(operation.enabled, version == "3");
        EXPECT_FALSE(operation.mask_id.has_value());
        auto params = color_balance_from_parameters(operation.parameters);
        ASSERT_TRUE(params) << params.error().message;
        EXPECT_EQ(params.value().mode, kColorBalanceModeSlopeOffsetPower);
        EXPECT_EQ(params.value().lift, (std::array<double, 4>{1.0, 1.0, 1.0, 1.0}));
        EXPECT_NEAR(params.value().gamma[0], 0.9999998807907104, 1.0e-12);
        EXPECT_DOUBLE_EQ(params.value().input_saturation, 1.0);
        EXPECT_NEAR(params.value().contrast, 1.3350998163223267, 1.0e-12);
        EXPECT_NEAR(params.value().grey_fulcrum_percent, 28.163089752197266, 1.0e-12);
        EXPECT_DOUBLE_EQ(params.value().output_saturation, 1.0);
        EXPECT_EQ(operation.parameters.size(), 19U);
        EXPECT_FALSE(operation.parameters.contains("global_y"));
    }
}

TEST_F(CliTest, LegacyXmpColorBalanceRejectsEveryUnfrozenVersionPresentationAndMaskState)
{
    const auto expect_rejected = [&](const LegacyColorBalanceXmpOptions &options,
                                     const ErrorCode code, const std::string_view reason)
    {
        auto imported = import_legacy_xmp(
            {legacy_color_balance_xmp(options), {"asset-1", "file:///fixture.raw", std::nullopt}});
        ASSERT_FALSE(imported);
        EXPECT_EQ(imported.error().code, code);
        EXPECT_EQ(imported.error().context.at("reason"), reason);
    };

    for (const std::string_view version : {"1", "2", "5"})
    {
        LegacyColorBalanceXmpOptions options;
        options.version = version;
        expect_rejected(options, ErrorCode::kUnsupported,
                        "unsupported_legacy_colorbalance_version");
    }
    LegacyColorBalanceXmpOptions wrong_length;
    wrong_length.parameters = "01000000";
    expect_rejected(wrong_length, ErrorCode::kValidation, "invalid_legacy_colorbalance_parameters");

    LegacyColorBalanceXmpOptions unsupported_mode;
    unsupported_mode.parameters =
        "020000000000803f0000803f0000803f0000803f0000803f0000803f0000803f0000803f"
        "0000803f0000803f0000803f0000803f0000803f0000803f000090410000803f";
    expect_rejected(unsupported_mode, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorbalance_mode");

    LegacyColorBalanceXmpOptions nonfinite;
    nonfinite.parameters =
        "010000000000c07f0000803f0000803f0000803f0000803f0000803f0000803f0000803f"
        "0000803f0000803f0000803f0000803f0000803f0000803f000090410000803f";
    expect_rejected(nonfinite, ErrorCode::kValidation, "invalid_legacy_colorbalance_parameters");

    LegacyColorBalanceXmpOptions invalid_enabled;
    invalid_enabled.enabled = "2";
    expect_rejected(invalid_enabled, ErrorCode::kValidation,
                    "invalid_legacy_colorbalance_parameters");

    for (const std::string_view priority : {"1", "00"})
    {
        LegacyColorBalanceXmpOptions options;
        options.multi_priority = priority;
        expect_rejected(options, ErrorCode::kUnsupported,
                        "unsupported_legacy_colorbalance_multi_state");
    }
    LegacyColorBalanceXmpOptions named;
    named.multi_name = "second";
    expect_rejected(named, ErrorCode::kUnsupported, "unsupported_legacy_colorbalance_multi_state");
    LegacyColorBalanceXmpOptions hand_edited;
    hand_edited.multi_name_hand_edited = "1";
    expect_rejected(hand_edited, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorbalance_multi_state");

    LegacyColorBalanceXmpOptions custom_blend;
    custom_blend.blend_parameters = "gz11eJxjZGBgYGYAgQVODGiAEV0AJ2iwh+CRyscOALejGMg=";
    expect_rejected(custom_blend, ErrorCode::kUnsupported, "unsupported_legacy_colorbalance_blend");
    LegacyColorBalanceXmpOptions parametric_mask;
    parametric_mask.blend_parameters =
        "gz06eJxjZWBgYGYAgRNODFDAyASlGfADjrXTbE8t2m/rW1Brz8DQYI+QaRgQvqYrl/3DZH77p/"
        "8r7OfK1dHRfuwAALvfIn8=";
    expect_rejected(parametric_mask, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorbalance_mask");
    LegacyColorBalanceXmpOptions explicit_mask;
    explicit_mask.extra_attributes = R"( darktable:mask_id="42")";
    expect_rejected(explicit_mask, ErrorCode::kUnsupported, "unsupported_legacy_colorbalance_mask");
    LegacyColorBalanceXmpOptions unknown;
    unknown.extra_attributes = R"( darktable:unproven="1")";
    expect_rejected(unknown, ErrorCode::kUnsupported, "unsupported_legacy_colorbalance_attribute");

    LegacyColorBalanceXmpOptions duplicate;
    auto duplicate_result = import_legacy_xmp({legacy_color_balance_xmp({duplicate, duplicate}),
                                               {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_FALSE(duplicate_result);
    EXPECT_EQ(duplicate_result.error().code, ErrorCode::kConflict);
    EXPECT_EQ(duplicate_result.error().context.at("reason"), "duplicate_legacy_colorbalance");
}

TEST_F(CliTest, LegacyXmpColorBalanceRealFixtureCensusIsNegativeAndSeparateFromSyntheticSupport)
{
    const auto fixture_root = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" / "frozen";
    std::vector<std::filesystem::path> xmp_paths;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(fixture_root))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".xmp")
        {
            xmp_paths.push_back(entry.path());
        }
    }
    std::sort(xmp_paths.begin(), xmp_paths.end());
    ASSERT_EQ(xmp_paths.size(), 158U);

    std::map<std::string, std::size_t, std::less<>> versions;
    std::map<std::string, std::size_t, std::less<>> enabled;
    std::map<std::string, std::size_t, std::less<>> priorities;
    std::map<std::string, std::size_t, std::less<>> names;
    std::map<std::string, std::size_t, std::less<>> hand_edited;
    std::map<std::string, std::size_t, std::less<>> blend_versions;
    std::map<std::string, std::size_t, std::less<>> history_positions;
    std::set<std::string, std::less<>> distinct_blends;
    std::vector<std::filesystem::path> color_balance_files;
    std::size_t record_count = 0U;
    std::size_t explicit_mask_attributes = 0U;
    for (const auto &path : xmp_paths)
    {
        const auto content = read_utf8_text_file(path.generic_string());
        ASSERT_TRUE(content) << content.error().message;
        QXmlStreamReader reader(
            QByteArray(content.value().data(), static_cast<qsizetype>(content.value().size())));
        std::size_t file_records = 0U;
        while (!reader.atEnd())
        {
            reader.readNext();
            if (!reader.isStartElement() || reader.name() != u"li" ||
                xml_attribute_value(reader.attributes(), u"operation") != "colorbalance")
            {
                continue;
            }
            ++record_count;
            ++file_records;
            for (const auto &attribute : reader.attributes())
            {
                explicit_mask_attributes += attribute.name().contains(u"mask") ? 1U : 0U;
            }
            ++versions[xml_attribute_value(reader.attributes(), u"modversion")
                           .value_or("<missing>")];
            ++enabled[xml_attribute_value(reader.attributes(), u"enabled").value_or("<missing>")];
            ++priorities[xml_attribute_value(reader.attributes(), u"multi_priority")
                             .value_or("<missing>")];
            ++names[xml_attribute_value(reader.attributes(), u"multi_name").value_or("<missing>")];
            ++hand_edited[xml_attribute_value(reader.attributes(), u"multi_name_hand_edited")
                              .value_or("<missing>")];
            const auto blend_version =
                xml_attribute_value(reader.attributes(), u"blendop_version").value_or("<missing>");
            const auto blend =
                xml_attribute_value(reader.attributes(), u"blendop_params").value_or("<missing>");
            ++blend_versions[blend_version];
            ++history_positions[xml_attribute_value(reader.attributes(), u"num")
                                    .value_or("<missing>")];
            distinct_blends.insert(blend_version + ":" + blend);
        }
        ASSERT_FALSE(reader.hasError()) << path << ": " << reader.errorString().toStdString();
        if (file_records != 0U)
        {
            EXPECT_EQ(file_records, 2U) << path;
            color_balance_files.push_back(path);
            auto imported = import_legacy_xmp(
                {content.value(), {"fixture", "file:///fixture.raw", std::nullopt}});
            ASSERT_FALSE(imported) << path;
            EXPECT_EQ(imported.error().code, ErrorCode::kUnsupported) << path;
            // These whole-history fixtures remain negative independently of the synthetic
            // v3/v4 positives: earlier unfrozen operations may reject before Color Balance.
        }
    }
    EXPECT_EQ(color_balance_files.size(), 2U);
    EXPECT_EQ(record_count, 4U);
    EXPECT_EQ(versions, (decltype(versions){{"3", 4U}}));
    EXPECT_EQ(enabled, (decltype(enabled){{"1", 4U}}));
    EXPECT_EQ(priorities, (decltype(priorities){{"0", 2U}, {"1", 2U}}));
    EXPECT_EQ(names, (decltype(names){{"", 2U}, {"1", 2U}}));
    EXPECT_EQ(hand_edited, (decltype(hand_edited){{"<missing>", 4U}}));
    EXPECT_EQ(blend_versions, (decltype(blend_versions){{"9", 4U}}));
    EXPECT_EQ(history_positions, (decltype(history_positions){{"15", 2U}, {"16", 2U}}));
    EXPECT_EQ(distinct_blends.size(), 4U);
    EXPECT_EQ(explicit_mask_attributes, 0U);
    EXPECT_NE(color_balance_files[0].generic_string().find("0033-blending-modes-uniform"),
              std::string::npos);
    EXPECT_NE(color_balance_files[1].generic_string().find("0034-blending-modes-parametric"),
              std::string::npos);
}

TEST_F(CliTest, LegacyXmpMapsTheVerbatimColorContrastV2RecordAndSyntheticV1Upgrade)
{
    auto imported = import_legacy_xmp(
        {legacy_color_contrast_xmp(), {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_EQ(imported.value().operations.size(), 3U);
    EXPECT_EQ(imported.value().operations.front().id, "ravo.color.input");
    EXPECT_EQ(imported.value().operations.back().id, "ravo.color.output");
    const auto &operation = imported.value().operations[1];
    EXPECT_EQ(operation.id, kColorContrastOperationId);
    EXPECT_EQ(operation.schema_version, kColorContrastOperationSchemaVersion);
    EXPECT_EQ(operation.instance_id, "legacy-colorcontrast-9");
    EXPECT_TRUE(operation.enabled);
    EXPECT_FALSE(operation.mask_id.has_value());
    auto params = color_contrast_from_parameters(operation.parameters);
    ASSERT_TRUE(params) << params.error().message;
    EXPECT_EQ(params.value(), (ColorContrastParams{2.5999999046325684, 0.0, 2.5, 0.0, true}));

    LegacyColorContrastXmpOptions v1_options;
    v1_options.history_position = "42";
    v1_options.version = "1";
    v1_options.parameters = kLegacyColorContrastV1Parameters;
    imported = import_legacy_xmp(
        {legacy_color_contrast_xmp(v1_options), {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_EQ(imported.value().operations.size(), 3U);
    EXPECT_EQ(imported.value().operations[1].instance_id, "legacy-colorcontrast-42");
    params = color_contrast_from_parameters(imported.value().operations[1].parameters);
    ASSERT_TRUE(params) << params.error().message;
    EXPECT_EQ(params.value(), (ColorContrastParams{2.5999999046325684, 0.0, 2.5, 0.0, false}));
}

TEST_F(CliTest, LegacyXmpColorContrastRejectsUnfrozenPresentationAndMalformedData)
{
    const auto expect_rejected = [&](const LegacyColorContrastXmpOptions &options,
                                     const ErrorCode code, const std::string_view reason)
    {
        auto imported = import_legacy_xmp(
            {legacy_color_contrast_xmp(options), {"asset-1", "file:///fixture.raw", std::nullopt}});
        ASSERT_FALSE(imported);
        EXPECT_EQ(imported.error().code, code);
        EXPECT_EQ(imported.error().context.at("reason"), reason);
    };

    LegacyColorContrastXmpOptions unsupported_version;
    unsupported_version.version = "3";
    expect_rejected(unsupported_version, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorcontrast_version");
    for (const std::string_view enabled_value : {"0", "2"})
    {
        LegacyColorContrastXmpOptions disabled;
        disabled.enabled = enabled_value;
        expect_rejected(disabled, ErrorCode::kUnsupported,
                        "unsupported_legacy_colorcontrast_enabled_state");
    }
    for (const std::string_view priority : {"1", "00"})
    {
        LegacyColorContrastXmpOptions multi;
        multi.multi_priority = priority;
        expect_rejected(multi, ErrorCode::kUnsupported,
                        "unsupported_legacy_colorcontrast_multi_state");
    }
    LegacyColorContrastXmpOptions named;
    named.multi_name = "second";
    expect_rejected(named, ErrorCode::kUnsupported, "unsupported_legacy_colorcontrast_multi_state");
    LegacyColorContrastXmpOptions hand_edited;
    hand_edited.multi_name_hand_edited = "1";
    expect_rejected(hand_edited, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorcontrast_multi_state");
    LegacyColorContrastXmpOptions custom_blend;
    custom_blend.blend_parameters = kLegacyGammaBlendV9;
    expect_rejected(custom_blend, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorcontrast_blend");
    LegacyColorContrastXmpOptions explicit_mask;
    explicit_mask.extra_attributes = R"( darktable:mask_id="42")";
    expect_rejected(explicit_mask, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorcontrast_mask");
    LegacyColorContrastXmpOptions unknown;
    unknown.extra_attributes = R"( darktable:unproven="1")";
    expect_rejected(unknown, ErrorCode::kUnsupported, "unsupported_legacy_colorcontrast_attribute");

    LegacyColorContrastXmpOptions wrong_length;
    wrong_length.parameters = "00000000";
    expect_rejected(wrong_length, ErrorCode::kValidation,
                    "invalid_legacy_colorcontrast_parameters");
    LegacyColorContrastXmpOptions nonfinite;
    nonfinite.parameters = "0000807f00000000000020400000000001000000";
    expect_rejected(nonfinite, ErrorCode::kValidation, "invalid_legacy_colorcontrast_parameters");
    LegacyColorContrastXmpOptions invalid_unbound;
    invalid_unbound.parameters = "6666264000000000000020400000000002000000";
    expect_rejected(invalid_unbound, ErrorCode::kValidation,
                    "invalid_legacy_colorcontrast_parameters");

    LegacyColorContrastXmpOptions duplicate;
    auto duplicate_result = import_legacy_xmp({legacy_color_contrast_xmp({duplicate, duplicate}),
                                               {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_FALSE(duplicate_result);
    EXPECT_EQ(duplicate_result.error().code, ErrorCode::kConflict);
    EXPECT_EQ(duplicate_result.error().context.at("reason"), "duplicate_legacy_colorcontrast");
}

TEST_F(CliTest, LegacyXmpColorContrastCensusPinsTheRealRecordAndFullDocumentNegative)
{
    const auto fixture_root = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" / "frozen";
    std::vector<std::filesystem::path> xmp_paths;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(fixture_root))
    {
        if (entry.is_regular_file() && entry.path().extension() == ".xmp")
        {
            xmp_paths.push_back(entry.path());
        }
    }
    std::sort(xmp_paths.begin(), xmp_paths.end());
    ASSERT_EQ(xmp_paths.size(), 158U);

    std::size_t records = 0U;
    std::optional<std::filesystem::path> record_path;
    std::optional<std::string> record_document;
    for (const auto &path : xmp_paths)
    {
        const auto content = read_utf8_text_file(path.generic_string());
        ASSERT_TRUE(content) << content.error().message;
        QXmlStreamReader reader(
            QByteArray(content.value().data(), static_cast<qsizetype>(content.value().size())));
        while (!reader.atEnd())
        {
            reader.readNext();
            if (!reader.isStartElement() || reader.name() != u"li" ||
                xml_attribute_value(reader.attributes(), u"operation") != "colorcontrast")
            {
                continue;
            }
            ++records;
            record_path = path;
            record_document = content.value();
            EXPECT_EQ(xml_attribute_value(reader.attributes(), u"num"), "9");
            EXPECT_EQ(xml_attribute_value(reader.attributes(), u"modversion"), "2");
            EXPECT_EQ(xml_attribute_value(reader.attributes(), u"enabled"), "1");
            EXPECT_EQ(xml_attribute_value(reader.attributes(), u"params"),
                      kLegacyColorContrastV2Parameters);
            EXPECT_EQ(xml_attribute_value(reader.attributes(), u"multi_priority"), "0");
            EXPECT_EQ(xml_attribute_value(reader.attributes(), u"multi_name"), "");
            EXPECT_EQ(xml_attribute_value(reader.attributes(), u"multi_name_hand_edited"),
                      std::nullopt);
            EXPECT_EQ(xml_attribute_value(reader.attributes(), u"blendop_version"), "10");
            EXPECT_EQ(xml_attribute_value(reader.attributes(), u"blendop_params"),
                      kLegacyColorContrastDefaultBlend);
        }
        ASSERT_FALSE(reader.hasError()) << path << ": " << reader.errorString().toStdString();
    }
    ASSERT_EQ(records, 1U);
    ASSERT_TRUE(record_path.has_value());
    ASSERT_TRUE(record_document.has_value());
    EXPECT_NE(record_path->generic_string().find("0038-colorcontrast"), std::string::npos);
    auto imported =
        import_legacy_xmp({*record_document, {"fixture", "file:///fixture.raw", std::nullopt}});
    ASSERT_FALSE(imported);
    EXPECT_EQ(imported.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(imported.error().context.at("reason"), "unsupported_legacy_mask");
}

TEST_F(CliTest, LegacyXmpMapsTheVerbatimColorCheckerV2RecordAndSyntheticV1Upgrade)
{
    auto imported = import_legacy_xmp(
        {legacy_color_checker_xmp(), {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_EQ(imported.value().operations.size(), 3U);
    EXPECT_EQ(imported.value().operations.front().id, "ravo.color.input");
    EXPECT_EQ(imported.value().operations.back().id, "ravo.color.output");
    const auto &operation = imported.value().operations[1];
    EXPECT_EQ(operation.id, kColorCheckerOperationId);
    EXPECT_EQ(operation.schema_version, kColorCheckerOperationSchemaVersion);
    EXPECT_EQ(operation.instance_id, "legacy-colorchecker-8");
    EXPECT_TRUE(operation.enabled);
    EXPECT_FALSE(operation.mask_id.has_value());
    auto params = color_checker_from_parameters(operation.parameters);
    ASSERT_TRUE(params) << params.error().message;
    ASSERT_EQ(params.value().patches.size(), kColorCheckerDefaultPatchCount);
    ColorCheckerParams expected_v2;
    expected_v2.patches[7].target_lab = {92.74998474121094, 97.59593200683594, 82.81928253173828};
    expected_v2.patches[19].target_lab = {72.97999572753906, 43.90998840332031, 35.799983978271484};
    expected_v2.patches[22].target_lab = {45.439998626708984, -0.41999998688697815,
                                          59.32999801635742};
    EXPECT_EQ(params.value(), expected_v2);

    ColorCheckerParams synthetic_v1;
    synthetic_v1.patches[0].target_lab = {41.25, -12.5, 7.75};
    const auto v1_payload = legacy_color_checker_v1_hex(synthetic_v1);
    LegacyColorCheckerXmpOptions v1_options;
    v1_options.version = "1";
    v1_options.parameters = v1_payload;
    imported = import_legacy_xmp(
        {legacy_color_checker_xmp(v1_options), {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(imported) << imported.error().message;
    params = color_checker_from_parameters(imported.value().operations[1].parameters);
    ASSERT_TRUE(params) << params.error().message;
    ASSERT_EQ(params.value().patches.size(), kColorCheckerDefaultPatchCount);
    EXPECT_EQ(params.value().patches[0].source_lab,
              (std::array<double, 3>{39.189998626708984, 13.760000228881836, 14.289999961853027}));
    EXPECT_EQ(params.value().patches[0].target_lab, (std::array<double, 3>{41.25, -12.5, 7.75}));
}


} // namespace
} // namespace ravo
