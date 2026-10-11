#pragma once

#include <string_view>
#include "ravo/recipe/develop.h"
#include "ravo/foundation/json.h"

namespace ravo
{
// Applies a bounded ordered numeric/text patch to an owned copy. All names,
// values and local attachment identities use the ordinary strict Recipe owner.
// local_id is borrowed only for this synchronous call; the result owns its data.
[[nodiscard]] Result<DevelopParams> apply_develop_patch(const DevelopParams &base,
                                                        const JsonValue &fields,
                                                        std::string_view local_id = {});
} // namespace ravo
