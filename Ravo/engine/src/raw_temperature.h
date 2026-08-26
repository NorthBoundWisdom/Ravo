#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "image_ops.h"
#include "ravo/recipe/develop.h"

namespace ravo
{

struct ResolvedTemperature
{
    std::array<float, kTemperatureChannelCount> coefficients{1.0F, 1.0F, 1.0F, 1.0F};
    std::string mode{std::string(kTemperatureModeAsShot)};
};

[[nodiscard]] Result<ResolvedTemperature> resolve_raw_temperature(const DecodedRaw &raw,
                                                                  const Recipe &recipe);

[[nodiscard]] Result<void> apply_temperature_rgb(WorkingImage &image,
                                                 const OperationInstance &operation,
                                                 const CancellationToken &cancellation);

[[nodiscard]] Result<std::vector<float>>
scale_temperature_cfa(const std::vector<float> &input, std::uint32_t width, std::uint32_t height,
                      std::uint32_t cfa_width, std::uint32_t cfa_height,
                      const std::vector<std::uint8_t> &cfa_channels,
                      const std::array<float, kTemperatureChannelCount> &coefficients,
                      const CancellationToken &cancellation);

} // namespace ravo
