#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <QXmlStreamReader>
#include <gtest/gtest.h>

#include "ravo/engine/engine.h"
#include "ravo/recipe/color_checker.h"

namespace ravo
{

class CliTest : public ::testing::Test
{
protected:
    CliTest();
    ~CliTest() override;

    void SetUp() override;

    EngineFacade engine;
};

[[nodiscard]] std::string mire1_path();
[[nodiscard]] std::string mire1_xtrans_path();

struct SourceFileSnapshot
{
    std::uintmax_t size = 0U;
    std::filesystem::file_time_type modified;
    std::uint64_t content_hash = 1469598103934665603ULL;

    [[nodiscard]] bool operator==(const SourceFileSnapshot &) const = default;
};

[[nodiscard]] std::optional<SourceFileSnapshot> source_file_snapshot(const std::string &path);
[[nodiscard]] std::uint64_t pixel_hash(const std::vector<std::uint8_t> &pixels) noexcept;

extern const std::string_view kLegacyPrimariesPayload;
extern const std::string_view kLegacyPrimariesDefaultBlend;

struct FrozenLegacyGammaBlendTuple
{
    std::string_view version;
    std::string_view parameters;
    std::size_t fixture_count;
};

extern const std::string_view kLegacyGammaBlendV9;
extern const std::string_view kLegacyGammaBlendGz14GuideOne;
extern const std::string_view kLegacyGammaBlendGz14GuideFive;
extern const std::string_view kLegacyGammaBlendGz12GuideOne;
extern const std::string_view kLegacyGammaBlendGz12GuideFive;
extern const std::string_view kLegacyGammaBlendGz11FeatherV1;
extern const std::string_view kLegacyGammaBlendV11UncompressedGuideFive;
extern const std::array<FrozenLegacyGammaBlendTuple, 12> kFrozenLegacyGammaBlendTuples;

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

[[nodiscard]] std::string legacy_gamma_xmp(const LegacyGammaXmpOptions &options = {});
[[nodiscard]] std::string legacy_flip_xmp(std::string_view parameters,
                                          std::string_view blend = kLegacyGammaBlendV9);
[[nodiscard]] std::string legacy_crop_xmp(std::string_view parameters,
                                          std::string_view version = "3",
                                          std::string_view blend = kLegacyGammaBlendV9);

extern const std::string_view kLegacyExposureV5ManualOne;
extern const std::string_view kLegacyExposureV6ManualOneBias;
extern const std::string_view kLegacyExposureV7ManualOneBothCompensations;
extern const std::string_view kLegacyColorBalanceV3FixturePayload;
extern const std::string_view kLegacyColorCheckerV2FixturePayload;
extern const std::string_view kLegacyColorCheckerDefaultBlend;

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
legacy_color_checker_xmp(const std::vector<LegacyColorCheckerXmpOptions> &entries);
[[nodiscard]] std::string
legacy_color_checker_xmp(const LegacyColorCheckerXmpOptions &options = {});

extern const std::string_view kLegacyColorContrastV2Parameters;
extern const std::string_view kLegacyColorContrastV1Parameters;
extern const std::string_view kLegacyColorContrastDefaultBlend;

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
legacy_color_contrast_xmp(const std::vector<LegacyColorContrastXmpOptions> &entries);
[[nodiscard]] std::string
legacy_color_contrast_xmp(const LegacyColorContrastXmpOptions &options = {});
[[nodiscard]] std::string
legacy_color_checker_v2_hex(const ColorCheckerParams &params, std::int32_t count,
                            std::optional<std::size_t> dirty_tail = std::nullopt);
[[nodiscard]] std::string legacy_color_checker_v1_hex(const ColorCheckerParams &params);

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

[[nodiscard]] std::string legacy_exposure_xmp(const std::vector<LegacyExposureXmpOptions> &entries);
[[nodiscard]] std::string legacy_exposure_xmp(const LegacyExposureXmpOptions &options = {});

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
legacy_color_balance_xmp(const std::vector<LegacyColorBalanceXmpOptions> &entries);
[[nodiscard]] std::string
legacy_color_balance_xmp(const LegacyColorBalanceXmpOptions &options = {});

[[nodiscard]] std::optional<std::string> xml_attribute_value(const QXmlStreamAttributes &attributes,
                                                             QStringView name);
[[nodiscard]] std::string
legacy_primaries_xmp(std::string_view version = "1", std::string_view enabled = "1",
                     std::string_view parameters = kLegacyPrimariesPayload,
                     std::string_view blend = kLegacyPrimariesDefaultBlend,
                     std::size_t instances = 1U, std::string_view blend_version = "13",
                     std::string_view extra_attributes = {});
[[nodiscard]] bool png_has_chunk(const std::filesystem::path &path,
                                 const std::array<char, 4> &type);
[[nodiscard]] std::vector<std::uint8_t> read_png_rgb(const std::filesystem::path &path);
[[nodiscard]] bool write_perspective_grid_png(const std::filesystem::path &path);
[[nodiscard]] std::string legacy_rgblevels_xmp(
    std::string_view parameters,
    std::string_view blend = "gz13eJxjYGBgYAZiCQYYOOHEgAYY0QVwggZ7CB6pfNoAAE4AGQc=");

} // namespace ravo
