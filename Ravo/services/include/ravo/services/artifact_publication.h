#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo
{

// Atomically publishes a complete UTF-8 artifact without replacing any path
// that already exists, including one created concurrently before publication.
[[nodiscard]] Result<void>
publish_text_artifact_no_replace(std::string_view destination, std::string_view utf8_text,
                                 const CancellationToken &cancellation = {});

// Public thin entry over the encoded-bytes publication owner. Same no-replace
// linearization as write_bytes_atomically / publish_no_replace; borrows bytes
// for the synchronous call and does not replace a concurrent competitor.
[[nodiscard]] Result<void>
publish_bytes_artifact_no_replace(std::string_view destination,
                                  const std::vector<std::uint8_t> &bytes,
                                  const CancellationToken &cancellation = {});

} // namespace ravo
