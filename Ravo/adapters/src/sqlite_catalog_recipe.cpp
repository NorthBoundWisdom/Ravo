#include "ravo/adapters/sqlite_catalog.h"

#include "catalog_sql_internal.h"

#include "catalog_repository_test_control.h"

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <limits>
#include <map>
#include <string_view>
#include <string>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include <QtCore/QFileInfo>
#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QMetaType>
#include <QtCore/QSaveFile>
#include <QtCore/QVariant>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlError>
#include <QtSql/QSqlQuery>

#include "ravo/domain/types.h"
#include "ravo/domain/uri.h"

namespace ravo
{
using namespace sqlite_internal;

Result<std::optional<std::string>>
SqliteCatalogRepository::load_recipe_json(const std::string_view asset_id) const
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    QSqlQuery query(impl_->database);
    query.prepare(QStringLiteral("SELECT recipe_json FROM asset_recipe WHERE asset_id = ?"));
    query.addBindValue(qstring_from_utf8(asset_id));
    if (!query.exec())
    {
        return map_sql_error(query, "load_recipe_json");
    }
    if (!query.next())
    {
        return std::optional<std::string>{};
    }
    return std::optional<std::string>{utf8_from_qstring(query.value(0).toString())};
}

Result<void> SqliteCatalogRepository::save_recipe_json(const std::string_view asset_id,
                                                       const std::int64_t recipe_schema_version,
                                                       const std::string_view recipe_json)
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    QSqlQuery query(impl_->database);
    query.prepare(QStringLiteral(
        "INSERT INTO asset_recipe(asset_id, recipe_schema_version, recipe_json, updated_unix_ms) "
        "VALUES (?, ?, ?, ?) "
        "ON CONFLICT(asset_id) DO UPDATE SET recipe_schema_version = excluded.recipe_schema_version, "
        "recipe_json = excluded.recipe_json, updated_unix_ms = excluded.updated_unix_ms"));
    query.addBindValue(qstring_from_utf8(asset_id));
    query.addBindValue(static_cast<qlonglong>(recipe_schema_version));
    query.addBindValue(qstring_from_utf8(recipe_json));
    query.addBindValue(static_cast<qlonglong>(now));
    if (!query.exec())
    {
        return map_sql_error(query, "save_recipe_json");
    }
    return {};
}

Result<void> SqliteCatalogRepository::clear_recipe(const std::string_view asset_id)
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    QSqlQuery query(impl_->database);
    query.prepare(QStringLiteral("DELETE FROM asset_recipe WHERE asset_id = ?"));
    query.addBindValue(qstring_from_utf8(asset_id));
    if (!query.exec())
    {
        return map_sql_error(query, "clear_recipe");
    }
    return {};
}

Result<RecipeCommitResult> SqliteCatalogRepository::commit_recipe(
    const std::string_view asset_id, const std::int64_t recipe_schema_version,
    const std::optional<std::string_view> recipe_json, const std::string_view history_json,
    const RecipeHistoryWrite history_write,
    const std::optional<std::int64_t> discard_history_after_seq,
    const std::optional<std::int64_t> coalesce_history_id)
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    if (discard_history_after_seq && *discard_history_after_seq < 0)
    {
        return make_error(ErrorCode::kValidation, "Recipe history cursor is invalid",
                          {{"seq", std::to_string(*discard_history_after_seq)}});
    }
    if (coalesce_history_id &&
        (*coalesce_history_id <= 0 || history_write != RecipeHistoryWrite::kAppendIfNew))
    {
        return make_error(ErrorCode::kValidation, "Recipe history coalesce request is invalid",
                          {{"history_id", std::to_string(*coalesce_history_id)}});
    }
    if (!impl_->database.transaction())
    {
        return make_error(ErrorCode::kIo, "Unable to start recipe transaction",
                          {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}});
    }

    if (discard_history_after_seq)
    {
        QSqlQuery discard(impl_->database);
        discard.prepare(
            QStringLiteral("DELETE FROM asset_recipe_history WHERE asset_id = ? AND seq > ?"));
        discard.addBindValue(qstring_from_utf8(asset_id));
        discard.addBindValue(static_cast<qlonglong>(*discard_history_after_seq));
        if (!discard.exec())
        {
            return impl_->abort_transaction(map_sql_error(discard, "discard_recipe_history"));
        }
    }

    const auto written = recipe_json ?
                             save_recipe_json(asset_id, recipe_schema_version, *recipe_json) :
                             clear_recipe(asset_id);
    if (!written)
    {
        return impl_->abort_transaction(written.error());
    }

    std::optional<std::int64_t> committed_history_id;
    if (history_write == RecipeHistoryWrite::kAppendIfNew)
    {
        QSqlQuery latest(impl_->database);
        latest.prepare(QStringLiteral(
            "SELECT id, kind, recipe_json FROM asset_recipe_history WHERE asset_id = ? "
            "ORDER BY seq DESC, id DESC LIMIT 1"));
        latest.addBindValue(qstring_from_utf8(asset_id));
        if (!latest.exec())
        {
            return impl_->abort_transaction(map_sql_error(latest, "latest_recipe_history"));
        }
        const bool has_latest = latest.next();
        const auto latest_id = has_latest ? latest.value(0).toLongLong() : 0;
        const bool latest_is_history =
            has_latest &&
            utf8_from_qstring(latest.value(1).toString()) == kRecipeHistoryKindHistory;
        const bool duplicate =
            latest_is_history && utf8_from_qstring(latest.value(2).toString()) == history_json;
        if (!duplicate)
        {
            if (coalesce_history_id && latest_is_history &&
                latest_id == static_cast<qlonglong>(*coalesce_history_id))
            {
                const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count();
                QSqlQuery replace(impl_->database);
                replace.prepare(QStringLiteral(
                    "UPDATE asset_recipe_history SET recipe_json = ?, created_unix_ms = ? "
                    "WHERE id = ? AND asset_id = ? AND kind = ?"));
                replace.addBindValue(qstring_from_utf8(history_json));
                replace.addBindValue(static_cast<qlonglong>(now));
                replace.addBindValue(latest_id);
                replace.addBindValue(qstring_from_utf8(asset_id));
                replace.addBindValue(qstring_from_utf8(kRecipeHistoryKindHistory));
                if (!replace.exec())
                {
                    return impl_->abort_transaction(
                        map_sql_error(replace, "coalesce_recipe_history"));
                }
                if (replace.numRowsAffected() != 1)
                {
                    return impl_->abort_transaction(make_error(
                        ErrorCode::kConflict, "Recipe history changed before it could coalesce",
                        {{"history_id", std::to_string(*coalesce_history_id)}}));
                }
                committed_history_id = *coalesce_history_id;
            }
            else
            {
                auto recorded = append_recipe_history(asset_id, kRecipeHistoryKindHistory,
                                                      std::nullopt, history_json);
                if (!recorded)
                {
                    return impl_->abort_transaction(recorded.error());
                }
                committed_history_id = recorded.value().id;
            }
        }
        else
        {
            committed_history_id = static_cast<std::int64_t>(latest_id);
        }
    }

    QSqlQuery revision_query(impl_->database);
    if (!revision_query.exec(
            QStringLiteral("UPDATE schema_info SET revision = revision + 1 WHERE id = 1")))
    {
        return impl_->abort_transaction(map_sql_error(revision_query, "bump_recipe_revision"));
    }
    if (!revision_query.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) ||
        !revision_query.next())
    {
        return impl_->abort_transaction(map_sql_error(revision_query, "read_recipe_revision"));
    }
    const auto revision = revision_query.value(0).toLongLong();
    if (!impl_->database.commit())
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Unable to commit recipe transaction",
                       {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}}));
    }
    impl_->snapshot.revision = revision;
    return RecipeCommitResult{revision, committed_history_id};
}

Result<void> SqliteCatalogRepository::upsert_writable_metadata(const std::string_view asset_id,
                                                               const WritableMetadata &metadata)
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    QSqlQuery query(impl_->database);
    query.prepare(QStringLiteral(
        "INSERT INTO asset_metadata(asset_id, title, description, creator, copyright) "
        "VALUES (?, ?, ?, ?, ?) "
        "ON CONFLICT(asset_id) DO UPDATE SET title = excluded.title, "
        "description = excluded.description, creator = excluded.creator, "
        "copyright = excluded.copyright"));
    query.addBindValue(qstring_from_utf8(asset_id));
    query.addBindValue(optional_string(metadata.title));
    query.addBindValue(optional_string(metadata.description));
    query.addBindValue(optional_string(metadata.creator));
    query.addBindValue(optional_string(metadata.copyright));
    if (!query.exec())
    {
        return map_sql_error(query, "upsert_writable_metadata");
    }
    return {};
}

namespace
{

[[nodiscard]] Result<std::int64_t> read_writable_revision(QSqlDatabase &database,
                                                          std::string_view action)
{
    QSqlQuery revision(database);
    if (!revision.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) ||
        !revision.next())
        return map_sql_error(revision, action);
    return revision.value(0).toLongLong();
}

[[nodiscard]] Result<void>
require_writable_revision(QSqlDatabase &database,
                          const std::optional<std::int64_t> expected_revision,
                          std::string_view action)
{
    auto current = read_writable_revision(database, action);
    if (!current)
        return current.error();
    if (expected_revision && *expected_revision != current.value())
    {
        return make_error(ErrorCode::kConflict, "Catalog revision is stale",
                          {{"reason", "stale_catalog_revision"},
                           {"expected_revision", std::to_string(*expected_revision)},
                           {"revision", std::to_string(current.value())}});
    }
    return {};
}

[[nodiscard]] Result<std::int64_t> bump_writable_revision_locked(QSqlDatabase &database)
{
    QSqlQuery update(database);
    if (!update.exec(QStringLiteral("UPDATE schema_info SET revision = revision + 1 WHERE id = 1")))
        return map_sql_error(update, "bump_writable_metadata_revision");
    return read_writable_revision(database, "read_writable_metadata_revision");
}

[[nodiscard]] Result<void> validate_writable_patch(const WritableMetadataPatch &patch)
{
    if (patch.empty())
    {
        return make_error(ErrorCode::kValidation, "Writable metadata patch is empty",
                          {{"reason", "empty_writable_metadata_patch"}});
    }
    const auto check = [](const bool update, const char *name,
                          const std::optional<std::string> &value) -> Result<void>
    {
        if (!update || !value)
            return {};
        return validate_metadata_field(name, *value);
    };
    if (auto title = check(patch.update_title, "title", patch.title); !title)
        return title.error();
    if (auto description = check(patch.update_description, "description", patch.description);
        !description)
        return description.error();
    if (auto creator = check(patch.update_creator, "creator", patch.creator); !creator)
        return creator.error();
    if (auto copyright = check(patch.update_copyright, "copyright", patch.copyright); !copyright)
        return copyright.error();
    return {};
}

[[nodiscard]] Result<WritableMetadata> load_writable_metadata_row(QSqlDatabase &database,
                                                                  std::string_view asset_id)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral(
        "SELECT title, description, creator, copyright FROM asset_metadata WHERE asset_id = ?"));
    query.addBindValue(qstring_from_utf8(asset_id));
    if (!query.exec())
        return map_sql_error(query, "load_writable_metadata");
    WritableMetadata metadata;
    if (!query.next())
        return metadata;
    metadata.title = string_column(query, 0);
    metadata.description = string_column(query, 1);
    metadata.creator = string_column(query, 2);
    metadata.copyright = string_column(query, 3);
    return metadata;
}

} // namespace

Result<WritableMetadataMutation> SqliteCatalogRepository::patch_assets_writable_metadata(
    const std::vector<std::string> &asset_ids, const WritableMetadataPatch &patch,
    const std::optional<std::int64_t> expected_revision)
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    if (asset_ids.empty())
    {
        return make_error(ErrorCode::kValidation,
                          "Writable metadata patch requires at least one asset",
                          {{"reason", "empty_writable_metadata_asset_list"}});
    }
    auto valid_patch = validate_writable_patch(patch);
    if (!valid_patch)
        return valid_patch.error();
    if (!impl_->database.transaction())
    {
        return make_error(ErrorCode::kIo, "Unable to start writable metadata transaction",
                          {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}});
    }
    auto revision_ok =
        require_writable_revision(impl_->database, expected_revision, "read_writable_revision");
    if (!revision_ok)
        return impl_->abort_transaction(revision_ok.error());

    std::vector<std::string> unique_assets;
    unique_assets.reserve(asset_ids.size());
    std::set<std::string, std::less<>> seen;
    for (const auto &asset_id : asset_ids)
    {
        if (asset_id.empty() || !seen.insert(asset_id).second)
        {
            return impl_->abort_transaction(make_error(
                ErrorCode::kValidation, "Writable metadata asset list is invalid",
                {{"reason", "invalid_writable_metadata_asset_list"}, {"asset_id", asset_id}}));
        }
        unique_assets.push_back(asset_id);
    }

    QSqlQuery existing(impl_->database);
    QStringList placeholders;
    placeholders.reserve(static_cast<qsizetype>(unique_assets.size()));
    for (std::size_t index = 0; index < unique_assets.size(); ++index)
        placeholders.push_back(QStringLiteral("?"));
    existing.prepare(QStringLiteral("SELECT COUNT(*) FROM asset WHERE id IN (") +
                     placeholders.join(QLatin1Char(',')) + QLatin1Char(')'));
    for (const auto &asset_id : unique_assets)
        existing.addBindValue(qstring_from_utf8(asset_id));
    if (!existing.exec() || !existing.next())
        return impl_->abort_transaction(map_sql_error(existing, "verify_writable_metadata_assets"));
    if (static_cast<std::size_t>(existing.value(0).toLongLong()) != unique_assets.size())
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kNotFound, "Writable metadata asset does not exist",
                       {{"reason", "unknown_writable_metadata_asset"}}));
    }

    for (const auto &asset_id : unique_assets)
    {
        auto current = load_writable_metadata_row(impl_->database, asset_id);
        if (!current)
            return impl_->abort_transaction(current.error());
        auto metadata = current.value();
        apply_writable_metadata_patch(metadata, patch);
        auto saved = upsert_writable_metadata(asset_id, metadata);
        if (!saved)
            return impl_->abort_transaction(saved.error());
    }

    auto revision = bump_writable_revision_locked(impl_->database);
    if (!revision)
        return impl_->abort_transaction(revision.error());
    impl_->snapshot.revision = revision.value();
    if (!impl_->database.commit())
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Unable to commit writable metadata patch",
                       {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}}));
    }

    WritableMetadataMutation mutation;
    mutation.revision = revision.value();
    mutation.assets.reserve(unique_assets.size());
    for (const auto &asset_id : unique_assets)
    {
        auto asset = find_asset_by_id(asset_id);
        if (!asset)
            return asset.error();
        if (!asset.value())
        {
            return make_error(
                ErrorCode::kNotFound, "Writable metadata asset does not exist",
                {{"reason", "unknown_writable_metadata_asset"}, {"asset_id", asset_id}});
        }
        mutation.assets.push_back(std::move(*asset.value()));
    }
    return mutation;
}

Result<void> SqliteCatalogRepository::upsert_capture_metadata(const std::string_view asset_id,
                                                              const CaptureMetadata &capture)
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    auto valid = validate_capture_metadata(capture);
    if (!valid)
    {
        return valid.error();
    }
    std::optional<std::string> local_exif;
    std::optional<std::string> subsecond;
    std::optional<std::int32_t> offset;
    std::optional<std::int32_t> latitude;
    std::optional<std::int32_t> longitude;
    std::optional<std::uint32_t> altitude_magnitude;
    std::optional<std::int32_t> altitude_ref;
    if (capture.captured_datetime)
    {
        local_exif = capture.captured_datetime->local_exif;
        subsecond = capture.captured_datetime->subsecond_digits;
        offset = capture.captured_datetime->utc_offset_minutes;
    }
    if (capture.location)
    {
        latitude = capture.location->latitude_e6;
        longitude = capture.location->longitude_e6;
        if (capture.location->altitude)
        {
            altitude_magnitude = capture.location->altitude->magnitude_mm;
            altitude_ref =
                capture.location->altitude->reference == CaptureAltitudeReference::kBelowSeaLevel ?
                    1 :
                    0;
        }
    }
    QSqlQuery query(impl_->database);
    query.prepare(QStringLiteral(
        "INSERT INTO asset_metadata(asset_id, camera_make, camera_model, iso, aperture, "
        "focal_length_mm, shutter_s, captured_unix_s, captured_local_exif, "
        "captured_subsecond_digits, captured_utc_offset_minutes, gps_latitude_e6, "
        "gps_longitude_e6, gps_altitude_magnitude_mm, gps_altitude_ref) VALUES "
        "(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(asset_id) DO UPDATE SET camera_make = excluded.camera_make, "
        "camera_model = excluded.camera_model, iso = excluded.iso, aperture = excluded.aperture, "
        "focal_length_mm = excluded.focal_length_mm, shutter_s = excluded.shutter_s, "
        "captured_unix_s = excluded.captured_unix_s, "
        "captured_local_exif = excluded.captured_local_exif, "
        "captured_subsecond_digits = excluded.captured_subsecond_digits, "
        "captured_utc_offset_minutes = excluded.captured_utc_offset_minutes, "
        "gps_latitude_e6 = excluded.gps_latitude_e6, "
        "gps_longitude_e6 = excluded.gps_longitude_e6, "
        "gps_altitude_magnitude_mm = excluded.gps_altitude_magnitude_mm, "
        "gps_altitude_ref = excluded.gps_altitude_ref"));
    query.addBindValue(qstring_from_utf8(asset_id));
    query.addBindValue(optional_string(capture.camera_make));
    query.addBindValue(optional_string(capture.camera_model));
    query.addBindValue(optional_double(capture.iso));
    query.addBindValue(optional_double(capture.aperture));
    query.addBindValue(optional_double(capture.focal_length_mm));
    query.addBindValue(optional_double(capture.shutter_s));
    query.addBindValue(optional_i64(capture.captured_unix_s));
    query.addBindValue(optional_string(local_exif));
    query.addBindValue(optional_string(subsecond));
    query.addBindValue(optional_i32(offset));
    query.addBindValue(optional_i32(latitude));
    query.addBindValue(optional_i32(longitude));
    if (altitude_magnitude)
    {
        query.addBindValue(static_cast<qlonglong>(*altitude_magnitude));
    }
    else
    {
        query.addBindValue(QVariant());
    }
    query.addBindValue(optional_i32(altitude_ref));
    if (!query.exec())
    {
        return map_sql_error(query, "upsert_capture_metadata");
    }
    return {};
}

Result<void> SqliteCatalogRepository::commit_imported_asset(const AssetRecord &asset)
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    if (capture_metadata_has_values(asset.capture))
    {
        auto valid = validate_capture_metadata(asset.capture);
        if (!valid)
        {
            return valid.error();
        }
    }
    if (impl_->consume_import_failure(testing::SqliteImportFailure::kTransactionBegin))
    {
        return make_error(ErrorCode::kIo, "Injected catalog import failure",
                          {{"reason", "injected_import_transaction_begin"}});
    }
    if (!impl_->database.transaction())
    {
        return make_error(ErrorCode::kIo, "Unable to start import publication transaction",
                          {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}});
    }
    if (impl_->consume_import_failure(testing::SqliteImportFailure::kAssetBind))
    {
        return impl_->abort_transaction(make_error(ErrorCode::kIo,
                                                   "Injected catalog import failure",
                                                   {{"reason", "injected_import_asset_bind"}}));
    }
    const auto inserted = insert_asset(asset);
    if (!inserted)
    {
        return impl_->abort_transaction(inserted.error());
    }
    if (impl_->consume_import_failure(testing::SqliteImportFailure::kAssetWrite))
    {
        return impl_->abort_transaction(make_error(ErrorCode::kIo,
                                                   "Injected catalog import failure",
                                                   {{"reason", "injected_import_asset_write"}}));
    }
    if (capture_metadata_has_values(asset.capture))
    {
        if (impl_->consume_import_failure(testing::SqliteImportFailure::kCaptureBind))
        {
            return impl_->abort_transaction(
                make_error(ErrorCode::kIo, "Injected catalog import failure",
                           {{"reason", "injected_import_capture_bind"}}));
        }
        const auto captured = upsert_capture_metadata(asset.id, asset.capture);
        if (!captured)
        {
            return impl_->abort_transaction(captured.error());
        }
        if (impl_->consume_import_failure(testing::SqliteImportFailure::kCaptureWrite))
        {
            return impl_->abort_transaction(
                make_error(ErrorCode::kIo, "Injected catalog import failure",
                           {{"reason", "injected_import_capture_write"}}));
        }
    }
    if (impl_->consume_import_failure(testing::SqliteImportFailure::kRevisionUpdate))
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Injected catalog import failure",
                       {{"reason", "injected_import_revision_update"}}));
    }
    QSqlQuery revision_query(impl_->database);
    if (!revision_query.exec(
            QStringLiteral("UPDATE schema_info SET revision = revision + 1 WHERE id = 1")))
    {
        return impl_->abort_transaction(map_sql_error(revision_query, "bump_import_revision"));
    }
    if (impl_->consume_import_failure(testing::SqliteImportFailure::kRevisionRead))
    {
        return impl_->abort_transaction(make_error(ErrorCode::kIo,
                                                   "Injected catalog import failure",
                                                   {{"reason", "injected_import_revision_read"}}));
    }
    if (!revision_query.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) ||
        !revision_query.next())
    {
        return impl_->abort_transaction(map_sql_error(revision_query, "read_import_revision"));
    }
    const auto revision = revision_query.value(0).toLongLong();
    if (impl_->consume_import_failure(testing::SqliteImportFailure::kCommit))
    {
        return impl_->abort_transaction(make_error(ErrorCode::kIo,
                                                   "Injected catalog import failure",
                                                   {{"reason", "injected_import_commit"}}));
    }
    if (impl_->import_failure == testing::SqliteImportFailure::kRollback)
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Injected catalog import failure",
                       {{"reason", "injected_import_before_rollback"}}));
    }
    if (!impl_->database.commit())
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Unable to commit import publication",
                       {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}}));
    }
    impl_->snapshot.revision = revision;
    return {};
}

Result<void> SqliteCatalogRepository::commit_refreshed_asset(const AssetRecord &asset)
{
    if (impl_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    auto valid = validate_capture_metadata(asset.capture);
    if (!valid)
        return valid.error();
    if (!impl_->database.transaction())
    {
        return make_error(ErrorCode::kIo, "Unable to begin metadata refresh transaction",
                          {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}});
    }
    auto updated = update_asset(asset);
    if (!updated)
        return impl_->abort_transaction(updated.error());
    auto captured = upsert_capture_metadata(asset.id, asset.capture);
    if (!captured)
        return impl_->abort_transaction(captured.error());
    QSqlQuery revision(impl_->database);
    if (!revision.exec(
            QStringLiteral("UPDATE schema_info SET revision = revision + 1 WHERE id = 1")))
        return impl_->abort_transaction(map_sql_error(revision, "bump_refresh_revision"));
    if (!revision.exec(QStringLiteral("SELECT revision FROM schema_info WHERE id = 1")) ||
        !revision.next())
        return impl_->abort_transaction(map_sql_error(revision, "read_refresh_revision"));
    const auto next_revision = revision.value(0).toLongLong();
    if (!impl_->database.commit())
    {
        return impl_->abort_transaction(
            make_error(ErrorCode::kIo, "Unable to commit metadata refresh transaction",
                       {{"qt_error", utf8_from_qstring(impl_->database.lastError().text())}}));
    }
    impl_->snapshot.revision = next_revision;
    return {};
}

Result<std::vector<RecipeHistoryEntry>>
SqliteCatalogRepository::list_recipe_history(const std::string_view asset_id) const
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    QSqlQuery query(impl_->database);
    query.prepare(
        QStringLiteral("SELECT id, asset_id, seq, kind, label, recipe_json, created_unix_ms "
                       "FROM asset_recipe_history WHERE asset_id = ? ORDER BY seq DESC, id DESC"));
    query.addBindValue(qstring_from_utf8(asset_id));
    if (!query.exec())
    {
        return map_sql_error(query, "list_recipe_history");
    }
    std::vector<RecipeHistoryEntry> entries;
    while (query.next())
    {
        entries.push_back(read_history(query));
    }
    return entries;
}

Result<std::optional<RecipeHistoryEntry>>
SqliteCatalogRepository::find_recipe_history(const std::int64_t history_id) const
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    QSqlQuery query(impl_->database);
    query.prepare(
        QStringLiteral("SELECT id, asset_id, seq, kind, label, recipe_json, created_unix_ms "
                       "FROM asset_recipe_history WHERE id = ?"));
    query.addBindValue(static_cast<qlonglong>(history_id));
    if (!query.exec())
    {
        return map_sql_error(query, "find_recipe_history");
    }
    if (!query.next())
    {
        return std::optional<RecipeHistoryEntry>{};
    }
    return std::optional<RecipeHistoryEntry>{read_history(query)};
}

Result<RecipeHistoryEntry> SqliteCatalogRepository::append_recipe_history(
    const std::string_view asset_id, const std::string_view kind,
    const std::optional<std::string_view> label, const std::string_view recipe_json)
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    if (kind != kRecipeHistoryKindHistory && kind != kRecipeHistoryKindSnapshot)
    {
        return make_error(ErrorCode::kValidation, "Recipe history kind is unsupported",
                          {{"kind", std::string(kind)}});
    }
    QSqlQuery max_seq(impl_->database);
    max_seq.prepare(QStringLiteral(
        "SELECT COALESCE(MAX(seq), 0) FROM asset_recipe_history WHERE asset_id = ?"));
    max_seq.addBindValue(qstring_from_utf8(asset_id));
    if (!max_seq.exec() || !max_seq.next())
    {
        return map_sql_error(max_seq, "max_recipe_history_seq");
    }
    const auto seq = max_seq.value(0).toLongLong() + 1;
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    QSqlQuery insert(impl_->database);
    insert.prepare(QStringLiteral(
        "INSERT INTO asset_recipe_history(asset_id, seq, kind, label, recipe_json, created_unix_ms) "
        "VALUES (?, ?, ?, ?, ?, ?)"));
    insert.addBindValue(qstring_from_utf8(asset_id));
    insert.addBindValue(seq);
    insert.addBindValue(qstring_from_utf8(kind));
    if (label)
    {
        insert.addBindValue(qstring_from_utf8(*label));
    }
    else
    {
        insert.addBindValue(QVariant());
    }
    insert.addBindValue(qstring_from_utf8(recipe_json));
    insert.addBindValue(static_cast<qlonglong>(now));
    if (!insert.exec())
    {
        return map_sql_error(insert, "append_recipe_history");
    }
    RecipeHistoryEntry entry;
    entry.id = insert.lastInsertId().toLongLong();
    entry.asset_id = std::string(asset_id);
    entry.seq = seq;
    entry.kind = std::string(kind);
    if (label)
    {
        entry.label = std::string(*label);
    }
    entry.recipe_json = std::string(recipe_json);
    entry.created_unix_ms = now;
    return entry;
}

Result<void> SqliteCatalogRepository::update_recipe_history_label(const std::int64_t history_id,
                                                                  const std::string_view label)
{
    if (impl_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog repository is closed");
    }
    QSqlQuery query(impl_->database);
    query.prepare(
        QStringLiteral("UPDATE asset_recipe_history SET label = ? WHERE id = ? AND kind = ?"));
    query.addBindValue(qstring_from_utf8(label));
    query.addBindValue(static_cast<qlonglong>(history_id));
    query.addBindValue(qstring_from_utf8(kRecipeHistoryKindSnapshot));
    if (!query.exec())
    {
        return map_sql_error(query, "update_recipe_history_label");
    }
    if (query.numRowsAffected() == 0)
    {
        return make_error(ErrorCode::kNotFound, "Recipe snapshot does not exist",
                          {{"history_id", std::to_string(history_id)}});
    }
    return {};
}

} // namespace ravo
