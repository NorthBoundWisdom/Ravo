#pragma once

#include "ravo/domain/types.h"
#include "ravo/engine/engine.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo
{

[[nodiscard]] Result<RenderedExportImage>
apply_export_output_sharpen(RenderedExportImage image, const ExportOutputSharpenOptions &options,
                            const CancellationToken &cancellation);

}
