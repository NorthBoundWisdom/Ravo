#pragma once

#include <array>

#include "ravo/recipe/develop.h"

namespace ravo::test
{

[[nodiscard]] inline TemperatureParams temperature_0000_params()
{
    // Static little-endian decode of the schema-v3 blob in
    // Ravo/tests/fixtures/frozen/0000-nop/nop.xmp. Its unused fourth coefficient is NaN in
    // the frozen struct and is canonicalized to neutral 1 for a three-color CFA.
    TemperatureParams params;
    params.mode = std::string(kTemperatureModeManual);
    params.coefficients =
        std::array<double, kTemperatureChannelCount>{2.115234375, 1.0, 1.3984375, 1.0};
    return params;
}

[[nodiscard]] inline TemperatureParams temperature_0171_late_params()
{
    // Static little-endian decode of the schema-v4 preset-4 blob in
    // Ravo/tests/fixtures/frozen/0171-capture-sharpen/capture-sharpen.xmp.
    TemperatureParams params;
    params.mode = std::string(kTemperatureModeAsShotToReference);
    params.coefficients =
        std::array<double, kTemperatureChannelCount>{2.115234375, 1.0, 1.3984375, 1.0};
    return params;
}

[[nodiscard]] inline TemperatureParams temperature_0177_four_channel_params()
{
    // Static little-endian decode of the schema-v4 four-Bayer fixture.
    TemperatureParams params;
    params.mode = std::string(kTemperatureModeAsShotToReference);
    params.coefficients = std::array<double, kTemperatureChannelCount>{
        1.0491400957107544, 1.0, 0.6953316926956177, 0.7223587036132812};
    return params;
}

} // namespace ravo::test
