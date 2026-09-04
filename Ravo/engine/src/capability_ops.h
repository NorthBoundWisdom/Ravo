#pragma once

#include "image_ops.h"
#include "raw_pipeline.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

[[nodiscard]] Result<void> apply_raw_hotpixels(DecodedRaw &raw, const OperationInstance &operation,
                                               const CancellationToken &cancellation);
[[nodiscard]] Result<void> apply_raw_highlights(DecodedRaw &raw, const OperationInstance &operation,
                                                const std::array<float, 4> &white_balance,
                                                const CancellationToken &cancellation);
[[nodiscard]] Result<void> apply_denoise_profile(WorkingImage &image,
                                                 const OperationInstance &operation,
                                                 const CancellationToken &cancellation);
[[nodiscard]] Result<void> apply_lens_correction(WorkingImage &image,
                                                 const OperationInstance &operation,
                                                 const CancellationToken &cancellation);
[[nodiscard]] Result<void> apply_channel_mixer_rgb(WorkingImage &image,
                                                   const OperationInstance &operation,
                                                   const CancellationToken &cancellation);
[[nodiscard]] Result<void> apply_color_balance_rgb(WorkingImage &image,
                                                   const OperationInstance &operation,
                                                   const CancellationToken &cancellation);
[[nodiscard]] Result<void> apply_color_equalizer(WorkingImage &image,
                                                 const OperationInstance &operation,
                                                 const CancellationToken &cancellation);
[[nodiscard]] Result<void> apply_graduated_nd(WorkingImage &image,
                                              const OperationInstance &operation,
                                              const CancellationToken &cancellation);
[[nodiscard]] Result<void> apply_tone_equalizer(WorkingImage &image,
                                                const OperationInstance &operation,
                                                const CancellationToken &cancellation);

} // namespace ravo
