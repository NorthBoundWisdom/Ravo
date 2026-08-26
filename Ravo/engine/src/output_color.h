#pragma once

#include <cstdint>
#include <vector>

#include "ravo/engine/engine.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/color.h"
#include "ravo/foundation/error.h"
#include "ravo/recipe/color_output.h"

namespace ravo
{

struct ProfiledOutputBuffer
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> channels;
    ColorProfileState color_profile;
};

[[nodiscard]] Result<OutputColorParams> resolve_output_color(const Recipe &recipe);
[[nodiscard]] Result<std::string> output_color_cache_fingerprint(const Recipe &recipe);
[[nodiscard]] Result<void> validate_output_profile_state(const ColorProfileState &profile);
[[nodiscard]] Result<ProfiledOutputBuffer>
apply_output_color(const LinearWorkingBuffer &input, const OutputColorParams &params,
                   const CancellationToken &cancellation);

} // namespace ravo
