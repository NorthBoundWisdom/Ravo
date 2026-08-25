#pragma once

#include <cstdint>
#include <string_view>

#include "ravo/engine/engine.h"

namespace ravo
{

[[nodiscard]] Result<InspectionResult> identify_raw(std::string_view input_uri);
[[nodiscard]] Result<EmbeddedPreview> extract_libraw_preview(std::string_view input_uri,
                                                             std::uint32_t max_edge,
                                                             const CancellationToken &cancellation);
[[nodiscard]] Result<DecodedRaw> decode_raw(std::string_view input_uri);
[[nodiscard]] Result<RenderedImage> render_raw(const DecodedRaw &raw, const RenderRequest &request);
[[nodiscard]] Result<void> write_png_atomically(std::string_view output_uri,
                                                const RenderedImage &image);

} // namespace ravo
