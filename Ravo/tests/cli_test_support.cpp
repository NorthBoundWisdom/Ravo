#include "cli_test_support.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <png.h>

#include <utility>

#include <QCoreApplication>

#include "ravo/foundation/log.h"

namespace ravo
{
namespace
{

void ensure_cli_test_qt_core()
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

} // namespace

CliTest::CliTest()
    : engine(
          []
          {
              auto created = EngineFacade::create_phase1();
              return std::move(created).value();
          }())
{
}

CliTest::~CliTest() = default;

void CliTest::SetUp()
{
    ensure_cli_test_qt_core();
    const auto created = EngineFacade::create_phase1();
    ASSERT_TRUE(created) << created.error().message;
    engine = std::move(created).value();
}

[[nodiscard]] std::string mire1_path()
{
    const auto path = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" /
                      "frozen" / "images" / "mire1.cr2";
    const auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

[[nodiscard]] std::string mire1_xtrans_path()
{
    const auto path = std::filesystem::path(RAVO_REPOSITORY_ROOT) / "Ravo" / "tests" / "fixtures" /
                      "frozen" / "images" / "mire1-xtrans.raf";
    const auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

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

const std::string_view kLegacyPrimariesPayload =
    "cc56143f4c37093e22c9293ed9ce573f448d12be7b149e3f1e206a3e85eb513f";
const std::string_view kLegacyPrimariesDefaultBlend =
    "gz09eJxjYGBgYAFiCQYYOOHEgAZY0QVwggZ7CB6pfOygYtaVAyCMi48L/AcCEA0AmawnoA==";

const std::string_view kLegacyGammaBlendV9 = "gz11eJxjYGBgkGAAgRNODGiAEV0AJ2iwh+CRyscOAAdeGQQ=";
const std::string_view kLegacyGammaBlendGz14GuideOne =
    "gz14eJxjYIAACQYYOOHEgAYY0QVwggZ7CB6pfNoAAEkgGQQ=";
const std::string_view kLegacyGammaBlendGz14GuideFive =
    "gz14eJxjYIAACQYYOOHEgAZY0QVwggZ7CB6pfNoAAE8gGQg=";
const std::string_view kLegacyGammaBlendGz12GuideOne =
    "gz12eJxjYIAACQYYOOHEgAYY0QVwggZ7CB6pfOqC/0AAogFjBh0A";
const std::string_view kLegacyGammaBlendGz12GuideFive =
    "gz12eJxjYIAACQYYOOHEgAZY0QVwggZ7CB6pfOqC/0AAogFpBh0E";
const std::string_view kLegacyGammaBlendGz11FeatherV1 =
    "gz11eJxjYIAACQYYOOHEgAZY0QWAgBGLGANDgz0Ej1Q+dcF/IADRAGpyHQU=";
const std::string_view kLegacyGammaBlendV11UncompressedGuideFive =
    "000000000000000018000000000000000000c84200000000000000000000000000000000050000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000";

const std::array<FrozenLegacyGammaBlendTuple, 12> kFrozenLegacyGammaBlendTuples{
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

[[nodiscard]] std::string legacy_gamma_xmp(const LegacyGammaXmpOptions &options)
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
                                          const std::string_view blend)
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
                                          const std::string_view version,
                                          const std::string_view blend)
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

const std::string_view kLegacyExposureV5ManualOne = "00000000000000000000803f00004842000080c0";
const std::string_view kLegacyExposureV6ManualOneBias =
    "00000000000000000000803f00004842000080c001000000";
const std::string_view kLegacyExposureV7ManualOneBothCompensations =
    "00000000000000000000803f00004842000080c00100000001000000";
const std::string_view kLegacyColorBalanceV3FixturePayload =
    "010000000000803f0000803f0000803f0000803ffeff7f3f0000803f0000803f0000803f"
    "0000803f0000803f0000803f0000803f0000803f8de4aa3f024ee1410000803f";
const std::string_view kLegacyColorCheckerV2FixturePayload =
    "gz04eJw7/FXcqTW72Sloh7tTTL+O05KCGCDudbq+uMqpWkTB6WGVD5D9yTGGf6KTx8N+p0LbZ462XLFOaWkaTodLFzv1H/"
    "J3+qbhA1R70Em+dZGTbVcrUI83UA+fk8i6xY4MdACHv0YA3TTR8ZvGnAOzZgYeXFLA6zhrJuuhde4CQD+pOR7+6uDEdX2"
    "7o8i6vQc9Hs5y9HiY5KgZI3lIvjXUaZ17o8Phr+5OtlxPD37TuLOP67ryfo+HVvtnzZTcZ8t1HchfvJc+fkhwfFjV57ik"
    "YP3Bde7rHY2NTx88e8ZnX1paCtDt5ocYGJocg3asOWjLlQqMq3YnY2PPQ4e//nKUb33oeH3xfKcdcpkHd8g9PfgmcIZ9t"
    "ci6fUAj9xfadu1bUjB3/5vAiv308QPutPSvfidJaenQ14nwtNR/yHRQpCU5w8NOpKSlK4v1nQZbWqpcvNSJlLSkb8wPVA9"
    "JS68Dc53olZYkgBgAYTUMzA==";
const std::string_view kLegacyColorCheckerDefaultBlend =
    "gz13eJxjYGBgYAJiCQYYOOHEgAZY0QVwggZ7CB6pfNoAAFJgGQo=";

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

[[nodiscard]] std::string legacy_color_checker_xmp(const LegacyColorCheckerXmpOptions &options)
{
    return legacy_color_checker_xmp(std::vector<LegacyColorCheckerXmpOptions>{options});
}

const std::string_view kLegacyColorContrastV2Parameters =
    "6666264000000000000020400000000001000000";
const std::string_view kLegacyColorContrastV1Parameters = "66662640000000000000204000000000";
const std::string_view kLegacyColorContrastDefaultBlend =
    "gz13eJxjYGBgYAJiCQYYOOHEgAYY0QVwggZ7CB6pfNoAAExgGQY=";

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

[[nodiscard]] std::string legacy_color_contrast_xmp(const LegacyColorContrastXmpOptions &options)
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

[[nodiscard]] std::string legacy_color_checker_v2_hex(const ColorCheckerParams &params,
                                                      const std::int32_t count,
                                                      const std::optional<std::size_t> dirty_tail)
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

[[nodiscard]] std::string legacy_exposure_xmp(const LegacyExposureXmpOptions &options)
{
    return legacy_exposure_xmp(std::vector<LegacyExposureXmpOptions>{options});
}

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

[[nodiscard]] std::string legacy_color_balance_xmp(const LegacyColorBalanceXmpOptions &options)
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
legacy_primaries_xmp(const std::string_view version, const std::string_view enabled,
                     const std::string_view parameters, const std::string_view blend,
                     const std::size_t instances, const std::string_view blend_version,
                     const std::string_view extra_attributes)
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

[[nodiscard]] std::string legacy_rgblevels_xmp(const std::string_view parameters,
                                               const std::string_view blend)
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

} // namespace ravo
