#pragma once

#include <cstdint>

#include "output_color.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"
#include "ravo/recipe/output_dither.h"

namespace ravo
{

enum class OutputDitherTarget : std::uint8_t
{
    kPreviewRgb8,
    kExportRgb8,
    kExportRgb16,
    kExportRgbFloat,
};

[[nodiscard]] Result<ProfiledOutputBuffer>
apply_output_dither(ProfiledOutputBuffer input, const OutputDitherParams &params,
                    OutputDitherTarget target, const CancellationToken &cancellation);
[[nodiscard]] Result<ProfiledOutputBuffer>
apply_output_dither(ProfiledOutputBuffer input, const OperationInstance &operation,
                    OutputDitherTarget target, const CancellationToken &cancellation);

namespace detail
{

enum class OutputDitherCheckpoint : std::uint8_t
{
    kBeforeValidation,
    kProcessRow,
    kBeforePublication,
};

using OutputDitherCheckpointCallback = void (*)(void *context, OutputDitherCheckpoint checkpoint,
                                                std::uint32_t progress) noexcept;

struct OutputDitherControl
{
    void *context = nullptr;
    OutputDitherCheckpointCallback checkpoint_callback = nullptr;
};

[[nodiscard]] Result<ProfiledOutputBuffer>
apply_output_dither_controlled(ProfiledOutputBuffer input, const OutputDitherParams &params,
                               OutputDitherTarget target, const CancellationToken &cancellation,
                               OutputDitherControl control = {});

} // namespace detail

} // namespace ravo
