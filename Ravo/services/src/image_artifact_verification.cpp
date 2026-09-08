#include "ravo/services/image_artifact_verification.h"

#include <map>
#include <string>

#include "ravo/adapters/text_file.h"
#include "ravo/foundation/color.h"

namespace ravo
{
namespace
{

[[nodiscard]] std::string sha256_bytes_hex(const std::vector<std::uint8_t> &bytes)
{
    const std::string_view view(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    return sha256_utf8_hex(view);
}

} // namespace

Result<VerifiedImageArtifact> verify_encoded_image_artifact(
    const RasterDecoder &decoder, const std::vector<std::uint8_t> &encoded,
    const ImageArtifactExpectation &expected, const CancellationToken &cancellation)
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    if (expected.mime_type.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "Image artifact MIME type is required",
                          {{"reason", "missing_mime_type"}});
    }
    if (encoded.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "Encoded image artifact is empty",
                          {{"reason", "empty_encoded_bytes"}});
    }
    if (encoded.size() > expected.max_encoded_bytes)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Encoded image artifact exceeds verification budget",
                          {{"reason", "encoded_budget_exceeded"},
                           {"bytes", std::to_string(encoded.size())},
                           {"max_bytes", std::to_string(expected.max_encoded_bytes)}});
    }

    const auto content_sha256 = sha256_bytes_hex(encoded);
    // Full decode: max_edge 0 keeps source dimensions for identity checks.
    auto decoded = decoder.decode_memory(encoded, 0U, cancellation);
    if (!decoded)
    {
        auto error = decoded.error();
        error.context.insert_or_assign("reason", "artifact_decode_failed");
        error.context.insert_or_assign("content_sha256", content_sha256);
        return error;
    }

    active = cancellation.check();
    if (!active)
    {
        return active.error();
    }

    const auto &raster = decoded.value();
    if (raster.width == 0U || raster.height == 0U)
    {
        return make_error(ErrorCode::kIo, "Decoded image artifact has empty dimensions",
                          {{"reason", "empty_dimensions"}, {"content_sha256", content_sha256}});
    }
    if (expected.width.has_value() && *expected.width != raster.width)
    {
        return make_error(ErrorCode::kConflict, "Decoded image width does not match expectation",
                          {{"reason", "width_mismatch"},
                           {"expected", std::to_string(*expected.width)},
                           {"actual", std::to_string(raster.width)},
                           {"content_sha256", content_sha256}});
    }
    if (expected.height.has_value() && *expected.height != raster.height)
    {
        return make_error(ErrorCode::kConflict, "Decoded image height does not match expectation",
                          {{"reason", "height_mismatch"},
                           {"expected", std::to_string(*expected.height)},
                           {"actual", std::to_string(raster.height)},
                           {"content_sha256", content_sha256}});
    }

    const auto &profile = raster.color_profile;
    if (profile.kind == ColorProfileKind::kMissing || profile.identifier.empty())
    {
        return make_error(
            ErrorCode::kIo, "Decoded image artifact is missing color identity",
            {{"reason", "missing_color_profile"}, {"content_sha256", content_sha256}});
    }
    if (expected.color_profile.has_value() &&
        *expected.color_profile != std::string_view(profile.identifier))
    {
        return make_error(ErrorCode::kConflict,
                          "Decoded color profile identity does not match expectation",
                          {{"reason", "color_profile_mismatch"},
                           {"expected", std::string(*expected.color_profile)},
                           {"actual", profile.identifier},
                           {"content_sha256", content_sha256}});
    }

    VerifiedImageArtifact report;
    report.mime_type = std::string(expected.mime_type);
    report.width = raster.width;
    report.height = raster.height;
    report.color_profile = profile.identifier;
    report.color_profile_fingerprint = color_profile_fingerprint(profile);
    report.byte_count = static_cast<std::uint64_t>(encoded.size());
    report.content_sha256 = content_sha256;
    return report;
}

} // namespace ravo
