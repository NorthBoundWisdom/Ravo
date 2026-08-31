#pragma once

#include <cstdint>
#include <string_view>
#include <system_error>

#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo::recovery_publication_internal
{

enum class Checkpoint : std::uint8_t
{
    kBeforeTemporaryOpen,
    kTemporaryCreated,
    kBeforeTemporaryWrite,
    kTemporaryChunkWritten,
    kBeforeTemporarySync,
    kBeforePublish,
};

struct CheckpointHook
{
    using Callback = std::error_code (*)(void *context, Checkpoint checkpoint,
                                         std::string_view path,
                                         std::uint64_t bytes_processed) noexcept;

    Callback callback = nullptr;
    void *context = nullptr;
};

// Source-private filesystem-adapter seam. Production calls this without a hook;
// adapter tests inject real stage failures and cancellation without exposing a
// product compatibility switch.
[[nodiscard]] Result<void> publish_no_replace(std::string_view output, std::string_view document,
                                              const CancellationToken &cancellation,
                                              CheckpointHook hook = {});

} // namespace ravo::recovery_publication_internal
