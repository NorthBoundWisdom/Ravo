#include "ravo/cli/application.h"
#include "application_internal.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <QByteArrayView>
#include <QCryptographicHash>

#ifdef emit
#undef emit
#endif

#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/filesystem_recovery_store.h"
#include "ravo/adapters/camera_noise_profile.h"
#include "ravo/adapters/crs_xmp.h"
#include "ravo/adapters/legacy_xmp.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/adapters/text_file.h"
#include "ravo/control/live_control.h"
#include "ravo/foundation/json.h"
#include "ravo/engine/noise_calibration.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/mask.h"
#include "ravo/recipe/operation.h"
#include "ravo/recipe/style.h"
#include "ravo/services/catalog_service.h"
#include "ravo/services/artifact_publication.h"
#include "ravo/services/display_presentation.h"

namespace ravo
{
namespace cli_internal
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

void write_human_error(std::ostream &stream, const TaskError &error)
{
    stream << "ravo: " << error_code_name(error.code) << ": " << error.message;
    for (const auto &[key, value] : error.context)
    {
        stream << " (" << key << "=" << value << ")";
    }
    stream << '\n';
}

} // namespace cli_internal
using namespace cli_internal;

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
    if (positional.size() == 1 && positional.front() == "develop-fields")
    {
        return emit(develop_fields_json(), json);
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
    if (positional.front() == "display-profile")
    {
        if (positional.size() < 2 || positional[1] != "status")
        {
            return emit(
                make_error(
                    ErrorCode::kInvalidArgument,
                    "Usage: ravo display-profile status [--profile <icc>] [--screen <token>]"),
                json);
        }
        std::string_view profile_path;
        std::string_view screen = "primary";
        for (std::size_t index = 2; index < positional.size(); ++index)
        {
            const auto argument = positional[index];
            if (argument == "--profile")
            {
                if (index + 1 >= positional.size())
                {
                    return emit(
                        make_error(ErrorCode::kInvalidArgument, "--profile requires a path"), json);
                }
                profile_path = positional[++index];
                continue;
            }
            if (argument == "--screen")
            {
                if (index + 1 >= positional.size())
                {
                    return emit(
                        make_error(ErrorCode::kInvalidArgument, "--screen requires a token"), json);
                }
                screen = positional[++index];
                continue;
            }
            return emit(make_error(ErrorCode::kInvalidArgument, "Unknown display-profile option",
                                   {{"option", std::string(argument)}}),
                        json);
        }
        Result<DisplayPresentationState> state =
            profile_path.empty() ? discover_monitor_presentation(screen) :
                                   inject_monitor_presentation_from_icc_path(profile_path, screen);
        if (!state)
        {
            return emit(state.error(), json);
        }
        return emit(display_presentation_state_to_json(state.value()), json);
    }
    if (positional.front() == "perspective")
    {
        return emit(run_perspective_analysis(engine_, positional), json);
    }
    if (positional.front() == "iq")
    {
        return emit(run_iq_command(positional), json);
    }
    if (positional.front() == "noise")
    {
        return emit(run_noise_command(positional), json);
    }
    if (positional.front() == "lut")
    {
        if (positional.size() != 3U || positional[1] != "inspect")
            return emit(
                make_error(ErrorCode::kInvalidArgument, "Usage: ravo lut inspect <file.cube>"),
                json);
        auto inspected = engine_.inspect_lut3d(positional[2], CancellationToken{});
        if (!inspected)
            return emit(inspected.error(), json);
        const auto channels = [](const std::array<float, 3> &values)
        {
            JsonValue::Array array;
            for (const float value : values)
                array.push_back(JsonValue::number(std::to_string(value)));
            return array;
        };
        return emit(JsonValue{JsonValue::Object{
                        {"domain_max", channels(inspected.value().domain_max)},
                        {"domain_min", channels(inspected.value().domain_min)},
                        {"fingerprint", inspected.value().fingerprint},
                        {"path", inspected.value().canonical_path},
                        {"size", JsonValue::number(std::to_string(inspected.value().size))},
                        {"title", inspected.value().title},
                    }},
                    json);
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
    if (positional.size() == 3 && positional[0] == "recipe" && positional[1] == "inspect")
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
        JsonValue::Array operations;
        std::size_t exposure_count = 0;
        std::size_t color_balance_rgb_count = 0;
        for (const auto &operation : recipe.value().operations)
        {
            if (operation.id == std::string(kExposureOperationId))
            {
                ++exposure_count;
            }
            if (operation.id == "ravo.color.colorbalancergb")
            {
                ++color_balance_rgb_count;
            }
            JsonValue::Object entry{
                {"bypass", operation.bypass},
                {"enabled", operation.enabled},
                {"id", operation.id},
                {"instance_id", operation.instance_id},
                {"schema_version", JsonValue::number(std::to_string(operation.schema_version))}};
            if (operation.name.has_value())
            {
                entry.emplace("name", *operation.name);
            }
            if (operation.mask_id.has_value())
            {
                entry.emplace("mask_id", *operation.mask_id);
            }
            operations.push_back(std::move(entry));
        }
        JsonValue::Array masks;
        for (const auto &mask : recipe.value().masks)
        {
            masks.push_back(JsonValue::Object{{"id", mask.id},
                                              {"kind", std::string(mask_kind_name(mask.kind))}});
        }
        return emit(
            JsonValue{JsonValue::Object{
                {"asset_id", recipe.value().asset.id},
                {"exposure_instance_count", JsonValue::number(std::to_string(exposure_count))},
                {"color_balance_rgb_instance_count",
                 JsonValue::number(std::to_string(color_balance_rgb_count))},
                {"masks", std::move(masks)},
                {"operations", std::move(operations)},
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
        Recipe recipe;
        std::string dialect = "legacy";
        std::string preset_name;
        std::vector<CrsOmission> omitted;
        if (is_crs_xmp_document(xmp.value()))
        {
            auto imported = import_crs_xmp(request);
            if (!imported)
            {
                return emit(imported.error(), json);
            }
            recipe = std::move(imported.value().recipe);
            dialect = "crs";
            preset_name = imported.value().name;
            omitted = imported.value().omitted;
        }
        else
        {
            auto imported = import_legacy_xmp(request);
            if (!imported)
            {
                return emit(imported.error(), json);
            }
            recipe = std::move(imported).value();
        }
        auto serialized = serialize_recipe(recipe);
        if (!serialized)
        {
            return emit(serialized.error(), json);
        }
        auto written = write_utf8_text_file_atomically(positional[8], serialized.value());
        if (!written)
        {
            return emit(written.error(), json);
        }
        JsonValue::Object payload{
            {"asset_id", recipe.asset.id},
            {"dialect", dialect},
            {"operation_count", JsonValue::number(std::to_string(recipe.operations.size()))},
            {"output", std::string(positional[8])},
            {"schema_version", JsonValue::number(std::to_string(recipe.schema_version))}};
        if (dialect == "crs")
        {
            payload.emplace("omitted", crs_omissions_json(omitted));
            payload.emplace("preset_name", preset_name);
        }
        return emit(JsonValue{std::move(payload)}, json);
    }
    if (positional.size() == 3 && positional[0] == "recipe" && positional[1] == "style-validate")
    {
        auto text = read_utf8_text_file(positional[2]);
        if (!text)
            return emit(text.error(), json);
        auto style = parse_recipe_style_json(text.value());
        if (!style)
            return emit(style.error(), json);
        auto valid = engine_.validate(style.value().recipe);
        if (!valid)
            return emit(valid.error(), json);
        return emit(JsonValue{JsonValue::Object{
                        {"name", style.value().name},
                        {"operation_count",
                         JsonValue::number(std::to_string(style.value().recipe.operations.size()))},
                        {"schema_version",
                         JsonValue::number(std::to_string(style.value().schema_version))}}},
                    json);
    }
    if (positional.size() == 7 && positional[0] == "recipe" && positional[1] == "style-create" &&
        positional[3] == "--name" && positional[5] == "--output")
    {
        auto text = read_utf8_text_file(positional[2]);
        if (!text)
            return emit(text.error(), json);
        auto recipe = parse_recipe_json(text.value());
        if (!recipe)
            return emit(recipe.error(), json);
        auto valid = engine_.validate(recipe.value());
        if (!valid)
            return emit(valid.error(), json);
        auto style = recipe_style_from_recipe(std::string(positional[4]), {}, recipe.value());
        if (!style)
            return emit(style.error(), json);
        auto serialized = serialize_recipe_style(style.value());
        if (!serialized)
            return emit(serialized.error(), json);
        auto written = write_utf8_text_file_atomically(positional[6], serialized.value());
        if (!written)
            return emit(written.error(), json);
        return emit(JsonValue{JsonValue::Object{
                        {"name", style.value().name},
                        {"operation_count",
                         JsonValue::number(std::to_string(style.value().recipe.operations.size()))},
                        {"output", std::string(positional[6])},
                        {"schema_version",
                         JsonValue::number(std::to_string(style.value().schema_version))}}},
                    json);
    }
    if (positional.size() == 9 && positional[0] == "recipe" && positional[1] == "style-apply" &&
        positional[3] == "--asset-id" && positional[5] == "--input" && positional[7] == "--output")
    {
        auto text = read_utf8_text_file(positional[2]);
        if (!text)
            return emit(text.error(), json);
        auto style = parse_recipe_style_json(text.value());
        if (!style)
            return emit(style.error(), json);
        auto recipe = apply_recipe_style(
            style.value(), {std::string(positional[4]), std::string(positional[6]), std::nullopt});
        if (!recipe)
            return emit(recipe.error(), json);
        auto valid = engine_.validate(recipe.value());
        if (!valid)
            return emit(valid.error(), json);
        auto serialized = serialize_recipe(recipe.value());
        if (!serialized)
            return emit(serialized.error(), json);
        auto written = write_utf8_text_file_atomically(positional[8], serialized.value());
        if (!written)
            return emit(written.error(), json);
        return emit(JsonValue{JsonValue::Object{
                        {"asset_id", recipe.value().asset.id},
                        {"name", style.value().name},
                        {"operation_count",
                         JsonValue::number(std::to_string(recipe.value().operations.size()))},
                        {"output", std::string(positional[8])}}},
                    json);
    }
    if (positional.size() == 7 && positional[0] == "recipe" && positional[1] == "style-apply" &&
        positional[3] == "--target-recipe" && positional[5] == "--output")
    {
        auto text = read_utf8_text_file(positional[2]);
        if (!text)
            return emit(text.error(), json);
        auto style = parse_recipe_style_json(text.value());
        if (!style)
            return emit(style.error(), json);
        auto style_valid = engine_.validate(style.value().recipe);
        if (!style_valid)
            return emit(style_valid.error(), json);
        auto target_text = read_utf8_text_file(positional[4]);
        if (!target_text)
            return emit(target_text.error(), json);
        auto target = parse_recipe_json(target_text.value());
        if (!target)
            return emit(target.error(), json);
        auto target_valid = engine_.validate(target.value());
        if (!target_valid)
            return emit(target_valid.error(), json);
        auto recipe = apply_recipe_style(style.value(), std::move(target).value());
        if (!recipe)
            return emit(recipe.error(), json);
        auto valid = engine_.validate(recipe.value());
        if (!valid)
            return emit(valid.error(), json);
        auto serialized = serialize_recipe(recipe.value());
        if (!serialized)
            return emit(serialized.error(), json);
        auto written = write_utf8_text_file_atomically(positional[6], serialized.value());
        if (!written)
            return emit(written.error(), json);
        return emit(JsonValue{JsonValue::Object{
                        {"asset_id", recipe.value().asset.id},
                        {"name", style.value().name},
                        {"operation_count",
                         JsonValue::number(std::to_string(recipe.value().operations.size()))},
                        {"output", std::string(positional[6])}}},
                    json);
    }
    if (positional.front() == "catalog")
    {
        return emit(run_catalog_command(engine_, positional), json);
    }
    if (positional.front() == "studio")
    {
        return emit(run_studio_command(engine_, positional), json);
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
        JsonValue::Object data{
            {"format", inspected.value().format},
            {"height", JsonValue::number(std::to_string(inspected.value().height))},
            {"input_uri", inspected.value().input_uri},
            {"is_raw", inspected.value().is_raw},
            {"make", inspected.value().make},
            {"model", inspected.value().model},
            {"width", JsonValue::number(std::to_string(inspected.value().width))}};
        if (inspected.value().is_raw)
        {
            const auto coeffs = [](const std::array<double, 4> &values)
            {
                JsonValue::Array items;
                for (const double value : values)
                {
                    items.push_back(JsonValue::number(std::to_string(value)));
                }
                return items;
            };
            data.emplace("has_as_shot_white_balance", inspected.value().has_as_shot_white_balance);
            data.emplace("raw_sensor", inspected.value().raw_sensor);
            data.emplace("cfa_width",
                         JsonValue::number(std::to_string(inspected.value().cfa_width)));
            data.emplace("cfa_height",
                         JsonValue::number(std::to_string(inspected.value().cfa_height)));
            data.emplace("default_demosaic_mode", inspected.value().default_demosaic_mode);
            data.emplace("as_shot_white_balance", coeffs(inspected.value().as_shot_white_balance));
            data.emplace("has_camera_reference_white_balance",
                         inspected.value().has_camera_reference_white_balance);
            data.emplace("camera_reference_white_balance",
                         coeffs(inspected.value().camera_reference_white_balance));
            data.emplace("dng_opcode_list2_present", inspected.value().dng_opcode_list2_present);
            data.emplace("dng_opcode_list3_present", inspected.value().dng_opcode_list3_present);
            data.emplace("dng_gain_map_count",
                         JsonValue::number(std::to_string(inspected.value().dng_gain_map_count)));
            data.emplace("dng_has_warp_rectilinear", inspected.value().dng_has_warp_rectilinear);
            data.emplace("dng_has_fix_vignette_radial",
                         inspected.value().dng_has_fix_vignette_radial);
            const auto opcode_ids = [](const std::vector<std::uint32_t> &values)
            {
                JsonValue::Array items;
                for (const std::uint32_t value : values)
                {
                    items.push_back(JsonValue::number(std::to_string(value)));
                }
                return items;
            };
            data.emplace("dng_skipped_optional_opcode_list2",
                         opcode_ids(inspected.value().dng_skipped_optional_opcode_list2));
            data.emplace("dng_skipped_optional_opcode_list3",
                         opcode_ids(inspected.value().dng_skipped_optional_opcode_list3));
        }
        return emit(JsonValue{std::move(data)}, json);
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
