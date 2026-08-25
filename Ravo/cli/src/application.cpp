#include "ravo/cli/application.h"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/legacy_xmp.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/adapters/text_file.h"
#include "ravo/foundation/json.h"
#include "ravo/recipe/develop.h"
#include "ravo/services/catalog_service.h"

namespace ravo
{

namespace
{

struct ParsedArguments
{
    bool json = false;
    std::vector<std::string_view> positional;
};

[[nodiscard]] Result<ParsedArguments>
parse_arguments(const std::span<const std::string_view> arguments)
{
    ParsedArguments parsed;
    for (const auto argument : arguments)
    {
        if (argument == "--json")
        {
            if (parsed.json)
            {
                return make_error(ErrorCode::kInvalidArgument, "--json can only be specified once");
            }
            parsed.json = true;
            continue;
        }
        if (argument == "--version")
        {
            parsed.positional.push_back(argument);
            continue;
        }
        parsed.positional.push_back(argument);
    }
    return parsed;
}

[[nodiscard]] JsonValue error_object(const TaskError &error)
{
    JsonValue::Object context;
    for (const auto &[key, value] : error.context)
    {
        context.emplace(key, value);
    }
    return JsonValue::Object{{"code", std::string(error_code_name(error.code))},
                             {"context", std::move(context)},
                             {"message", error.message}};
}

[[nodiscard]] JsonValue success_envelope(JsonValue data)
{
    return JsonValue::Object{{"data", std::move(data)},
                             {"diagnostics", JsonValue::Array{}},
                             {"ok", true},
                             {"type", "ravo.cli.result"},
                             {"version", JsonValue::number("1")}};
}

[[nodiscard]] JsonValue failure_envelope(const TaskError &error)
{
    return JsonValue::Object{{"diagnostics", JsonValue::Array{}},
                             {"error", error_object(error)},
                             {"ok", false},
                             {"type", "ravo.cli.result"},
                             {"version", JsonValue::number("1")}};
}

struct RenderCliArguments
{
    std::string_view input;
    std::string_view recipe;
    std::string_view output;
    std::string_view backend = "cpu";
    bool backend_specified = false;
    std::optional<std::uint32_t> width;
    std::optional<std::uint32_t> height;
};

[[nodiscard]] Result<std::uint32_t> parse_dimension(const std::string_view text,
                                                    const std::string_view option)
{
    std::uint32_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Render dimension must be a positive integer",
                          {{"option", std::string(option)}, {"value", std::string(text)}});
    }
    return value;
}

[[nodiscard]] Result<RenderCliArguments>
parse_render_arguments(const std::span<const std::string_view> positional)
{
    if (positional.size() < 2 || positional[0] != "render" || positional[1].starts_with("--"))
    {
        return make_error(
            ErrorCode::kInvalidArgument,
            "Usage: ravo render <input> --recipe <recipe> --output <png> [--backend cpu]");
    }

    RenderCliArguments result{positional[1], {}, {}, "cpu", false, std::nullopt, std::nullopt};
    for (std::size_t index = 2; index < positional.size(); ++index)
    {
        const auto option = positional[index];
        if (option != "--recipe" && option != "--output" && option != "--backend" &&
            option != "--width" && option != "--height")
        {
            return make_error(ErrorCode::kInvalidArgument, "Unknown render option",
                              {{"option", std::string(option)}});
        }
        if (index + 1 >= positional.size() || positional[index + 1].starts_with("--"))
        {
            return make_error(ErrorCode::kInvalidArgument, "Render option requires a value",
                              {{"option", std::string(option)}});
        }
        const auto value = positional[++index];
        if (option == "--recipe")
        {
            if (!result.recipe.empty())
            {
                return make_error(ErrorCode::kInvalidArgument, "Render recipe was specified twice");
            }
            result.recipe = value;
        }
        else if (option == "--output")
        {
            if (!result.output.empty())
            {
                return make_error(ErrorCode::kInvalidArgument, "Render output was specified twice");
            }
            result.output = value;
        }
        else if (option == "--backend")
        {
            if (result.backend_specified)
            {
                return make_error(ErrorCode::kInvalidArgument,
                                  "Render backend was specified twice");
            }
            result.backend = value;
            result.backend_specified = true;
        }
        else
        {
            auto dimension = parse_dimension(value, option);
            if (!dimension)
            {
                return dimension.error();
            }
            auto &target = option == "--width" ? result.width : result.height;
            if (target.has_value())
            {
                return make_error(ErrorCode::kInvalidArgument,
                                  "Render dimension was specified twice",
                                  {{"option", std::string(option)}});
            }
            target = dimension.value();
        }
    }
    if (result.recipe.empty() || result.output.empty())
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Render requires --recipe and --output options");
    }
    if (result.backend != "cpu")
    {
        return make_error(ErrorCode::kUnsupported, "Only the CPU render backend is available",
                          {{"backend", std::string(result.backend)}});
    }
    return result;
}

struct CatalogCliArguments
{
    std::string_view catalog;
    std::vector<std::string_view> inputs;
    std::string_view asset_id;
    std::optional<int> rating;
    std::optional<double> exposure_ev;
    std::optional<double> saturation;
    std::optional<double> contrast;
    std::optional<std::uint32_t> max_edge;
    std::vector<std::pair<std::string, double>> develop_sets;
    std::string_view output;
    std::string_view format;
    std::optional<int> quality;
};

[[nodiscard]] Result<int> parse_int_flag(const std::string_view text, const std::string_view option)
{
    int value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    {
        return make_error(ErrorCode::kInvalidArgument, "Option requires an integer",
                          {{"option", std::string(option)}, {"value", std::string(text)}});
    }
    return value;
}

[[nodiscard]] Result<double> parse_double_flag(const std::string_view text,
                                               const std::string_view option)
{
    const std::string owned(text);
    char *end = nullptr;
    const double value = std::strtod(owned.c_str(), &end);
    if (end != owned.c_str() + owned.size() || !std::isfinite(value))
    {
        return make_error(ErrorCode::kInvalidArgument, "Option requires a finite number",
                          {{"option", std::string(option)}, {"value", std::string(text)}});
    }
    return value;
}

[[nodiscard]] Result<CatalogCliArguments>
parse_catalog_flags(const std::span<const std::string_view> positional)
{
    CatalogCliArguments result;
    for (std::size_t index = 2; index < positional.size(); ++index)
    {
        const auto option = positional[index];
        if (index + 1 >= positional.size() || positional[index + 1].starts_with("--"))
        {
            return make_error(ErrorCode::kInvalidArgument, "Catalog option requires a value",
                              {{"option", std::string(option)}});
        }
        const auto value = positional[++index];
        if (option == "--path" || option == "--catalog")
        {
            if (!result.catalog.empty())
            {
                return make_error(ErrorCode::kInvalidArgument, "Catalog path was specified twice");
            }
            result.catalog = value;
        }
        else if (option == "--input")
        {
            result.inputs.push_back(value);
        }
        else if (option == "--asset-id")
        {
            if (!result.asset_id.empty())
            {
                return make_error(ErrorCode::kInvalidArgument, "Asset ID was specified twice");
            }
            result.asset_id = value;
        }
        else if (option == "--rating")
        {
            auto rating = parse_int_flag(value, option);
            if (!rating)
            {
                return rating.error();
            }
            result.rating = rating.value();
        }
        else if (option == "--exposure-ev")
        {
            auto parsed = parse_double_flag(value, option);
            if (!parsed)
            {
                return parsed.error();
            }
            result.exposure_ev = parsed.value();
        }
        else if (option == "--saturation")
        {
            auto parsed = parse_double_flag(value, option);
            if (!parsed)
            {
                return parsed.error();
            }
            result.saturation = parsed.value();
        }
        else if (option == "--contrast")
        {
            auto parsed = parse_double_flag(value, option);
            if (!parsed)
            {
                return parsed.error();
            }
            result.contrast = parsed.value();
        }
        else if (option == "--set")
        {
            const auto owned = std::string(value);
            const auto split = owned.find('=');
            if (split == std::string::npos || split == 0 || split + 1 == owned.size())
            {
                return make_error(ErrorCode::kInvalidArgument, "--set requires name=value",
                                  {{"value", owned}});
            }
            auto parsed = parse_double_flag(owned.substr(split + 1), option);
            if (!parsed)
            {
                return parsed.error();
            }
            result.develop_sets.emplace_back(owned.substr(0, split), parsed.value());
        }
        else if (option == "--max-edge")
        {
            auto dimension = parse_dimension(value, option);
            if (!dimension)
            {
                return dimension.error();
            }
            result.max_edge = dimension.value();
        }
        else if (option == "--output")
        {
            if (!result.output.empty())
            {
                return make_error(ErrorCode::kInvalidArgument, "Output path was specified twice");
            }
            result.output = value;
        }
        else if (option == "--format")
        {
            if (!result.format.empty())
            {
                return make_error(ErrorCode::kInvalidArgument, "Export format was specified twice");
            }
            result.format = value;
        }
        else if (option == "--quality")
        {
            auto quality = parse_int_flag(value, option);
            if (!quality)
            {
                return quality.error();
            }
            result.quality = quality.value();
        }
        else
        {
            return make_error(ErrorCode::kInvalidArgument, "Unknown catalog option",
                              {{"option", std::string(option)}});
        }
    }
    return result;
}

[[nodiscard]] Result<std::unique_ptr<CatalogService>>
open_catalog_session(const EngineFacade &engine, const std::string_view path, const bool create)
{
    auto repository =
        create ? SqliteCatalogRepository::create(path) : SqliteCatalogRepository::open(path);
    if (!repository)
    {
        return repository.error();
    }
    auto cache = FilesystemPreviewCache::create(std::string(path) + ".preview");
    if (!cache)
    {
        return cache.error();
    }
    return std::make_unique<CatalogService>(engine, std::move(repository).value(),
                                            std::make_unique<QtRasterDecoder>(),
                                            std::move(cache).value());
}

[[nodiscard]] JsonValue asset_to_json(const AssetRecord &asset)
{
    return JsonValue::Object{
        {"color_label", std::string(color_label_name(asset.review.color_label))},
        {"has_edits", asset.has_edits},
        {"id", asset.id},
        {"import_state", asset.import_state},
        {"media_type", asset.media_type},
        {"rating", JsonValue::number(std::to_string(asset.review.rating))},
        {"rejected", asset.review.rejected},
        {"uri", asset.normalized_uri},
    };
}

[[nodiscard]] Result<JsonValue>
run_catalog_command(const EngineFacade &engine, const std::span<const std::string_view> positional)
{
    if (positional.size() < 2)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Usage: ravo catalog <create|import|list|preview|recipe|develop|rate|"
                          "export> --catalog <path>");
    }
    const auto subcommand = positional[1];
    auto flags = parse_catalog_flags(positional);
    if (!flags)
    {
        return flags.error();
    }
    if (flags.value().catalog.empty())
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Catalog commands require --catalog or --path");
    }

    if (subcommand == "create")
    {
        auto session = open_catalog_session(engine, flags.value().catalog, true);
        if (!session)
        {
            return session.error();
        }
        auto snapshot = session.value()->snapshot();
        if (!snapshot)
        {
            return snapshot.error();
        }
        return JsonValue{JsonValue::Object{
            {"catalog_id", snapshot.value().catalog_id},
            {"path", snapshot.value().database_path},
            {"schema_version", JsonValue::number(std::to_string(snapshot.value().schema_version))},
        }};
    }

    auto session = open_catalog_session(engine, flags.value().catalog, false);
    if (!session)
    {
        return session.error();
    }
    auto &service = *session.value();

    if (subcommand == "import")
    {
        if (flags.value().inputs.empty())
        {
            return make_error(ErrorCode::kInvalidArgument, "catalog import requires --input");
        }
        std::vector<std::string> inputs;
        inputs.reserve(flags.value().inputs.size());
        for (const auto input : flags.value().inputs)
        {
            inputs.emplace_back(input);
        }
        auto imported = service.import_inputs(inputs, CancellationToken{});
        if (!imported)
        {
            return imported.error();
        }
        JsonValue::Array items;
        int imported_count = 0;
        for (const auto &item : imported.value())
        {
            JsonValue::Object row{
                {"input", item.input_path},
                {"status",
                 std::string(item.status == ImportItemStatus::kImported    ? "imported" :
                             item.status == ImportItemStatus::kDuplicate   ? "duplicate" :
                             item.status == ImportItemStatus::kUnsupported ? "unsupported" :
                                                                             "failed")}};
            if (item.asset)
            {
                row.emplace("asset", asset_to_json(*item.asset));
            }
            if (item.status == ImportItemStatus::kImported)
            {
                ++imported_count;
            }
            items.emplace_back(std::move(row));
        }
        return JsonValue{JsonValue::Object{
            {"imported", JsonValue::number(std::to_string(imported_count))},
            {"items", std::move(items)},
        }};
    }
    if (subcommand == "list")
    {
        auto snapshot = service.snapshot();
        if (!snapshot)
        {
            return snapshot.error();
        }
        auto listed = service.list_assets();
        if (!listed)
        {
            return listed.error();
        }
        JsonValue::Array assets;
        for (const auto &asset : listed.value())
        {
            assets.push_back(asset_to_json(asset));
        }
        return JsonValue{JsonValue::Object{
            {"assets", std::move(assets)},
            {"catalog_id", snapshot.value().catalog_id},
            {"schema_version", JsonValue::number(std::to_string(snapshot.value().schema_version))},
        }};
    }
    if (subcommand == "preview")
    {
        if (flags.value().asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument, "catalog preview requires --asset-id");
        }
        PreviewRequest request;
        request.asset_id = std::string(flags.value().asset_id);
        request.max_edge = flags.value().max_edge.value_or(kDefaultPreviewMaxEdge);
        auto previewed = service.request_preview(request);
        if (!previewed)
        {
            return previewed.error();
        }
        return JsonValue{JsonValue::Object{
            {"asset_id", previewed.value().asset_id},
            {"cache_path", previewed.value().cache_path},
            {"height", JsonValue::number(std::to_string(previewed.value().height))},
            {"original_missing", previewed.value().original_missing},
            {"width", JsonValue::number(std::to_string(previewed.value().width))},
        }};
    }
    if (subcommand == "recipe")
    {
        if (flags.value().asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument, "catalog recipe requires --asset-id");
        }
        auto recipe = service.load_recipe(flags.value().asset_id);
        if (!recipe)
        {
            return recipe.error();
        }
        auto serialized = serialize_recipe(recipe.value());
        if (!serialized)
        {
            return serialized.error();
        }
        auto parsed = parse_json(serialized.value());
        if (!parsed)
        {
            return parsed.error();
        }
        auto has_edits = service.asset_has_edits(flags.value().asset_id);
        if (!has_edits)
        {
            return has_edits.error();
        }
        return JsonValue{JsonValue::Object{
            {"asset_id", std::string(flags.value().asset_id)},
            {"has_edits", has_edits.value()},
            {"recipe", std::move(parsed).value()},
        }};
    }
    if (subcommand == "develop")
    {
        if (flags.value().asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument, "catalog develop requires --asset-id");
        }
        auto loaded = service.load_recipe(flags.value().asset_id);
        if (!loaded)
        {
            return loaded.error();
        }
        auto params = develop_from_recipe(loaded.value());
        if (!params)
        {
            return params.error();
        }
        if (flags.value().exposure_ev)
        {
            params.value().exposure_ev = *flags.value().exposure_ev;
        }
        if (flags.value().saturation)
        {
            params.value().saturation = *flags.value().saturation;
        }
        if (flags.value().contrast)
        {
            params.value().contrast = *flags.value().contrast;
        }
        for (const auto &[name, value] : flags.value().develop_sets)
        {
            if (!apply_develop_field(params.value(), name, value))
            {
                return make_error(ErrorCode::kInvalidArgument, "Unknown develop field",
                                  {{"name", name}});
            }
        }
        clamp_develop(params.value());
        auto saved = service.save_develop(flags.value().asset_id, params.value());
        if (!saved)
        {
            return saved.error();
        }
        return asset_to_json(saved.value());
    }
    if (subcommand == "rate")
    {
        if (flags.value().asset_id.empty() || !flags.value().rating)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog rate requires --asset-id and --rating");
        }
        auto rated = service.set_rating(flags.value().asset_id, *flags.value().rating);
        if (!rated)
        {
            return rated.error();
        }
        return asset_to_json(rated.value());
    }
    if (subcommand == "export")
    {
        if (flags.value().asset_id.empty() || flags.value().output.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog export requires --asset-id and --output");
        }
        ExportRequest request;
        request.asset_id = std::string(flags.value().asset_id);
        request.output_path = std::string(flags.value().output);
        if (!flags.value().format.empty())
        {
            auto format = parse_export_format(flags.value().format);
            if (!format)
            {
                return format.error();
            }
            request.format = format.value();
        }
        if (flags.value().quality)
        {
            request.jpeg_quality = *flags.value().quality;
        }
        if (flags.value().max_edge)
        {
            request.max_edge = *flags.value().max_edge;
        }
        auto exported = service.export_asset(request);
        if (!exported)
        {
            return exported.error();
        }
        return JsonValue{JsonValue::Object{
            {"asset_id", exported.value().asset_id},
            {"bytes", JsonValue::number(std::to_string(exported.value().bytes_written))},
            {"format", std::string(export_format_name(exported.value().format))},
            {"height", JsonValue::number(std::to_string(exported.value().height))},
            {"output", exported.value().output_path},
            {"width", JsonValue::number(std::to_string(exported.value().width))},
        }};
    }
    return make_error(ErrorCode::kInvalidArgument, "Unknown catalog subcommand",
                      {{"subcommand", std::string(subcommand)}});
}

void write_human_error(std::ostream &stream, const TaskError &error)
{
    stream << "ravo: " << error_code_name(error.code) << ": " << error.message;
    for (const auto &[key, value] : error.context)
    {
        stream << " (" << key << "=" << value << ")";
    }
    stream << '\n';
}

} // namespace

CliApplication::CliApplication(const EngineFacade &engine, std::ostream &stdout_stream,
                               std::ostream &stderr_stream)
    : engine_(engine)
    , stdout_stream_(stdout_stream)
    , stderr_stream_(stderr_stream)
{
}

int CliApplication::run(const std::span<const std::string_view> arguments) const
{
    const auto emit = [this](const Result<JsonValue> &result, const bool json)
    {
        if (result)
        {
            if (json)
            {
                stdout_stream_ << serialize_json(success_envelope(result.value())) << '\n';
            }
            else
            {
                stdout_stream_ << serialize_json(result.value()) << '\n';
            }
            return 0;
        }
        if (json)
        {
            stdout_stream_ << serialize_json(failure_envelope(result.error())) << '\n';
        }
        else
        {
            write_human_error(stderr_stream_, result.error());
        }
        return cli_exit_code(result.error().code);
    };

    auto parsed = parse_arguments(arguments);
    if (!parsed)
    {
        return emit(parsed.error(), false);
    }
    if (parsed.value().positional.empty())
    {
        return emit(make_error(ErrorCode::kInvalidArgument, "A Ravo command is required"),
                    parsed.value().json);
    }
    const auto &positional = parsed.value().positional;
    const bool json = parsed.value().json;

    if (positional.size() == 1 && positional.front() == "--version")
    {
        return emit(JsonValue{JsonValue::Object{
                        {"name", "Ravo"}, {"protocol", "ravo-cli/v1"}, {"version", RAVO_VERSION}}},
                    json);
    }
    if (positional.size() == 1 && positional.front() == "operations")
    {
        JsonValue::Array operations;
        for (const auto &descriptor : engine_.operations())
        {
            auto operation = operation_descriptor_to_json(descriptor);
            if (!operation)
            {
                return emit(operation.error(), json);
            }
            operations.push_back(std::move(operation).value());
        }
        return emit(JsonValue{JsonValue::Object{{"operations", std::move(operations)}}}, json);
    }
    if (positional.size() == 3 && positional[0] == "recipe" && positional[1] == "validate")
    {
        auto text = read_utf8_text_file(positional[2]);
        if (!text)
        {
            return emit(text.error(), json);
        }
        auto recipe = parse_recipe_json(text.value());
        if (!recipe)
        {
            return emit(recipe.error(), json);
        }
        auto valid = engine_.validate(recipe.value());
        if (!valid)
        {
            return emit(valid.error(), json);
        }
        return emit(JsonValue{JsonValue::Object{
                        {"asset_id", recipe.value().asset.id},
                        {"operation_count",
                         JsonValue::number(std::to_string(recipe.value().operations.size()))},
                        {"schema_version",
                         JsonValue::number(std::to_string(recipe.value().schema_version))}}},
                    json);
    }
    if (positional.size() == 9 && positional[0] == "recipe" && positional[1] == "import-xmp" &&
        positional[3] == "--asset-id" && positional[5] == "--input" && positional[7] == "--output")
    {
        auto xmp = read_utf8_text_file(positional[2]);
        if (!xmp)
        {
            return emit(xmp.error(), json);
        }
        LegacyXmpImportRequest request{
            xmp.value(), {std::string(positional[4]), std::string(positional[6]), std::nullopt}};
        auto recipe = import_legacy_xmp(request);
        if (!recipe)
        {
            return emit(recipe.error(), json);
        }
        auto serialized = serialize_recipe(recipe.value());
        if (!serialized)
        {
            return emit(serialized.error(), json);
        }
        auto written = write_utf8_text_file_atomically(positional[8], serialized.value());
        if (!written)
        {
            return emit(written.error(), json);
        }
        return emit(JsonValue{JsonValue::Object{
                        {"asset_id", recipe.value().asset.id},
                        {"operation_count",
                         JsonValue::number(std::to_string(recipe.value().operations.size()))},
                        {"output", std::string(positional[8])},
                        {"schema_version",
                         JsonValue::number(std::to_string(recipe.value().schema_version))}}},
                    json);
    }
    if (positional.front() == "catalog")
    {
        return emit(run_catalog_command(engine_, positional), json);
    }
    if (positional.front() == "inspect")
    {
        if (positional.size() != 2)
        {
            return emit(make_error(ErrorCode::kInvalidArgument, "Usage: ravo inspect <input>"),
                        json);
        }
        auto inspected = engine_.inspect(positional[1], CancellationToken{});
        if (!inspected)
        {
            return emit(inspected.error(), json);
        }
        return emit(JsonValue{JsonValue::Object{
                        {"format", inspected.value().format},
                        {"height", JsonValue::number(std::to_string(inspected.value().height))},
                        {"input_uri", inspected.value().input_uri},
                        {"is_raw", inspected.value().is_raw},
                        {"make", inspected.value().make},
                        {"model", inspected.value().model},
                        {"width", JsonValue::number(std::to_string(inspected.value().width))}}},
                    json);
    }
    if (positional.front() == "render")
    {
        auto parsed_render = parse_render_arguments(positional);
        if (!parsed_render)
        {
            return emit(parsed_render.error(), json);
        }
        auto recipe_text = read_utf8_text_file(parsed_render.value().recipe);
        if (!recipe_text)
        {
            return emit(recipe_text.error(), json);
        }
        auto recipe = parse_recipe_json(recipe_text.value());
        if (!recipe)
        {
            return emit(recipe.error(), json);
        }
        RenderRequest request;
        request.recipe = std::move(recipe).value();
        request.recipe.asset.input_uri = std::string(parsed_render.value().input);
        request.asset = request.recipe.asset;
        request.output_uri = std::string(parsed_render.value().output);
        request.output_width = parsed_render.value().width;
        request.output_height = parsed_render.value().height;
        request.correlation_id = "cli-render";
        const auto rendered = engine_.render(request);
        if (!rendered)
        {
            return emit(rendered.error(), json);
        }
        return emit(JsonValue{JsonValue::Object{
                        {"correlation_id", rendered.value().correlation_id},
                        {"height", JsonValue::number(std::to_string(rendered.value().height))},
                        {"output", rendered.value().output_uri},
                        {"width", JsonValue::number(std::to_string(rendered.value().width))}}},
                    json);
    }
    return emit(make_error(ErrorCode::kInvalidArgument, "Invalid Ravo command"), json);
}

} // namespace ravo
