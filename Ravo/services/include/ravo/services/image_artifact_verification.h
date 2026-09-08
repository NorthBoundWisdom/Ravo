#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/domain/raster_decoder.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo
{

// Owning report for an encoded image artifact verified through RasterDecoder.
// Description text is never treated as color identity.
struct VerifiedImageArtifact
{
    std::string type = "ravo.image_artifact";
    int version = 1;
    std::string mime_type;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string color_profile;
    std::string color_profile_fingerprint;
    std::uint64_t byte_count = 0;
    std::string content_sha256;
};

struct ImageArtifactExpectation
{
    std::string_view mime_type = "image/png";
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
    // Exact decoded identifier when set (not a free-form description match).
    std::optional<std::string_view> color_profile;
    // Exact color_profile_fingerprint(decoded) when set — binds renderer/encoder
    // identity without treating builtin vs embedded descriptors as interchangeable.
    std::optional<std::string_view> color_profile_fingerprint;
    std::size_t max_encoded_bytes = 64U * 1024U * 1024U;
};

// Verifies immutable encoded bytes with the shared raster owner. Does not
// publish files. Failures and cancellation leave no side effects.
[[nodiscard]] Result<VerifiedImageArtifact> verify_encoded_image_artifact(
    const RasterDecoder &decoder, const std::vector<std::uint8_t> &encoded,
    const ImageArtifactExpectation &expected, const CancellationToken &cancellation = {});

} // namespace ravo
