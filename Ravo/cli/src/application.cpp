#include "ravo/cli/application.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <ostream>
#include <set>
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
    bool baseline = false;
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
    std::string_view tag;
    std::string_view add;
    std::string_view remove;
    std::string_view title;
    std::string_view description;
    std::string_view creator;
    std::string_view copyright;
    std::string_view label;
    std::optional<std::int64_t> history_id;
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
        if (option == "--baseline")
        {
            if (result.baseline)
            {
                return make_error(ErrorCode::kInvalidArgument,
                                  "--baseline can only be specified once");
            }
            result.baseline = true;
            continue;
        }
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
        else if (option == "--tag")
        {
            result.tag = value;
        }
        else if (option == "--add")
        {
            result.add = value;
        }
        else if (option == "--remove")
        {
            result.remove = value;
        }
        else if (option == "--title")
        {
            result.title = value;
        }
        else if (option == "--description")
        {
            result.description = value;
        }
        else if (option == "--creator")
        {
            result.creator = value;
        }
        else if (option == "--copyright")
        {
            result.copyright = value;
        }
        else if (option == "--label")
        {
            result.label = value;
        }
        else if (option == "--history-id")
        {
            auto parsed = parse_int_flag(value, option);
            if (!parsed)
            {
                return parsed.error();
            }
            result.history_id = parsed.value();
        }
        else
        {
            return make_error(ErrorCode::kInvalidArgument, "Unknown catalog option",
                              {{"option", std::string(option)}});
        }
    }
    return result;
}

struct AppliedDevelopOverride
{
    std::string name;
    double value = 0.0;
};

[[nodiscard]] Result<std::vector<AppliedDevelopOverride>>
apply_develop_overrides(DevelopParams &params, const CatalogCliArguments &flags)
{
    std::vector<AppliedDevelopOverride> applied;
    std::set<std::string, std::less<>> names;
    const auto apply_one = [&](const std::string_view name, const double value) -> Result<void>
    {
        if (!names.emplace(name).second)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "Develop field was specified more than once",
                              {{"name", std::string(name)}});
        }
        DevelopParams candidate = params;
        auto assigned = apply_develop_field_strict(candidate, name, value);
        if (!assigned)
        {
            return assigned.error();
        }
        params = std::move(candidate);
        applied.push_back({std::string(name), value});
        return {};
    };

    if (flags.exposure_ev)
    {
        auto result = apply_one("exposure", *flags.exposure_ev);
        if (!result)
        {
            return result.error();
        }
    }
    if (flags.saturation)
    {
        auto result = apply_one("saturation", *flags.saturation);
        if (!result)
        {
            return result.error();
        }
    }
    if (flags.contrast)
    {
        auto result = apply_one("contrast", *flags.contrast);
        if (!result)
        {
            return result.error();
        }
    }
    for (const auto &[name, value] : flags.develop_sets)
    {
        auto result = apply_one(name, value);
        if (!result)
        {
            return result.error();
        }
    }
    clamp_develop(params);
    return applied;
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

[[nodiscard]] JsonValue optional_string_json(const std::optional<std::string> &value)
{
    if (!value)
    {
        return nullptr;
    }
    return *value;
}

[[nodiscard]] JsonValue asset_to_json(const AssetRecord &asset)
{
    JsonValue::Array tags;
    for (const auto &tag : asset.tags)
    {
        tags.push_back(tag);
    }
    JsonValue::Object metadata{
        {"copyright", optional_string_json(asset.metadata.copyright)},
        {"creator", optional_string_json(asset.metadata.creator)},
        {"description", optional_string_json(asset.metadata.description)},
        {"title", optional_string_json(asset.metadata.title)},
    };
    JsonValue::Object capture{
        {"aperture", asset.capture.aperture ?
                         JsonValue::number(std::to_string(*asset.capture.aperture)) :
                         JsonValue{nullptr}},
        {"camera_make", optional_string_json(asset.capture.camera_make)},
        {"camera_model", optional_string_json(asset.capture.camera_model)},
        {"focal_length_mm", asset.capture.focal_length_mm ?
                                JsonValue::number(std::to_string(*asset.capture.focal_length_mm)) :
                                JsonValue{nullptr}},
        {"iso", asset.capture.iso ? JsonValue::number(std::to_string(*asset.capture.iso)) :
                                    JsonValue{nullptr}},
        {"shutter_s", asset.capture.shutter_s ?
                          JsonValue::number(std::to_string(*asset.capture.shutter_s)) :
                          JsonValue{nullptr}},
    };
    return JsonValue::Object{
        {"capture", std::move(capture)},
        {"color_label", std::string(color_label_name(asset.review.color_label))},
        {"has_edits", asset.has_edits},
        {"id", asset.id},
        {"import_state", asset.import_state},
        {"media_type", asset.media_type},
        {"metadata", std::move(metadata)},
        {"rating", JsonValue::number(std::to_string(asset.review.rating))},
        {"rejected", asset.review.rejected},
        {"tags", std::move(tags)},
        {"uri", asset.normalized_uri},
    };
}

[[nodiscard]] Result<JsonValue> probe_statistics_json(const PreviewResult &preview)
{
    RasterBuffer raster;
    raster.width = preview.width;
    raster.height = preview.height;
    raster.srgb = preview.rgb;
    raster.color_profile = preview.color_profile;
    auto histogram = collect_rgb_histogram(raster);
    if (!histogram)
    {
        return histogram.error();
    }

    const std::uint64_t pixels = static_cast<std::uint64_t>(preview.width) * preview.height;
    const std::array<const std::array<std::uint32_t, kRgbHistogramBins> *, 3> channels{
        &histogram.value().red,
        &histogram.value().green,
        &histogram.value().blue,
    };
    std::array<std::uint64_t, 3> sums{};
    std::array<std::uint32_t, 3> minima{};
    std::array<std::uint32_t, 3> maxima{};
    std::array<std::uint32_t, 3> zeros{};
    std::array<std::uint32_t, 3> full{};
    for (std::size_t channel = 0; channel < channels.size(); ++channel)
    {
        bool found = false;
        for (std::uint32_t bin = 0; bin < kRgbHistogramBins; ++bin)
        {
            const auto count = (*channels[channel])[bin];
            sums[channel] += static_cast<std::uint64_t>(bin) * count;
            if (count != 0)
            {
                if (!found)
                {
                    minima[channel] = bin;
                    found = true;
                }
                maxima[channel] = bin;
            }
        }
        zeros[channel] = (*channels[channel])[0];
        full[channel] = (*channels[channel])[kRgbHistogramBins - 1U];
    }

    const auto integer_array = [](const auto &values)
    {
        JsonValue::Array result;
        result.reserve(values.size());
        for (const auto value : values)
        {
            result.push_back(JsonValue::number(std::to_string(value)));
        }
        return result;
    };
    JsonValue::Array means;
    means.reserve(sums.size());
    for (const auto sum : sums)
    {
        means.push_back(JsonValue::number(
            std::to_string(static_cast<double>(sum) / static_cast<double>(pixels))));
    }
    const double luma_mean =
        (0.2126 * static_cast<double>(sums[0]) + 0.7152 * static_cast<double>(sums[1]) +
         0.0722 * static_cast<double>(sums[2])) /
        static_cast<double>(pixels);
    return JsonValue{JsonValue::Object{
        {"channel_full_counts", integer_array(full)},
        {"channel_maxima", integer_array(maxima)},
        {"channel_means", std::move(means)},
        {"channel_minima", integer_array(minima)},
        {"channel_sums", integer_array(sums)},
        {"channel_zero_counts", integer_array(zeros)},
        {"display_luma_mean", JsonValue::number(std::to_string(luma_mean))},
        {"histogram_peak", JsonValue::number(std::to_string(histogram.value().max_count))},
        {"pixels", JsonValue::number(std::to_string(pixels))},
    }};
}

[[nodiscard]] Result<JsonValue>
run_catalog_command(const EngineFacade &engine, const std::span<const std::string_view> positional)
{
    if (positional.size() < 2)
    {
        return make_error(
            ErrorCode::kInvalidArgument,
            "Usage: ravo catalog <create|import|list|preview|probe|recipe|develop|rate|"
            "export|tag|metadata|history|snapshot|restore> --catalog <path>");
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
    if (flags.value().baseline && subcommand != "probe")
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "--baseline is only valid for catalog probe");
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
            if (item.error)
            {
                row.emplace("error", error_object(*item.error));
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
        LibraryQuery query;
        if (!flags.value().tag.empty())
        {
            auto tag = normalize_tag_name(flags.value().tag);
            if (!tag)
            {
                return tag.error();
            }
            query.tag = tag.value();
        }
        auto listed = service.list_assets(query);
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
    if (subcommand == "probe")
    {
        if (flags.value().asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument, "catalog probe requires --asset-id");
        }
        auto stored_before = service.load_recipe(flags.value().asset_id);
        if (!stored_before)
        {
            return stored_before.error();
        }
        auto serialized_before = serialize_recipe(stored_before.value());
        if (!serialized_before)
        {
            return serialized_before.error();
        }
        auto previews_before = service.list_previews();
        if (!previews_before)
        {
            return previews_before.error();
        }
        auto source = flags.value().baseline ?
                          service.load_baseline_recipe(flags.value().asset_id) :
                          stored_before;
        if (!source)
        {
            return source.error();
        }
        auto params = develop_from_recipe(source.value());
        if (!params)
        {
            return params.error();
        }
        auto applied = apply_develop_overrides(params.value(), flags.value());
        if (!applied)
        {
            return applied.error();
        }

        PreviewRequest request;
        request.asset_id = std::string(flags.value().asset_id);
        request.max_edge = flags.value().max_edge.value_or(512U);
        request.prefer_embedded_preview = false;
        request.persist_preview_record = false;
        auto previewed = service.request_preview(request, params.value());
        if (!previewed)
        {
            return previewed.error();
        }
        if (!previewed.value().cache_path.empty() || previewed.value().rgb.empty())
        {
            return make_error(ErrorCode::kIo,
                              "Develop probe did not return a non-persistent memory preview");
        }
        auto statistics = probe_statistics_json(previewed.value());
        if (!statistics)
        {
            return statistics.error();
        }
        auto stored_after = service.load_recipe(flags.value().asset_id);
        if (!stored_after)
        {
            return stored_after.error();
        }
        auto serialized_after = serialize_recipe(stored_after.value());
        if (!serialized_after)
        {
            return serialized_after.error();
        }
        if (serialized_before.value() != serialized_after.value())
        {
            return make_error(ErrorCode::kIo, "Develop probe unexpectedly changed the recipe");
        }
        auto previews_after = service.list_previews();
        if (!previews_after)
        {
            return previews_after.error();
        }
        if (previews_before.value() != previews_after.value())
        {
            return make_error(ErrorCode::kIo, "Develop probe unexpectedly changed preview records");
        }

        JsonValue::Object overrides;
        for (const auto &item : applied.value())
        {
            overrides.emplace(item.name, JsonValue::number(std::to_string(item.value)));
        }
        return JsonValue{JsonValue::Object{
            {"asset_id", previewed.value().asset_id},
            {"baseline", flags.value().baseline},
            {"color_profile", previewed.value().color_profile.identifier},
            {"height", JsonValue::number(std::to_string(previewed.value().height))},
            {"original_missing", previewed.value().original_missing},
            {"overrides", std::move(overrides)},
            {"preview_records_unchanged", true},
            {"recipe_unchanged", true},
            {"statistics", std::move(statistics).value()},
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
        auto applied = apply_develop_overrides(params.value(), flags.value());
        if (!applied)
        {
            return applied.error();
        }
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
            request.jpeg_options.quality = *flags.value().quality;
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
    if (subcommand == "tag")
    {
        if (flags.value().asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument, "catalog tag requires --asset-id");
        }
        auto asset = service.list_assets();
        if (!asset)
        {
            return asset.error();
        }
        const AssetRecord *selected = nullptr;
        for (const auto &item : asset.value())
        {
            if (item.id == flags.value().asset_id)
            {
                selected = &item;
                break;
            }
        }
        if (selected == nullptr)
        {
            return make_error(ErrorCode::kNotFound, "Asset does not exist",
                              {{"asset_id", std::string(flags.value().asset_id)}});
        }
        std::vector<std::string> tags = selected->tags;
        if (!flags.value().add.empty())
        {
            auto parsed = parse_tag_list(flags.value().add);
            if (!parsed)
            {
                return parsed.error();
            }
            for (auto &tag : parsed.value())
            {
                if (std::find(tags.begin(), tags.end(), tag) == tags.end())
                {
                    tags.push_back(std::move(tag));
                }
            }
        }
        if (!flags.value().remove.empty())
        {
            auto parsed = parse_tag_list(flags.value().remove);
            if (!parsed)
            {
                return parsed.error();
            }
            tags.erase(std::remove_if(tags.begin(), tags.end(),
                                      [&](const std::string &tag)
                                      {
                                          return std::find(parsed.value().begin(),
                                                           parsed.value().end(),
                                                           tag) != parsed.value().end();
                                      }),
                       tags.end());
        }
        if (!flags.value().add.empty() || !flags.value().remove.empty())
        {
            auto saved = service.set_tags(flags.value().asset_id, tags);
            if (!saved)
            {
                return saved.error();
            }
            return asset_to_json(saved.value());
        }
        return asset_to_json(*selected);
    }
    if (subcommand == "metadata")
    {
        if (flags.value().asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument, "catalog metadata requires --asset-id");
        }
        auto loaded = service.list_assets();
        if (!loaded)
        {
            return loaded.error();
        }
        const AssetRecord *selected = nullptr;
        for (const auto &item : loaded.value())
        {
            if (item.id == flags.value().asset_id)
            {
                selected = &item;
                break;
            }
        }
        if (selected == nullptr)
        {
            return make_error(ErrorCode::kNotFound, "Asset does not exist",
                              {{"asset_id", std::string(flags.value().asset_id)}});
        }
        WritableMetadata metadata = selected->metadata;
        bool write = false;
        const auto assign = [&](const std::string_view text, std::optional<std::string> &field)
        {
            if (!text.empty())
            {
                field = std::string(text);
                write = true;
            }
        };
        assign(flags.value().title, metadata.title);
        assign(flags.value().description, metadata.description);
        assign(flags.value().creator, metadata.creator);
        assign(flags.value().copyright, metadata.copyright);
        if (write)
        {
            auto saved = service.set_writable_metadata(flags.value().asset_id, metadata);
            if (!saved)
            {
                return saved.error();
            }
            return asset_to_json(saved.value());
        }
        return asset_to_json(*selected);
    }
    if (subcommand == "history")
    {
        if (flags.value().asset_id.empty())
        {
            return make_error(ErrorCode::kInvalidArgument, "catalog history requires --asset-id");
        }
        auto history = service.list_recipe_history(flags.value().asset_id);
        if (!history)
        {
            return history.error();
        }
        JsonValue::Array entries;
        for (const auto &entry : history.value())
        {
            entries.push_back(JsonValue::Object{
                {"id", JsonValue::number(std::to_string(entry.id))},
                {"kind", entry.kind},
                {"label", entry.label ? JsonValue{*entry.label} : JsonValue{nullptr}},
                {"seq", JsonValue::number(std::to_string(entry.seq))},
            });
        }
        return JsonValue{JsonValue::Object{
            {"asset_id", std::string(flags.value().asset_id)},
            {"history", std::move(entries)},
        }};
    }
    if (subcommand == "snapshot")
    {
        if (flags.value().asset_id.empty() || flags.value().label.empty())
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog snapshot requires --asset-id and --label");
        }
        auto saved = service.create_recipe_snapshot(flags.value().asset_id, flags.value().label);
        if (!saved)
        {
            return saved.error();
        }
        return asset_to_json(saved.value());
    }
    if (subcommand == "restore")
    {
        if (flags.value().asset_id.empty() || !flags.value().history_id)
        {
            return make_error(ErrorCode::kInvalidArgument,
                              "catalog restore requires --asset-id and --history-id");
        }
        auto restored =
            service.restore_recipe_history(flags.value().asset_id, *flags.value().history_id);
        if (!restored)
        {
            return restored.error();
        }
        return asset_to_json(restored.value());
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
