#include <filesystem>
#include <system_error>
#include <string>

#include <gtest/gtest.h>

#include "ravo/adapters/crs_xmp.h"
#include "ravo/adapters/text_file.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/recipe/develop.h"
#include "ravo/services/catalog_service.h"
#include "ravo/services/xmp_interchange.h"

#include "catalog_test_support.h"

namespace ravo
{
namespace
{

[[nodiscard]] std::string write_adjacent_xmp(const std::string &original_path,
                                             const std::string_view xmp_utf8)
{
    std::filesystem::path path(original_path);
    path.replace_extension(".xmp");
    const auto text = path.string();
    auto written = write_utf8_text_file_replace_atomically(text, xmp_utf8);
    EXPECT_TRUE(written) << (written ? "" : written.error().message) << " path=" << text;
    return text;
}

[[nodiscard]] std::string minimal_crs_xmp(const double exposure_ev)
{
    DevelopParams look;
    look.exposure_ev = exposure_ev;
    auto exported = export_crs_xmp({look, "Fixture"});
    EXPECT_TRUE(exported) << exported.error().message;
    return exported.value().xmp_utf8;
}

TEST_F(CatalogServiceTest, XmpInterchangeConflictMatrixAndFailClosed)
{
    ASSERT_TRUE(open_service(true)) << "open failed";
    const auto local_png = (root / "photo.png").string();
    std::error_code copy_error;
    std::filesystem::copy_file(png_fixture_path(), local_png,
                               std::filesystem::copy_options::overwrite_existing, copy_error);
    ASSERT_FALSE(copy_error) << copy_error.message();
    auto imported = service->import_one(local_png, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    const auto original = [&]
    {
        auto status = service->xmp_interchange_status(asset_id);
        EXPECT_TRUE(status) << status.error().message;
        return status.value().original_path;
    }();

    auto missing = service->xmp_interchange_status(asset_id);
    ASSERT_TRUE(missing) << missing.error().message;
    EXPECT_EQ(missing.value().conflict_class, XmpInterchangeConflictClass::kMissing);

    const auto sidecar_path = write_adjacent_xmp(original, minimal_crs_xmp(-0.41));
    auto before = read_file_identity(original);
    ASSERT_TRUE(before) << before.error().message;
    const auto before_sha = sha256_file_hex(original);
    ASSERT_TRUE(before_sha) << before_sha.error().message;

    auto sidecar_newer = service->xmp_interchange_status(asset_id);
    ASSERT_TRUE(sidecar_newer) << sidecar_newer.error().message;
    EXPECT_EQ(sidecar_newer.value().conflict_class, XmpInterchangeConflictClass::kSidecarNewer);
    EXPECT_TRUE(sidecar_newer.value().crs_parse_ok);

    auto blocked = service->xmp_interchange_import(asset_id, XmpInterchangeResolve::kAbort);
    ASSERT_FALSE(blocked);
    EXPECT_EQ(blocked.error().code, ErrorCode::kConflict);
    EXPECT_EQ(blocked.error().context.at("conflict_class"), "sidecar-newer");

    auto applied = service->xmp_interchange_import(asset_id, XmpInterchangeResolve::kSidecar);
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(applied.value().status.conflict_class, XmpInterchangeConflictClass::kIdentical);
    auto recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(recipe);
    auto develop = develop_from_recipe(recipe.value());
    ASSERT_TRUE(develop);
    EXPECT_NEAR(develop.value().exposure_ev, -0.41, 1e-9);

    auto after_import = read_file_identity(original);
    ASSERT_TRUE(after_import) << after_import.error().message;
    EXPECT_EQ(after_import.value().size_bytes, before.value().size_bytes);
    EXPECT_EQ(after_import.value().mtime_unix_ms, before.value().mtime_unix_ms);
    const auto after_sha = sha256_file_hex(original);
    ASSERT_TRUE(after_sha);
    EXPECT_EQ(after_sha.value(), before_sha.value());

    // Catalog edit without touching sidecar => catalog-newer.
    develop.value().exposure_ev = 0.25;
    ASSERT_TRUE(service->save_develop(asset_id, develop.value()));
    auto catalog_newer = service->xmp_interchange_status(asset_id);
    ASSERT_TRUE(catalog_newer) << catalog_newer.error().message;
    EXPECT_EQ(catalog_newer.value().conflict_class, XmpInterchangeConflictClass::kCatalogNewer);

    auto import_blocked = service->xmp_interchange_import(asset_id, XmpInterchangeResolve::kAbort);
    ASSERT_FALSE(import_blocked);
    EXPECT_EQ(import_blocked.error().context.at("conflict_class"), "catalog-newer");

    auto exported = service->xmp_interchange_export(asset_id, XmpInterchangeResolve::kCatalog);
    ASSERT_TRUE(exported) << exported.error().message;
    EXPECT_EQ(exported.value().status.conflict_class, XmpInterchangeConflictClass::kIdentical);

    // External sidecar edit while catalog unchanged since baseline => sidecar-newer again.
    ASSERT_TRUE(write_utf8_text_file_replace_atomically(sidecar_path, minimal_crs_xmp(1.0)));
    auto again_sidecar = service->xmp_interchange_status(asset_id);
    ASSERT_TRUE(again_sidecar);
    EXPECT_EQ(again_sidecar.value().conflict_class, XmpInterchangeConflictClass::kSidecarNewer);

    // Change catalog too => both-changed.
    develop.value().exposure_ev = -1.0;
    ASSERT_TRUE(service->save_develop(asset_id, develop.value()));
    auto both = service->xmp_interchange_status(asset_id);
    ASSERT_TRUE(both);
    EXPECT_EQ(both.value().conflict_class, XmpInterchangeConflictClass::kBothChanged);
    auto both_blocked = service->xmp_interchange_import(asset_id, XmpInterchangeResolve::kAbort);
    ASSERT_FALSE(both_blocked);
    EXPECT_EQ(both_blocked.error().context.at("conflict_class"), "both-changed");
    auto both_export_blocked =
        service->xmp_interchange_export(asset_id, XmpInterchangeResolve::kAbort);
    ASSERT_FALSE(both_export_blocked);
    EXPECT_EQ(both_export_blocked.error().context.at("reason"), "xmp_export_conflict");

    // Fail-closed unsupported CRS key.
    auto unsupported_text = minimal_crs_xmp(0.0);
    const auto marker = unsupported_text.find("crs:HasSettings=\"True\"");
    ASSERT_NE(marker, std::string::npos);
    unsupported_text.insert(marker, "crs:Texture=\"20\" ");
    ASSERT_TRUE(write_utf8_text_file_replace_atomically(sidecar_path, unsupported_text));
    auto unsupported_status = service->xmp_interchange_status(asset_id);
    ASSERT_TRUE(unsupported_status);
    EXPECT_FALSE(unsupported_status.value().crs_parse_ok);
    EXPECT_EQ(unsupported_status.value().crs_parse_reason.value_or(""), "unsupported_crs_key");
    auto unsupported_import =
        service->xmp_interchange_import(asset_id, XmpInterchangeResolve::kSidecar);
    ASSERT_FALSE(unsupported_import);
    EXPECT_EQ(unsupported_import.error().code, ErrorCode::kUnsupported);

    auto final_original = read_file_identity(original);
    ASSERT_TRUE(final_original) << final_original.error().message;
    EXPECT_EQ(final_original.value().size_bytes, before.value().size_bytes);
    EXPECT_EQ(final_original.value().mtime_unix_ms, before.value().mtime_unix_ms);
    const auto final_sha = sha256_file_hex(original);
    ASSERT_TRUE(final_sha);
    EXPECT_EQ(final_sha.value(), before_sha.value());
}

} // namespace
} // namespace ravo
