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

TEST_F(CliTest, VersionJsonUsesTheVersionedEnvelopeAndNoStderrLogs)
{
    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    const std::vector<std::string_view> arguments{"--version", "--json"};

    EXPECT_EQ(application.run(std::span{arguments}), 0);
    const std::string expected =
        R"({"data":{"name":"Ravo","protocol":"ravo-cli/v1","version":")" RAVO_TEST_VERSION
        R"("},"diagnostics":[],"ok":true,"type":"ravo.cli.result","version":1})"
        "\n";
    EXPECT_EQ(stdout_stream.str(), expected);
    EXPECT_TRUE(stderr_stream.str().empty());
}

TEST_F(CliTest, RealCliJsonStdoutContainsOnlyTheProtocolEnvelope)
{
    QProcess process;
    process.start(QStringLiteral(RAVO_CLI_EXECUTABLE),
                  {QStringLiteral("--version"), QStringLiteral("--json")});
    ASSERT_TRUE(process.waitForStarted());
    ASSERT_TRUE(process.waitForFinished());
    EXPECT_EQ(process.exitStatus(), QProcess::NormalExit);
    EXPECT_EQ(process.exitCode(), 0);
    const QByteArray stdout_bytes = process.readAllStandardOutput();
    const QByteArray stderr_bytes = process.readAllStandardError();
    const auto parsed = parse_json(stdout_bytes.toStdString());
    ASSERT_TRUE(parsed) << parsed.error().message << " stdout=" << stdout_bytes.constData();
    const auto *ok = parsed.value().find("ok");
    ASSERT_NE(ok, nullptr);
    ASSERT_NE(ok->boolean_if(), nullptr);
    EXPECT_TRUE(*ok->boolean_if());
    EXPECT_TRUE(stderr_bytes.isEmpty()) << stderr_bytes.constData();
}

TEST_F(CliTest, RealCliImportDoesNotLeakOptionalXmpDiagnosticsToStderr)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-cli-xmp-log-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    const auto source = root / "with-xmp.jpg";
    QImage image(8, 6, QImage::Format_RGB888);
    image.fill(QColor(40, 80, 120));
    ASSERT_TRUE(image.save(QString::fromStdString(source.string()), "JPEG", 90));

    std::ifstream input(source, std::ios::binary);
    ASSERT_TRUE(input);
    std::vector<std::uint8_t> jpeg{std::istreambuf_iterator<char>(input),
                                   std::istreambuf_iterator<char>()};
    ASSERT_GE(jpeg.size(), 2U);
    ASSERT_EQ(jpeg[0], 0xffU);
    ASSERT_EQ(jpeg[1], 0xd8U);
    std::string xmp = "http://ns.adobe.com/xap/1.0/";
    xmp.push_back('\0');
    xmp += R"(<?xpacket begin=""?><x:xmpmeta xmlns:x="adobe:ns:meta/"/><?xpacket end="w"?>)";
    ASSERT_LT(xmp.size() + 2U, 65536U);
    const auto segment_size = static_cast<std::uint16_t>(xmp.size() + 2U);
    std::vector<std::uint8_t> with_xmp;
    with_xmp.reserve(jpeg.size() + xmp.size() + 4U);
    with_xmp.insert(with_xmp.end(), jpeg.begin(), jpeg.begin() + 2);
    with_xmp.push_back(0xffU);
    with_xmp.push_back(0xe1U);
    with_xmp.push_back(static_cast<std::uint8_t>(segment_size >> 8U));
    with_xmp.push_back(static_cast<std::uint8_t>(segment_size));
    with_xmp.insert(with_xmp.end(), xmp.begin(), xmp.end());
    with_xmp.insert(with_xmp.end(), jpeg.begin() + 2, jpeg.end());
    std::ofstream output(source, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output);
    output.write(reinterpret_cast<const char *>(with_xmp.data()),
                 static_cast<std::streamsize>(with_xmp.size()));
    output.close();
    ASSERT_TRUE(output);

    const auto run = [](const QStringList &arguments)
    {
        QProcess process;
        process.start(QStringLiteral(RAVO_CLI_EXECUTABLE), arguments);
        EXPECT_TRUE(process.waitForStarted());
        EXPECT_TRUE(process.waitForFinished());
        return std::tuple{process.exitCode(), process.readAllStandardOutput(),
                          process.readAllStandardError()};
    };
    const QString catalog = QString::fromStdString((root / "library.sqlite").string());
    const auto [create_code, create_stdout, create_stderr] =
        run({QStringLiteral("catalog"), QStringLiteral("create"), QStringLiteral("--path"), catalog,
             QStringLiteral("--json")});
    ASSERT_EQ(create_code, 0) << create_stdout.constData();
    EXPECT_TRUE(create_stderr.isEmpty()) << create_stderr.constData();
    const auto [import_code, import_stdout, import_stderr] =
        run({QStringLiteral("catalog"), QStringLiteral("import"), QStringLiteral("--catalog"),
             catalog, QStringLiteral("--input"), QString::fromStdString(source.string()),
             QStringLiteral("--json")});
    ASSERT_EQ(import_code, 0) << import_stdout.constData();
    EXPECT_TRUE(import_stderr.isEmpty()) << import_stderr.constData();
    auto imported = parse_json(import_stdout.toStdString());
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_NE(imported.value().find("ok"), nullptr);
    ASSERT_NE(imported.value().find("ok")->boolean_if(), nullptr);
    EXPECT_TRUE(*imported.value().find("ok")->boolean_if());

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_F(CliTest, OperationsJsonContainsTheReservedDescriptors)
{
    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    const std::vector<std::string_view> arguments{"operations", "--json"};

    EXPECT_EQ(application.run(std::span{arguments}), 0);
    const auto response = parse_json(stdout_stream.str());
    ASSERT_TRUE(response) << response.error().message;
    const auto *data = response.value().find("data");
    ASSERT_NE(data, nullptr);
    const auto *operations = data->find("operations");
    ASSERT_NE(operations, nullptr);
    ASSERT_NE(operations->array_if(), nullptr);
    EXPECT_EQ(operations->array_if()->size(), kPhase1OperationCount);
    EXPECT_NE(operations->array_if()->end(),
              std::find_if(operations->array_if()->begin(), operations->array_if()->end(),
                           [](const JsonValue &operation)
                           {
                               const auto *id = operation.find("id");
                               return id != nullptr && id->string_if() != nullptr &&
                                      *id->string_if() == "ravo.color.channelmixerrgb";
                           }));
    EXPECT_NE(operations->array_if()->end(),
              std::find_if(operations->array_if()->begin(), operations->array_if()->end(),
                           [](const JsonValue &operation)
                           {
                               const auto *id = operation.find("id");
                               return id != nullptr && id->string_if() != nullptr &&
                                      *id->string_if() == "ravo.color.colorchecker";
                           }));
    EXPECT_NE(operations->array_if()->end(),
              std::find_if(operations->array_if()->begin(), operations->array_if()->end(),
                           [](const JsonValue &operation)
                           {
                               const auto *id = operation.find("id");
                               return id != nullptr && id->string_if() != nullptr &&
                                      *id->string_if() == "ravo.color.temperature";
                           }));
    EXPECT_EQ(operations->array_if()->end(),
              std::find_if(operations->array_if()->begin(), operations->array_if()->end(),
                           [](const JsonValue &operation)
                           {
                               const auto *id = operation.find("id");
                               return id != nullptr && id->string_if() != nullptr &&
                                      *id->string_if() == "ravo.color.white_balance";
                           }));
    EXPECT_NE(operations->array_if()->end(),
              std::find_if(operations->array_if()->begin(), operations->array_if()->end(),
                           [](const JsonValue &operation)
                           {
                               const auto *id = operation.find("id");
                               return id != nullptr && id->string_if() != nullptr &&
                                      *id->string_if() == "ravo.color.colorbalancergb";
                           }));
    EXPECT_NE(operations->array_if()->end(),
              std::find_if(operations->array_if()->begin(), operations->array_if()->end(),
                           [](const JsonValue &operation)
                           {
                               const auto *id = operation.find("id");
                               return id != nullptr && id->string_if() != nullptr &&
                                      *id->string_if() == "ravo.color.colorbalance";
                           }));
    EXPECT_NE(operations->array_if()->end(),
              std::find_if(operations->array_if()->begin(), operations->array_if()->end(),
                           [](const JsonValue &operation)
                           {
                               const auto *id = operation.find("id");
                               return id != nullptr && id->string_if() != nullptr &&
                                      *id->string_if() == kColorCorrectionOperationId;
                           }));
    const auto color_contrast =
        std::find_if(operations->array_if()->begin(), operations->array_if()->end(),
                     [](const JsonValue &operation)
                     {
                         const auto *id = operation.find("id");
                         return id != nullptr && id->string_if() != nullptr &&
                                *id->string_if() == kColorContrastOperationId;
                     });
    ASSERT_NE(color_contrast, operations->array_if()->end());
    const auto *schema = color_contrast->find("parameter_schema_version");
    ASSERT_NE(schema, nullptr);
    ASSERT_NE(schema->number_if(), nullptr);
    EXPECT_EQ(schema->number_if()->text, std::to_string(kColorContrastOperationSchemaVersion));
    EXPECT_TRUE(stderr_stream.str().empty());
}

TEST_F(CliTest, PerspectiveAnalysisIsStructuredReadOnlyAndRejectsUnknownMode)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-cli-perspective-" + generate_catalog_id());
    ASSERT_TRUE(std::filesystem::create_directories(root));
    const auto input = root / "grid.png";
    ASSERT_TRUE(write_perspective_grid_png(input));
    const auto before = source_file_snapshot(input.string());
    ASSERT_TRUE(before.has_value());

    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    const std::string input_string = input.generic_string();
    const std::vector<std::string_view> arguments{"perspective", "analyze", input_string,
                                                  "--mode",      "full",    "--json"};
    EXPECT_EQ(application.run(std::span{arguments}), 0) << stderr_stream.str();
    const auto response = parse_json(stdout_stream.str());
    ASSERT_TRUE(response) << response.error().message;
    const auto *data = response.value().find("data");
    ASSERT_NE(data, nullptr);
    ASSERT_NE(data->find("algorithm"), nullptr);
    EXPECT_EQ(*data->find("algorithm")->string_if(), "bounded_hough_robust_fit_v1");
    ASSERT_NE(data->find("lines"), nullptr);
    EXPECT_GE(data->find("lines")->array_if()->size(), 4U);
    for (const auto &line : *data->find("lines")->array_if())
    {
        for (const std::string_view coordinate : {"x1", "x2", "y1", "y2"})
        {
            ASSERT_NE(line.find(coordinate), nullptr);
            ASSERT_NE(line.find(coordinate)->number_if(), nullptr);
            const double value = std::stod(line.find(coordinate)->number_if()->text);
            EXPECT_GE(value, 0.0);
            EXPECT_LE(value, 1.0);
        }
    }
    EXPECT_EQ(source_file_snapshot(input.string()), before);

    std::ostringstream bad_stdout;
    std::ostringstream bad_stderr;
    const CliApplication bad_application(engine, bad_stdout, bad_stderr);
    const std::vector<std::string_view> bad_arguments{"perspective", "analyze",  input_string,
                                                      "--mode",      "diagonal", "--json"};
    EXPECT_NE(bad_application.run(std::span{bad_arguments}), 0);
    const auto bad_response = parse_json(bad_stdout.str());
    ASSERT_TRUE(bad_response) << bad_response.error().message;
    ASSERT_NE(bad_response.value().find("error"), nullptr);
    EXPECT_EQ(*bad_response.value().find("error")->find("code")->string_if(), "invalid_argument");
    EXPECT_TRUE(bad_stderr.str().empty());
}

TEST_F(CliTest, InspectCommandReturnsFrozenRawMetadata)
{
    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    const auto input_argument = mire1_path();
    const std::vector<std::string_view> arguments{"inspect", input_argument, "--json"};

    EXPECT_EQ(application.run(std::span{arguments}), 0);
    const auto response = parse_json(stdout_stream.str());
    ASSERT_TRUE(response) << response.error().message;
    const auto *data = response.value().find("data");
    ASSERT_NE(data, nullptr);
    const auto *raw = data->find("is_raw");
    ASSERT_NE(raw, nullptr);
    ASSERT_NE(raw->boolean_if(), nullptr);
    EXPECT_TRUE(*raw->boolean_if());
    ASSERT_NE(data->find("raw_sensor"), nullptr);
    EXPECT_EQ(*data->find("raw_sensor")->string_if(), "bayer");
    ASSERT_NE(data->find("cfa_width"), nullptr);
    EXPECT_EQ(data->find("cfa_width")->number_if()->text, "2");
    ASSERT_NE(data->find("cfa_height"), nullptr);
    EXPECT_EQ(data->find("cfa_height")->number_if()->text, "2");
    ASSERT_NE(data->find("default_demosaic_mode"), nullptr);
    EXPECT_EQ(*data->find("default_demosaic_mode")->string_if(), "rcd");
    const auto *as_shot = data->find("has_as_shot_white_balance");
    ASSERT_NE(as_shot, nullptr);
    ASSERT_NE(as_shot->boolean_if(), nullptr);
    EXPECT_TRUE(*as_shot->boolean_if());
    EXPECT_TRUE(stderr_stream.str().empty());
}

TEST_F(CliTest, InspectCommandReportsXTransCfaAndDefaultDemosaic)
{
    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    const auto input = mire1_xtrans_path();
    const std::vector<std::string_view> arguments{"inspect", input, "--json"};
    ASSERT_EQ(application.run(std::span{arguments}), 0) << stdout_stream.str();
    auto response = parse_json(stdout_stream.str());
    ASSERT_TRUE(response) << response.error().message;
    const auto *data = response.value().find("data");
    ASSERT_NE(data, nullptr);
    ASSERT_NE(data->find("raw_sensor"), nullptr);
    EXPECT_EQ(*data->find("raw_sensor")->string_if(), "xtrans");
    EXPECT_EQ(data->find("cfa_width")->number_if()->text, "6");
    EXPECT_EQ(data->find("cfa_height")->number_if()->text, "6");
    EXPECT_EQ(*data->find("default_demosaic_mode")->string_if(), "markesteijn3");
    EXPECT_TRUE(stderr_stream.str().empty());
}

TEST_F(CliTest, RenderCommandUsesItsInputAndWritesBoundedPngFromCanonicalRecipe)
{
    const auto directory = std::filesystem::temp_directory_path();
    const auto recipe_path = directory / "ravo-cli-render.recipe.json";
    const auto output_path = directory / "ravo-cli-render.png";
    std::error_code ignored;
    std::filesystem::remove(recipe_path, ignored);
    std::filesystem::remove(output_path, ignored);
    const auto input_argument = mire1_path();
    const auto source_before = source_file_snapshot(input_argument);
    ASSERT_TRUE(source_before.has_value());

    Recipe recipe;
    recipe.asset = {"mire1", "file:///recipe-placeholder.raw", std::nullopt};
    ProfileGammaParams profile_gamma;
    profile_gamma.mode = std::string(kProfileGammaModeGamma);
    profile_gamma.linear = 0.08;
    profile_gamma.gamma = 0.55;
    auto profile_gamma_parameters = profile_gamma_to_parameters(profile_gamma);
    ASSERT_TRUE(profile_gamma_parameters) << profile_gamma_parameters.error().message;
    recipe.operations.push_back({std::string(kProfileGammaOperationId),
                                 kProfileGammaOperationSchemaVersion, "profilegamma-1", true,
                                 std::move(profile_gamma_parameters).value(), std::nullopt});
    recipe.operations.push_back({"ravo.color.input", 1, "color-input-1", true,
                                 input_color_to_parameters(InputColorParams{}), std::nullopt});
    PrimariesParams primaries;
    primaries.red_hue = 0.18;
    primaries.red_purity = 1.15;
    recipe.operations.push_back({std::string(kPrimariesOperationId), 1, "primaries-1", true,
                                 primaries_to_parameters(primaries), std::nullopt});
    ExposureParams exposure;
    exposure.mode = std::string(kExposureModeDeflicker);
    exposure.black = -0.01;
    exposure.compensate_exposure_bias = true;
    exposure.compensate_highlight_preservation = true;
    recipe.operations.push_back({std::string(kExposureOperationId), kExposureOperationSchemaVersion,
                                 "exposure-deflicker-1", true, exposure_to_parameters(exposure),
                                 std::nullopt});
    ColorCheckerParams color_checker;
    color_checker.patches[7].target_lab = {92.74998474121094, 97.59593200683594, 82.81928253173828};
    auto color_checker_parameters = color_checker_to_parameters(color_checker);
    ASSERT_TRUE(color_checker_parameters) << color_checker_parameters.error().message;
    recipe.operations.push_back({std::string(kColorCheckerOperationId),
                                 kColorCheckerOperationSchemaVersion, "colorchecker-1", true,
                                 std::move(color_checker_parameters).value(), std::nullopt});
    ColorBalanceParams color_balance;
    color_balance.mode = std::string(kColorBalanceModeLiftGammaGain);
    color_balance.lift[1] = 1.02;
    color_balance.gamma[2] = 0.94;
    color_balance.gain[3] = 1.08;
    color_balance.input_saturation = 0.9;
    color_balance.contrast = 1.1;
    color_balance.output_saturation = 1.05;
    recipe.operations.push_back({std::string(kColorBalanceOperationId),
                                 kColorBalanceOperationSchemaVersion, "colorbalance-1", true,
                                 color_balance_to_parameters(color_balance), std::nullopt});
    ColorCorrectionParams color_correction;
    color_correction.highlight_a = 12.5;
    color_correction.highlight_b = -8.25;
    color_correction.shadow_a = -4.75;
    color_correction.shadow_b = 9.5;
    color_correction.saturation = 1.375;
    auto color_correction_parameters = color_correction_to_parameters(color_correction);
    ASSERT_TRUE(color_correction_parameters) << color_correction_parameters.error().message;
    recipe.operations.push_back({std::string(kColorCorrectionOperationId),
                                 kColorCorrectionOperationSchemaVersion, "colorcorrection-1", true,
                                 std::move(color_correction_parameters).value(), std::nullopt});
    OutputColorParams output_color;
    output_color.output_profile = std::string(kInputProfileDisplayP3);
    recipe.operations.push_back({"ravo.color.output", 1, "color-output-1", true,
                                 output_color_to_parameters(output_color), std::nullopt});
    const auto serialized = serialize_recipe(recipe);
    ASSERT_TRUE(serialized) << serialized.error().message;
    {
        std::ofstream output(recipe_path, std::ios::binary);
        ASSERT_TRUE(output);
        output << serialized.value();
    }

    const auto recipe_u8 = recipe_path.generic_u8string();
    const std::string recipe_argument(recipe_u8.begin(), recipe_u8.end());
    const auto output_u8 = output_path.generic_u8string();
    const std::string output_argument(output_u8.begin(), output_u8.end());
    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    const std::vector<std::string_view> arguments{
        "render",        input_argument, "--recipe", recipe_argument, "--output",
        output_argument, "--backend",    "cpu",      "--width",       "64",
        "--height",      "48",           "--json"};

    EXPECT_EQ(application.run(std::span{arguments}), 0);
    ASSERT_TRUE(std::filesystem::exists(output_path));
    std::ifstream output(output_path, std::ios::binary);
    ASSERT_TRUE(output);
    std::array<char, 8> signature{};
    output.read(signature.data(), static_cast<std::streamsize>(signature.size()));
    EXPECT_EQ(std::string(signature.data(), signature.size()), std::string("\x89PNG\r\n\x1a\n", 8));
    EXPECT_TRUE(png_has_chunk(output_path, {'i', 'C', 'C', 'P'}));
    EXPECT_FALSE(png_has_chunk(output_path, {'s', 'R', 'G', 'B'}));
    EXPECT_TRUE(stderr_stream.str().empty());
    output.close();

    Recipe direct_recipe = recipe;
    direct_recipe.asset.input_uri = input_argument;
    Recipe baseline_recipe = direct_recipe;
    baseline_recipe.operations.erase(
        std::remove_if(baseline_recipe.operations.begin(), baseline_recipe.operations.end(),
                       [](const OperationInstance &operation)
                       { return operation.id == kColorCorrectionOperationId; }),
        baseline_recipe.operations.end());
    RenderRequest direct_request;
    direct_request.asset = direct_recipe.asset;
    direct_request.recipe = std::move(direct_recipe);
    direct_request.output_width = 64U;
    direct_request.output_height = 48U;
    auto direct = engine.render_to_image(direct_request);
    ASSERT_TRUE(direct) << direct.error().message;
    RenderRequest baseline_request;
    baseline_request.asset = baseline_recipe.asset;
    baseline_request.recipe = std::move(baseline_recipe);
    baseline_request.output_width = 64U;
    baseline_request.output_height = 48U;
    auto baseline = engine.render_to_image(baseline_request);
    ASSERT_TRUE(baseline) << baseline.error().message;
    const auto cli_pixels = read_png_rgb(output_path);
    EXPECT_EQ(cli_pixels, direct.value().rgb);
    EXPECT_EQ(pixel_hash(cli_pixels), pixel_hash(direct.value().rgb));
    EXPECT_NE(pixel_hash(cli_pixels), pixel_hash(baseline.value().rgb))
        << "the canonical Color Correction operation must change the rendered RAW pixels";

    const auto source_after = source_file_snapshot(input_argument);
    ASSERT_TRUE(source_after.has_value());
    EXPECT_EQ(*source_after, *source_before);

    std::filesystem::remove(recipe_path, ignored);
    std::filesystem::remove(output_path, ignored);
}

TEST_F(CliTest, RenderCommandMatchesDirectEngineForCanonicalColorHarmonizerMask)
{
    const auto directory = std::filesystem::temp_directory_path();
    const auto recipe_path = directory / "ravo-cli-canonical-mask.recipe.json";
    const auto output_path = directory / "ravo-cli-canonical-mask.png";
    std::error_code ignored;
    std::filesystem::remove(recipe_path, ignored);
    std::filesystem::remove(output_path, ignored);

    Recipe recipe;
    recipe.asset = {"mire1", "file:///recipe-placeholder.raw", std::nullopt};
    recipe.operations.push_back({"ravo.color.input", 1, "color-input-1", true,
                                 input_color_to_parameters(InputColorParams{}), std::nullopt});
    auto harmonizer = color_harmonizer_to_parameters(ColorHarmonizerParams{});
    ASSERT_TRUE(harmonizer) << harmonizer.error().message;
    recipe.masks.push_back({"all", kCanonicalMaskSchemaVersion, MaskKind::kAll});
    recipe.operations.push_back({std::string(kColorHarmonizerOperationId),
                                 kColorHarmonizerOperationSchemaVersion, "harmonizer-1", true,
                                 std::move(harmonizer).value(), "all"});
    recipe.operations.push_back({"ravo.color.output", 1, "color-output-1", true,
                                 output_color_to_parameters(OutputColorParams{}), std::nullopt});
    const auto serialized = serialize_recipe(recipe);
    ASSERT_TRUE(serialized) << serialized.error().message;
    {
        std::ofstream file(recipe_path, std::ios::binary);
        ASSERT_TRUE(file);
        file << serialized.value();
    }

    const auto input = mire1_path();
    const auto recipe_u8 = recipe_path.generic_u8string();
    const std::string recipe_argument(recipe_u8.begin(), recipe_u8.end());
    const auto output_u8 = output_path.generic_u8string();
    const std::string output_argument(output_u8.begin(), output_u8.end());
    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    const std::vector<std::string_view> arguments{
        "render", input,      "--recipe", recipe_argument, "--output", output_argument, "--width",
        "64",     "--height", "48",       "--json"};
    ASSERT_EQ(application.run(std::span{arguments}), 0) << stderr_stream.str();

    Recipe direct_recipe = recipe;
    direct_recipe.asset.input_uri = input;
    RenderRequest direct_request;
    direct_request.asset = direct_recipe.asset;
    direct_request.recipe = std::move(direct_recipe);
    direct_request.output_width = 64U;
    direct_request.output_height = 48U;
    const auto direct = engine.render_to_image(direct_request);
    ASSERT_TRUE(direct) << direct.error().message;
    EXPECT_EQ(read_png_rgb(output_path), direct.value().rgb);

    std::filesystem::remove(recipe_path, ignored);
    std::filesystem::remove(output_path, ignored);
}

TEST_F(CliTest, RecipeValidateUsesTheFacadeAndReturnsMachineData)
{
    const auto recipe = serialize_recipe(test::valid_recipe());
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto path = std::filesystem::temp_directory_path() / "ravo-cli-contract-recipe.json";
    {
        std::ofstream output(path, std::ios::binary);
        ASSERT_TRUE(output);
        output << recipe.value();
    }

    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    const auto utf8_path = path.generic_u8string();
    const std::string path_argument(utf8_path.begin(), utf8_path.end());
    const std::vector<std::string_view> arguments{"recipe", "validate", path_argument, "--json"};
    const int exit_code = application.run(std::span{arguments});
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    EXPECT_EQ(exit_code, 0);
    const auto response = parse_json(stdout_stream.str());
    ASSERT_TRUE(response) << response.error().message;
    const auto *data = response.value().find("data");
    ASSERT_NE(data, nullptr);
    const auto *asset_id = data->find("asset_id");
    ASSERT_NE(asset_id, nullptr);
    ASSERT_NE(asset_id->string_if(), nullptr);
    EXPECT_EQ(*asset_id->string_if(), "asset-1");
    EXPECT_TRUE(stderr_stream.str().empty());
}

TEST_F(CliTest, Utf8FilePathsAreResolvedInsideTheQtAdapter)
{
    const auto path = std::filesystem::temp_directory_path() /
                      std::filesystem::path(std::u8string(u8"ravo-path-contract.json"));
    {
        std::ofstream output(path, std::ios::binary);
        ASSERT_TRUE(output);
        output << "unicode-path-contract";
    }
    const auto qt_path = path.generic_u8string();
    const std::string path_utf8(qt_path.begin(), qt_path.end());

    const auto content = read_utf8_text_file(path_utf8);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    ASSERT_TRUE(content) << content.error().message;
    EXPECT_EQ(content.value(), "unicode-path-contract");
}

TEST_F(CliTest, LegacyXmpImportCreatesAnExplicitInputRecipeAtomically)
{
    const auto directory = std::filesystem::temp_directory_path();
    const auto xmp_path = directory / "ravo-empty-history.xmp";
    const auto recipe_path = directory / "ravo-empty-history.recipe.json";
    {
        std::ofstream output(xmp_path, std::ios::binary);
        ASSERT_TRUE(output);
        output << R"(<?xml version="1.0" encoding="UTF-8"?>
<x:xmpmeta xmlns:x="adobe:ns:meta/"
           xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
           xmlns:darktable="http://darktable.sf.net/">
  <rdf:RDF><rdf:Description><darktable:history><rdf:Seq/></darktable:history></rdf:Description></rdf:RDF>
</x:xmpmeta>)";
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
    std::error_code ignored;
    std::filesystem::remove(xmp_path, ignored);
    std::filesystem::remove(recipe_path, ignored);

    EXPECT_EQ(exit_code, 0);
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto parsed = parse_recipe_json(recipe.value());
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_EQ(parsed.value().operations.size(), 2U);
    EXPECT_EQ(parsed.value().operations.front().id, "ravo.color.input");
    EXPECT_EQ(parsed.value().operations.back().id, "ravo.color.output");
    EXPECT_TRUE(stderr_stream.str().empty());
}

TEST_F(CliTest, FrozenNopXmpMapsItsInputProfileExplicitly)
{
    const auto path =
        std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" / "frozen" / "0000-nop" / "nop.xmp";
    const auto path_u8 = path.generic_u8string();
    const std::string path_argument(path_u8.begin(), path_u8.end());
    const auto xmp = read_utf8_text_file(path_argument);
    ASSERT_TRUE(xmp) << xmp.error().message;
    const LegacyXmpImportRequest request{xmp.value(), {"mire1", mire1_path(), std::nullopt}};

    const auto imported = import_legacy_xmp(request);

    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_EQ(imported.value().operations.size(), 2U);
    EXPECT_EQ(imported.value().operations.front().id, "ravo.color.input");
    auto input = input_color_from_parameters(imported.value().operations.front().parameters);
    ASSERT_TRUE(input) << input.error().message;
    EXPECT_EQ(input.value().input_profile, kInputProfileEnhancedMatrix);
    EXPECT_EQ(input.value().working_profile, kInputProfileLinearRec2020);
    EXPECT_EQ(imported.value().operations.back().id, "ravo.color.output");
    EXPECT_TRUE(imported.value().masks.empty());
}

TEST_F(CliTest, LegacyXmpMapsFlipOrientationOntoCanonicalGeometry)
{
    const auto import_flip = [](const std::string_view parameters)
    {
        return import_legacy_xmp(
            {legacy_flip_xmp(parameters), {"asset-1", "file:///fixture.raw", std::nullopt}});
    };

    auto auto_orient = import_flip("ffffffff");
    ASSERT_TRUE(auto_orient) << auto_orient.error().message;
    ASSERT_EQ(auto_orient.value().operations.size(), 2U);
    EXPECT_EQ(auto_orient.value().operations.front().id, "ravo.color.input");
    EXPECT_EQ(auto_orient.value().operations.back().id, "ravo.color.output");

    auto none = import_flip("00000000");
    ASSERT_TRUE(none) << none.error().message;
    EXPECT_EQ(none.value().operations.size(), 2U);

    auto clockwise = import_flip("05000000");
    ASSERT_TRUE(clockwise) << clockwise.error().message;
    ASSERT_EQ(clockwise.value().operations.size(), 3U);
    EXPECT_EQ(clockwise.value().operations[1].id, "ravo.geometry.rotate");
    EXPECT_EQ(
        std::get<std::int64_t>(clockwise.value().operations[1].parameters.at("quarters").value), 1);

    auto transpose = import_flip("04000000");
    ASSERT_TRUE(transpose) << transpose.error().message;
    ASSERT_EQ(transpose.value().operations.size(), 4U);
    EXPECT_EQ(transpose.value().operations[1].id, "ravo.geometry.rotate");
    EXPECT_EQ(transpose.value().operations[2].id, "ravo.geometry.flip");
    EXPECT_EQ(
        std::get<std::int64_t>(transpose.value().operations[2].parameters.at("horizontal").value),
        1);

    auto rejected = import_flip("08000000");
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(rejected.error().context.at("reason"), "unsupported_legacy_flip_orientation");
}

TEST_F(CliTest, LegacyXmpMapsCropBoxOntoCanonicalGeometry)
{
    constexpr std::string_view kIdentity = "00000000000000000000803f0000803fffffffffffffffff";
    constexpr std::string_view kQuarter = "0000803e0000803e0000403f0000403fffffffffffffffff";
    constexpr std::string_view kFixtureV1 = "00000000b1588f34f50bdc3e438e243f0100000001000000";
    constexpr std::string_view kEmpty = "0000003f0000003f0000003f0000003fffffffffffffffff";

    auto identity = import_legacy_xmp(
        {legacy_crop_xmp(kIdentity), {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(identity) << identity.error().message;
    ASSERT_EQ(identity.value().operations.size(), 2U);

    auto quarter = import_legacy_xmp(
        {legacy_crop_xmp(kQuarter), {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(quarter) << quarter.error().message;
    ASSERT_EQ(quarter.value().operations.size(), 3U);
    EXPECT_EQ(quarter.value().operations[1].id, "ravo.geometry.crop");
    EXPECT_NEAR(std::get<double>(quarter.value().operations[1].parameters.at("x").value), 0.25,
                1e-6);
    EXPECT_NEAR(std::get<double>(quarter.value().operations[1].parameters.at("width").value), 0.5,
                1e-6);

    auto fixture =
        import_legacy_xmp({legacy_crop_xmp(kFixtureV1, "1", kLegacyGammaBlendGz14GuideFive),
                           {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(fixture) << fixture.error().message;
    ASSERT_EQ(fixture.value().operations.size(), 3U);
    EXPECT_EQ(fixture.value().operations[1].id, "ravo.geometry.crop");
    EXPECT_NEAR(std::get<double>(fixture.value().operations[1].parameters.at("width").value),
                0.42977872, 1e-6);

    auto empty = import_legacy_xmp(
        {legacy_crop_xmp(kEmpty), {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(empty.error().context.at("reason"), "unsupported_legacy_crop_box");
}

[[nodiscard]] std::string legacy_ashift_xmp(const std::string_view parameters,
                                            const std::string_view version = "4")
{
    std::string document = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq>
<rdf:li darktable:num="9" darktable:operation="ashift" darktable:modversion=")";
    document += version;
    document += R"(" darktable:enabled="1" darktable:params=")";
    document += parameters;
    document +=
        R"(" darktable:multi_name="" darktable:multi_priority="0" darktable:blendop_version="9" darktable:blendop_params=")";
    document += kLegacyGammaBlendV9;
    document += R"("/>
</rdf:Seq></darktable:history></rdf:Description>
</rdf:RDF>)";
    return document;
}

TEST_F(CliTest, LegacyXmpMapsGenericAshiftOntoCanonicalPerspective)
{
    constexpr std::string_view kIdentity =
        "00000000000000000000000000000000000048420000803f0000c8420000803f0000000000000000000000000000803f000000000000803f00000000";
    constexpr std::string_view kRotation =
        "00002040000000000000000000000000000048420000803f0000c8420000803f0000000000000000000000000000803f000000000000803f00000000";
    constexpr std::string_view kPerspective =
        "90eb913f102db23df853e3bd8cc2753d0000c8420000803f0000c8420000803f000000000000000000000000000000000000803f000000000000803f";

    auto identity = import_legacy_xmp(
        {legacy_ashift_xmp(kIdentity, "5"), {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(identity) << identity.error().message;
    ASSERT_EQ(identity.value().operations.size(), 2U);

    auto rotated = import_legacy_xmp(
        {legacy_ashift_xmp(kRotation, "5"), {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(rotated) << rotated.error().message;
    ASSERT_EQ(rotated.value().operations.size(), 3U);
    EXPECT_EQ(rotated.value().operations[1].id, kPerspectiveOperationId);
    EXPECT_NEAR(
        std::get<double>(rotated.value().operations[1].parameters.at("rotation_degrees").value),
        2.5, 1e-5);

    auto perspective = import_legacy_xmp(
        {legacy_ashift_xmp(kPerspective), {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_TRUE(perspective) << perspective.error().message;
    ASSERT_EQ(perspective.value().operations.size(), 3U);
    EXPECT_EQ(perspective.value().operations[1].id, kPerspectiveOperationId);
    EXPECT_NEAR(
        std::get<double>(perspective.value().operations[1].parameters.at("vertical_shift").value),
        0.087, 1e-6);
    EXPECT_NEAR(
        std::get<double>(perspective.value().operations[1].parameters.at("horizontal_shift").value),
        -0.111, 1e-6);
    EXPECT_NEAR(std::get<double>(perspective.value().operations[1].parameters.at("shear").value),
                0.06, 1e-6);
    EXPECT_EQ(std::get<std::string>(
                  perspective.value().operations[1].parameters.at("interpolation").value),
              kPerspectiveInterpolationLanczos3);

    std::string specific_lens(kPerspective);
    specific_lens.replace(64U, 8U, "01000000");
    auto unsupported_lens = import_legacy_xmp(
        {legacy_ashift_xmp(specific_lens), {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_FALSE(unsupported_lens);
    EXPECT_EQ(unsupported_lens.error().context.at("reason"), "unsupported_legacy_ashift_lens_mode");

    std::string aspect_crop(kPerspective);
    aspect_crop.replace(80U, 8U, "02000000");
    auto unsupported_crop = import_legacy_xmp(
        {legacy_ashift_xmp(aspect_crop), {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_FALSE(unsupported_crop);
    EXPECT_EQ(unsupported_crop.error().context.at("reason"), "unsupported_legacy_ashift_crop_mode");
}

[[nodiscard]] std::string legacy_rgblevels_xmp(
    const std::string_view parameters,
    const std::string_view blend = "gz13eJxjYGBgYAZiCQYYOOHEgAYY0QVwggZ7CB6pfNoAAE4AGQc=")
{
    std::string document = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq>
<rdf:li darktable:num="8" darktable:operation="rgblevels" darktable:modversion="1" darktable:enabled="1" darktable:params=")";
    document += parameters;
    document +=
        R"(" darktable:multi_name="" darktable:multi_priority="0" darktable:blendop_version="10" darktable:blendop_params=")";
    document += blend;
    document += R"("/>
</rdf:Seq></darktable:history></rdf:Description>
</rdf:RDF>)";
    return document;
}


} // namespace
} // namespace ravo
