#include "application_internal.h"
#include "ravo/services/photo_merge.h"

namespace ravo::cli_internal
{
Result<JsonValue> run_catalog_merge_command(CatalogService &service,
                                            const std::string_view subcommand,
                                            const CatalogCliArguments &flags)
{
    if (!flags.expected_revision)
        return make_error(ErrorCode::kInvalidArgument,
                          "Photo merge requires --revision from catalog list",
                          {{"reason", "merge_revision_required"}});
    PhotoMergeRequest request;
    for (const auto id : flags.asset_ids)
        request.asset_ids.emplace_back(id);
    request.options.kind =
        subcommand == "hdr-merge" ? PhotoMergeKind::kHdr : PhotoMergeKind::kPanorama;
    request.options.auto_align = !flags.merge_no_align;
    request.options.auto_crop = !flags.merge_no_crop;
    request.options.exposure_ev = flags.merge_exposure_stops;
    if (flags.merge_deghost)
        request.options.deghost_threshold = *flags.merge_deghost;
    request.max_edge = flags.max_edge.value_or(0);
    request.output_path = std::string(flags.output);
    request.expected_catalog_revision = flags.expected_revision;
    auto merged = service.merge_selected_photos(request);
    if (!merged)
        return merged.error();
    const auto &result = merged.value();
    const auto &a = result.artifact;
    JsonValue::Array transforms;
    for (const auto &alignment : result.alignments)
    {
        JsonValue::Array matrix;
        for (const auto value : alignment.source_to_reference)
        {
            auto number = parameter_value_to_json(ParameterValue{value});
            if (!number)
                return number.error();
            matrix.push_back(std::move(number).value());
        }
        transforms.emplace_back(JsonValue::Object{
            {"source_to_reference", std::move(matrix)},
            {"inliers", JsonValue::number(std::to_string(alignment.inliers))},
            {"residual_pixels", JsonValue::number(std::to_string(alignment.residual_pixels))}});
    }
    return JsonValue{JsonValue::Object{
        {"schema", result.schema},
        {"version", JsonValue::number("1")},
        {"kind", result.kind == PhotoMergeKind::kHdr ? "hdr" : "panorama"},
        {"asset", asset_to_json(result.asset)},
        {"output", result.output_path},
        {"provenance", result.provenance_path},
        {"originals_unchanged", result.originals_unchanged},
        {"origin_x", JsonValue::number(std::to_string(result.origin_x))},
        {"origin_y", JsonValue::number(std::to_string(result.origin_y))},
        {"exposure_normalization_ev",
         JsonValue::number(std::to_string(result.exposure_normalization_ev))},
        {"deghosted_pixels", JsonValue::number(std::to_string(result.deghosted_pixels))},
        {"alignments", std::move(transforms)},
        {"artifact",
         JsonValue::Object{{"type", a.type},
                           {"version", JsonValue::number("1")},
                           {"path", result.output_path},
                           {"mime_type", a.mime_type},
                           {"width", JsonValue::number(std::to_string(a.width))},
                           {"height", JsonValue::number(std::to_string(a.height))},
                           {"color_profile", a.color_profile},
                           {"color_profile_fingerprint", a.color_profile_fingerprint},
                           {"byte_count", JsonValue::number(std::to_string(a.byte_count))},
                           {"content_sha256", a.content_sha256},
                           {"lifecycle", "persistent_derived_asset"}}}}};
}
} // namespace ravo::cli_internal
