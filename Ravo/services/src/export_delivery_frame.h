#pragma once

#include "ravo/domain/types.h"
#include "ravo/engine/engine.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"
#include "ravo/recipe/canvas_frame.h"

namespace ravo
{

[[nodiscard]] Result<FrameParams>
frame_params_from_export_options(const ExportFrameOptions &options);

[[nodiscard]] Result<RenderedExportImage>
apply_export_delivery_frame(RenderedExportImage image, const ExportFrameOptions &options,
                            const CancellationToken &cancellation);

} // namespace ravo
