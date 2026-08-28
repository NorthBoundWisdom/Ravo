#pragma once

#include "ravo/engine/engine.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

[[nodiscard]] Result<void> apply_raw_denoise(DecodedRaw &raw, const OperationInstance &operation,
                                             const CancellationToken &cancellation);

} // namespace ravo
