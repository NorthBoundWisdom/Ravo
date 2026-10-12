#include "ravo/services/asset_annotation.h"
#include "ravo/services/catalog_service.h"

#include <array>
#include <charconv>
#include <set>
#include <exception>
#include <new>

namespace ravo
{
namespace
{
using MetadataField = std::pair<std::string_view, std::optional<std::string> WritableMetadata::*>;
constexpr std::array<MetadataField, 14> fields{
    {{"title", &WritableMetadata::title},
     {"description", &WritableMetadata::description},
     {"creator", &WritableMetadata::creator},
     {"copyright", &WritableMetadata::copyright},
     {"country", &WritableMetadata::country},
     {"province_state", &WritableMetadata::province_state},
     {"city", &WritableMetadata::city},
     {"sublocation", &WritableMetadata::sublocation},
     {"headline", &WritableMetadata::headline},
     {"credit", &WritableMetadata::credit},
     {"source", &WritableMetadata::source},
     {"instructions", &WritableMetadata::instructions},
     {"usage_terms", &WritableMetadata::usage_terms},
     {"job_id", &WritableMetadata::job_id}}};
JsonValue closed_object(JsonValue::Object properties)
{
    return JsonValue::Object{{"type", "object"},
                             {"properties", std::move(properties)},
                             {"additionalProperties", false},
                             {"minProperties", JsonValue::number("1")}};
}
TaskError invalid(std::string field)
{
    return make_error(ErrorCode::kInvalidArgument, "Invalid annotation field or value",
                      {{"field", std::move(field)}});
}
JsonValue error_json(const TaskError &error)
{
    JsonValue::Object context;
    for (const auto &[key, value] : error.context)
        context.emplace(key, value);
    return JsonValue::Object{{"code", std::string(error_code_name(error.code))},
                             {"message", error.message},
                             {"context", std::move(context)}};
}
} // namespace

JsonValue asset_annotation_fields_schema(const AssetAnnotationKind kind)
{
    if (kind == AssetAnnotationKind::kReview)
        return closed_object(
            {{"rating", JsonValue::Object{{"type", "integer"},
                                          {"minimum", JsonValue::number("0")},
                                          {"maximum", JsonValue::number("5")}}},
             {"color_label",
              JsonValue::Object{
                  {"type", "string"},
                  {"enum", JsonValue::Array{"none", "red", "yellow", "green", "blue", "purple"}}}},
             {"flag", JsonValue::Object{{"type", "string"},
                                        {"enum", JsonValue::Array{"pick", "reject", "unflag"}}}}});
    if (kind == AssetAnnotationKind::kMetadata)
    {
        JsonValue::Object properties;
        for (const auto &[name, member] : fields)
            properties.emplace(name, JsonValue::Object{{"type", JsonValue::Array{"string", "null"}},
                                                       {"maxLength", JsonValue::number("65536")}});
        return closed_object(std::move(properties));
    }
    return JsonValue::Object{
        {"type", "array"},
        {"maxItems", JsonValue::number("256")},
        {"items", JsonValue::Object{{"type", "string"},
                                    {"minLength", JsonValue::number("1")},
                                    {"maxLength", JsonValue::number("4096")}}}};
}

JsonValue asset_annotation_capabilities_json()
{
    return JsonValue::Object{
        {"schema", "ravo.asset.annotation-capabilities/v1"},
        {"version", JsonValue::number("1")},
        {"scope", "explicit_primary_asset"},
        {"keyword_action", "replace"},
        {"metadata_null", "clear"},
        {"max_payload_bytes", JsonValue::number("65536")},
        {"original_writes", false},
        {"max_state_bytes", JsonValue::number("1048576")},
        {"kinds",
         JsonValue::Object{
             {"review", asset_annotation_fields_schema(AssetAnnotationKind::kReview)},
             {"metadata", asset_annotation_fields_schema(AssetAnnotationKind::kMetadata)},
             {"keywords", asset_annotation_fields_schema(AssetAnnotationKind::kKeywords)}}}};
}

Result<AssetAnnotationEdit> parse_asset_annotation_edit(const JsonValue &payload)
try
{
    if (!payload.object_if() || payload.object_if()->size() != 3 ||
        serialize_json(payload).size() > 65536)
        return invalid("payload");
    const auto *version = payload.find("version"), *kind = payload.find("kind"),
               *values = payload.find("fields");
    if (!version || !version->number_if() || version->number_if()->text != "1" || !kind ||
        !kind->string_if() || !values)
        return invalid("version/kind/fields");
    AssetAnnotationEdit edit;
    if (*kind->string_if() == "keywords")
    {
        edit.kind = AssetAnnotationKind::kKeywords;
        if (!values->array_if() || values->array_if()->size() > 256)
            return invalid("keywords");
        std::set<std::string> seen;
        for (const auto &value : *values->array_if())
        {
            if (!value.string_if() || value.string_if()->size() > 4096)
                return invalid("keywords");
            auto parsed = parse_keyword_path(*value.string_if());
            if (!parsed)
                return parsed.error();
            auto canonical = join_keyword_path(parsed.value());
            if (!canonical)
                return canonical.error();
            if (!seen.insert(canonical.value()).second)
                return invalid("duplicate_keyword");
            edit.keywords.push_back(std::move(canonical).value());
        }
        return edit;
    }
    if (!values->object_if() || values->object_if()->empty())
        return invalid("fields");
    if (*kind->string_if() == "metadata")
    {
        edit.kind = AssetAnnotationKind::kMetadata;
        for (const auto &[name, value] : *values->object_if())
        {
            if (!value.is_null() && !value.string_if())
                return invalid(name);
            auto patch = writable_metadata_patch_for_field(
                name,
                value.is_null() ? std::nullopt : std::optional<std::string>{*value.string_if()},
                edit.metadata);
            if (!patch)
                return patch.error();
            edit.metadata = std::move(patch).value();
        }
        return edit;
    }
    if (*kind->string_if() != "review")
        return invalid("kind");
    for (const auto &[name, value] : *values->object_if())
    {
        if (name == "rating")
        {
            int rating = -1;
            if (!value.number_if())
                return invalid(name);
            const auto &text = value.number_if()->text;
            auto converted = std::from_chars(text.data(), text.data() + text.size(), rating);
            if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size() ||
                rating < 0 || rating > 5)
                return invalid(name);
            edit.review.rating = rating;
        }
        else if (name == "color_label")
        {
            if (!value.string_if())
                return invalid(name);
            auto label = parse_color_label(*value.string_if());
            if (!label)
                return label.error();
            edit.review.color_label = label.value();
        }
        else if (name == "flag")
        {
            if (!value.string_if())
                return invalid(name);
            const auto &flag = *value.string_if();
            if (flag != "pick" && flag != "reject" && flag != "unflag")
                return invalid(name);
            edit.review.picked = flag == "pick";
            edit.review.rejected = flag == "reject";
        }
        else
            return invalid(name);
    }
    return edit;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Unable to allocate annotation edit",
                      {{"reason", "annotation_allocation_failed"}, {"mutation_applied", "false"}});
}

JsonValue asset_annotation_state_json(const AssetRecord &asset)
{
    constexpr std::size_t maximum = 1024U * 1024U;
    std::size_t bytes = 2048;
    const auto fits = [&](const std::string &text)
    {
        if (text.size() > maximum - bytes)
            return false;
        for (const char byte : text)
        {
            const auto value = static_cast<unsigned char>(byte);
            const std::size_t cost = value < 0x20 ? 6 : value == '"' || value == '\\' ? 2 : 1;
            if (cost > maximum - bytes)
                return false;
            bytes += cost;
        }
        if (bytes > maximum - 3)
            return false;
        bytes += 3;
        return true;
    };
    bool bounded = true;
    for (const auto &[name, member] : fields)
        if (const auto &value = asset.metadata.*member; value && !fits(*value))
            bounded = false;
    for (const auto &tag : asset.tags)
        if (!fits(tag))
            bounded = false;
    if (!bounded)
        return JsonValue::Object{{"schema", "ravo.asset.annotations/v1"},
                                 {"asset_id", asset.id},
                                 {"available", false},
                                 {"reason", "annotation_state_budget_exceeded"}};
    JsonValue::Object metadata;
    for (const auto &[name, member] : fields)
    {
        const auto &value = asset.metadata.*member;
        metadata.emplace(name, value ? JsonValue{*value} : JsonValue{nullptr});
    }
    JsonValue::Array tags;
    for (const auto &tag : asset.tags)
        tags.emplace_back(tag);
    return JsonValue::Object{
        {"schema", "ravo.asset.annotations/v1"},
        {"asset_id", asset.id},
        {"available", true},
        {"review",
         JsonValue::Object{{"rating", JsonValue::number(std::to_string(asset.review.rating))},
                           {"color_label", std::string(color_label_name(asset.review.color_label))},
                           {"picked", asset.review.picked},
                           {"rejected", asset.review.rejected}}},
        {"metadata", std::move(metadata)},
        {"keywords", std::move(tags)}};
}

Result<AssetAnnotationResult> apply_asset_annotation(CatalogService &service,
                                                     const std::string_view asset_id,
                                                     const AssetAnnotationEdit &edit,
                                                     const std::int64_t expected_revision,
                                                     const CancellationToken &cancellation)
try
{
    if (asset_id.empty() || asset_id.size() > 256 || expected_revision < 0)
        return invalid("target/revision");
    if (auto active = cancellation.check(); !active)
        return active.error();
    if (edit.review.picked.value_or(false) && edit.review.rejected.value_or(false))
        return invalid("conflicting_review_flags");
    if (edit.kind == AssetAnnotationKind::kKeywords && edit.keywords.size() > 256)
        return invalid("keywords");
    AssetAnnotationResult result;
    result.asset_id = asset_id;
    std::optional<TaskError> failure;
    if (edit.kind == AssetAnnotationKind::kReview)
    {
        CullReviewRequest request;
        request.asset_id = asset_id;
        request.rating = edit.review.rating;
        request.color_label = edit.review.color_label;
        request.flag_action =
            edit.review.picked.value_or(false)         ? CullReviewFlagAction::kPick :
            edit.review.rejected.value_or(false)       ? CullReviewFlagAction::kReject :
            edit.review.picked || edit.review.rejected ? CullReviewFlagAction::kUnflag :
                                                         CullReviewFlagAction::kUnchanged;
        request.expected_catalog_revision = expected_revision;
        request.cancellation = cancellation;
        auto changed = service.cull().apply_cull_review(request);
        if (!changed)
            failure = changed.error();
        else
        {
            result.revision = changed.value().revision;
            result.asset = std::move(changed).value().asset;
        }
    }
    else if (edit.kind == AssetAnnotationKind::kMetadata)
    {
        auto changed = service.metadata().set_writable_metadata_selection(
            {std::string(asset_id)}, edit.metadata, expected_revision, cancellation);
        if (!changed)
            failure = changed.error();
        else
        {
            result.revision = changed.value().revision;
            result.asset = std::move(changed).value().assets.front();
        }
    }
    else if (edit.kind == AssetAnnotationKind::kKeywords)
    {
        auto changed = service.metadata().set_tags_selection({std::string(asset_id)}, edit.keywords,
                                                             expected_revision, cancellation);
        if (!changed)
            failure = changed.error();
        else
        {
            result.revision = changed.value().revision;
            result.asset = std::move(changed).value().assets.front();
        }
    }
    else
        return invalid("kind");
    if (failure)
    {
        const auto committed = failure->context.find("catalog_committed"),
                   revision = failure->context.find("committed_revision");
        if (committed == failure->context.end() || committed->second != "true")
            return *failure;
        if (revision == failure->context.end())
            return *failure;
        const auto &text = revision->second;
        auto parsed = std::from_chars(text.data(), text.data() + text.size(), result.revision);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
            result.revision < 0)
            return *failure;
        result.warnings.push_back(*failure);
        // Recovery can fail after the transaction. Recover a projection only
        // while its catalog revision is still the confirmed commit revision.
        auto before = service.library().snapshot();
        if (!before)
            result.warnings.push_back(before.error());
        else if (before.value().revision != result.revision)
            result.warnings.push_back(make_error(ErrorCode::kConflict,
                                                 "Annotation readback revision changed",
                                                 {{"reason", "annotation_readback_changed"}}));
        else
        {
            auto asset = service.library().find_asset(asset_id);
            auto after = service.library().snapshot();
            if (!asset)
                result.warnings.push_back(asset.error());
            if (!after)
                result.warnings.push_back(after.error());
            else if (after.value().revision != result.revision)
                result.warnings.push_back(make_error(ErrorCode::kConflict,
                                                     "Annotation readback revision changed",
                                                     {{"reason", "annotation_readback_changed"}}));
            else if (asset)
                result.asset = std::move(asset).value();
        }
    }
    return result;
}
catch (const std::exception &error)
{
    return make_error(ErrorCode::kIo, "Annotation execution raised an exception",
                      {{"reason", "annotation_execution_exception"},
                       {"mutation_applied", "unknown"},
                       {"detail", std::string(std::string_view(error.what()).substr(0, 4096))}});
}
catch (...)
{
    return make_error(
        ErrorCode::kIo, "Annotation execution raised an unknown exception",
        {{"reason", "annotation_execution_exception"}, {"mutation_applied", "unknown"}});
}

JsonValue asset_annotation_result_json(const AssetAnnotationResult &result)
{
    JsonValue::Array warnings;
    for (const auto &warning : result.warnings)
        warnings.push_back(error_json(warning));
    auto annotations =
        result.asset ? asset_annotation_state_json(*result.asset) : JsonValue{nullptr};
    const auto *available = annotations.find("available");
    const bool readback_available =
        available && available->boolean_if() && *available->boolean_if();
    return JsonValue::Object{
        {"schema", "ravo.asset.annotation-result/v1"},
        {"asset_id", result.asset_id},
        {"catalog_revision", JsonValue::number(std::to_string(result.revision))},
        {"catalog_committed", true},
        {"warnings", std::move(warnings)},
        {"readback_available", readback_available},
        {"annotations", std::move(annotations)}};
}
} // namespace ravo
