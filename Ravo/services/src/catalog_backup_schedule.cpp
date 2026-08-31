#include "ravo/services/catalog_service.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "atomic_publication_internal.h"

namespace ravo
{
namespace
{

constexpr std::size_t kMaximumScheduledBackupEntries = 10'000U;

[[nodiscard]] std::filesystem::path path_from_utf8(const std::string_view value)
{
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

[[nodiscard]] std::string path_utf8(const std::filesystem::path &path)
{
    const auto encoded = path.generic_u8string();
    return {reinterpret_cast<const char *>(encoded.data()), encoded.size()};
}

[[nodiscard]] TaskError schedule_error(const ErrorCode code, std::string message,
                                       std::string reason, std::string path = {},
                                       std::string detail = {})
{
    std::map<std::string, std::string, std::less<>> context{{"reason", std::move(reason)}};
    if (!path.empty())
        context.emplace("path", std::move(path));
    if (!detail.empty())
        context.emplace("detail", std::move(detail));
    return make_error(code, std::move(message), std::move(context));
}

[[nodiscard]] std::int64_t next_run(const std::int64_t now_unix_ms,
                                    const std::int64_t interval_minutes) noexcept
{
    constexpr std::int64_t kMinuteMs = 60'000;
    if (interval_minutes > (std::numeric_limits<std::int64_t>::max() - now_unix_ms) / kMinuteMs)
        return std::numeric_limits<std::int64_t>::max();
    return now_unix_ms + interval_minutes * kMinuteMs;
}

[[nodiscard]] bool safe_catalog_id(const std::string_view value) noexcept
{
    return value.starts_with("cat_") && value.size() <= 180U &&
           std::all_of(value.begin(), value.end(),
                       [](const char character)
                       {
                           return (character >= 'a' && character <= 'z') ||
                                  (character >= 'A' && character <= 'Z') ||
                                  (character >= '0' && character <= '9') || character == '-' ||
                                  character == '_';
                       });
}

struct VerifiedScheduledBackup
{
    CatalogBackupArtifact artifact;
    std::filesystem::path path;
};

[[nodiscard]] bool is_strict_scheduled_backup_name(const std::string_view name,
                                                   const std::string_view prefix) noexcept
{
    constexpr std::string_view suffix = ".ravobackup";
    if (!name.starts_with(prefix) || !name.ends_with(suffix) ||
        name.size() <= prefix.size() + suffix.size())
        return false;
    const auto timestamp = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    std::int64_t parsed = 0;
    const auto result =
        std::from_chars(timestamp.data(), timestamp.data() + timestamp.size(), parsed);
    return result.ec == std::errc{} && result.ptr == timestamp.data() + timestamp.size() &&
           parsed >= 0 && std::to_string(parsed) == timestamp;
}

} // namespace

Result<CatalogBackupPolicy> CatalogService::backup_policy() const
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return repository_->backup_policy();
}

Result<CatalogBackupPolicy> CatalogService::set_backup_policy(CatalogBackupPolicy policy,
                                                              const std::int64_t now_unix_ms)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    if (now_unix_ms < 0)
        return make_error(ErrorCode::kInvalidArgument, "Backup policy time must be non-negative");
    auto current = repository_->backup_policy();
    if (!current)
        return current.error();
    current.value().enabled = policy.enabled;
    if (policy.enabled || !policy.destination_directory.empty())
    {
        current.value().destination_directory = std::move(policy.destination_directory);
        current.value().interval_minutes = policy.interval_minutes;
        current.value().retention_count = policy.retention_count;
    }
    current.value().next_run_unix_ms =
        current.value().enabled ?
            std::optional<std::int64_t>{next_run(now_unix_ms, current.value().interval_minutes)} :
            std::nullopt;
    auto valid = validate_catalog_backup_policy(current.value());
    if (!valid)
        return valid.error();
    if (current.value().enabled)
    {
        std::error_code error;
        const auto destination = path_from_utf8(current.value().destination_directory);
        const auto status = std::filesystem::symlink_status(destination, error);
        if (error || !std::filesystem::is_directory(status))
            return schedule_error(
                ErrorCode::kValidation, "Scheduled backup destination is not an existing directory",
                "backup_schedule_destination_invalid", path_utf8(destination), error.message());
    }
    auto saved = repository_->save_backup_policy(current.value());
    if (!saved)
        return saved.error();
    return current.value();
}

Result<CatalogBackupScheduleResult>
CatalogService::run_scheduled_backup(const std::int64_t now_unix_ms,
                                     const CancellationToken &cancellation, const bool force)
{
    if (repository_ == nullptr || recovery_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    if (now_unix_ms < 0)
        return make_error(ErrorCode::kInvalidArgument, "Backup schedule time must be non-negative");
    auto active = cancellation.check();
    if (!active)
        return active.error();
    auto policy = repository_->backup_policy();
    if (!policy)
        return policy.error();
    CatalogBackupScheduleResult result;
    result.policy = policy.value();
    if (!policy.value().enabled || (!force && policy.value().next_run_unix_ms &&
                                    now_unix_ms < *policy.value().next_run_unix_ms))
        return result;

    const auto destination_root = path_from_utf8(policy.value().destination_directory);
    std::error_code destination_error;
    const auto destination_status =
        std::filesystem::symlink_status(destination_root, destination_error);
    if (destination_error || !std::filesystem::is_directory(destination_status))
    {
        auto error = schedule_error(ErrorCode::kValidation,
                                    "Scheduled backup destination is not an existing directory",
                                    "backup_schedule_destination_invalid",
                                    path_utf8(destination_root), destination_error.message());
        policy.value().last_error = error.message;
        policy.value().next_run_unix_ms = next_run(now_unix_ms, policy.value().interval_minutes);
        static_cast<void>(repository_->save_backup_policy(policy.value()));
        return error;
    }
    auto snapshot = repository_->snapshot();
    if (!snapshot)
        return snapshot.error();
    if (!safe_catalog_id(snapshot.value().catalog_id))
        return schedule_error(ErrorCode::kValidation, "Catalog identity is not safe for backup",
                              "invalid_scheduled_backup_catalog_id");
    const auto filename = std::string("ravo-") + snapshot.value().catalog_id + "-" +
                          std::to_string(now_unix_ms) + ".ravobackup";
    const auto destination = destination_root / path_from_utf8(filename);
    auto backup = create_backup(path_utf8(destination), cancellation);
    if (!backup)
    {
        policy.value().last_error = backup.error().message;
        policy.value().next_run_unix_ms = next_run(now_unix_ms, policy.value().interval_minutes);
        static_cast<void>(repository_->save_backup_policy(policy.value()));
        return backup.error();
    }
    result.ran = true;
    result.backup = backup.value();
    const auto published_failure = [&](TaskError error) -> Result<CatalogBackupScheduleResult>
    {
        error.context.insert_or_assign("backup_published", "true");
        error.context.insert_or_assign("backup", backup.value().path);
        if (error.code != ErrorCode::kCancelled)
        {
            policy.value().last_error = error.message;
            policy.value().next_run_unix_ms =
                next_run(now_unix_ms, policy.value().interval_minutes);
            auto saved = repository_->save_backup_policy(policy.value());
            if (!saved)
            {
                error.context.insert_or_assign("policy_update_failed", "true");
                error.context.insert_or_assign("policy_update_error", saved.error().message);
            }
        }
        return error;
    };

    std::vector<VerifiedScheduledBackup> verified;
    std::error_code enumeration_error;
    std::size_t entries = 0U;
    const auto prefix = std::string("ravo-") + snapshot.value().catalog_id + "-";
    for (std::filesystem::directory_iterator iterator(destination_root, enumeration_error), end;
         iterator != end; iterator.increment(enumeration_error))
    {
        if (enumeration_error)
            break;
        active = cancellation.check();
        if (!active)
            return published_failure(active.error());
        if (++entries > kMaximumScheduledBackupEntries)
            return published_failure(schedule_error(
                ErrorCode::kValidation, "Scheduled backup directory contains too many entries",
                "backup_schedule_entry_count_exceeded", path_utf8(destination_root)));
        const auto name = path_utf8(iterator->path().filename());
        if (!is_strict_scheduled_backup_name(name, prefix))
            continue;
        const auto status = iterator->symlink_status(enumeration_error);
        if (enumeration_error || !std::filesystem::is_directory(status))
        {
            result.retained_unverified_paths.push_back(path_utf8(iterator->path()));
            enumeration_error.clear();
            continue;
        }
        auto candidate = verify_backup(path_utf8(iterator->path()), cancellation);
        if (!candidate ||
            candidate.value().artifact.catalog.catalog_id != snapshot.value().catalog_id)
        {
            result.retained_unverified_paths.push_back(path_utf8(iterator->path()));
            continue;
        }
        verified.push_back({candidate.value().artifact, iterator->path()});
    }
    if (enumeration_error)
        return published_failure(
            schedule_error(ErrorCode::kIo, "Unable to enumerate scheduled backups",
                           "backup_schedule_enumeration_failed", path_utf8(destination_root),
                           enumeration_error.message()));
    std::sort(verified.begin(), verified.end(),
              [](const VerifiedScheduledBackup &left, const VerifiedScheduledBackup &right)
              {
                  if (left.artifact.created_unix_ms != right.artifact.created_unix_ms)
                      return left.artifact.created_unix_ms > right.artifact.created_unix_ms;
                  return left.path > right.path;
              });
    for (std::size_t index = static_cast<std::size_t>(policy.value().retention_count);
         index < verified.size(); ++index)
    {
        active = cancellation.check();
        if (!active)
            return published_failure(active.error());
        const auto quarantine = atomic_publication_internal::temporary_candidate(
            verified[index].path, "backup-retention");
        const auto moved =
            atomic_publication_internal::publish_no_replace(verified[index].path, quarantine);
        if (moved)
            return published_failure(
                schedule_error(ErrorCode::kIo, "Unable to quarantine expired backup",
                               "backup_retention_quarantine_failed",
                               path_utf8(verified[index].path), moved.message()));
        auto quarantined = verify_backup(path_utf8(quarantine), CancellationToken{});
        if (!quarantined ||
            quarantined.value().artifact.catalog.catalog_id != snapshot.value().catalog_id ||
            quarantined.value().artifact.catalog.sha256 != verified[index].artifact.catalog.sha256)
        {
            const auto restored =
                atomic_publication_internal::publish_no_replace(quarantine, verified[index].path);
            auto error = schedule_error(
                ErrorCode::kConflict, "Expired backup changed during retention",
                "backup_retention_identity_changed", path_utf8(verified[index].path));
            if (restored)
                error.context.insert_or_assign("quarantine_retained", path_utf8(quarantine));
            return published_failure(std::move(error));
        }
        std::error_code remove_error;
        std::filesystem::remove_all(quarantine, remove_error);
        if (remove_error)
            return published_failure(schedule_error(
                ErrorCode::kIo, "Unable to remove expired verified backup",
                "backup_retention_remove_failed", path_utf8(quarantine), remove_error.message()));
        result.removed_backups.push_back(path_utf8(verified[index].path));
    }

    policy.value().last_success_unix_ms = now_unix_ms;
    policy.value().next_run_unix_ms = next_run(now_unix_ms, policy.value().interval_minutes);
    policy.value().last_backup_bytes = backup.value().catalog.bytes + backup.value().sidecar_bytes;
    policy.value().last_error.reset();
    auto saved = repository_->save_backup_policy(policy.value());
    if (!saved)
    {
        auto error = saved.error();
        error.context.insert_or_assign("backup_published", "true");
        error.context.insert_or_assign("backup", backup.value().path);
        return error;
    }
    result.policy = policy.value();
    return result;
}

} // namespace ravo
