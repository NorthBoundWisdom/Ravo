#pragma once

#include <string_view>

#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo
{

// Atomically publishes a complete UTF-8 artifact without replacing any path
// that already exists, including one created concurrently before publication.
[[nodiscard]] Result<void>
publish_text_artifact_no_replace(std::string_view destination, std::string_view utf8_text,
                                 const CancellationToken &cancellation = {});

} // namespace ravo
