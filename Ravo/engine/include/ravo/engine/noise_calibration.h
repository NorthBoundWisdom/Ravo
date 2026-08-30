#pragma once

#include <span>

#include "ravo/foundation/camera_noise.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo
{

// Fits variance = gaussian_variance + poisson_slope * signal_mean. The input is
// immutable and expressed in black-subtracted uint16 sensor code values.
[[nodiscard]] Result<CameraNoiseFit> fit_camera_noise(std::span<const CameraNoiseSample> samples,
                                                      const CancellationToken &cancellation = {});

} // namespace ravo
