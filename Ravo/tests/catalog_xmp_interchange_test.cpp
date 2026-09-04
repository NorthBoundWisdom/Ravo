#include <filesystem>
#include <system_error>
#include <string>

#include <gtest/gtest.h>

#include "ravo/adapters/crs_xmp.h"
#include "ravo/adapters/text_file.h"
#include "ravo/adapters/xmp_adjacent_metadata.h"
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

[[nodiscard]] std::string adjacent_metadata_crs_xmp(const double exposure_ev,
                                                    const WritableMetadata &writable,
                                                    const std::vector<std::string> &keywords)
{
    DevelopParams look;
    look.exposure_ev = exposure_ev;
    auto exported = export_xmp_adjacent_interchange({look, "Fixture", writable, keywords});
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
    EXPECT_TRUE(sidecar_newer.value().metadata_parse_ok);

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

TEST_F(CatalogServiceTest, XmpAdjacentKeywordIptcLocationMergeMatrix)
{
    ASSERT_TRUE(open_service(true)) << "open failed";
    const auto local_png = (root / "meta-photo.png").string();
    std::error_code copy_error;
    std::filesystem::copy_file(png_fixture_path(), local_png,
                               std::filesystem::copy_options::overwrite_existing, copy_error);
    ASSERT_FALSE(copy_error) << copy_error.message();
    auto imported = service->import_one(local_png, CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto asset_id = imported.value().asset->id;
    auto status0 = service->xmp_interchange_status(asset_id);
    ASSERT_TRUE(status0) << status0.error().message;
    const auto original = status0.value().original_path;

    WritableMetadata sidecar_meta;
    sidecar_meta.title = "Sidecar Title";
    sidecar_meta.description = "Sidecar Desc";
    sidecar_meta.creator = "Sidecar Creator";
    sidecar_meta.copyright = "© Sidecar";
    sidecar_meta.country = "France";
    sidecar_meta.province_state = "Île-de-France";
    sidecar_meta.city = "Paris";
    sidecar_meta.sublocation = "Louvre";
    sidecar_meta.headline = "Louvre night";
    sidecar_meta.credit = "Wire Desk";
    sidecar_meta.source = "AFP";
    sidecar_meta.instructions = "No crop";
    sidecar_meta.usage_terms = "Editorial";
    sidecar_meta.job_id = "JOB-42";
    const auto sidecar_path = write_adjacent_xmp(
        original, adjacent_metadata_crs_xmp(-0.2, sidecar_meta, {"Nature|Birds", "Archive"}));

    auto before = read_file_identity(original);
    ASSERT_TRUE(before) << before.error().message;
    const auto before_sha = sha256_file_hex(original);
    ASSERT_TRUE(before_sha);

    auto status = service->xmp_interchange_status(asset_id);
    ASSERT_TRUE(status) << status.error().message;
    EXPECT_EQ(status.value().conflict_class, XmpInterchangeConflictClass::kSidecarNewer);
    EXPECT_TRUE(status.value().crs_parse_ok);
    EXPECT_TRUE(status.value().metadata_parse_ok);
    EXPECT_FALSE(status.value().has_adjacent_metadata);

    auto blocked = service->xmp_interchange_import(asset_id, XmpInterchangeResolve::kAbort);
    ASSERT_FALSE(blocked);
    EXPECT_EQ(blocked.error().context.at("conflict_class"), "sidecar-newer");

    auto applied = service->xmp_interchange_import(asset_id, XmpInterchangeResolve::kSidecar);
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_TRUE(applied.value().applied_crs);
    EXPECT_TRUE(applied.value().applied_metadata);
    EXPECT_TRUE(applied.value().applied_keywords);
    EXPECT_EQ(applied.value().status.conflict_class, XmpInterchangeConflictClass::kIdentical);
    EXPECT_TRUE(applied.value().status.has_adjacent_metadata);

    ASSERT_TRUE(applied.value().asset.metadata.title);
    EXPECT_EQ(*applied.value().asset.metadata.title, "Sidecar Title");
    ASSERT_TRUE(applied.value().asset.metadata.country);
    EXPECT_EQ(*applied.value().asset.metadata.country, "France");
    ASSERT_TRUE(applied.value().asset.metadata.sublocation);
    EXPECT_EQ(*applied.value().asset.metadata.sublocation, "Louvre");
    ASSERT_TRUE(applied.value().asset.metadata.headline);
    EXPECT_EQ(*applied.value().asset.metadata.headline, "Louvre night");
    ASSERT_TRUE(applied.value().asset.metadata.credit);
    EXPECT_EQ(*applied.value().asset.metadata.credit, "Wire Desk");
    ASSERT_TRUE(applied.value().asset.metadata.source);
    EXPECT_EQ(*applied.value().asset.metadata.source, "AFP");
    ASSERT_TRUE(applied.value().asset.metadata.instructions);
    EXPECT_EQ(*applied.value().asset.metadata.instructions, "No crop");
    ASSERT_TRUE(applied.value().asset.metadata.usage_terms);
    EXPECT_EQ(*applied.value().asset.metadata.usage_terms, "Editorial");
    ASSERT_TRUE(applied.value().asset.metadata.job_id);
    EXPECT_EQ(*applied.value().asset.metadata.job_id, "JOB-42");
    ASSERT_EQ(applied.value().asset.tags.size(), 2U);
    EXPECT_EQ(applied.value().asset.tags[0], "Archive");
    EXPECT_EQ(applied.value().asset.tags[1], "Nature|Birds");

    auto recipe = service->load_recipe(asset_id);
    ASSERT_TRUE(recipe);
    auto develop = develop_from_recipe(recipe.value());
    ASSERT_TRUE(develop);
    EXPECT_NEAR(develop.value().exposure_ev, -0.2, 1e-9);

    // Catalog metadata edit alone => catalog-newer.
    WritableMetadata edited = applied.value().asset.metadata;
    edited.title = "Catalog Title";
    ASSERT_TRUE(service->set_writable_metadata(asset_id, edited));
    auto catalog_newer = service->xmp_interchange_status(asset_id);
    ASSERT_TRUE(catalog_newer);
    EXPECT_EQ(catalog_newer.value().conflict_class, XmpInterchangeConflictClass::kCatalogNewer);

    auto export_blocked = service->xmp_interchange_export(asset_id, XmpInterchangeResolve::kAbort);
    ASSERT_FALSE(export_blocked);
    auto exported = service->xmp_interchange_export(asset_id, XmpInterchangeResolve::kCatalog);
    ASSERT_TRUE(exported) << exported.error().message;
    EXPECT_EQ(exported.value().status.conflict_class, XmpInterchangeConflictClass::kIdentical);
    auto exported_text = read_utf8_text_file(sidecar_path);
    ASSERT_TRUE(exported_text);
    EXPECT_NE(exported_text.value().find("Catalog Title"), std::string::npos);
    EXPECT_NE(exported_text.value().find("Nature|Birds"), std::string::npos);
    EXPECT_NE(exported_text.value().find("photoshop:Country"), std::string::npos);
    EXPECT_NE(exported_text.value().find("photoshop:Headline"), std::string::npos);
    EXPECT_NE(exported_text.value().find("xmpRights:UsageTerms"), std::string::npos);
    EXPECT_NE(exported_text.value().find("photoshop:TransmissionReference"), std::string::npos);

    // Keyword-only catalog change also participates.
    ASSERT_TRUE(service->set_tags(asset_id, {"Nature|Birds", "Archive", "Travel"}));
    auto keyword_newer = service->xmp_interchange_status(asset_id);
    ASSERT_TRUE(keyword_newer);
    EXPECT_EQ(keyword_newer.value().conflict_class, XmpInterchangeConflictClass::kCatalogNewer);

    // Fail-closed structured hierarchical keywords.
    auto bad = minimal_crs_xmp(0.0);
    const auto close = bad.find("</rdf:Description>");
    ASSERT_NE(close, std::string::npos);
    const std::string structured =
        "   <lr:hierarchicalSubject xmlns:lr=\"http://ns.adobe.com/lightroom/1.0/\">\n"
        "    <rdf:Bag>\n"
        "     <rdf:li>\n"
        "      <rdf:Description>\n"
        "       <rdf:value>Animals</rdf:value>\n"
        "      </rdf:Description>\n"
        "     </rdf:li>\n"
        "    </rdf:Bag>\n"
        "   </lr:hierarchicalSubject>\n";
    // Need lr namespace on Description — inject structured block before close.
    bad.insert(close, structured);
    // Also ensure xmlns:lr exists for the parser namespace binding via default xmlns on element.
    ASSERT_TRUE(write_utf8_text_file_replace_atomically(sidecar_path, bad));
    auto bad_status = service->xmp_interchange_status(asset_id);
    ASSERT_TRUE(bad_status) << bad_status.error().message;
    EXPECT_FALSE(bad_status.value().metadata_parse_ok);
    EXPECT_EQ(bad_status.value().metadata_parse_reason.value_or(""),
              "unsupported_hierarchical_keyword_shape");
    auto bad_import = service->xmp_interchange_import(asset_id, XmpInterchangeResolve::kSidecar);
    ASSERT_FALSE(bad_import);
    EXPECT_EQ(bad_import.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(bad_import.error().context.at("reason"), "unsupported_hierarchical_keyword_shape");

    // Flat dc:subject with '|' without lr:hierarchicalSubject fails closed.
    auto flat_bad = minimal_crs_xmp(0.1);
    const auto flat_close = flat_bad.find("</rdf:Description>");
    ASSERT_NE(flat_close, std::string::npos);
    flat_bad.insert(flat_close, "   <dc:subject xmlns:dc=\"http://purl.org/dc/elements/1.1/\">\n"
                                "    <rdf:Bag>\n"
                                "     <rdf:li>Nature|Birds</rdf:li>\n"
                                "    </rdf:Bag>\n"
                                "   </dc:subject>\n");
    ASSERT_TRUE(write_utf8_text_file_replace_atomically(sidecar_path, flat_bad));
    auto flat_status = service->xmp_interchange_status(asset_id);
    ASSERT_TRUE(flat_status);
    EXPECT_FALSE(flat_status.value().metadata_parse_ok);

    auto after = read_file_identity(original);
    ASSERT_TRUE(after);
    EXPECT_EQ(after.value().size_bytes, before.value().size_bytes);
    EXPECT_EQ(after.value().mtime_unix_ms, before.value().mtime_unix_ms);
    const auto after_sha = sha256_file_hex(original);
    ASSERT_TRUE(after_sha);
    EXPECT_EQ(after_sha.value(), before_sha.value());
}

TEST_F(CatalogServiceTest, XmpAdjacentMetadataOnlySidecarImport)
{
    ASSERT_TRUE(open_service(true));
    const auto local_png = (root / "meta-only.png").string();
    std::error_code copy_error;
    std::filesystem::copy_file(png_fixture_path(), local_png,
                               std::filesystem::copy_options::overwrite_existing, copy_error);
    ASSERT_FALSE(copy_error);
    auto imported = service->import_one(local_png, CancellationToken{});
    ASSERT_TRUE(imported);
    const auto asset_id = imported.value().asset->id;
    auto status0 = service->xmp_interchange_status(asset_id);
    ASSERT_TRUE(status0);
    const auto original = status0.value().original_path;

    const std::string meta_only =
        "<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
        " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
        "  <rdf:Description rdf:about=\"\"\n"
        "    xmlns:dc=\"http://purl.org/dc/elements/1.1/\"\n"
        "    xmlns:photoshop=\"http://ns.adobe.com/photoshop/1.0/\"\n"
        "    photoshop:City=\"Kyoto\">\n"
        "   <dc:title>\n    <rdf:Alt>\n     <rdf:li xml:lang=\"x-default\">Meta Only</rdf:li>\n"
        "    </rdf:Alt>\n   </dc:title>\n"
        "   <dc:subject>\n    <rdf:Bag>\n     <rdf:li>Temple</rdf:li>\n"
        "    </rdf:Bag>\n   </dc:subject>\n"
        "  </rdf:Description>\n"
        " </rdf:RDF>\n"
        "</x:xmpmeta>\n"
        "<?xpacket end=\"w\"?>\n";
    ASSERT_FALSE(write_adjacent_xmp(original, meta_only).empty());

    auto status = service->xmp_interchange_status(asset_id);
    ASSERT_TRUE(status) << status.error().message;
    EXPECT_FALSE(status.value().crs_parse_ok);
    EXPECT_TRUE(status.value().metadata_parse_ok);
    EXPECT_EQ(status.value().conflict_class, XmpInterchangeConflictClass::kSidecarNewer);

    auto applied = service->xmp_interchange_import(asset_id, XmpInterchangeResolve::kSidecar);
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_FALSE(applied.value().applied_crs);
    EXPECT_TRUE(applied.value().applied_metadata);
    EXPECT_TRUE(applied.value().applied_keywords);
    ASSERT_TRUE(applied.value().asset.metadata.title);
    EXPECT_EQ(*applied.value().asset.metadata.title, "Meta Only");
    ASSERT_TRUE(applied.value().asset.metadata.city);
    EXPECT_EQ(*applied.value().asset.metadata.city, "Kyoto");
    ASSERT_EQ(applied.value().asset.tags.size(), 1U);
    EXPECT_EQ(applied.value().asset.tags.front(), "Temple");
}

} // namespace
} // namespace ravo
