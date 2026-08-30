#pragma once

#include <cstdint>
#include <vector>

#include "ravo/engine/engine.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/color.h"
#include "ravo/foundation/error.h"
#include "ravo/recipe/color_input.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

struct ProfiledColorBuffer
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> channels;
    ColorProfileState color_profile;
    CanonicalRoiScale canonical_roi_scale{};
    std::optional<AttachedPixelFrame> mask_attached_frame;
};

[[nodiscard]] Result<InputColorParams> resolve_input_color(const Recipe &recipe);
[[nodiscard]] Result<std::string> input_color_cache_fingerprint(const Recipe &recipe);
[[nodiscard]] Result<LinearWorkingBuffer> apply_input_color(const ProfiledColorBuffer &input,
                                                            const InputColorParams &params,
                                                            const CancellationToken &cancellation);
[[nodiscard]] Result<LinearWorkingBuffer>
convert_working_profile(const LinearWorkingBuffer &input, std::string_view target_profile,
                        const CancellationToken &cancellation);
// Resolves the matrix/profile tag for a built-in linear working space without
// transforming pixels. This remains engine-private so resource adapters and UI
// code cannot manufacture colour-profile state.
[[nodiscard]] Result<ColorProfileState>
builtin_linear_working_profile_state(std::string_view target_profile);

} // namespace ravo
