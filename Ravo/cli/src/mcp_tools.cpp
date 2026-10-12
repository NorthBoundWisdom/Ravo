#include "mcp_internal.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <set>
#include "ravo/foundation/parse_number.h"

namespace ravo::cli_internal
{
namespace
{
JsonValue text_schema(std::size_t maximum = 256)
{
    return JsonValue::Object{{"type", "string"},
                             {"minLength", JsonValue::number("1")},
                             {"maxLength", JsonValue::number(std::to_string(maximum))}};
}
JsonValue integer_schema(std::uint64_t minimum, std::uint64_t maximum)
{
    return JsonValue::Object{{"type", "integer"},
                             {"minimum", JsonValue::number(std::to_string(minimum))},
                             {"maximum", JsonValue::number(std::to_string(maximum))}};
}
JsonValue object_schema(JsonValue::Object properties, JsonValue::Array required = {})
{
    return JsonValue::Object{{"type", "object"},
                             {"properties", std::move(properties)},
                             {"required", std::move(required)},
                             {"additionalProperties", false}};
}
JsonValue binding_schema()
{
    JsonValue::Object properties{{"session_id", text_schema(128)}, {"asset_id", text_schema(256)}};
    JsonValue::Array required{"session_id", "asset_id"};
    for (const char *name : {"expected_session_revision", "expected_selection_revision",
                             "expected_recipe_revision", "expected_catalog_revision"})
    {
        properties.emplace(name, integer_schema(0, 9007199254740991ULL));
        required.emplace_back(name);
    }
    return object_schema(std::move(properties), std::move(required));
}
JsonValue fields_schema()
{
    auto value = JsonValue::Object{{"type", JsonValue::Array{"number", "string"}},
                                   {"maxLength", JsonValue::number("65536")}};
    return JsonValue::Object{
        {"type", "array"},
        {"minItems", JsonValue::number("1")},
        {"maxItems", JsonValue::number("256")},
        {"items", object_schema({{"name", text_schema(256)}, {"value", std::move(value)}},
                                {"name", "value"})}};
}
JsonValue candidate_schema()
{
    auto base = binding_schema();
    auto properties = *base.find("properties")->object_if();
    auto required = *base.find("required")->array_if();
    properties.emplace("kind", JsonValue::Object{{"const", "patch"}});
    properties.emplace("fields", fields_schema());
    properties.emplace("local_id", text_schema(256));
    for (const char *name : {"base_recipe_sha256", "target_recipe_sha256"})
    {
        properties.emplace(name, JsonValue::Object{{"type", "string"},
                                                   {"minLength", JsonValue::number("64")},
                                                   {"maxLength", JsonValue::number("64")}});
        required.emplace_back(name);
    }
    required.emplace_back("kind");
    required.emplace_back("fields");
    return object_schema(
        {{"type", JsonValue::Object{{"const", "ravo.develop.candidate"}}},
         {"version", JsonValue::Object{{"const", JsonValue::number("1")}}},
         {"session_id", text_schema(128)},
         {"asset_id", text_schema(256)},
         {"commit_request", object_schema(std::move(properties), std::move(required))}},
        {"type", "version", "session_id", "asset_id", "commit_request"});
}
struct Tool
{
    std::string name;
    std::string description;
    JsonValue schema;
    bool read_only = true;
};
const std::vector<Tool> &tools()
{
    static const auto list = []
    {
        const auto session = text_schema(128);
        const auto wait = JsonValue{JsonValue::Object{{"type", "boolean"}}};
        const auto annotations = cli_annotation_capabilities();
        const auto &annotation_kinds = *annotations.find("kinds");
        std::vector<Tool> result{
            {"ravo.review.apply",
             "Set rating/color_label/pick/reject/unflag on the explicit primary asset without auto-advance. Requires fresh saved global binding. Preserve operation_id; committed results may contain recovery warnings.",
             object_schema({{"binding", binding_schema()},
                            {"fields", *annotation_kinds.find("review")},
                            {"operation_id", text_schema(200)},
                            {"wait", wait}},
                           {"binding", "fields"}),
             false},
            {"ravo.metadata.patch",
             "Patch explicit catalog-owned IPTC fields on the primary asset. Null clears a field; omitted fields stay unchanged. Writes no original/XMP or Develop history. Read state.selection.annotations first and preserve operation_id.",
             object_schema({{"binding", binding_schema()},
                            {"fields", *annotation_kinds.find("metadata")},
                            {"operation_id", text_schema(200)},
                            {"wait", wait}},
                           {"binding", "fields"}),
             false},
            {"ravo.keywords.replace",
             "Replace the explicit primary asset's complete hierarchical keyword path set. Empty array explicitly clears membership, not keyword-tree definitions. Read current annotations first; preserve operation_id and inspect recovery warnings.",
             object_schema({{"binding", binding_schema()},
                            {"keywords", *annotation_kinds.find("keywords")},
                            {"operation_id", text_schema(200)},
                            {"wait", wait}},
                           {"binding", "keywords"}),
             false},
            {"ravo.catalog.query",
             "Read a bounded catalog page through the shared CLI/service. Explicit catalog path; edit and serialize the returned query object as query_json (strict complete versioned Ravo query document). Pass expected_revision on subsequent pages to reject catalog changes.",
             object_schema({{"catalog", text_schema(4096)},
                            {"query_json", text_schema(65536)},
                            {"set_id", text_schema(256)},
                            {"stack_expanded", JsonValue::Object{{"type", "boolean"}}},
                            {"offset", integer_schema(0, 9007199254740991ULL)},
                            {"limit", integer_schema(1, 512)},
                            {"expected_revision", integer_schema(0, 9007199254740991ULL)}},
                           {"catalog"})},
            {"ravo.sessions.list",
             "Discover live Studio sessions. Choose an explicit session; never guess the foreground photo.",
             object_schema({{"workspace_root", text_schema(4096)}})},
            {"ravo.studio.state",
             "Read current/saved recipes, binding, editing scope, busy state and a bounded selection page.",
             object_schema({{"session_id", session},
                            {"offset", integer_schema(0, 9007199254740991ULL)},
                            {"limit", integer_schema(0, 512)}},
                           {"session_id"})},
            {"ravo.studio.select",
             "Select one explicit catalog asset through Studio's command owner. Requires a fresh current binding and saved global editing context. Receipt is ephemeral; read fresh non-busy state before editing. Reuse the reserved operation_id to reconcile, never replay an unknown selection.",
             object_schema({{"binding", binding_schema()},
                            {"target_asset_id", text_schema(256)},
                            {"operation_id", text_schema(200)},
                            {"wait", wait}},
                           {"binding", "target_asset_id"}),
             false},
            {"ravo.develop.fields",
             "Recipe-owned numeric/text field kinds and bounds. Values use strict native field units; applicability is validated by evaluate.",
             object_schema({})},
            {"ravo.preview.read",
             "Read the exact binding's current image without mutation. Returns verified PNG pixels and an immutable resource URI.",
             object_schema({{"binding", binding_schema()}, {"max_edge", integer_schema(1, 1600)}},
                           {"binding"})},
            {"ravo.develop.evaluate",
             "Read-only trial against the exact current recipe. Changes no draft, recipe, history or preview record. Return a candidate for commit.",
             object_schema({{"binding", binding_schema()},
                            {"fields", fields_schema()},
                            {"local_id", text_schema(256)},
                            {"max_edge", integer_schema(1, 1600)}},
                           {"binding", "fields"})},
            {"ravo.develop.commit",
             "Commit an evaluated candidate once. Reuse the same reserved operation_id to recover an interrupted response; never replay an unknown write with a new binding.",
             object_schema({{"session_id", session},
                            {"candidate", candidate_schema()},
                            {"operation_id", text_schema(200)},
                            {"wait", wait}},
                           {"session_id", "candidate"}),
             false},
            {"ravo.operations.reserve",
             "Reserve a session-local idempotency ID before a write. Expired/unknown IDs cannot start a write.",
             object_schema({{"session_id", session}}, {"session_id"}), false},
            {"ravo.operations.get",
             "Read the durable-commit outcome of an operation, including partial batch results. Missing receipt means unknown; do not auto-replay.",
             object_schema(
                 {{"session_id", session}, {"operation_id", text_schema(200)}, {"wait", wait}},
                 {"session_id", "operation_id"})},
            {"ravo.operations.cancel",
             "Request cancellation before commit. A committed operation remains committed and its receipt is returned unchanged.",
             object_schema(
                 {{"session_id", session}, {"operation_id", text_schema(200)}, {"wait", wait}},
                 {"session_id", "operation_id"}),
             false},
            {"ravo.history.list",
             "List bounded history entries for an explicit selected photo. Restore uses the history ID and a fresh binding.",
             object_schema({{"binding", binding_schema()},
                            {"offset", integer_schema(0, 9007199254740991ULL)},
                            {"limit", integer_schema(1, 200)}},
                           {"binding"})},
            {"ravo.history.restore",
             "Restore an explicit history entry through Studio's existing command owner. Zero restores the product baseline.",
             object_schema({{"binding", binding_schema()},
                            {"history_id", integer_schema(0, 2147483647)},
                            {"operation_id", text_schema(200)},
                            {"wait", wait}},
                           {"binding", "history_id"}),
             false},
            {"ravo.mask.apply",
             "Apply an explicit local-mask action through Studio: create/select/set/component/rename/enable/invert/duplicate/delete/draw/done or gesture_begin/gesture_update/gesture_end/gesture_cancel. Read fields.mask_kinds and state.editing_scope handles/token. Coordinates are normalized to the developed image [0,1]. Staged is not a saved commit.",
             object_schema({{"binding", binding_schema()},
                            {"action", text_schema(64)},
                            {"arguments", JsonValue::Object{{"type", "object"}}},
                            {"operation_id", text_schema(200)},
                            {"wait", wait}},
                           {"binding", "action", "arguments"}),
             false},
            {"ravo.develop.batch",
             "Apply one strict global field patch to up to 256 explicitly confirmed selected photos. Per-photo commits are independent; inspect partial/cancelled results.",
             object_schema(
                 {{"binding", binding_schema()},
                  {"fields", fields_schema()},
                  {"asset_ids", JsonValue::Object{{"type", "array"},
                                                  {"items", text_schema(256)},
                                                  {"minItems", JsonValue::number("1")},
                                                  {"maxItems", JsonValue::number("256")}}},
                  {"operation_id", text_schema(200)},
                  {"wait", wait}},
                 {"binding", "fields", "asset_ids"}),
             false}};
        result.push_back(
            {"ravo.annotations.read",
             "Read rating, flags, writable metadata and hierarchical keywords for 1-32 explicit catalog assets at one expected revision. Read these states before a batch. Unavailable projections are explicit, never empty facts.",
             object_schema(
                 {{"catalog", text_schema(4096)},
                  {"expected_revision", integer_schema(0, 9007199254740991ULL)},
                  {"asset_ids",
                   JsonValue::Object{{"type", "array"},
                                     {"items", text_schema(256)},
                                     {"minItems", JsonValue::number("1")},
                                     {"maxItems", *annotations.find("batch")->find("max_items")}}}},
                 {"catalog", "expected_revision", "asset_ids"})});
        for (const auto kind : {"review", "metadata", "keywords"})
        {
            const auto values = std::string_view(kind) == "keywords" ? "keywords" : "fields";
            const auto name = std::string_view(kind) == "review"   ? "ravo.review.apply_batch" :
                              std::string_view(kind) == "metadata" ? "ravo.metadata.patch_batch" :
                                                                     "ravo.keywords.replace_batch";
            result.push_back(
                {name,
                 "Apply per-item edits to 1-32 explicit unique assets after annotations.read. Total payload is 64 KiB. Preflight all targets, then commit independently in order; stop on first error/cancel and inspect every item. Requires saved global binding. Preserve operation_id and reconcile unknown outcomes; no original/XMP/history writes.",
                 object_schema(
                     {{"binding", binding_schema()},
                      {"items",
                       JsonValue::Object{
                           {"type", "array"},
                           {"minItems", JsonValue::number("1")},
                           {"maxItems", *annotations.find("batch")->find("max_items")},
                           {"items", object_schema({{"asset_id", text_schema(256)},
                                                    {values, *annotation_kinds.find(kind)}},
                                                   {"asset_id", values})}}},
                      {"operation_id", text_schema(200)},
                      {"wait", wait}},
                     {"binding", "items"}),
                 false});
        }
        // The deadline is a transport bound, never a business-state override.
        for (auto &tool : result)
        {
            auto schema = *tool.schema.object_if();
            auto properties = *schema.at("properties").object_if();
            properties.emplace("timeout_ms", integer_schema(100, 120000));
            schema["properties"] = JsonValue{std::move(properties)};
            tool.schema = JsonValue{std::move(schema)};
        }
        std::sort(result.begin(), result.end(),
                  [](const auto &a, const auto &b) { return a.name < b.name; });
        return result;
    }();
    return list;
}
Result<void> validate(const JsonValue &value, const JsonValue &schema, const std::string &location)
{
    const auto fail = [&]() -> Result<void>
    {
        return make_error(ErrorCode::kInvalidArgument, "Tool argument does not match its schema",
                          {{"location", location}});
    };
    const auto *type = schema.find("type");
    if (const auto *constant = schema.find("const");
        constant && serialize_json(value) != serialize_json(*constant))
        return fail();
    if (const auto *enumeration = schema.find("enum");
        enumeration && enumeration->array_if() &&
        std::none_of(enumeration->array_if()->begin(), enumeration->array_if()->end(),
                     [&](const auto &item)
                     { return serialize_json(item) == serialize_json(value); }))
        return fail();
    auto matches = [&](std::string_view name)
    {
        if (name == "null")
            return value.is_null();
        if (name == "object")
            return value.object_if() != nullptr;
        if (name == "array")
            return value.array_if() != nullptr;
        if (name == "string")
            return value.string_if() != nullptr;
        if (name == "boolean")
            return value.boolean_if() != nullptr;
        if (name == "number")
        {
            double number = 0;
            return value.number_if() && parse_ascii_double(value.number_if()->text, number);
        }
        if (name == "integer")
        {
            if (!value.number_if())
                return false;
            std::uint64_t number = 0;
            const auto &text = value.number_if()->text;
            auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
            return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
        }
        return false;
    };
    if (type)
    {
        if (type->string_if() && !matches(*type->string_if()))
            return fail();
        if (type->array_if() &&
            std::none_of(type->array_if()->begin(), type->array_if()->end(), [&](const auto &item)
                         { return item.string_if() && matches(*item.string_if()); }))
            return fail();
    }
    auto bound = [&](std::string_view field, double actual, bool minimum) -> bool
    {
        const auto *entry = schema.find(field);
        if (!entry || !entry->number_if())
            return true;
        double number = 0;
        return parse_ascii_double(entry->number_if()->text, number) &&
               (minimum ? actual >= number : actual <= number);
    };
    if (const auto *text = value.string_if())
        if (!bound("minLength", static_cast<double>(text->size()), true) ||
            !bound("maxLength", static_cast<double>(text->size()), false))
            return fail();
    if (const auto *number = value.number_if())
    {
        double parsed = 0;
        if (!parse_ascii_double(number->text, parsed) || !bound("minimum", parsed, true) ||
            !bound("maximum", parsed, false))
            return fail();
    }
    if (const auto *array = value.array_if())
    {
        if (!bound("minItems", static_cast<double>(array->size()), true) ||
            !bound("maxItems", static_cast<double>(array->size()), false))
            return fail();
        if (const auto *item_schema = schema.find("items"))
            for (std::size_t index = 0; index < array->size(); ++index)
            {
                auto valid = validate((*array)[index], *item_schema,
                                      location + "[" + std::to_string(index) + "]");
                if (!valid)
                    return valid.error();
            }
    }
    if (const auto *object = value.object_if())
    {
        if (!bound("minProperties", static_cast<double>(object->size()), true) ||
            !bound("maxProperties", static_cast<double>(object->size()), false))
            return fail();
        if (const auto *required = schema.find("required"); required && required->array_if())
            for (const auto &name : *required->array_if())
                if (name.string_if() && !object->contains(*name.string_if()))
                    return fail();
        const auto *properties = schema.find("properties");
        const auto *additional = schema.find("additionalProperties");
        for (const auto &[name, item] : *object)
        {
            const auto *property = properties ? properties->find(name) : nullptr;
            if (!property)
            {
                if (additional && additional->boolean_if() && !*additional->boolean_if())
                    return fail();
                continue;
            }
            auto valid = validate(item, *property, location + "." + name);
            if (!valid)
                return valid.error();
        }
    }
    return {};
}
QString text(const JsonValue &object, std::string_view name)
{
    const auto *value = object.find(name);
    if (!value)
        return {};
    if (value->string_if())
        return QString::fromStdString(*value->string_if());
    if (value->number_if())
        return QString::fromStdString(value->number_if()->text);
    if (value->boolean_if())
        return *value->boolean_if() ? QStringLiteral("true") : QStringLiteral("false");
    return {};
}
} // namespace

JsonValue mcp_tool_definitions()
{
    JsonValue::Array result;
    for (const auto &tool : tools())
        result.push_back(JsonValue::Object{
            {"name", tool.name},
            {"description", tool.description},
            {"inputSchema", tool.schema},
            {"outputSchema",
             JsonValue::Object{
                 {"type", "object"},
                 {"required", JsonValue::Array{"type", "version", "tool"}},
                 {"properties",
                  JsonValue::Object{
                      {"type", JsonValue::Object{{"const", "ravo.mcp.tool_result"}}},
                      {"version", JsonValue::Object{{"const", JsonValue::number("1")}}},
                      {"tool", JsonValue::Object{{"type", "string"}}},
                      {"data", JsonValue::Object{{"type", "object"}}},
                      {"error", JsonValue::Object{{"type", "object"}}}}},
                 {"oneOf",
                  JsonValue::Array{JsonValue::Object{{"required", JsonValue::Array{"data"}}},
                                   JsonValue::Object{{"required", JsonValue::Array{"error"}}}}}}},
            {"annotations",
             JsonValue::Object{
                 {"readOnlyHint", tool.read_only},
                 {"destructiveHint", false},
                 {"idempotentHint", tool.read_only || tool.name == "ravo.operations.cancel"},
                 {"openWorldHint", false}}}});
    return JsonValue{std::move(result)};
}

Result<McpInvocation> mcp_invocation(const std::string_view name, const JsonValue &arguments,
                                     const QString &request_path, const QString &image_path)
{
    const auto found = std::find_if(tools().begin(), tools().end(),
                                    [&](const auto &tool) { return tool.name == name; });
    if (found == tools().end())
        return make_error(ErrorCode::kNotFound, "Unknown MCP tool");
    auto valid = validate(arguments, found->schema, "arguments");
    if (!valid)
        return valid.error();
    McpInvocation result;
    if (const auto *timeout = arguments.find("timeout_ms"))
    {
        const auto &value = timeout->number_if()->text;
        auto parsed = std::from_chars(value.data(), value.data() + value.size(), result.timeout_ms);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
            return make_error(ErrorCode::kInvalidArgument, "Tool timeout is invalid");
    }
    const auto *binding = arguments.find("binding");
    result.session_id = binding ? text(*binding, "session_id").toStdString() :
                                  text(arguments, "session_id").toStdString();
    result.operation_id = text(arguments, "operation_id").toStdString();
    if (name == "ravo.annotations.read")
    {
        result.arguments = {"catalog",    "annotations",
                            "--catalog",  text(arguments, "catalog"),
                            "--revision", text(arguments, "expected_revision")};
        for (const auto &id : *arguments.find("asset_ids")->array_if())
            result.arguments << "--asset-id" << QString::fromStdString(*id.string_if());
    }
    else if (name == "ravo.catalog.query")
    {
        result.arguments = {
            "catalog",
            "list",
            "--catalog",
            text(arguments, "catalog"),
            "--limit",
            arguments.find("limit") ? text(arguments, "limit") : QStringLiteral("100")};
        for (const auto pair :
             {std::pair{"offset", "--offset"}, std::pair{"query_json", "--query"},
              std::pair{"set_id", "--set-id"}, std::pair{"expected_revision", "--revision"}})
            if (arguments.find(pair.first))
                result.arguments << pair.second << text(arguments, pair.first);
        if (const auto *expanded = arguments.find("stack_expanded");
            expanded && *expanded->boolean_if())
            result.arguments << "--stack-expanded";
    }
    else if (name == "ravo.sessions.list")
    {
        result.arguments = {"studio", "sessions"};
        if (arguments.find("workspace_root"))
            result.arguments << "--workspace-root" << text(arguments, "workspace_root");
    }
    else if (name == "ravo.develop.fields")
        result.arguments = {"studio", "capabilities"};
    else if (name == "ravo.studio.state")
    {
        result.arguments = {"studio", "state"};
        for (const char *field : {"offset", "limit"})
            if (arguments.find(field))
                result.arguments << (QStringLiteral("--") + field) << text(arguments, field);
    }
    else if (name.starts_with("ravo.operations."))
    {
        result.arguments = {
            "studio", "operation", "--action",
            QString::fromUtf8(
                name.substr(std::string_view("ravo.operations.").size()).data(),
                static_cast<qsizetype>(name.size() - std::string_view("ravo.operations.").size()))};
        if (!result.operation_id.empty())
            result.arguments << "--operation-id" << QString::fromStdString(result.operation_id);
        if (arguments.find("wait"))
            result.arguments << "--wait" << text(arguments, "wait");
    }
    else if (name == "ravo.preview.read")
    {
        result.image = true;
        result.arguments = {"studio", "preview", "--asset-id", text(*binding, "asset_id")};
        for (const auto pair :
             {std::pair{"expected_session_revision", "--expect-session-revision"},
              std::pair{"expected_selection_revision", "--expect-selection-revision"},
              std::pair{"expected_recipe_revision", "--expect-recipe-revision"},
              std::pair{"expected_catalog_revision", "--expect-catalog-revision"}})
            result.arguments << pair.second << text(*binding, pair.first);
    }
    else if (name == "ravo.develop.evaluate")
    {
        result.image = true;
        auto request = *binding->object_if();
        request.emplace("fields", *arguments.find("fields"));
        if (arguments.find("local_id"))
            request.emplace("local_id", *arguments.find("local_id"));
        result.request = JsonValue{std::move(request)};
        result.arguments = {"studio", "evaluate", "--request", request_path};
    }
    else if (name == "ravo.history.list")
    {
        result.arguments = {"studio",
                            "history",
                            "--asset-id",
                            text(*binding, "asset_id"),
                            "--expect-selection-revision",
                            text(*binding, "expected_selection_revision")};
        result.arguments << "--expect-session-revision"
                         << text(*binding, "expected_session_revision")
                         << "--expect-recipe-revision" << text(*binding, "expected_recipe_revision")
                         << "--expect-catalog-revision"
                         << text(*binding, "expected_catalog_revision");
        for (const char *field : {"offset", "limit"})
            if (arguments.find(field))
                result.arguments << (QStringLiteral("--") + field) << text(arguments, field);
    }
    else
    {
        result.write = true;
        JsonValue request;
        if (name == "ravo.develop.commit")
            request = *arguments.find("candidate");
        else
        {
            auto body = *binding->object_if();
            if (name == "ravo.review.apply_batch" || name == "ravo.metadata.patch_batch" ||
                name == "ravo.keywords.replace_batch")
            {
                const auto kind = name == "ravo.review.apply_batch"   ? "review" :
                                  name == "ravo.metadata.patch_batch" ? "metadata" :
                                                                        "keywords";
                const auto values = name == "ravo.keywords.replace_batch" ? "keywords" : "fields";
                JsonValue::Array items;
                for (const auto &item : *arguments.find("items")->array_if())
                    items.push_back(JsonValue::Object{
                        {"asset_id", *item.find("asset_id")},
                        {"annotation", JsonValue::Object{{"version", JsonValue::number("1")},
                                                         {"kind", kind},
                                                         {"fields", *item.find(values)}}}});
                body.emplace("kind", "annotation_batch");
                body.emplace("items", std::move(items));
            }
            else if (name == "ravo.review.apply" || name == "ravo.metadata.patch" ||
                     name == "ravo.keywords.replace")
            {
                const auto kind = name == "ravo.review.apply"   ? "review" :
                                  name == "ravo.metadata.patch" ? "metadata" :
                                                                  "keywords";
                body.emplace("kind", "annotation");
                body.emplace(
                    "annotation",
                    JsonValue::Object{
                        {"version", JsonValue::number("1")},
                        {"kind", kind},
                        {"fields", *arguments.find(name == "ravo.keywords.replace" ? "keywords" :
                                                                                     "fields")}});
            }
            else if (name == "ravo.studio.select")
            {
                body.emplace("kind", "select");
                body.emplace("target_asset_id", *arguments.find("target_asset_id"));
            }
            else if (name == "ravo.history.restore")
            {
                body.emplace("kind", "restore");
                body.emplace("history_id", *arguments.find("history_id"));
            }
            else if (name == "ravo.mask.apply")
            {
                body.emplace("kind", "mask");
                body.emplace("action", *arguments.find("action"));
                body.emplace("arguments", *arguments.find("arguments"));
            }
            else
            {
                body.emplace("kind", "batch");
                body.emplace("fields", *arguments.find("fields"));
                body.emplace("asset_ids", *arguments.find("asset_ids"));
            }
            request = JsonValue{std::move(body)};
        }
        result.request = std::move(request);
        result.arguments = {
            "studio",    "commit",
            "--request", request_path,
            "--wait",    arguments.find("wait") ? text(arguments, "wait") : QStringLiteral("true")};
    }
    if (result.image)
        result.arguments << "--output" << image_path << "--max-edge"
                         << (arguments.find("max_edge") ? text(arguments, "max_edge") :
                                                          QStringLiteral("960"));
    if (!result.session_id.empty())
        result.arguments << "--session-id" << QString::fromStdString(result.session_id);
    if (name != "ravo.catalog.query" && name != "ravo.annotations.read")
        result.arguments << "--timeout-ms" << QString::number(result.timeout_ms);
    result.arguments << "--json";
    return result;
}
} // namespace ravo::cli_internal
