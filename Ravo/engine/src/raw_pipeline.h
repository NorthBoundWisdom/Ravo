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
[[nodiscard]] Result<RawInspectPreview>
inspect_raw_with_embedded_preview(std::string_view input_uri, std::uint32_t max_edge,
                                  const CancellationToken &cancellation);
[[nodiscard]] Result<DecodedRaw> decode_raw(std::string_view input_uri);
[[nodiscard]] std::uint64_t estimate_raw_render_memory(const DecodedRaw &raw, const Recipe &recipe,
                                                       std::uint32_t width,
                                                       std::uint32_t height) noexcept;
[[nodiscard]] Result<void> write_png_atomically(std::string_view output_uri,
                                                const RenderedImage &image);

} // namespace ravo
