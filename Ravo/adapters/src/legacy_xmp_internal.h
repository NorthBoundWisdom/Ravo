#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <QtCore/QXmlStreamReader>

#include "ravo/adapters/legacy_xmp.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/mask.h"

namespace ravo::legacy_xmp_internal
{

struct LegacyExposureParams
{
    ExposureParams params;
};

struct LegacyExposureCandidate
{
    QXmlStreamAttributes attributes;
    std::uint64_t history_position = 0;
};

struct LegacyColorBalanceCandidate
{
    QXmlStreamAttributes attributes;
    std::uint64_t history_position = 0;
};

struct LegacyColorCheckerCandidate
{
    QXmlStreamAttributes attributes;
    std::uint64_t history_position = 0;
};

struct LegacyColorCorrectionCandidate
{
    QXmlStreamAttributes attributes;
    std::uint64_t history_position = 0;
};

struct LegacyColorContrastCandidate
{
    QXmlStreamAttributes attributes;
    std::uint64_t history_position = 0;
};

struct LegacyColorHarmonizerCandidate
{
    QXmlStreamAttributes attributes;
    std::uint64_t history_position = 0;
};

struct LegacyColorReconstructionCandidate
{
    QXmlStreamAttributes attributes;
    std::uint64_t history_position = 0;
};

struct LegacySharpenCandidate
{
    QXmlStreamAttributes attributes;
    std::uint64_t history_position = 0;
};

struct LegacyDehazeCandidate
{
    QXmlStreamAttributes attributes;
    std::uint64_t history_position = 0;
};

struct LegacyOutputDitherCandidate
{
    QXmlStreamAttributes attributes;
    std::uint64_t history_position = 0;
};

struct LegacyCanvasCandidate
{
    QXmlStreamAttributes attributes;
    std::uint64_t history_position = 0;
};

struct LegacyFrameCandidate
{
    QXmlStreamAttributes attributes;
    std::uint64_t history_position = 0;
};

struct LegacyColorZonesCandidate
{
    QXmlStreamAttributes attributes;
    std::uint64_t history_position = 0;
};

struct LegacyMonochromeCandidate
{
    QXmlStreamAttributes attributes;
    std::uint64_t history_position = 0;
};

struct LegacySplitToningCandidate
{
    QXmlStreamAttributes attributes;
    std::uint64_t history_position = 0;
};

struct LegacyVelviaCandidate
{
    QXmlStreamAttributes attributes;
    std::uint64_t history_position = 0;
};

struct LegacyRetouchCandidate
{
    QXmlStreamAttributes attributes;
    std::uint64_t history_position = 0;
};

struct LegacyMaskRecord
{
    std::uint64_t history_position = 0;
    std::int32_t id = 0;
    std::int32_t type = 0;
    std::int32_t version = 0;
    std::size_t point_count = 0U;
    std::string points;
    std::string source;
};

struct LegacyRetouchMapping
{
    OperationInstance operation;
    std::vector<Mask> masks;
};

// Repository-history evidence used only for the synthetic legacy v1 upgrade.

[[nodiscard]] std::string utf8(QStringView value);
[[nodiscard]] Result<void> validate_asset(const AssetDescriptor &asset);
[[nodiscard]] Result<std::string> required_attribute(const QXmlStreamAttributes &attributes,
                                                     QStringView name, std::string_view operation);
[[nodiscard]] std::optional<std::string> attribute_value(const QXmlStreamAttributes &attributes,
                                                         QStringView name);
[[nodiscard]] bool has_attribute(const QXmlStreamAttributes &attributes, QStringView name);
[[nodiscard]] std::int32_t read_i32(const std::vector<std::uint8_t> &data,
                                    std::size_t offset) noexcept;
[[nodiscard]] float read_f32(const std::vector<std::uint8_t> &data, std::size_t offset) noexcept;
[[nodiscard]] Result<std::vector<std::uint8_t>>
decode_legacy_parameter_blob(std::string_view encoded, std::size_t expected_size,
                             std::string_view operation);
[[nodiscard]] Result<std::vector<std::uint8_t>>
decode_legacy_parameter_blob_min(std::string_view encoded, std::size_t minimum_size,
                                 std::string_view operation);
[[nodiscard]] Result<std::string> fixed_string(const std::vector<std::uint8_t> &data,
                                               std::size_t offset, std::size_t capacity);
[[nodiscard]] Result<std::uint64_t> legacy_history_position(std::string_view value,
                                                            std::string_view attribute,
                                                            std::string_view operation,
                                                            std::string_view reason);

inline constexpr std::array<std::array<float, 3>, kColorCheckerDefaultPatchCount>
    kLegacyColorCheckerV1Sources{{
        {39.19F, 13.76F, 14.29F},  {65.18F, 19.00F, 17.32F},  {49.46F, -4.23F, -22.95F},
        {42.85F, -13.33F, 22.12F}, {55.18F, 9.44F, -24.94F},  {70.36F, -32.77F, -0.04F},
        {62.92F, 35.49F, 57.10F},  {40.75F, 11.41F, -46.03F}, {52.10F, 48.11F, 16.89F},
        {30.67F, 21.19F, -20.81F}, {73.08F, -23.55F, 56.97F}, {72.43F, 17.48F, 68.20F},
        {30.97F, 12.67F, -46.30F}, {56.43F, -40.66F, 31.94F}, {43.40F, 50.68F, 28.84F},
        {82.45F, 2.41F, 80.25F},   {51.98F, 50.68F, -14.84F}, {51.02F, -27.63F, -28.03F},
        {95.97F, -0.40F, 1.24F},   {81.10F, -0.83F, -0.43F},  {66.81F, -1.08F, -0.70F},
        {50.98F, -0.19F, -0.30F},  {35.72F, -0.69F, -1.11F},  {21.46F, 0.06F, -0.95F},
    }};

struct BuiltinRawOperation
{
    std::string_view id;
    std::string_view version;
    std::string_view parameters;
};

inline constexpr std::array kBuiltinRawOperations{
    BuiltinRawOperation{"rawprepare", "1",
                        "1e000000120000000600000002000000060406040204020420350000"},
    BuiltinRawOperation{"temperature", "3", "006007400000803f0000b33f0000c07f"},
    BuiltinRawOperation{"highlights", "2", "000000000000803f00000000000000000000803f"},
    BuiltinRawOperation{"demosaic", "3", "0000000000000000000000000000000000000000"},
};

inline constexpr std::string_view kDefaultBlendParameters =
    "gz11eJxjYGBgkGAAgRNODGiAEV0AJ2iwh+CRyscOAAdeGQQ=";
inline constexpr std::string_view kPrimariesDefaultBlendParameters =
    "gz09eJxjYGBgYAFiCQYYOOHEgAZY0QVwggZ7CB6pfOygYtaVAyCMi48L/AcCEA0AmawnoA==";

struct LegacyGammaBlendTuple
{
    std::string_view version;
    std::string_view parameters;
};

inline constexpr std::string_view kGammaBlendGz14GuideOne =
    "gz14eJxjYIAACQYYOOHEgAYY0QVwggZ7CB6pfNoAAEkgGQQ=";
inline constexpr std::string_view kGammaBlendGz14GuideFive =
    "gz14eJxjYIAACQYYOOHEgAZY0QVwggZ7CB6pfNoAAE8gGQg=";
inline constexpr std::string_view kGammaBlendGz12GuideOne =
    "gz12eJxjYIAACQYYOOHEgAYY0QVwggZ7CB6pfOqC/0AAogFjBh0A";
inline constexpr std::string_view kGammaBlendGz12GuideFive =
    "gz12eJxjYIAACQYYOOHEgAZY0QVwggZ7CB6pfOqC/0AAogFpBh0E";
inline constexpr std::string_view kGammaBlendGz11FeatherV1 =
    "gz11eJxjYIAACQYYOOHEgAZY0QWAgBGLGANDgz0Ej1Q+dcF/IADRAGpyHQU=";
inline constexpr std::string_view kGammaBlendV11UncompressedGuideFive =
    "000000000000000018000000000000000000c84200000000000000000000000000000000050000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000";

inline constexpr std::array kLegacyGammaBlendTuples{
    LegacyGammaBlendTuple{"9", kDefaultBlendParameters},
    LegacyGammaBlendTuple{"10", kGammaBlendGz14GuideOne},
    LegacyGammaBlendTuple{"11", kGammaBlendV11UncompressedGuideFive},
    LegacyGammaBlendTuple{"11", kGammaBlendGz14GuideOne},
    LegacyGammaBlendTuple{"11", kGammaBlendGz14GuideFive},
    LegacyGammaBlendTuple{"12", kGammaBlendGz12GuideFive},
    LegacyGammaBlendTuple{"12", kGammaBlendGz14GuideOne},
    LegacyGammaBlendTuple{"12", kGammaBlendGz14GuideFive},
    LegacyGammaBlendTuple{"13", kGammaBlendGz11FeatherV1},
    LegacyGammaBlendTuple{"13", kGammaBlendGz12GuideOne},
    LegacyGammaBlendTuple{"13", kGammaBlendGz12GuideFive},
    LegacyGammaBlendTuple{"14", kGammaBlendGz11FeatherV1},
};

inline constexpr std::string_view kExposureBlendV11UncompressedScene =
    "000000000300000018000000000000000000c84200000000000000000000000000000000050000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f00000000000000000000803f"
    "0000803f00000000000000000000803f0000803f00000000000000000000803f0000803f000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000000000000";

inline constexpr std::array kLegacyExposureBlendTuples{
    LegacyGammaBlendTuple{"9", kDefaultBlendParameters},
    LegacyGammaBlendTuple{"10", "gz13eJxjYGBgYAZiCQYYOOHEgAYY0QVwggZ7CB6pfNoAAE4AGQc="},
    LegacyGammaBlendTuple{"11", kExposureBlendV11UncompressedScene},
    LegacyGammaBlendTuple{"11", "gz10eJxjYGBgYAFiCQYYOOHEgAZY0QVwggZ7CB6pfOygYtaVAyCMi08IAAB/xiOk"},
    LegacyGammaBlendTuple{"11", "gz13eJxjYGBgYAZiCQYYOOHEgAYY0QVwggZ7CB6pfNoAAE4AGQc="},
    LegacyGammaBlendTuple{"11", "gz13eJxjYGBgYAZiCQYYOOHEgAZY0QVwggZ7CB6pfNoAAFQAGQs="},
    LegacyGammaBlendTuple{"12", kPrimariesDefaultBlendParameters},
    LegacyGammaBlendTuple{"12", "gz10eJxjYGBgYAFiCQYYOOHEgAZY0QVwggZ7CB6pfOygYtaVAyCMi08IAAB/xiOk"},
    LegacyGammaBlendTuple{
        "13", "gz08eJxjYGBgYAFiCQYYOOHEgAZY0QWAgBGLGANDgz0Ej1Q+dlAx68oBEMbFxwX+AwGIBgCbGCeh"},
    LegacyGammaBlendTuple{"13", kPrimariesDefaultBlendParameters},
    LegacyGammaBlendTuple{
        "14", "gz08eJxjYGBgYAFiCQYYOOHEgAZY0QWAgBGLGANDgz0Ej1Q+dlAx68oBEMbFxwX+AwGIBgCbGCeh"},
};

inline constexpr std::array<std::string_view, 2> kFrozenColorBalanceParametricBlends{
    "gz06eJxjZWBgYGYAgRNODFDAyASlGfADjrXTbE8t2m/rW1Brz8DQYI+QaRgQvqYrl/3DZH77p/8r7OfK1dHRfuwAALvfIn8=",
    "gz05eJxjZWBgYGEAgRNODFDAyASlGfADjrXTbE8t2m/78Lik/cxncvYImQZ7CKYvf4M0l32GFb991c4ie6Mb5XS0HzsAACQpI3A=",
};

inline constexpr std::string_view kFrozenColorCheckerBlendV11 =
    "gz13eJxjYGBgYAJiCQYYOOHEgAZY0QVwggZ7CB6pfNoAAFJgGQo=";

inline constexpr std::array kFrozenColorCorrectionBlendTuples{
    LegacyGammaBlendTuple{"9", kDefaultBlendParameters},
    LegacyGammaBlendTuple{"11", "gz13eJxjYGBgYAJiCQYYOOHEgAZY0QVwggZ7CB6pfNoAAFJgGQo="},
};

inline constexpr std::string_view kFrozenColorContrastBlendV10 =
    "gz13eJxjYGBgYAJiCQYYOOHEgAYY0QVwggZ7CB6pfNoAAExgGQY=";

inline constexpr std::string_view kFrozenColorHarmonizerBlendV14 =
    "gz08eJxjYGBgYAFiCQYYOOHEgAZY0QWAgBGLGANDgz0Ej1Q+dlAx68oBEMbFxwX+AwGIBgCbGCeh";

inline constexpr std::string_view kFrozenColorReconstructionBlendV10 =
    "gz13eJxjYGBgYAJiCQYYOOHEgAYY0QVwggZ7CB6pfNoAAExgGQY=";

inline constexpr std::string_view kFrozenSharpenBlendV11 =
    "gz13eJxjYGBgYAJiCQYYOOHEgAZY0QVwggZ7CB6pfNoAAFJgGQo=";

inline constexpr std::string_view kFrozenDehazeBlendV13 =
    "gz08eJxjYGBgYAFiCQYYOOHEgAZY0QWAgBGLGANDgz0Ej1Q+dlAx68oBEMbFxwX+AwGIBgCbGCeh";

inline constexpr std::string_view kLegacyFlipBlendGz14 =
    "gz14eJxjYIAACQYYOOHEgAYY0QVwggZ7CB6pfNoAAEkgGQQ=";
inline constexpr std::string_view kLegacyGeometryBlendGz14GuideFive =
    "gz14eJxjYIAACQYYOOHEgAZY0QVwggZ7CB6pfNoAAE8gGQg=";
inline constexpr std::string_view kLegacyToneBlendGz13 =
    "gz13eJxjYGBgYAZiCQYYOOHEgAYY0QVwggZ7CB6pfNoAAE4AGQc=";
inline constexpr std::string_view kLegacyRawDenoiseBlendGz13 =
    "gz13eJxjYGBgYARiCQYYOOHEgAYY0QVwggZ7CB6pfNoAAErAGQU=";

struct LeftoverRawDenoise
{
    double threshold = 0.0;
    std::array<std::array<double, 5>, 4> bands{{
        {{0.5, 0.5, 0.5, 0.5, 0.5}},
        {{0.5, 0.5, 0.5, 0.5, 0.5}},
        {{0.5, 0.5, 0.5, 0.5, 0.5}},
        {{0.5, 0.5, 0.5, 0.5, 0.5}},
    }};

    [[nodiscard]] bool is_identity() const noexcept
    {
        return threshold <= 0.0;
    }
};

[[nodiscard]] Result<OperationInstance>
map_color_checker_candidate(const LegacyColorCheckerCandidate &candidate);
[[nodiscard]] Result<OperationInstance>
map_color_correction_candidate(const LegacyColorCorrectionCandidate &candidate);
[[nodiscard]] Result<OperationInstance>
map_color_contrast_candidate(const LegacyColorContrastCandidate &candidate);
[[nodiscard]] Result<OperationInstance>
map_color_harmonizer_candidate(const LegacyColorHarmonizerCandidate &candidate);
[[nodiscard]] Result<OperationInstance>
map_color_reconstruction_candidate(const LegacyColorReconstructionCandidate &candidate);
[[nodiscard]] Result<OperationInstance>
map_sharpen_candidate(const LegacySharpenCandidate &candidate);
[[nodiscard]] Result<OperationInstance>
map_dehaze_candidate(const LegacyDehazeCandidate &candidate);
[[nodiscard]] Result<OperationInstance>
map_output_dither_candidate(const LegacyOutputDitherCandidate &candidate);
[[nodiscard]] Result<OperationInstance>
map_canvas_candidate(const LegacyCanvasCandidate &candidate);
[[nodiscard]] Result<OperationInstance> map_frame_candidate(const LegacyFrameCandidate &candidate);
[[nodiscard]] Result<OperationInstance>
map_color_zones_candidate(const LegacyColorZonesCandidate &candidate);
[[nodiscard]] Result<OperationInstance>
map_monochrome_candidate(const LegacyMonochromeCandidate &candidate);
[[nodiscard]] Result<OperationInstance>
map_split_toning_candidate(const LegacySplitToningCandidate &candidate);
[[nodiscard]] Result<OperationInstance>
map_velvia_candidate(const LegacyVelviaCandidate &candidate);
[[nodiscard]] Result<OperationInstance>
map_color_balance_candidate(const LegacyColorBalanceCandidate &candidate);
[[nodiscard]] Result<OperationInstance>
map_exposure_candidate(const LegacyExposureCandidate &candidate);
[[nodiscard]] Result<void> absorb_legacy_gamma(const QXmlStreamAttributes &attributes);
[[nodiscard]] Result<bool> absorb_builtin_raw_operation(std::string_view operation,
                                                        const QXmlStreamAttributes &attributes);
[[nodiscard]] Result<LeftoverFlipGeometry> map_legacy_flip(const QXmlStreamAttributes &attributes);
[[nodiscard]] Result<LeftoverCropBox> map_legacy_crop(const QXmlStreamAttributes &attributes);
[[nodiscard]] Result<PerspectiveParams> map_legacy_ashift(const QXmlStreamAttributes &attributes);
[[nodiscard]] Result<RgbLevelsParams> map_legacy_rgblevels(const QXmlStreamAttributes &attributes);
[[nodiscard]] Result<RgbCurveParams> map_legacy_rgbcurve(const QXmlStreamAttributes &attributes);
[[nodiscard]] Result<LeftoverRawDenoise>
map_legacy_rawdenoise(const QXmlStreamAttributes &attributes);
[[nodiscard]] Result<LegacyRetouchCandidate>
capture_retouch_candidate(const QXmlStreamAttributes &attributes);
[[nodiscard]] Result<LegacyRetouchMapping>
map_retouch_candidate(const LegacyRetouchCandidate &candidate,
                      const std::vector<LegacyMaskRecord> &mask_records);
[[nodiscard]] Result<std::vector<LegacyMaskRecord>>
parse_legacy_mask_history(QXmlStreamReader &reader);

} // namespace ravo::legacy_xmp_internal
