#include "ravo/services/catalog_service.h"

#include "catalog_internal.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "ravo/adapters/text_file.h"
#include "ravo/domain/uri.h"
#include "ravo/services/dng_smart_preview.h"

namespace ravo
{
namespace
{

struct FileFingerprint
{
    std::string sha256;
    std::uint64_t size_bytes = 0;
    std::int64_t mtime_unix_ms = 0;
};

[[nodiscard]] Result<std::string> original_path_for_asset(const AssetRecord &asset)
{
    auto location = normalize_local_input(asset.normalized_uri);
    if (!location)
        return location.error();
    return location.value().path;
}

[[nodiscard]] Result<FileFingerprint> fingerprint_file(const std::string_view path)
{
    auto digest = sha256_file_hex(path);
    if (!digest)
        return digest.error();
    auto identity = read_file_identity(path);
    if (!identity)
        return identity.error();
    FileFingerprint fingerprint;
    fingerprint.sha256 = std::move(digest).value();
    fingerprint.size_bytes = identity.value().size_bytes;
    fingerprint.mtime_unix_ms = identity.value().mtime_unix_ms;
    return fingerprint;
}

[[nodiscard]] bool same_fingerprint(const FileFingerprint &left,
                                    const FileFingerprint &right) noexcept
{
    return left.sha256 == right.sha256 && left.size_bytes == right.size_bytes &&
           left.mtime_unix_ms == right.mtime_unix_ms;
}

[[nodiscard]] std::string smart_preview_root(const std::string_view database_path,
                                             const std::string_view asset_id)
{
    return std::string(database_path) + ".ravo/smart-previews/" + std::string(asset_id);
}

} // namespace

bool dng_converter_is_packaged() noexcept
{
    return false;
}

bool smart_preview_encoder_is_packaged() noexcept
{
    return false;
}

Result<DngConversionResult>
CatalogService::convert_asset_to_dng(const DngConversionRequest &request)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kInvalidArgument, "Catalog is not open",
                          {{"reason", "catalog_not_open"}});
    }
    if (request.asset_id.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "DNG conversion requires an asset id",
                          {{"reason", "missing_asset_id"}});
    }
    if (request.cancellation.is_cancellation_requested())
    {
        return make_error(ErrorCode::kCancelled, "DNG conversion was cancelled",
                          {{"reason", "cancelled"}});
    }

    auto source = repository_->find_asset_by_id(request.asset_id);
    if (!source)
        return source.error();
    if (!source.value())
    {
        return make_error(ErrorCode::kNotFound, "Source asset was not found",
                          {{"asset_id", request.asset_id}, {"reason", "asset_not_found"}});
    }

    auto original_path = original_path_for_asset(*source.value());
    if (!original_path)
        return original_path.error();
    auto before = fingerprint_file(original_path.value());
    if (!before)
        return before.error();

    if (!dng_converter_is_packaged())
    {
        auto after = fingerprint_file(original_path.value());
        if (!after)
            return after.error();
        if (!same_fingerprint(before.value(), after.value()))
        {
            return make_error(ErrorCode::kConflict,
                              "Source original changed during DNG conversion preflight",
                              {{"asset_id", request.asset_id},
                               {"path", original_path.value()},
                               {"reason", "source_mutated_during_dng_convert"}});
        }
        (void)request.output_path;
        DngConversionResult result;
        result.asset_id = request.asset_id;
        result.source_path = original_path.value();
        result.originals_unchanged = true;
        result.converter_available = false;
        result.reason = "dng_converter_unavailable";
        return result;
    }

    (void)request.output_path;
    return make_error(
        ErrorCode::kUnsupported, "Packaged DNG converter path is not implemented in this build",
        {{"asset_id", request.asset_id}, {"reason", "dng_converter_path_unimplemented"}});
}

Result<SmartPreviewStatus>
CatalogService::smart_preview_status(const std::string_view asset_id) const
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kInvalidArgument, "Catalog is not open",
                          {{"reason", "catalog_not_open"}});
    }
    if (asset_id.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "Smart Preview status requires an asset id",
                          {{"reason", "missing_asset_id"}});
    }

    auto source = repository_->find_asset_by_id(asset_id);
    if (!source)
        return source.error();
    if (!source.value())
    {
        return make_error(ErrorCode::kNotFound, "Source asset was not found",
                          {{"asset_id", std::string(asset_id)}, {"reason", "asset_not_found"}});
    }

    auto snapshot = repository_->snapshot();
    if (!snapshot)
        return snapshot.error();

    SmartPreviewStatus status;
    status.asset_id = std::string(asset_id);
    status.encoder_available = smart_preview_encoder_is_packaged();
    status.develop_fallback = false;
    status.reason = status.encoder_available ? "smart_preview_encoder_packaged" :
                                               "smart_preview_encoder_unavailable";

    const auto root = smart_preview_root(snapshot.value().database_path, asset_id);
    std::error_code error;
    if (std::filesystem::is_directory(utf8_path(root), error) && !error)
    {
        status.present = true;
        status.path = root;
    }
    return status;
}

Result<SmartPreviewStatus>
CatalogService::ensure_smart_preview(const SmartPreviewEnsureRequest &request)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kInvalidArgument, "Catalog is not open",
                          {{"reason", "catalog_not_open"}});
    }
    if (request.asset_id.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "Smart Preview requires an asset id",
                          {{"reason", "missing_asset_id"}});
    }
    if (request.cancellation.is_cancellation_requested())
    {
        return make_error(ErrorCode::kCancelled, "Smart Preview generation was cancelled",
                          {{"reason", "cancelled"}});
    }

    auto source = repository_->find_asset_by_id(request.asset_id);
    if (!source)
        return source.error();
    if (!source.value())
    {
        return make_error(ErrorCode::kNotFound, "Source asset was not found",
                          {{"asset_id", request.asset_id}, {"reason", "asset_not_found"}});
    }

    auto original_path = original_path_for_asset(*source.value());
    if (!original_path)
        return original_path.error();
    auto before = fingerprint_file(original_path.value());
    if (!before)
        return before.error();

    if (!smart_preview_encoder_is_packaged())
    {
        auto after = fingerprint_file(original_path.value());
        if (!after)
            return after.error();
        if (!same_fingerprint(before.value(), after.value()))
        {
            return make_error(ErrorCode::kConflict,
                              "Source original changed during Smart Preview preflight",
                              {{"asset_id", request.asset_id},
                               {"path", original_path.value()},
                               {"reason", "source_mutated_during_smart_preview"}});
        }
        SmartPreviewStatus status;
        status.asset_id = request.asset_id;
        status.encoder_available = false;
        status.present = false;
        status.develop_fallback = false;
        status.reason = "smart_preview_encoder_unavailable";
        return status;
    }

    return make_error(ErrorCode::kUnsupported,
                      "Packaged Smart Preview encoder path is not implemented",
                      {{"asset_id", request.asset_id},
                       {"reason", "smart_preview_encoder_path_unimplemented"},
                       {"develop_fallback", "false"}});
}

} // namespace ravo
