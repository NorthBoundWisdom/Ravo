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
#include <utility>
#include <vector>

#include <QByteArray>
#include <QCoreApplication>
#include <QProcess>
#include <QXmlStreamReader>
#include <gtest/gtest.h>
#include <png.h>

#include "ravo/adapters/legacy_xmp.h"
#include "ravo/adapters/text_file.h"
#include "ravo/cli/application.h"
#include "ravo/domain/types.h"
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

#include "capture_metadata_test_support.h"
#include "test_support.h"

namespace ravo
{
namespace
{

void ensure_qt_core()
{
    static const bool logging = []
    {
        ravo::init_logging("ravo-cli-tests");
        return true;
    }();
    static_cast<void>(logging);
    if (QCoreApplication::instance() != nullptr)
    {
        return;
    }
    static int argc = 1;
    static char dummy[] = "ravo-cli-tests";
    static char *argv[] = {dummy, nullptr};
    static auto *app = new QCoreApplication(argc, argv);
    static_cast<void>(app);
}

class CliTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensure_qt_core();
        const auto created = EngineFacade::create_phase1();
        ASSERT_TRUE(created) << created.error().message;
        engine = std::move(created).value();
    }

    EngineFacade engine = []
    {
        auto created = EngineFacade::create_phase1();
        return std::move(created).value();
    }();
};

[[nodiscard]] std::string mire1_path()
{
    const auto path =
        std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests" / "images" / "mire1.cr2";
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
        std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests" / "0000-nop" / "nop.xmp";
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

TEST_F(CliTest, LegacyXmpGammaFixtureCensusPinsEveryFrozenMandatoryBoundary)
{
    const auto fixture_root = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests";
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

    std::array<std::size_t, kFrozenLegacyGammaBlendTuples.size()> tuple_counts{};
    std::size_t gamma_count = 0U;
    std::size_t missing_hand_edited_count = 0U;
    std::size_t zero_hand_edited_count = 0U;
    for (const auto &path : xmp_paths)
    {
        SCOPED_TRACE(path.generic_string());
        const auto path_u8 = path.generic_u8string();
        const std::string path_utf8(path_u8.begin(), path_u8.end());
        const auto content = read_utf8_text_file(path_utf8);
        ASSERT_TRUE(content) << content.error().message;
        const QByteArray bytes(content.value().data(),
                               static_cast<qsizetype>(content.value().size()));
        QXmlStreamReader reader(bytes);
        std::size_t file_gamma_count = 0U;
        while (!reader.atEnd())
        {
            reader.readNext();
            if (!reader.isStartElement() || reader.name() != u"li" ||
                xml_attribute_value(reader.attributes(), u"operation") != "gamma")
            {
                continue;
            }

            ++file_gamma_count;
            ++gamma_count;
            const auto version = xml_attribute_value(reader.attributes(), u"modversion");
            const auto enabled = xml_attribute_value(reader.attributes(), u"enabled");
            const auto parameters = xml_attribute_value(reader.attributes(), u"params");
            const auto multi_priority = xml_attribute_value(reader.attributes(), u"multi_priority");
            const auto multi_name = xml_attribute_value(reader.attributes(), u"multi_name");
            const auto multi_name_hand_edited =
                xml_attribute_value(reader.attributes(), u"multi_name_hand_edited");
            const auto blend_version = xml_attribute_value(reader.attributes(), u"blendop_version");
            const auto blend_parameters =
                xml_attribute_value(reader.attributes(), u"blendop_params");
            const auto history_position = xml_attribute_value(reader.attributes(), u"num");

            ASSERT_TRUE(version);
            EXPECT_EQ(*version, "1");
            ASSERT_TRUE(enabled);
            EXPECT_EQ(*enabled, "1");
            ASSERT_TRUE(parameters);
            EXPECT_EQ(*parameters, "0000000000000000");
            ASSERT_TRUE(multi_priority);
            EXPECT_EQ(*multi_priority, "0");
            ASSERT_TRUE(multi_name);
            EXPECT_TRUE(multi_name->empty());
            ASSERT_TRUE(history_position);
            EXPECT_FALSE(history_position->empty());
            EXPECT_TRUE(std::all_of(history_position->begin(), history_position->end(),
                                    [](const char value) { return value >= '0' && value <= '9'; }));
            if (multi_name_hand_edited)
            {
                EXPECT_EQ(*multi_name_hand_edited, "0");
                ++zero_hand_edited_count;
            }
            else
            {
                ++missing_hand_edited_count;
            }

            for (const auto &attribute : reader.attributes())
            {
                const auto name = attribute.name();
                EXPECT_FALSE(name.contains(u"mask"));
                EXPECT_TRUE(name == u"num" || name == u"operation" || name == u"enabled" ||
                            name == u"modversion" || name == u"params" || name == u"multi_name" ||
                            name == u"multi_priority" || name == u"multi_name_hand_edited" ||
                            name == u"blendop_version" || name == u"blendop_params")
                    << name.toString().toStdString();
            }

            ASSERT_TRUE(blend_version);
            ASSERT_TRUE(blend_parameters);
            const auto tuple = std::find_if(kFrozenLegacyGammaBlendTuples.begin(),
                                            kFrozenLegacyGammaBlendTuples.end(),
                                            [&](const FrozenLegacyGammaBlendTuple &candidate)
                                            {
                                                return candidate.version == *blend_version &&
                                                       candidate.parameters == *blend_parameters;
                                            });
            ASSERT_NE(tuple, kFrozenLegacyGammaBlendTuples.end());
            ++tuple_counts[static_cast<std::size_t>(
                std::distance(kFrozenLegacyGammaBlendTuples.begin(), tuple))];
        }
        ASSERT_FALSE(reader.hasError()) << reader.errorString().toStdString();
        EXPECT_EQ(file_gamma_count, 1U);
    }

    EXPECT_EQ(gamma_count, 158U);
    EXPECT_EQ(missing_hand_edited_count, 99U);
    EXPECT_EQ(zero_hand_edited_count, 59U);
    for (std::size_t index = 0; index < kFrozenLegacyGammaBlendTuples.size(); ++index)
    {
        SCOPED_TRACE(std::string(kFrozenLegacyGammaBlendTuples[index].version));
        EXPECT_EQ(tuple_counts[index], kFrozenLegacyGammaBlendTuples[index].fixture_count);
    }
}

TEST_F(CliTest, LegacyXmpGammaAbsorbsOnlyTheTwelveExactFrozenBlendTuples)
{
    for (std::size_t index = 0; index < kFrozenLegacyGammaBlendTuples.size(); ++index)
    {
        const auto &tuple = kFrozenLegacyGammaBlendTuples[index];
        SCOPED_TRACE(std::string(tuple.version) + ":" + std::to_string(index));
        LegacyGammaXmpOptions options;
        options.blend_version = tuple.version;
        options.blend_parameters = tuple.parameters;
        if ((index % 2U) == 0U)
        {
            options.multi_name_hand_edited = "0";
        }
        const auto imported = import_legacy_xmp(
            {legacy_gamma_xmp(options), {"asset-1", "file:///fixture.raw", std::nullopt}});

        ASSERT_TRUE(imported) << imported.error().message;
        ASSERT_EQ(imported.value().operations.size(), 2U);
        EXPECT_EQ(imported.value().operations.front().id, "ravo.color.input");
        EXPECT_EQ(imported.value().operations.back().id, "ravo.color.output");
        EXPECT_TRUE(std::none_of(imported.value().operations.begin(),
                                 imported.value().operations.end(),
                                 [](const OperationInstance &operation)
                                 {
                                     return operation.id == "ravo.core.gamma" ||
                                            operation.id == kProfileGammaOperationId;
                                 }));
        EXPECT_TRUE(imported.value().masks.empty());
    }
}

TEST_F(CliTest, LegacyXmpGammaRejectsEveryUnprovenHistoryState)
{
    const auto expect_rejected = [](const LegacyGammaXmpOptions &options,
                                    const ErrorCode expected_code,
                                    const std::string_view expected_reason)
    {
        const auto imported = import_legacy_xmp(
            {legacy_gamma_xmp(options), {"asset-1", "file:///fixture.raw", std::nullopt}});
        ASSERT_FALSE(imported);
        EXPECT_EQ(imported.error().code, expected_code);
        EXPECT_EQ(imported.error().context.at("legacy_operation"), "gamma");
        EXPECT_EQ(imported.error().context.at("reason"), expected_reason);
    };

    LegacyGammaXmpOptions options;
    options.version = "2";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_version");

    options = {};
    options.version.reset();
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_version");

    options = {};
    options.enabled = "0";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_disabled");

    options = {};
    options.enabled.reset();
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_disabled");

    options = {};
    options.parameters = "0000000000000001";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_parameters");

    options = {};
    options.parameters.reset();
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_parameters");

    options = {};
    options.instances = 2U;
    expect_rejected(options, ErrorCode::kConflict, "duplicate_legacy_gamma");

    options = {};
    options.blend_version = "15";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_blend");

    options = {};
    options.blend_version.reset();
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_blend");

    options = {};
    options.blend_parameters = kLegacyGammaBlendGz14GuideOne;
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_blend");

    options = {};
    options.blend_parameters.reset();
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_blend");

    options = {};
    options.extra_attributes = R"( darktable:mask_id="42")";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_mask");

    options = {};
    options.extra_attributes = R"( darktable:blendop_mask_id="42")";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_mask");

    options = {};
    options.multi_priority = "1";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_multi_state");

    options = {};
    options.multi_priority.reset();
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_multi_state");

    options = {};
    options.multi_name = "edited";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_multi_state");

    options = {};
    options.multi_name.reset();
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_multi_state");

    options = {};
    options.multi_name_hand_edited = "1";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_multi_state");

    options = {};
    options.extra_attributes = R"( darktable:unproven_state="1")";
    expect_rejected(options, ErrorCode::kUnsupported, "unsupported_legacy_gamma_attribute");
}

TEST_F(CliTest, LegacyXmpDecodesAndImportsTheProvenRgbPrimariesSingleton)
{
    const auto decoded = decode_legacy_primaries_v1_parameters(kLegacyPrimariesPayload);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_NEAR(decoded.value().achromatic_tint_hue, 0.5794494152, 1e-7);
    EXPECT_NEAR(decoded.value().achromatic_tint_purity, 0.1340000033, 1e-7);
    EXPECT_NEAR(decoded.value().red_hue, 0.1658063233, 1e-7);
    EXPECT_NEAR(decoded.value().red_purity, 0.8429999948, 1e-7);
    EXPECT_NEAR(decoded.value().green_hue, -0.1431170106, 1e-7);
    EXPECT_NEAR(decoded.value().green_purity, 1.2350000143, 1e-7);
    EXPECT_NEAR(decoded.value().blue_hue, 0.2286381423, 1e-7);
    EXPECT_NEAR(decoded.value().blue_purity, 0.8199999928, 1e-7);
    auto canonical = primaries_from_parameters(primaries_to_parameters(decoded.value()));
    ASSERT_TRUE(canonical) << canonical.error().message;
    EXPECT_EQ(canonical.value(), decoded.value());

    const auto short_payload = decode_legacy_primaries_v1_parameters("00000000");
    ASSERT_FALSE(short_payload);
    EXPECT_EQ(short_payload.error().code, ErrorCode::kValidation);
    const auto non_finite_payload = decode_legacy_primaries_v1_parameters(
        "0000c07f00000000000000000000803f000000000000803f000000000000803f");
    ASSERT_FALSE(non_finite_payload);
    EXPECT_EQ(non_finite_payload.error().code, ErrorCode::kValidation);

    const auto document = legacy_primaries_xmp();
    const LegacyXmpImportRequest request{document,
                                         {"asset-1", "file:///fixture.raw", std::nullopt}};
    const auto imported = import_legacy_xmp(request);
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_EQ(imported.value().operations.size(), 3U);
    EXPECT_EQ(imported.value().operations.front().id, "ravo.color.input");
    const auto &primaries = imported.value().operations[1];
    EXPECT_EQ(primaries.id, kPrimariesOperationId);
    EXPECT_EQ(primaries.instance_id, "legacy-primaries-0");
    EXPECT_TRUE(primaries.enabled);
    auto imported_params = primaries_from_parameters(primaries.parameters);
    ASSERT_TRUE(imported_params) << imported_params.error().message;
    EXPECT_EQ(imported_params.value(), decoded.value());
    EXPECT_EQ(imported.value().operations.back().id, "ravo.color.output");
    auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    ASSERT_TRUE(validate_recipe(imported.value(), registry.value()));
}

TEST_F(CliTest, LegacyXmpRejectsUnprovenRgbPrimariesHistoryStates)
{
    const auto import_document = [](std::string document)
    { return import_legacy_xmp({document, {"asset-1", "file:///fixture.raw", std::nullopt}}); };

    auto disabled = import_document(legacy_primaries_xmp("1", "0"));
    ASSERT_FALSE(disabled);
    EXPECT_EQ(disabled.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(disabled.error().context.at("reason"), "unsupported_legacy_primaries_disabled");

    auto version = import_document(legacy_primaries_xmp("2"));
    ASSERT_FALSE(version);
    EXPECT_EQ(version.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(version.error().context.at("reason"), "unsupported_legacy_primaries_version");

    auto blend = import_document(
        legacy_primaries_xmp("1", "1", kLegacyPrimariesPayload, "unsupported-blend"));
    ASSERT_FALSE(blend);
    EXPECT_EQ(blend.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(blend.error().context.at("reason"), "unsupported_legacy_blend");

    auto mask = import_document(legacy_primaries_xmp("1", "1", kLegacyPrimariesPayload,
                                                     kLegacyPrimariesDefaultBlend, 1U, "13",
                                                     R"( darktable:mask_id="mask-1")"));
    ASSERT_FALSE(mask);
    EXPECT_EQ(mask.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(mask.error().context.at("reason"), "unsupported_legacy_mask");

    auto duplicate = import_document(
        legacy_primaries_xmp("1", "1", kLegacyPrimariesPayload, kLegacyPrimariesDefaultBlend, 2U));
    ASSERT_FALSE(duplicate);
    EXPECT_EQ(duplicate.error().code, ErrorCode::kConflict);
    EXPECT_EQ(duplicate.error().context.at("reason"), "duplicate_legacy_primaries");
}

TEST_F(CliTest, LegacyXmpPrimariesImportCommandWritesCanonicalRecipe)
{
    const auto directory = std::filesystem::temp_directory_path();
    const auto xmp_path = directory / "ravo-primaries-v1.xmp";
    const auto recipe_path = directory / "ravo-primaries-v1.recipe.json";
    std::error_code ignored;
    std::filesystem::remove(xmp_path, ignored);
    std::filesystem::remove(recipe_path, ignored);
    {
        std::ofstream output(xmp_path, std::ios::binary);
        ASSERT_TRUE(output);
        output << legacy_primaries_xmp();
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

    const auto exit_code = application.run(std::span{arguments});
    const auto recipe = read_utf8_text_file(recipe_argument);
    std::filesystem::remove(xmp_path, ignored);
    std::filesystem::remove(recipe_path, ignored);

    EXPECT_EQ(exit_code, 0);
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto parsed = parse_recipe_json(recipe.value());
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_EQ(parsed.value().operations.size(), 3U);
    EXPECT_EQ(parsed.value().operations[1].id, kPrimariesOperationId);
    EXPECT_EQ(parsed.value().operations.back().id, "ravo.color.output");
    EXPECT_TRUE(stderr_stream.str().empty());
}

TEST_F(CliTest, LegacyXmpOperationsRemainExplicitlyUnsupported)
{
    constexpr std::string_view xmp = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description><darktable:history><rdf:Seq><rdf:li darktable:operation="exposure"/></rdf:Seq></darktable:history></rdf:Description>
</rdf:RDF>)";
    const LegacyXmpImportRequest request{xmp, {"asset-1", "file:///fixture.raw", std::nullopt}};

    const auto imported = import_legacy_xmp(request);

    ASSERT_FALSE(imported);
    EXPECT_EQ(imported.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(imported.error().context.at("legacy_operation"), "exposure");
}

TEST_F(CliTest, LegacyXmpProfileGammaRejectsMissingFrozenPayloadEvidence)
{
    constexpr std::string_view xmp = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq>
    <rdf:li darktable:operation="profile_gamma" darktable:modversion="2" darktable:enabled="1"/>
  </rdf:Seq></darktable:history></rdf:Description>
</rdf:RDF>)";
    const LegacyXmpImportRequest request{xmp, {"asset-1", "file:///fixture.raw", std::nullopt}};

    const auto imported = import_legacy_xmp(request);

    ASSERT_FALSE(imported);
    EXPECT_EQ(imported.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(imported.error().context.at("legacy_operation"), "profile_gamma");
    EXPECT_EQ(imported.error().context.at("reason"), "unsupported_legacy_profile_gamma_no_fixture");
}

TEST_F(CliTest, LegacyXmpMapsStrictExposureV5V6AndV7Payloads)
{
    struct Case
    {
        std::string_view version;
        std::string_view parameters;
        bool enabled;
        bool exposure_bias;
        bool highlight_preservation;
    };
    constexpr std::array cases{
        Case{"5", kLegacyExposureV5ManualOne, true, false, false},
        Case{"6", kLegacyExposureV6ManualOneBias, true, true, false},
        Case{"7", kLegacyExposureV7ManualOneBothCompensations, false, true, true},
    };
    for (const auto &test_case : cases)
    {
        LegacyExposureXmpOptions options;
        options.version = test_case.version;
        options.parameters = test_case.parameters;
        options.enabled = test_case.enabled ? "1" : "0";
        const auto xmp = legacy_exposure_xmp(options);
        const LegacyXmpImportRequest request{xmp, {"asset-1", "file:///fixture.raw", std::nullopt}};

        const auto imported = import_legacy_xmp(request);

        ASSERT_TRUE(imported) << test_case.version << ": " << imported.error().message;
        ASSERT_EQ(imported.value().operations.size(), 3U);
        EXPECT_EQ(imported.value().operations.front().id, "ravo.color.input");
        const auto &operation = imported.value().operations[1];
        EXPECT_EQ(operation.id, kExposureOperationId);
        EXPECT_EQ(operation.schema_version, kExposureOperationSchemaVersion);
        EXPECT_EQ(operation.instance_id, "legacy-exposure-8");
        EXPECT_EQ(operation.enabled, test_case.enabled);
        auto params = exposure_from_parameters(operation.parameters);
        ASSERT_TRUE(params) << params.error().message;
        EXPECT_EQ(params.value().mode, kExposureModeManual);
        EXPECT_DOUBLE_EQ(params.value().black, 0.0);
        EXPECT_DOUBLE_EQ(params.value().exposure_ev, 1.0);
        EXPECT_DOUBLE_EQ(params.value().deflicker_percentile, 50.0);
        EXPECT_DOUBLE_EQ(params.value().deflicker_target_ev, -4.0);
        EXPECT_EQ(params.value().compensate_exposure_bias, test_case.exposure_bias);
        EXPECT_EQ(params.value().compensate_highlight_preservation,
                  test_case.highlight_preservation);
    }
}

TEST_F(CliTest, LegacyXmpExposureFixtureCensusPinsRevisionsSingletonsMasksAndBlendStates)
{
    const auto fixture_root = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests";
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
    const auto path = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests" /
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
    const auto fixture_root = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests";
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
    const auto fixture_root = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests";
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

TEST_F(CliTest, LegacyXmpColorCheckerIgnoresStaleAndNonFiniteInactiveTailBits)
{
    ColorCheckerParams source;
    const auto payload = legacy_color_checker_v2_hex(source, 1, 1U);
    LegacyColorCheckerXmpOptions options;
    options.parameters = payload;

    auto imported = import_legacy_xmp(
        {legacy_color_checker_xmp(options), {"asset-1", "file:///fixture.raw", std::nullopt}});

    ASSERT_TRUE(imported) << imported.error().message;
    auto params = color_checker_from_parameters(imported.value().operations[1].parameters);
    ASSERT_TRUE(params) << params.error().message;
    ASSERT_EQ(params.value().patches.size(), 1U);
    EXPECT_EQ(params.value().patches.front(), source.patches.front());
}

TEST_F(CliTest, LegacyXmpColorCheckerRejectsUnfrozenVersionPresentationAndActiveData)
{
    const auto expect_rejected = [&](const LegacyColorCheckerXmpOptions &options,
                                     const ErrorCode code, const std::string_view reason)
    {
        auto imported = import_legacy_xmp(
            {legacy_color_checker_xmp(options), {"asset-1", "file:///fixture.raw", std::nullopt}});
        ASSERT_FALSE(imported);
        EXPECT_EQ(imported.error().code, code);
        EXPECT_EQ(imported.error().context.at("reason"), reason);
    };

    LegacyColorCheckerXmpOptions unsupported_version;
    unsupported_version.version = "3";
    expect_rejected(unsupported_version, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorchecker_version");
    LegacyColorCheckerXmpOptions disabled;
    disabled.enabled = "0";
    expect_rejected(disabled, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorchecker_enabled_state");
    LegacyColorCheckerXmpOptions noncanonical_revision;
    noncanonical_revision.history_position = "08";
    expect_rejected(noncanonical_revision, ErrorCode::kValidation,
                    "invalid_legacy_colorchecker_revision");
    for (const std::string_view priority : {"1", "00"})
    {
        LegacyColorCheckerXmpOptions multi;
        multi.multi_priority = priority;
        expect_rejected(multi, ErrorCode::kUnsupported,
                        "unsupported_legacy_colorchecker_multi_state");
    }
    LegacyColorCheckerXmpOptions named;
    named.multi_name = "second";
    expect_rejected(named, ErrorCode::kUnsupported, "unsupported_legacy_colorchecker_multi_state");
    LegacyColorCheckerXmpOptions hand_edited;
    hand_edited.multi_name_hand_edited = "1";
    expect_rejected(hand_edited, ErrorCode::kUnsupported,
                    "unsupported_legacy_colorchecker_multi_state");
    LegacyColorCheckerXmpOptions custom_blend;
    custom_blend.blend_parameters = kLegacyGammaBlendV9;
    expect_rejected(custom_blend, ErrorCode::kUnsupported, "unsupported_legacy_colorchecker_blend");
    LegacyColorCheckerXmpOptions explicit_mask;
    explicit_mask.extra_attributes = R"( darktable:mask_id="42")";
    expect_rejected(explicit_mask, ErrorCode::kUnsupported, "unsupported_legacy_colorchecker_mask");
    LegacyColorCheckerXmpOptions unknown;
    unknown.extra_attributes = R"( darktable:unproven="1")";
    expect_rejected(unknown, ErrorCode::kUnsupported, "unsupported_legacy_colorchecker_attribute");

    LegacyColorCheckerXmpOptions wrong_length;
    wrong_length.parameters = "00000000";
    expect_rejected(wrong_length, ErrorCode::kValidation, "invalid_legacy_colorchecker_parameters");
    ColorCheckerParams finite;
    const auto too_many_payload = legacy_color_checker_v2_hex(finite, 50);
    LegacyColorCheckerXmpOptions too_many;
    too_many.parameters = too_many_payload;
    expect_rejected(too_many, ErrorCode::kValidation, "invalid_legacy_colorchecker_parameters");
    ColorCheckerParams nonfinite;
    nonfinite.patches[0].source_lab[0] = std::numeric_limits<double>::quiet_NaN();
    const auto nonfinite_payload = legacy_color_checker_v2_hex(nonfinite, 1);
    LegacyColorCheckerXmpOptions invalid_component;
    invalid_component.parameters = nonfinite_payload;
    expect_rejected(invalid_component, ErrorCode::kValidation,
                    "invalid_legacy_colorchecker_parameters");

    LegacyColorCheckerXmpOptions duplicate;
    auto duplicate_result = import_legacy_xmp({legacy_color_checker_xmp({duplicate, duplicate}),
                                               {"asset-1", "file:///fixture.raw", std::nullopt}});
    ASSERT_FALSE(duplicate_result);
    EXPECT_EQ(duplicate_result.error().code, ErrorCode::kConflict);
    EXPECT_EQ(duplicate_result.error().context.at("reason"), "duplicate_legacy_colorchecker");
}

TEST_F(CliTest, LegacyXmpColorCheckerCensusPinsTheOneRealRecordAndFullDocumentNegative)
{
    const auto fixture_root = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests";
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

    std::size_t documents_naming_colorchecker = 0U;
    std::size_t record_count = 0U;
    std::optional<std::filesystem::path> record_path;
    for (const auto &path : xmp_paths)
    {
        const auto content = read_utf8_text_file(path.generic_string());
        ASSERT_TRUE(content) << content.error().message;
        documents_naming_colorchecker += content.value().find("colorchecker") != std::string::npos;
        QXmlStreamReader reader(
            QByteArray(content.value().data(), static_cast<qsizetype>(content.value().size())));
        while (!reader.atEnd())
        {
            reader.readNext();
            if (!reader.isStartElement() || reader.name() != u"li" ||
                xml_attribute_value(reader.attributes(), u"operation") != "colorchecker")
            {
                continue;
            }
            ++record_count;
            record_path = path;
            EXPECT_EQ(xml_attribute_value(reader.attributes(), u"num"), "8");
            EXPECT_EQ(xml_attribute_value(reader.attributes(), u"modversion"), "2");
            EXPECT_EQ(xml_attribute_value(reader.attributes(), u"enabled"), "1");
            EXPECT_EQ(xml_attribute_value(reader.attributes(), u"multi_priority"), "0");
            EXPECT_EQ(xml_attribute_value(reader.attributes(), u"multi_name"), "");
            EXPECT_EQ(xml_attribute_value(reader.attributes(), u"multi_name_hand_edited"),
                      std::nullopt);
            EXPECT_EQ(xml_attribute_value(reader.attributes(), u"blendop_version"), "11");
            EXPECT_EQ(xml_attribute_value(reader.attributes(), u"blendop_params"),
                      kLegacyColorCheckerDefaultBlend);
            EXPECT_EQ(xml_attribute_value(reader.attributes(), u"params"),
                      kLegacyColorCheckerV2FixturePayload);
            for (const auto &attribute : reader.attributes())
            {
                EXPECT_FALSE(attribute.name().contains(u"mask"));
            }
        }
        ASSERT_FALSE(reader.hasError()) << path << ": " << reader.errorString().toStdString();
    }
    EXPECT_EQ(documents_naming_colorchecker, 23U);
    ASSERT_EQ(record_count, 1U);
    ASSERT_TRUE(record_path.has_value());
    EXPECT_NE(record_path->generic_string().find("0098-colorchecker/colorchecker.xmp"),
              std::string::npos);

    const auto document = read_utf8_text_file(record_path->generic_string());
    ASSERT_TRUE(document) << document.error().message;
    const auto imported =
        import_legacy_xmp({document.value(), {"fixture", "file:///fixture.raw", std::nullopt}});
    ASSERT_FALSE(imported);
    EXPECT_EQ(imported.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(imported.error().context.at("legacy_operation"), "rawprepare");
}

TEST_F(CliTest, LegacyXmpAllowsAnEmptyMaskHistoryContainer)
{
    constexpr std::string_view xmp = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6">
    <darktable:masks_history><rdf:Seq/></darktable:masks_history>
  </rdf:Description>
</rdf:RDF>)";
    const LegacyXmpImportRequest request{xmp, {"asset-1", "file:///fixture.raw", std::nullopt}};

    const auto imported = import_legacy_xmp(request);

    ASSERT_TRUE(imported) << imported.error().message;
    EXPECT_TRUE(imported.value().masks.empty());
}

TEST_F(CliTest, LegacyXmpRejectsActualMaskHistoryWithoutACanonicalMaskGraph)
{
    constexpr std::string_view xmp = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6">
    <darktable:masks_history><rdf:Seq><rdf:li darktable:num="0"/></rdf:Seq></darktable:masks_history>
  </rdf:Description>
</rdf:RDF>)";
    const LegacyXmpImportRequest request{xmp, {"asset-1", "file:///fixture.raw", std::nullopt}};

    const auto imported = import_legacy_xmp(request);

    ASSERT_FALSE(imported);
    EXPECT_EQ(imported.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(imported.error().context.at("reason"), "unsupported_legacy_mask");
}

TEST_F(CliTest, LegacyXmpImportRejectsAnExistingOutputPathWithoutOverwritingIt)
{
    const auto directory = std::filesystem::temp_directory_path();
    const auto xmp_path = directory / "ravo-output-conflict.xmp";
    const auto recipe_path = directory / "ravo-output-conflict.recipe.json";
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
    {
        std::ofstream output(recipe_path, std::ios::binary);
        ASSERT_TRUE(output);
        output << "pre-existing recipe";
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
    const auto content = read_utf8_text_file(recipe_argument);
    const auto response = parse_json(stdout_stream.str());
    std::error_code ignored;
    std::filesystem::remove(xmp_path, ignored);
    std::filesystem::remove(recipe_path, ignored);

    EXPECT_EQ(exit_code, 6);
    ASSERT_TRUE(content) << content.error().message;
    EXPECT_EQ(content.value(), "pre-existing recipe");
    ASSERT_TRUE(response) << response.error().message;
    const auto *error = response.value().find("error");
    ASSERT_NE(error, nullptr);
    const auto *code = error->find("code");
    ASSERT_NE(code, nullptr);
    ASSERT_NE(code->string_if(), nullptr);
    EXPECT_EQ(*code->string_if(), "conflict");
    EXPECT_TRUE(stderr_stream.str().empty());
}

TEST_F(CliTest, JsonFailuresStayStructuredAndDoNotWriteHumanLogsToStdout)
{
    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    const std::vector<std::string_view> arguments{"recipe", "validate", "--json"};

    EXPECT_EQ(application.run(std::span{arguments}), 2);
    const auto response = parse_json(stdout_stream.str());
    ASSERT_TRUE(response) << response.error().message;
    const auto *ok = response.value().find("ok");
    ASSERT_NE(ok, nullptr);
    ASSERT_NE(ok->boolean_if(), nullptr);
    EXPECT_FALSE(*ok->boolean_if());
    const auto *error = response.value().find("error");
    ASSERT_NE(error, nullptr);
    const auto *code = error->find("code");
    ASSERT_NE(code, nullptr);
    ASSERT_NE(code->string_if(), nullptr);
    EXPECT_EQ(*code->string_if(), "invalid_argument");
    EXPECT_TRUE(stderr_stream.str().empty());
}

TEST_F(CliTest, CatalogCreateImportListPreviewAndDevelop)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-cli-catalog-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    const auto catalog = (root / "library.sqlite").string();
    const auto png = (std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests" /
                      "0000-nop" / "expected.png")
                         .generic_u8string();
    const std::string png_path(png.begin(), png.end());

    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);

    EXPECT_EQ(application.run(
                  std::vector<std::string_view>{"catalog", "create", "--path", catalog, "--json"}),
              0)
        << stdout_stream.str();
    auto created = parse_json(stdout_stream.str());
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_TRUE(stderr_stream.str().empty());

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{
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

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(
        application.run(std::vector<std::string_view>{"catalog", "rate", "--catalog", catalog,
                                                      "--asset-id", id, "--rating", "4", "--json"}),
        0)
        << stdout_stream.str();

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "develop", "--catalog", catalog, "--asset-id", id, "--exposure-ev",
                  "0.5", "--set", "vignette=0.4", "--set", "velvia=0.2", "--json"}),
              0)
        << stdout_stream.str();

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(
                  std::vector<std::string_view>{"catalog", "list", "--catalog", catalog, "--json"}),
              0)
        << stdout_stream.str();
    auto listed = parse_json(stdout_stream.str());
    ASSERT_TRUE(listed) << listed.error().message;
    data = listed.value().find("data");
    ASSERT_NE(data, nullptr);
    const auto *assets = data->find("assets");
    ASSERT_NE(assets, nullptr);
    ASSERT_NE(assets->array_if(), nullptr);
    ASSERT_EQ(assets->array_if()->size(), 1U);
    const auto *has_edits = assets->array_if()->front().find("has_edits");
    ASSERT_NE(has_edits, nullptr);
    ASSERT_NE(has_edits->boolean_if(), nullptr);
    EXPECT_TRUE(*has_edits->boolean_if());
    const auto *rating = assets->array_if()->front().find("rating");
    ASSERT_NE(rating, nullptr);
    ASSERT_NE(rating->number_if(), nullptr);
    EXPECT_EQ(rating->number_if()->text, "4");

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{"catalog", "preview", "--catalog",
                                                            catalog, "--asset-id", id, "--json"}),
              0)
        << stdout_stream.str();
    auto previewed = parse_json(stdout_stream.str());
    ASSERT_TRUE(previewed) << previewed.error().message;
    data = previewed.value().find("data");
    ASSERT_NE(data, nullptr);
    const auto *cache_path = data->find("cache_path");
    ASSERT_NE(cache_path, nullptr);
    ASSERT_NE(cache_path->string_if(), nullptr);
    EXPECT_TRUE(std::filesystem::exists(*cache_path->string_if()));
    EXPECT_TRUE(stderr_stream.str().empty());

    const auto export_png = (root / "cli-export.png").string();
    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "export", "--catalog", catalog, "--asset-id", id, "--output",
                  export_png, "--format", "png", "--json"}),
              0)
        << stdout_stream.str();
    auto exported = parse_json(stdout_stream.str());
    ASSERT_TRUE(exported) << exported.error().message;
    EXPECT_TRUE(std::filesystem::exists(export_png));

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "export", "--catalog", catalog, "--asset-id", id, "--output",
                  export_png, "--format", "png", "--json"}),
              6)
        << stdout_stream.str();
    auto conflict = parse_json(stdout_stream.str());
    ASSERT_TRUE(conflict) << conflict.error().message;
    const auto *error = conflict.value().find("error");
    ASSERT_NE(error, nullptr);
    const auto *code = error->find("code");
    ASSERT_NE(code, nullptr);
    ASSERT_NE(code->string_if(), nullptr);
    EXPECT_EQ(*code->string_if(), "conflict");

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_F(CliTest, CatalogListJsonIncludesCapturedAtAndGps)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-cli-capture-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    const auto catalog = (root / "library.sqlite").string();
    const auto raw =
        (std::filesystem::path(RAVO_REPOSITORY_ROOT) / "legacy" / "tests" / "images" / "mire1.cr2")
            .generic_u8string();
    const std::string raw_path(raw.begin(), raw.end());
    const auto located_path = root / "located.tif";
    const auto below_zero_path = root / "below-zero.tif";
    {
        const auto bytes = test_support::make_capture_exif_tiff();
        std::ofstream output(located_path, std::ios::binary);
        output.write(reinterpret_cast<const char *>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    {
        test_support::CaptureExifProfile profile;
        profile.latitude_ref = 'S';
        profile.longitude_ref = 'W';
        profile.altitude_ref = 1U;
        profile.altitude = {0U, 1U};
        const auto bytes = test_support::make_capture_exif_tiff(profile);
        std::ofstream output(below_zero_path, std::ios::binary);
        output.write(reinterpret_cast<const char *>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }
    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine, stdout_stream, stderr_stream);
    EXPECT_EQ(application.run(
                  std::vector<std::string_view>{"catalog", "create", "--path", catalog, "--json"}),
              0);
    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(std::vector<std::string_view>{
                  "catalog", "import", "--catalog", catalog, "--input", raw_path, "--json"}),
              0)
        << stdout_stream.str();
    for (const auto &path : {located_path, below_zero_path})
    {
        stdout_stream.str({});
        stdout_stream.clear();
        const auto text = path.string();
        EXPECT_EQ(application.run(std::vector<std::string_view>{
                      "catalog", "import", "--catalog", catalog, "--input", text, "--json"}),
                  0)
            << stdout_stream.str();
    }
    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(
                  std::vector<std::string_view>{"catalog", "list", "--catalog", catalog, "--json"}),
              0)
        << stdout_stream.str();
    const std::string first_list = stdout_stream.str();
    auto listed = parse_json(first_list);
    ASSERT_TRUE(listed) << listed.error().message;
    const auto *data = listed.value().find("data");
    ASSERT_NE(data, nullptr);
    const auto *assets = data->find("assets");
    ASSERT_NE(assets, nullptr);
    ASSERT_EQ(assets->array_if()->size(), 3U);
    const auto find_asset = [&](const std::string_view filename) -> const JsonValue *
    {
        for (const auto &asset : *assets->array_if())
        {
            const auto *uri = asset.find("uri");
            if (uri != nullptr && uri->string_if() != nullptr &&
                uri->string_if()->find(filename) != std::string::npos)
            {
                return &asset;
            }
        }
        return nullptr;
    };
    const auto *raw_asset = find_asset("mire1.cr2");
    ASSERT_NE(raw_asset, nullptr);
    const auto *capture = raw_asset->find("capture");
    ASSERT_NE(capture, nullptr);
    const auto *captured_at = capture->find("captured_at");
    ASSERT_NE(captured_at, nullptr);
    ASSERT_NE(captured_at->string_if(), nullptr);
    EXPECT_EQ(*captured_at->string_if(), "2007-09-11T13:53:33.18");
    const auto *gps = capture->find("gps");
    ASSERT_NE(gps, nullptr);
    EXPECT_TRUE(gps->is_null());

    const auto verify_gps = [](const JsonValue &asset, const std::string_view latitude,
                               const std::string_view longitude, const std::string_view altitude)
    {
        const auto *capture_value = asset.find("capture");
        ASSERT_NE(capture_value, nullptr);
        const auto *captured_at_value = capture_value->find("captured_at");
        ASSERT_NE(captured_at_value, nullptr);
        ASSERT_NE(captured_at_value->string_if(), nullptr);
        EXPECT_EQ(*captured_at_value->string_if(), "2007-09-11T13:53:33.18+02:00");
        const auto *gps_value = capture_value->find("gps");
        ASSERT_NE(gps_value, nullptr);
        const auto *lat = gps_value->find("latitude");
        const auto *lon = gps_value->find("longitude");
        const auto *alt = gps_value->find("altitude_m");
        ASSERT_NE(lat, nullptr);
        ASSERT_NE(lon, nullptr);
        ASSERT_NE(alt, nullptr);
        ASSERT_NE(lat->number_if(), nullptr);
        ASSERT_NE(lon->number_if(), nullptr);
        ASSERT_NE(alt->number_if(), nullptr);
        EXPECT_EQ(lat->number_if()->text, latitude);
        EXPECT_EQ(lon->number_if()->text, longitude);
        EXPECT_EQ(alt->number_if()->text, altitude);
    };
    const auto *located = find_asset("located.tif");
    const auto *below_zero = find_asset("below-zero.tif");
    ASSERT_NE(located, nullptr);
    ASSERT_NE(below_zero, nullptr);
    verify_gps(*located, "49.253239", "3.050766", "123.456");
    verify_gps(*below_zero, "-49.253239", "-3.050766", "0");

    stdout_stream.str({});
    stdout_stream.clear();
    EXPECT_EQ(application.run(
                  std::vector<std::string_view>{"catalog", "list", "--catalog", catalog, "--json"}),
              0);
    EXPECT_EQ(stdout_stream.str(), first_list);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
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
    ASSERT_EQ(application.run(std::vector<std::string_view>{"catalog", "develop", "--catalog",
                                                            catalog, "--asset-id", id, "--set",
                                                            "exposure=1", "--json"}),
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

} // namespace
} // namespace ravo
