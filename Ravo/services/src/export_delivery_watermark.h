#pragma once

#include "ravo/domain/types.h"
#include "ravo/engine/engine.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"
#include "ravo/recipe/recipe.h"
#include "ravo/recipe/watermark.h"

namespace ravo
{

[[nodiscard]] Result<WatermarkParams>
watermark_params_from_export_options(const ExportWatermarkOptions &options);

[[nodiscard]] Result<RenderedExportImage>
apply_export_delivery_watermark(RenderedExportImage image, const ExportWatermarkOptions &options,
                                const AssetDescriptor &asset,
                                const CancellationToken &cancellation);

} // namespace ravo
