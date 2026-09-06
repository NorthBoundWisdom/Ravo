#pragma once

#include "ravo/domain/raster_decoder.h"
#include "ravo/engine/engine.h"

namespace ravo
{
// Catalog-independent workspace/browse decode. The caller exclusively owns the
// engine and raster adapter for this synchronous call; no catalog or cache is
// accessed. Inputs are borrowed until return, pixels/errors are owned values.
[[nodiscard]] Result<RasterBuffer> decode_import_thumbnail(const EngineFacade &engine,
                                                           const RasterDecoder &raster,
                                                           std::string_view path,
                                                           const CancellationToken &cancellation);
} // namespace ravo
