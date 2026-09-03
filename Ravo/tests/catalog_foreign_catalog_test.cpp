#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "ravo/adapters/text_file.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/services/catalog_service.h"
#include "ravo/services/foreign_catalog.h"

#include "catalog_test_support.h"

namespace ravo
{
namespace
{

[[nodiscard]] std::string foreign_fixture_path(const std::string_view name)
{
    return repository_path(std::filesystem::path("Ravo") / "tests" / "fixtures" /
                           "foreign_catalog" / std::string(name));
}

[[nodiscard]] std::string reason_of(const TaskError &error)
{
    const auto it = error.context.find("reason");
    return it == error.context.end() ? std::string{} : it->second;
}

[[nodiscard]] const ForeignCatalogItemReport *
find_item(const ForeignCatalogConversionReport &report, const std::string_view foreign_id)
{
    for (const auto &item : report.items)
    {
        if (item.foreign_id == foreign_id)
            return &item;
    }
    return nullptr;
}

[[nodiscard]] bool contains(const std::vector<std::string> &values, const std::string_view value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

struct FileSnapshot
{
    std::string sha256;
    std::uint64_t size_bytes = 0;
    std::int64_t mtime_unix_ms = 0;

    [[nodiscard]] bool operator==(const FileSnapshot &) const noexcept = default;
};

[[nodiscard]] FileSnapshot snapshot_of(const std::string &path)
{
    FileSnapshot snapshot;
    auto digest = sha256_file_hex(path);
    EXPECT_TRUE(digest) << path;
    if (digest)
        snapshot.sha256 = digest.value();
    auto identity = read_file_identity(path);
    EXPECT_TRUE(identity) << path;
    if (identity)
    {
        snapshot.size_bytes = identity.value().size_bytes;
        snapshot.mtime_unix_ms = identity.value().mtime_unix_ms;
    }
    return snapshot;
}

} // namespace

TEST_F(CatalogServiceTest, ForeignCatalogConversionMapsLightroomFixtureIntoNewCatalog)
{
    ASSERT_TRUE(open_service(true));
    const auto source = foreign_fixture_path("lightroom-classic-v1.json");
    const auto original = png_fixture_path();
    const auto crs_sidecar = repository_path(std::filesystem::path("Ravo") / "tests" / "fixtures" /
                                             "crs_pv2012_heishijiao.xmp");
    const auto before_source = snapshot_of(source);
    const auto before_original = snapshot_of(original);
    const auto before_sidecar = snapshot_of(crs_sidecar);

    ForeignCatalogConversionRequest request;
    request.source_path = source;
    auto converted = service->convert_foreign_catalog(request);
    ASSERT_TRUE(converted) << converted.error().message;
    const auto &report = converted.value();
    EXPECT_EQ(report.schema, kForeignCatalogFixtureContractVersion);
    EXPECT_EQ(report.schema_version, kForeignCatalogFixtureSchemaVersion);
    EXPECT_EQ(report.source_kind, ForeignCatalogSourceKind::kLightroomClassic);
    EXPECT_EQ(report.source_product_version, std::optional<std::string>{"13.5"});
    EXPECT_EQ(report.destination_catalog, database_path);
    EXPECT_NE(report.destination_catalog, report.source_path);
    EXPECT_EQ(report.imported, 1U);
    EXPECT_EQ(report.skipped, 1U);
    EXPECT_EQ(report.failed, 0U);
    EXPECT_TRUE(report.originals_unchanged);
    EXPECT_FALSE(report.cancelled);
    ASSERT_EQ(report.items.size(), 2U);

    const auto *mapped = find_item(report, "lr-mapped-1");
    ASSERT_NE(mapped, nullptr);
    EXPECT_EQ(mapped->status, ForeignCatalogItemStatus::kImported);
    ASSERT_TRUE(mapped->asset_id.has_value());
    EXPECT_TRUE(contains(mapped->mapped_fields, "rating"));
    EXPECT_TRUE(contains(mapped->mapped_fields, "color_label"));
    EXPECT_TRUE(contains(mapped->mapped_fields, "title"));
    EXPECT_TRUE(contains(mapped->mapped_fields, "city"));
    EXPECT_TRUE(contains(mapped->mapped_fields, "keywords"));
    EXPECT_TRUE(contains(mapped->mapped_fields, "crs"));

    const auto *skipped = find_item(report, "lr-missing-1");
    ASSERT_NE(skipped, nullptr);
    EXPECT_EQ(skipped->status, ForeignCatalogItemStatus::kSkipped);
    EXPECT_TRUE(contains(skipped->reasons, "missing_original"));
    EXPECT_FALSE(skipped->asset_id.has_value());

    auto assets = service->list_assets();
    ASSERT_TRUE(assets) << assets.error().message;
    ASSERT_EQ(assets.value().size(), 1U);
    const auto &asset = assets.value().front();
    EXPECT_EQ(asset.id, *mapped->asset_id);
    EXPECT_EQ(asset.review.rating, 4);
    EXPECT_EQ(asset.review.color_label, ColorLabel::kRed);
    EXPECT_FALSE(asset.review.rejected);
    EXPECT_EQ(asset.metadata.title, std::optional<std::string>{"Mapped sunset"});
    EXPECT_EQ(asset.metadata.creator, std::optional<std::string>{"Ravo Tests"});
    EXPECT_EQ(asset.metadata.country, std::optional<std::string>{"Japan"});
    EXPECT_EQ(asset.metadata.city, std::optional<std::string>{"Kyoto"});
    EXPECT_EQ(asset.metadata.sublocation, std::optional<std::string>{"Fushimi"});
    EXPECT_TRUE(contains(asset.tags, "Travel|Japan"));
    EXPECT_TRUE(asset.has_edits);

    // Source tree stays byte-identical and read-only.
    EXPECT_EQ(snapshot_of(source), before_source);
    EXPECT_EQ(snapshot_of(original), before_original);
    EXPECT_EQ(snapshot_of(crs_sidecar), before_sidecar);

    // A second conversion into the now-populated catalog fails closed.
    auto again = service->convert_foreign_catalog(request);
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, ErrorCode::kConflict);
    EXPECT_EQ(reason_of(again.error()), "destination_catalog_not_empty");
    auto after_conflict = service->list_assets();
    ASSERT_TRUE(after_conflict) << after_conflict.error().message;
    EXPECT_EQ(after_conflict.value().size(), 1U);
}

TEST_F(CatalogServiceTest, ForeignCatalogConversionCountsUnsupportedCaptureOneAdjusts)
{
    ASSERT_TRUE(open_service(true));
    ForeignCatalogConversionRequest request;
    request.source_path = foreign_fixture_path("capture-one-v1.json");
    request.source_kind = ForeignCatalogSourceKind::kCaptureOne;
    auto converted = service->convert_foreign_catalog(request);
    ASSERT_TRUE(converted) << converted.error().message;
    const auto &report = converted.value();
    EXPECT_EQ(report.source_kind, ForeignCatalogSourceKind::kCaptureOne);
    EXPECT_EQ(report.imported, 2U);
    EXPECT_EQ(report.skipped, 0U);
    EXPECT_EQ(report.failed, 0U);
    EXPECT_EQ(report.unsupported_fields, 2U);

    const auto *unsupported = find_item(report, "c1-unsupported-look-1");
    ASSERT_NE(unsupported, nullptr);
    EXPECT_EQ(unsupported->status, ForeignCatalogItemStatus::kImported);
    ASSERT_EQ(unsupported->unsupported_fields.size(), 2U);
    EXPECT_EQ(unsupported->unsupported_fields.front().key, "c1.filmgrain");
    EXPECT_EQ(unsupported->unsupported_fields.front().reason, "unsupported_foreign_adjust");
    EXPECT_FALSE(contains(unsupported->mapped_fields, "crs"));

    auto assets = service->list_assets();
    ASSERT_TRUE(assets) << assets.error().message;
    EXPECT_EQ(assets.value().size(), 2U);
}

TEST_F(CatalogServiceTest, ForeignCatalogConversionFailsClosedOnUnsupportedSourcesAndModes)
{
    ASSERT_TRUE(open_service(true));
    const auto expect_rejected =
        [&](const std::string &source, const ErrorCode code, const std::string_view reason)
    {
        ForeignCatalogConversionRequest request;
        request.source_path = source;
        auto converted = service->convert_foreign_catalog(request);
        ASSERT_FALSE(converted) << source;
        EXPECT_EQ(converted.error().code, code) << source;
        EXPECT_EQ(reason_of(converted.error()), reason) << source;
    };

    expect_rejected(foreign_fixture_path("unsupported-schema.json"), ErrorCode::kUnsupported,
                    "unsupported_source_schema");
    expect_rejected(foreign_fixture_path("unsupported-version.json"), ErrorCode::kUnsupported,
                    "unsupported_source_version");
    // Vendor binary catalogs stay unsupported: no Adobe runtime in the package.
    expect_rejected(foreign_fixture_path("not-a-catalog.lrcat"), ErrorCode::kUnsupported,
                    "unsupported_source_schema");
    expect_rejected(repository_path(std::filesystem::path("Ravo") / "tests" / "fixtures" /
                                    "foreign_catalog" / "absent-source.json"),
                    ErrorCode::kNotFound, "foreign_catalog_source_missing");
    // Capture One session directories are not a packaged reader either.
    expect_rejected(
        repository_path(std::filesystem::path("Ravo") / "tests" / "fixtures" / "foreign_catalog"),
        ErrorCode::kUnsupported, "unsupported_source_schema");

    ForeignCatalogConversionRequest empty;
    auto missing_source = service->convert_foreign_catalog(empty);
    ASSERT_FALSE(missing_source);
    EXPECT_EQ(missing_source.error().code, ErrorCode::kInvalidArgument);
    EXPECT_EQ(reason_of(missing_source.error()), "foreign_catalog_source_missing");

    // A declared source kind must match the document.
    ForeignCatalogConversionRequest mismatched;
    mismatched.source_path = foreign_fixture_path("capture-one-v1.json");
    mismatched.source_kind = ForeignCatalogSourceKind::kLightroomClassic;
    auto kind_rejected = service->convert_foreign_catalog(mismatched);
    ASSERT_FALSE(kind_rejected);
    EXPECT_EQ(kind_rejected.error().code, ErrorCode::kValidation);
    EXPECT_EQ(reason_of(kind_rejected.error()), "foreign_catalog_source_kind_mismatch");

    ForeignCatalogConversionRequest moved;
    moved.source_path = foreign_fixture_path("lightroom-classic-v1.json");
    moved.mode = ImportTransferMode::kMove;
    auto move_rejected = service->convert_foreign_catalog(moved);
    ASSERT_FALSE(move_rejected);
    EXPECT_EQ(move_rejected.error().code, ErrorCode::kInvalidArgument);
    EXPECT_EQ(reason_of(move_rejected.error()), "foreign_catalog_move_rejected");

    auto unknown_kind = parse_foreign_catalog_source_kind("aperture");
    ASSERT_FALSE(unknown_kind);
    EXPECT_EQ(unknown_kind.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(reason_of(unknown_kind.error()), "unsupported_source_kind");

    // Every fail-closed path leaves the destination catalog empty.
    auto assets = service->list_assets();
    ASSERT_TRUE(assets) << assets.error().message;
    EXPECT_TRUE(assets.value().empty());
}

TEST_F(CatalogServiceTest, ForeignCatalogConversionCancelReportsRemainingItems)
{
    ASSERT_TRUE(open_service(true));
    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel("conversion-test-cancel"));
    ForeignCatalogConversionRequest request;
    request.source_path = foreign_fixture_path("lightroom-classic-v1.json");
    request.cancellation = cancellation.token();
    auto converted = service->convert_foreign_catalog(request);
    ASSERT_FALSE(converted);
    EXPECT_EQ(converted.error().code, ErrorCode::kCancelled);
    auto assets = service->list_assets();
    ASSERT_TRUE(assets) << assets.error().message;
    EXPECT_TRUE(assets.value().empty());
}

} // namespace ravo
