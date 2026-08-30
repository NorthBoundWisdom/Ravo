#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ravo
{

inline constexpr std::string_view kCameraNoiseSampleSchema = "ravo.camera-noise-samples";
inline constexpr std::uint32_t kCameraNoiseSampleSchemaVersion = 1U;
inline constexpr std::string_view kCameraNoiseProfileSchema = "ravo.camera-noise-profile";
inline constexpr std::uint32_t kCameraNoiseProfileSchemaVersion = 1U;
inline constexpr std::string_view kCameraNoiseSignalUnits = "black_subtracted_uint16_code_values";
inline constexpr std::string_view kCameraNoiseModel =
    "variance=gaussian_variance+poisson_slope*signal";
inline constexpr std::string_view kCameraNoiseFitPolicy = "theil_sen_mad_4_5_weighted_nnls_v1";
inline constexpr std::size_t kCameraNoiseMinimumSamples = 8U;
inline constexpr std::size_t kCameraNoiseMaximumSamples = 1024U;
inline constexpr double kCameraNoiseMinimumSignalSpan = 256.0;

struct CameraNoiseIdentity
{
    std::string make;
    std::string model;
    std::uint32_t iso = 0U;

    [[nodiscard]] bool operator==(const CameraNoiseIdentity &) const = default;
};

struct CameraNoiseSample
{
    double signal_mean = 0.0;
    double variance = 0.0;
    std::uint64_t count = 0U;

    [[nodiscard]] bool operator==(const CameraNoiseSample &) const = default;
};

struct CameraNoiseCalibrationDocument
{
    CameraNoiseIdentity identity;
    std::vector<CameraNoiseSample> samples;

    [[nodiscard]] bool operator==(const CameraNoiseCalibrationDocument &) const = default;
};

struct CameraNoiseFit
{
    double gaussian_variance = 0.0;
    double poisson_slope = 0.0;
    double weighted_rmse = 0.0;
    double weighted_r_squared = 0.0;
    std::size_t input_sample_count = 0U;
    std::size_t retained_sample_count = 0U;

    [[nodiscard]] bool operator==(const CameraNoiseFit &) const = default;
};

struct CameraNoiseProfile
{
    CameraNoiseIdentity identity;
    CameraNoiseFit fit;
    std::string source_samples_sha256;
    std::string payload_sha256;

    [[nodiscard]] bool operator==(const CameraNoiseProfile &) const = default;
};

} // namespace ravo
