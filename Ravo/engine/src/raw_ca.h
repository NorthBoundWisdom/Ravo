#pragma once

#include <array>

#include "ravo/engine/engine.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

[[nodiscard]] Result<void> apply_raw_cacorrect(DecodedRaw &raw,
                                               const OperationInstance &operation,
                                               const std::array<float, 4> &white_balance,
                                               const CancellationToken &cancellation);

} // namespace ravo
