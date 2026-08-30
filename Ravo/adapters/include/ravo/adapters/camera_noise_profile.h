#pragma once

#include <string>
#include <string_view>

#include "ravo/foundation/camera_noise.h"
#include "ravo/foundation/error.h"

namespace ravo
{

inline constexpr std::uintmax_t kCameraNoiseDocumentMaximumBytes = 4U * 1024U * 1024U;

[[nodiscard]] Result<CameraNoiseCalibrationDocument>
parse_camera_noise_calibration_json(std::string_view text);
[[nodiscard]] Result<std::string>
serialize_camera_noise_calibration_json(const CameraNoiseCalibrationDocument &document);
[[nodiscard]] Result<std::string>
camera_noise_calibration_sha256(const CameraNoiseCalibrationDocument &document);

[[nodiscard]] Result<std::string>
serialize_camera_noise_profile_json(const CameraNoiseIdentity &identity, const CameraNoiseFit &fit,
                                    std::string_view source_samples_sha256);
[[nodiscard]] Result<CameraNoiseProfile> parse_camera_noise_profile_json(std::string_view text);

} // namespace ravo
