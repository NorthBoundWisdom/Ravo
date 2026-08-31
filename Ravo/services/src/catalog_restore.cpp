#include "ravo/services/catalog_service.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "atomic_publication_internal.h"
#include "catalog_internal.h"

namespace ravo
{
namespace
{

constexpr int kRestoreTemporaryAttempts = 16;

[[nodiscard]] std::filesystem::path path_from_utf8(const std::string_view value)
{
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

[[nodiscard]] std::string path_utf8(const std::filesystem::path &path)
{
    const auto encoded = path.generic_u8string();
    return {reinterpret_cast<const char *>(encoded.data()), encoded.size()};
}

[[nodiscard]] std::string sidecar_filename(const std::string_view asset_id,
                                           const std::int64_t generation)
{
    return std::string(asset_id) + "." + std::to_string(generation) + ".ravo.json";
}

[[nodiscard]] TaskError restore_error(const ErrorCode code, std::string message, std::string reason,
                                      std::string path = {}, std::string detail = {})
{
    std::map<std::string, std::string, std::less<>> context{{"reason", std::move(reason)}};
    if (!path.empty())
        context.emplace("path", std::move(path));
    if (!detail.empty())
        context.emplace("detail", std::move(detail));
    return make_error(code, std::move(message), std::move(context));
}

[[nodiscard]] bool path_is_within(const std::filesystem::path &candidate,
                                  const std::filesystem::path &root) noexcept
{
    auto candidate_part = candidate.begin();
    auto root_part = root.begin();
    for (; root_part != root.end(); ++root_part, ++candidate_part)
    {
        if (candidate_part == candidate.end() || *candidate_part != *root_part)
            return false;
    }
    return true;
}

[[nodiscard]] Result<std::filesystem::path> absolute_normal_path(const std::filesystem::path &path,
                                                                 const std::string_view reason)
{
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error)
        return restore_error(ErrorCode::kIo, "Unable to resolve restore path", std::string(reason),
                             path_utf8(path), error.message());
    auto canonical = std::filesystem::weakly_canonical(absolute, error);
    if (error)
        return restore_error(ErrorCode::kIo, "Unable to canonicalize restore path",
                             std::string(reason), path_utf8(path), error.message());
    return canonical;
}

[[nodiscard]] Result<void> require_absent(const std::filesystem::path &path,
                                          const std::string_view reason)
{
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (!error && status.type() != std::filesystem::file_type::not_found)
        return restore_error(ErrorCode::kConflict, "Restore destination already exists",
                             std::string(reason), path_utf8(path));
    if (error && error != std::errc::no_such_file_or_directory)
        return restore_error(ErrorCode::kIo, "Unable to inspect restore destination",
                             "restore_destination_inspect_failed", path_utf8(path),
                             error.message());
    return {};
}

[[nodiscard]] Result<void> report_progress(const CatalogRestoreProgressCallback &callback,
                                           const CancellationToken &cancellation,
                                           const CatalogRestoreStage stage,
                                           const std::size_t completed, const std::size_t total,
                                           const std::uint64_t bytes_completed,
                                           const std::string_view path)
{
    if (callback)
        callback(CatalogRestoreProgress{stage, completed, total, bytes_completed});
    auto active = cancellation.check();
    if (active)
        return {};
    auto error = std::move(active).error();
    error.context.insert_or_assign("stage", std::string(catalog_restore_stage_name(stage)));
    if (!path.empty())
        error.context.insert_or_assign("path", std::string(path));
    return error;
}

[[nodiscard]] Result<void> verify_support_root(const std::filesystem::path &support_root,
                                               const std::vector<RecoveryArtifact> &expected,
                                               const RecoveryStore &recovery_verifier,
                                               const CancellationToken &cancellation)
{
    std::error_code error;
    const auto root_status = std::filesystem::symlink_status(support_root, error);
    if (error || !std::filesystem::is_directory(root_status))
        return restore_error(ErrorCode::kValidation, "Restore support root is invalid",
                             "restore_support_root_invalid", path_utf8(support_root),
                             error.message());
    std::set<std::string, std::less<>> root_entries;
    for (std::filesystem::directory_iterator iterator(support_root, error), end; iterator != end;
         iterator.increment(error))
    {
        if (error)
            return restore_error(ErrorCode::kIo, "Unable to enumerate restore support root",
                                 "restore_support_enumeration_failed", path_utf8(support_root),
                                 error.message());
        root_entries.insert(path_utf8(iterator->path().filename()));
    }
    if (error || root_entries != std::set<std::string, std::less<>>{"sidecars"})
        return restore_error(ErrorCode::kValidation, "Restore support layout is invalid",
                             "restore_support_layout_invalid", path_utf8(support_root),
                             error.message());

    const auto sidecar_root = support_root / "sidecars";
    const auto sidecar_status = std::filesystem::symlink_status(sidecar_root, error);
    if (error || !std::filesystem::is_directory(sidecar_status))
        return restore_error(ErrorCode::kValidation, "Restore sidecar root is invalid",
                             "restore_sidecar_root_invalid", path_utf8(sidecar_root),
                             error.message());
    std::set<std::string, std::less<>> expected_names;
    for (const auto &artifact : expected)
        expected_names.insert(sidecar_filename(artifact.asset_id, artifact.generation));
    std::set<std::string, std::less<>> actual_names;
    for (std::filesystem::directory_iterator iterator(sidecar_root, error), end; iterator != end;
         iterator.increment(error))
    {
        if (error)
            return restore_error(ErrorCode::kIo, "Unable to enumerate restored sidecars",
                                 "restore_sidecar_enumeration_failed", path_utf8(sidecar_root),
                                 error.message());
        std::error_code status_error;
        const auto status = iterator->symlink_status(status_error);
        if (status_error || !std::filesystem::is_regular_file(status))
            return restore_error(ErrorCode::kValidation, "Restored sidecar is not regular",
                                 "restore_sidecar_not_regular", path_utf8(iterator->path()),
                                 status_error.message());
        actual_names.insert(path_utf8(iterator->path().filename()));
    }
    if (error || actual_names != expected_names)
        return restore_error(ErrorCode::kValidation, "Restored sidecar set is invalid",
                             "restore_sidecar_layout_invalid", path_utf8(sidecar_root),
                             error.message());
    for (const auto &artifact : expected)
    {
        auto active = cancellation.check();
        if (!active)
            return active.error();
        const auto restored =
            sidecar_root / sidecar_filename(artifact.asset_id, artifact.generation);
        auto verified = recovery_verifier.verify_artifact(path_utf8(restored), artifact.asset_id,
                                                          artifact.generation, cancellation);
        if (!verified)
            return verified.error();
        if (verified.value().sha256 != artifact.sha256 || verified.value().bytes != artifact.bytes)
            return restore_error(ErrorCode::kValidation,
                                 "Restored sidecar differs from verified backup",
                                 "restore_sidecar_mismatch", path_utf8(restored));
    }
    return {};
}

[[nodiscard]] Result<void> remove_verified_support(const std::filesystem::path &support_root,
                                                   const std::vector<RecoveryArtifact> &expected,
                                                   const RecoveryStore &recovery_verifier,
                                                   TaskError &primary)
{
    auto still_owned =
        verify_support_root(support_root, expected, recovery_verifier, CancellationToken{});
    if (!still_owned)
    {
        primary.context.insert_or_assign("restore_support_retained", "true");
        primary.context.insert_or_assign("restore_support_error", still_owned.error().message);
        primary.context.insert_or_assign("restore_support_root", path_utf8(support_root));
        return {};
    }
    std::error_code cleanup_error;
    std::filesystem::remove_all(support_root, cleanup_error);
    if (cleanup_error)
    {
        primary.context.insert_or_assign("cleanup_failed", "true");
        primary.context.insert_or_assign("cleanup_error", cleanup_error.message());
        primary.context.insert_or_assign("restore_support_retained", "true");
        primary.context.insert_or_assign("restore_support_root", path_utf8(support_root));
    }
    return {};
}

} // namespace

Result<CatalogRestoreResult>
restore_catalog_backup(const CatalogBackupDatabaseVerifier &backup_database_verifier,
                       const CatalogRestoreDatabaseVerifier &restored_database_verifier,
                       const RecoveryStore &recovery_verifier, const CatalogRestoreRequest &request,
                       const CatalogRestoreProgressCallback &progress)
{
    if (request.backup_directory.empty() || request.destination_catalog.empty())
        return restore_error(ErrorCode::kInvalidArgument,
                             "Backup and restore destination must not be empty",
                             "invalid_restore_request");
    const auto backup_path = path_from_utf8(request.backup_directory);
    const auto destination_path = path_from_utf8(request.destination_catalog);
    if (destination_path.filename().empty())
        return restore_error(ErrorCode::kInvalidArgument,
                             "Restore destination must name a catalog file",
                             "invalid_restore_destination", request.destination_catalog);
    auto backup_absolute = absolute_normal_path(backup_path, "restore_backup_path_invalid");
    auto destination_absolute =
        absolute_normal_path(destination_path, "restore_destination_path_invalid");
    if (!backup_absolute)
        return backup_absolute.error();
    if (!destination_absolute)
        return destination_absolute.error();
    const auto support_path = path_from_utf8(request.destination_catalog + ".ravo");
    auto support_absolute = absolute_normal_path(support_path, "restore_support_path_invalid");
    if (!support_absolute)
        return support_absolute.error();
    if (path_is_within(destination_absolute.value(), backup_absolute.value()) ||
        path_is_within(support_absolute.value(), backup_absolute.value()))
        return restore_error(ErrorCode::kInvalidArgument,
                             "Restore destination must be outside the source backup",
                             "restore_destination_overlaps_backup", request.destination_catalog);
    auto absent = require_absent(destination_path, "restore_destination_conflict");
    if (!absent)
        return absent.error();
    absent = require_absent(support_path, "restore_support_conflict");
    if (!absent)
        return absent.error();
    const auto parent = destination_path.parent_path().empty() ? std::filesystem::path(".") :
                                                                 destination_path.parent_path();
    std::error_code parent_error;
    const auto parent_status = std::filesystem::status(parent, parent_error);
    if (parent_error || !std::filesystem::is_directory(parent_status))
        return restore_error(ErrorCode::kIo, "Restore parent is not a directory",
                             "restore_parent_invalid", path_utf8(parent), parent_error.message());

    auto progress_result =
        report_progress(progress, request.cancellation, CatalogRestoreStage::kVerifySource, 0U, 0U,
                        0U, request.backup_directory);
    if (!progress_result)
        return progress_result.error();
    auto source = verify_catalog_backup(backup_database_verifier, recovery_verifier,
                                        request.backup_directory, request.cancellation);
    if (!source)
        return source.error();
    std::vector<RecoveryArtifact> source_sidecars;
    source_sidecars.reserve(source.value().artifact.catalog.recovery_states.size());
    const auto backup_sidecar_root = backup_path / kCatalogBackupSidecarDirectory;
    for (const auto &state : source.value().artifact.catalog.recovery_states)
    {
        const auto source_path =
            backup_sidecar_root / sidecar_filename(state.asset_id, state.generation);
        auto artifact = recovery_verifier.verify_artifact(path_utf8(source_path), state.asset_id,
                                                          state.generation, request.cancellation);
        if (!artifact)
            return artifact.error();
        source_sidecars.push_back(std::move(artifact).value());
    }

    atomic_publication_internal::OwnedTemporaryDirectory stage;
    std::error_code create_error;
    for (int attempt = 0; attempt < kRestoreTemporaryAttempts && stage.path().empty(); ++attempt)
    {
        const auto candidate =
            atomic_publication_internal::temporary_candidate(destination_path, "catalog-restore");
        create_error.clear();
        if (std::filesystem::create_directory(candidate, create_error))
            stage.reset(candidate);
        else if (create_error && create_error != std::errc::file_exists)
            break;
    }
    if (stage.path().empty())
        return restore_error(ErrorCode::kIo, "Unable to create restore staging directory",
                             "restore_staging_create_failed", request.destination_catalog,
                             create_error.message());
    const auto fail = [&stage](TaskError error) -> Result<CatalogRestoreResult>
    {
        const auto cleanup_error = stage.remove();
        if (cleanup_error)
        {
            error.context.insert_or_assign("cleanup_failed", "true");
            error.context.insert_or_assign("cleanup_error", cleanup_error.message());
        }
        return error;
    };

    const auto stage_catalog = stage.path() / kCatalogBackupCatalogFilename;
    progress_result =
        report_progress(progress, request.cancellation, CatalogRestoreStage::kStageDatabase, 0U, 1U,
                        0U, path_utf8(stage_catalog));
    if (!progress_result)
        return fail(progress_result.error());
    auto copied_catalog = copy_file_atomically(source.value().artifact.catalog.path,
                                               path_utf8(stage_catalog), request.cancellation);
    if (!copied_catalog)
        return fail(copied_catalog.error());
    if (copied_catalog.value() != source.value().artifact.catalog.bytes)
        return fail(restore_error(ErrorCode::kValidation,
                                  "Staged catalog byte count does not match backup",
                                  "restore_catalog_size_mismatch", path_utf8(stage_catalog)));

    const auto stage_support = stage.path() / "support";
    const auto stage_sidecars = stage_support / "sidecars";
    if (!std::filesystem::create_directories(stage_sidecars, create_error))
        return fail(restore_error(ErrorCode::kIo, "Unable to create restore sidecar staging",
                                  "restore_sidecar_staging_create_failed",
                                  path_utf8(stage_sidecars), create_error.message()));
    std::uint64_t copied_sidecar_bytes = 0U;
    for (std::size_t index = 0; index < source_sidecars.size(); ++index)
    {
        progress_result = report_progress(
            progress, request.cancellation, CatalogRestoreStage::kStageSidecars, index,
            source_sidecars.size(), copied_sidecar_bytes, path_utf8(stage_sidecars));
        if (!progress_result)
            return fail(progress_result.error());
        const auto &source_artifact = source_sidecars[index];
        const auto destination =
            stage_sidecars / sidecar_filename(source_artifact.asset_id, source_artifact.generation);
        auto copied = copy_file_atomically(source_artifact.path, path_utf8(destination),
                                           request.cancellation);
        if (!copied)
            return fail(copied.error());
        if (copied.value() != source_artifact.bytes)
            return fail(restore_error(ErrorCode::kValidation,
                                      "Staged sidecar byte count does not match backup",
                                      "restore_sidecar_size_mismatch", path_utf8(destination)));
        copied_sidecar_bytes += copied.value();
    }

    progress_result = report_progress(
        progress, request.cancellation, CatalogRestoreStage::kVerifyStaging, source_sidecars.size(),
        source_sidecars.size(), copied_sidecar_bytes, path_utf8(stage.path()));
    if (!progress_result)
        return fail(progress_result.error());
    auto staged_catalog = backup_database_verifier.verify_backup_database(
        path_utf8(stage_catalog), source.value().artifact.catalog.sha256, request.cancellation);
    if (!staged_catalog)
        return fail(staged_catalog.error());
    if (staged_catalog.value().catalog_id != source.value().artifact.catalog.catalog_id ||
        staged_catalog.value().revision != source.value().artifact.catalog.revision ||
        staged_catalog.value().recovery_states != source.value().artifact.catalog.recovery_states)
        return fail(restore_error(ErrorCode::kValidation,
                                  "Staged catalog identity differs from backup",
                                  "restore_catalog_identity_mismatch", path_utf8(stage_catalog)));
    auto staged_support = verify_support_root(stage_support, source_sidecars, recovery_verifier,
                                              request.cancellation);
    if (!staged_support)
        return fail(staged_support.error());

    progress_result =
        report_progress(progress, request.cancellation, CatalogRestoreStage::kPublishSupport,
                        source_sidecars.size(), source_sidecars.size(), copied_sidecar_bytes,
                        path_utf8(support_path));
    if (!progress_result)
        return fail(progress_result.error());
    const auto support_publish_error =
        atomic_publication_internal::publish_no_replace(stage_support, support_path);
    if (support_publish_error)
    {
        const auto code =
            support_publish_error == std::errc::file_exists ? ErrorCode::kConflict : ErrorCode::kIo;
        return fail(restore_error(code, "Unable to publish restore support root",
                                  code == ErrorCode::kConflict ? "restore_support_conflict" :
                                                                 "restore_support_publish_failed",
                                  path_utf8(support_path), support_publish_error.message()));
    }

    progress_result =
        report_progress(progress, request.cancellation, CatalogRestoreStage::kPublishCatalog, 0U,
                        1U, copied_sidecar_bytes, request.destination_catalog);
    if (!progress_result)
    {
        auto error = progress_result.error();
        static_cast<void>(
            remove_verified_support(support_path, source_sidecars, recovery_verifier, error));
        return fail(std::move(error));
    }
    const auto catalog_publish_error =
        atomic_publication_internal::publish_no_replace(stage_catalog, destination_path);
    if (catalog_publish_error)
    {
        const auto code =
            catalog_publish_error == std::errc::file_exists ? ErrorCode::kConflict : ErrorCode::kIo;
        auto error = restore_error(code, "Unable to publish restored catalog",
                                   code == ErrorCode::kConflict ? "restore_destination_conflict" :
                                                                  "restore_catalog_publish_failed",
                                   request.destination_catalog, catalog_publish_error.message());
        static_cast<void>(
            remove_verified_support(support_path, source_sidecars, recovery_verifier, error));
        return fail(std::move(error));
    }

    // The catalog file is the commit point. From here on, never remove either
    // published path or report cancellation as if no restore occurred.
    const auto stage_cleanup_error = stage.remove();
    progress_result =
        report_progress(progress, CancellationToken{}, CatalogRestoreStage::kOpenCatalog, 1U, 1U,
                        copied_sidecar_bytes, request.destination_catalog);
    static_cast<void>(progress_result);
    auto opened = restored_database_verifier.verify_restored_catalog(
        request.destination_catalog, source.value().artifact.catalog.catalog_id,
        CancellationToken{});
    if (!opened)
    {
        auto error = opened.error();
        error.context.insert_or_assign("restore_published", "true");
        error.context.insert_or_assign("catalog", request.destination_catalog);
        error.context.insert_or_assign("support_root", path_utf8(support_path));
        if (stage_cleanup_error)
        {
            error.context.insert_or_assign("cleanup_failed", "true");
            error.context.insert_or_assign("cleanup_error", stage_cleanup_error.message());
        }
        return error;
    }
    auto final_support =
        verify_support_root(support_path, source_sidecars, recovery_verifier, CancellationToken{});
    if (!final_support)
    {
        auto error = final_support.error();
        error.context.insert_or_assign("restore_published", "true");
        error.context.insert_or_assign("catalog", request.destination_catalog);
        return error;
    }

    CatalogRestoreResult result;
    result.source_backup = source.value().artifact;
    result.catalog = std::move(opened).value();
    result.support_root = path_utf8(support_path);
    result.previews_rebuild_required = true;
    result.published = true;
    if (stage_cleanup_error)
    {
        auto error = restore_error(ErrorCode::kIo, "Restore succeeded but staging cleanup failed",
                                   "restore_staging_cleanup_failed", path_utf8(stage.path()),
                                   stage_cleanup_error.message());
        error.context.insert_or_assign("restore_published", "true");
        error.context.insert_or_assign("catalog", request.destination_catalog);
        error.context.insert_or_assign("support_root", path_utf8(support_path));
        return error;
    }
    if (progress)
        progress(
            CatalogRestoreProgress{CatalogRestoreStage::kComplete, 1U, 1U, copied_sidecar_bytes});
    return result;
}

} // namespace ravo
