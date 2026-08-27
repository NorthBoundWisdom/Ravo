#pragma once

#include <cstddef>
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
// Shared layout/model checks for every packer. bytes_per_pixel is 3, 6, or 12.
[[nodiscard]] Result<std::size_t>
validate_profiled_output_for_pack(const ProfiledOutputBuffer &input, std::size_t bytes_per_pixel);

// Synchronous final display boundary. The input and token are borrowed only for
// this call; a successful result owns both its RGB8 samples and exact profile state.
[[nodiscard]] Result<RenderedImage>
encode_profiled_output_rgb8(const ProfiledOutputBuffer &input,
                            const CancellationToken &cancellation);

// Packs one ProfiledOutputBuffer into exactly one tagged payload. RGB8 uses the
// frozen 255-scale rule; RGB16 uses round(clamp(s,0,1)*65535); float keeps
// finite post-output-colour values without integer clamping.
[[nodiscard]] Result<RenderedExportImage>
encode_profiled_output(const ProfiledOutputBuffer &input, RenderSampleKind sample_kind,
                       const CancellationToken &cancellation);

} // namespace ravo
