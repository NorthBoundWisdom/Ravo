#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>

#include "ravo/adapters/sqlite_catalog.h"
#include "catalog_repository_test_control.h"

namespace ravo
{

struct SqliteCatalogRepository::Impl
{
    QString connection_name;
    QSqlDatabase database;
    std::string database_path;
    CatalogSnapshot snapshot;
    testing::SqliteImportFailure import_failure = testing::SqliteImportFailure::kNone;
    testing::SqliteRecoveryFailure recovery_failure = testing::SqliteRecoveryFailure::kNone;
    testing::SqliteFolderRelinkFailure folder_relink_failure =
        testing::SqliteFolderRelinkFailure::kNone;
    testing::SqliteReviewFailure review_failure = testing::SqliteReviewFailure::kNone;

    [[nodiscard]] bool consume_import_failure(testing::SqliteImportFailure expected) noexcept;
    [[nodiscard]] TaskError abort_transaction(TaskError primary);
    [[nodiscard]] bool consume_recovery_failure(testing::SqliteRecoveryFailure expected) noexcept;
    [[nodiscard]] bool
    consume_folder_relink_failure(testing::SqliteFolderRelinkFailure expected) noexcept;
    [[nodiscard]] bool consume_review_failure(testing::SqliteReviewFailure expected) noexcept;
    [[nodiscard]] Result<void> exec(const QString &sql, std::string_view action);
    [[nodiscard]] Result<void> repair_v5_capture_columns();
};

namespace sqlite_internal
{

extern const char *const kAssetSelect;
extern const char *const kAssetPageSelect;
extern const char *const kPreviewSelect;

[[nodiscard]] QString qstring_from_utf8(std::string_view text);
[[nodiscard]] std::string utf8_from_qstring(const QString &text);
void append_library_query_predicates(const LibraryQuery &query, QStringList &predicates,
                                     QVariantList &bindings);
[[nodiscard]] TaskError map_sql_error(const QSqlQuery &query, std::string_view action);
[[nodiscard]] Result<std::size_t> count_library_query(QSqlDatabase &database,
                                                      const LibraryQuery &query);
[[nodiscard]] Result<LibrarySetRecord> read_library_set(QSqlDatabase &database, QSqlQuery &query);
[[nodiscard]] Result<std::set<std::string, std::less<>>>
asset_metadata_columns(QSqlDatabase &database);
[[nodiscard]] Result<std::set<std::string, std::less<>>> asset_columns(QSqlDatabase &database);
[[nodiscard]] QVariant optional_string(const std::optional<std::string> &value);
[[nodiscard]] QVariant optional_u32(const std::optional<std::uint32_t> &value);
[[nodiscard]] QVariant optional_i64(const std::optional<std::int64_t> &value);
[[nodiscard]] QVariant optional_i32(const std::optional<std::int32_t> &value);
[[nodiscard]] QVariant optional_double(const std::optional<double> &value);
[[nodiscard]] std::optional<std::string> string_column(const QSqlQuery &query, int index);
[[nodiscard]] std::optional<std::uint32_t> u32_column(const QSqlQuery &query, int index);
[[nodiscard]] std::optional<std::int64_t> i64_column(const QSqlQuery &query, int index);
[[nodiscard]] std::optional<double> double_column(const QSqlQuery &query, int index);
[[nodiscard]] AssetRecord read_asset(const QSqlQuery &query);
[[nodiscard]] Result<void> attach_asset_fields(QSqlDatabase &database,
                                               std::vector<AssetRecord> &assets);
[[nodiscard]] Result<void> attach_asset_fields(QSqlDatabase &database, AssetRecord &asset);
[[nodiscard]] RecipeHistoryEntry read_history(const QSqlQuery &query);
[[nodiscard]] PreviewRecord read_preview(const QSqlQuery &query);
[[nodiscard]] AssetRecoveryState read_recovery_state(const QSqlQuery &query, int first_column = 0);
[[nodiscard]] QString next_connection_name();
[[nodiscard]] Result<CatalogDatabaseArtifact>
verify_database_artifact(const std::string_view path, const CancellationToken &cancellation);
[[nodiscard]] Result<CatalogDatabaseArtifact>
inspect_backup_database(std::string_view path, std::string_view expected_sha256,
                        const CancellationToken &cancellation);
[[nodiscard]] Result<void> copy_database_snapshot(std::string_view source_path,
                                                  std::string_view output_path,
                                                  const CancellationToken &cancellation);
[[nodiscard]] Result<void> strip_backup_preview_rows(std::string_view path,
                                                     const CancellationToken &cancellation);

} // namespace sqlite_internal
} // namespace ravo
