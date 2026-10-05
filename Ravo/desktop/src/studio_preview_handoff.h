#pragma once

#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"

namespace ravo
{
// Consume Engine's borrowed double-buffer token on the rendering executor,
// before another render can resize or reuse it. Only owned bytes cross to UI.
[[nodiscard]] Result<void> own_preview_pixels_for_handoff(PreviewResult &preview,
                                                          const CancellationToken &cancellation);
} // namespace ravo
