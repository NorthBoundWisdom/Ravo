#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <sstream>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include "dng_opcodes.h"
#include <tiffio.h>

#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/filesystem_recovery_store.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/cli/application.h"
#include "ravo/domain/types.h"
#include "ravo/foundation/json.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/develop.h"
#include "ravo/services/catalog_service.h"

namespace ravo
{
namespace
{

void append_u32(std::vector<std::uint8_t> &bytes, const std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value >> 24U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(value));
}

void append_u64(std::vector<std::uint8_t> &bytes, const std::uint64_t value)
{
    for (int shift = 56; shift >= 0; shift -= 8)
    {
        bytes.push_back(static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
    }
}

void append_float(std::vector<std::uint8_t> &bytes, const float value)
{
    append_u32(bytes, std::bit_cast<std::uint32_t>(value));
}

void append_double(std::vector<std::uint8_t> &bytes, const double value)
{
    append_u64(bytes, std::bit_cast<std::uint64_t>(value));
}

struct TestOpcode
{
    std::uint32_t id = 0U;
    std::uint32_t flags = 0U;
    std::vector<std::uint8_t> payload;
    std::uint32_t minimum_version = 0x01030000U;
};

[[nodiscard]] std::vector<std::uint8_t> opcode_list(const std::vector<TestOpcode> &operations)
{
    std::vector<std::uint8_t> bytes;
    append_u32(bytes, static_cast<std::uint32_t>(operations.size()));
    for (const auto &operation : operations)
    {
        append_u32(bytes, operation.id);
        append_u32(bytes, operation.minimum_version);
        append_u32(bytes, operation.flags);
        append_u32(bytes, static_cast<std::uint32_t>(operation.payload.size()));
        bytes.insert(bytes.end(), operation.payload.begin(), operation.payload.end());
    }
    return bytes;
}

[[nodiscard]] std::vector<std::uint8_t>
gain_map_payload_custom(const std::uint32_t top, const std::uint32_t left,
                        const std::uint32_t bottom, const std::uint32_t right,
                        const std::uint32_t plane, const std::uint32_t planes,
                        const std::uint32_t row_pitch, const std::uint32_t column_pitch,
                        const std::uint32_t points_vertical, const std::uint32_t points_horizontal,
                        const std::uint32_t map_planes, const std::span<const float> gains)
{
    std::vector<std::uint8_t> bytes;
    append_u32(bytes, top);
    append_u32(bytes, left);
    append_u32(bytes, bottom);
    append_u32(bytes, right);
    append_u32(bytes, plane);
    append_u32(bytes, planes);
    append_u32(bytes, row_pitch);
    append_u32(bytes, column_pitch);
    append_u32(bytes, points_vertical);
    append_u32(bytes, points_horizontal);
    append_double(bytes, points_vertical > 1U ? 1.0 / (points_vertical - 1U) : 1.0);
    append_double(bytes, points_horizontal > 1U ? 1.0 / (points_horizontal - 1U) : 1.0);
    append_double(bytes, 0.0);
    append_double(bytes, 0.0);
    append_u32(bytes, map_planes);
    for (const float gain : gains)
    {
        append_float(bytes, gain);
    }
    return bytes;
}

[[nodiscard]] std::vector<std::uint8_t>
gain_map_payload(const std::uint32_t top, const std::uint32_t left, const std::uint32_t width,
                 const std::uint32_t height, const std::array<float, 4> gains)
{
    return gain_map_payload_custom(top, left, height, width, 0U, 1U, 2U, 2U, 2U, 2U, 1U, gains);
}

[[nodiscard]] std::vector<std::uint8_t> warp_payload(const std::array<double, 6> coefficients = {
                                                         1.0, 0.0, 0.0, 0.0, 0.0, 0.0})
{
    std::vector<std::uint8_t> bytes;
    append_u32(bytes, 1U);
    for (const double coefficient : coefficients)
    {
        append_double(bytes, coefficient);
    }
    append_double(bytes, 0.5);
    append_double(bytes, 0.5);
    return bytes;
}

[[nodiscard]] std::vector<std::uint8_t>
vignette_payload(const std::array<double, 5> coefficients = {})
{
    std::vector<std::uint8_t> bytes;
    for (const double coefficient : coefficients)
    {
        append_double(bytes, coefficient);
    }
    append_double(bytes, 0.5);
    append_double(bytes, 0.5);
    return bytes;
}

[[nodiscard]] WorkingImage test_image(const std::uint32_t width = 3U,
                                      const std::uint32_t height = 3U)
{
    WorkingImage image;
    image.width = width;
    image.height = height;
    image.color_profile.kind = ColorProfileKind::kMatrix;
    image.color_profile.model = ColorModel::kRgb;
    image.color_profile.identifier = "camera";
    image.rgb.resize(static_cast<std::size_t>(width) * height * 3U);
    for (std::size_t index = 0U; index < image.rgb.size(); ++index)
    {
        image.rgb[index] = static_cast<float>(index + 1U) / 32.0F;
    }
    return image;
}

[[nodiscard]] bool write_synthetic_dng(const std::filesystem::path &path,
                                       const std::span<const std::uint8_t> list2,
                                       const std::span<const std::uint8_t> list3)
{
    constexpr std::uint32_t width = 32U;
    constexpr std::uint32_t height = 32U;
    TIFF *tiff = TIFFOpen(path.string().c_str(), "w");
    if (tiff == nullptr)
    {
        return false;
    }
    char opcode_list2_name[] = "OpcodeList2";
    char opcode_list3_name[] = "OpcodeList3";
    const std::array<TIFFFieldInfo, 2> opcode_fields{
        TIFFFieldInfo{TIFFTAG_OPCODELIST2, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_UNDEFINED,
                      FIELD_CUSTOM, 1U, 1U, opcode_list2_name},
        TIFFFieldInfo{TIFFTAG_OPCODELIST3, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_UNDEFINED,
                      FIELD_CUSTOM, 1U, 1U, opcode_list3_name}};
    if (TIFFMergeFieldInfo(tiff, opcode_fields.data(),
                           static_cast<std::uint32_t>(opcode_fields.size())) != 0)
    {
        TIFFClose(tiff);
        return false;
    }
    const std::array<std::uint8_t, 4> dng_version{1U, 4U, 0U, 0U};
    const std::array<std::uint8_t, 4> dng_backward_version{1U, 3U, 0U, 0U};
    const std::array<std::uint16_t, 2> cfa_repeat{2U, 2U};
    const std::array<std::uint8_t, 4> cfa_pattern{0U, 1U, 1U, 2U};
    const std::array<std::uint16_t, 2> black_repeat{1U, 1U};
    const std::array<float, 1> black_level{100.0F};
    const std::array<std::uint32_t, 1> white_level{1100U};
    const std::array<float, 2> default_scale{1.0F, 1.0F};
    const std::array<float, 2> default_crop_origin{0.0F, 0.0F};
    const std::array<float, 2> default_crop_size{static_cast<float>(width),
                                                 static_cast<float>(height)};
    const std::array<float, 9> color_matrix{1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    const std::array<float, 3> as_shot_neutral{1.0F, 1.0F, 1.0F};
    const std::array<std::uint32_t, 4> active_area{0U, 0U, height, width};

    bool ok = true;
    const auto set = [&](const int status) { ok = ok && status == 1; };
    set(TIFFSetField(tiff, TIFFTAG_SUBFILETYPE, 0U));
    set(TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, width));
    set(TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, height));
    set(TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 16U));
    set(TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_NONE));
    set(TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_CFA));
    set(TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, 1U));
    set(TIFFSetField(tiff, TIFFTAG_ROWSPERSTRIP, height));
    set(TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG));
    set(TIFFSetField(tiff, TIFFTAG_DNGVERSION, dng_version.data()));
    set(TIFFSetField(tiff, TIFFTAG_DNGBACKWARDVERSION, dng_backward_version.data()));
    set(TIFFSetField(tiff, TIFFTAG_UNIQUECAMERAMODEL, "Ravo Synthetic DNG"));
    set(TIFFSetField(tiff, TIFFTAG_CFAREPEATPATTERNDIM, cfa_repeat.data()));
    set(TIFFSetField(tiff, TIFFTAG_CFAPATTERN, static_cast<std::uint16_t>(cfa_pattern.size()),
                     cfa_pattern.data()));
    set(TIFFSetField(tiff, TIFFTAG_BLACKLEVELREPEATDIM, black_repeat.data()));
    set(TIFFSetField(tiff, TIFFTAG_BLACKLEVEL, static_cast<std::uint16_t>(black_level.size()),
                     black_level.data()));
    set(TIFFSetField(tiff, TIFFTAG_WHITELEVEL, static_cast<std::uint16_t>(white_level.size()),
                     white_level.data()));
    set(TIFFSetField(tiff, TIFFTAG_DEFAULTSCALE, default_scale.data()));
    set(TIFFSetField(tiff, TIFFTAG_DEFAULTCROPORIGIN, default_crop_origin.data()));
    set(TIFFSetField(tiff, TIFFTAG_DEFAULTCROPSIZE, default_crop_size.data()));
    set(TIFFSetField(tiff, TIFFTAG_COLORMATRIX1, static_cast<std::uint16_t>(color_matrix.size()),
                     color_matrix.data()));
    set(TIFFSetField(tiff, TIFFTAG_CALIBRATIONILLUMINANT1, 21U));
    set(TIFFSetField(tiff, TIFFTAG_ASSHOTNEUTRAL,
                     static_cast<std::uint16_t>(as_shot_neutral.size()), as_shot_neutral.data()));
    set(TIFFSetField(tiff, TIFFTAG_ACTIVEAREA, active_area.data()));
    if (!list2.empty())
    {
        set(TIFFSetField(tiff, TIFFTAG_OPCODELIST2, static_cast<std::uint32_t>(list2.size()),
                         list2.data()));
    }
    if (!list3.empty())
    {
        set(TIFFSetField(tiff, TIFFTAG_OPCODELIST3, static_cast<std::uint32_t>(list3.size()),
                         list3.data()));
    }
    std::array<std::uint16_t, width> row{};
    for (std::uint32_t y = 0U; ok && y < height; ++y)
    {
        for (std::uint32_t x = 0U; x < width; ++x)
        {
            row[x] = static_cast<std::uint16_t>(200U + y * width + x);
        }
        ok = TIFFWriteScanline(tiff, row.data(), y, 0U) >= 0;
    }
    TIFFClose(tiff);
    return ok;
}

[[nodiscard]] std::uint64_t file_hash(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    std::uint64_t hash = 1469598103934665603ULL;
    char value = 0;
    while (input.get(value))
    {
        hash ^= static_cast<std::uint8_t>(value);
        hash *= 1099511628211ULL;
    }
    return hash;
}

class SyntheticDngDirectory
{
public:
    SyntheticDngDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("ravo-dng-opcodes-" + generate_catalog_id()))
    {
        std::filesystem::create_directories(path_);
    }

    ~SyntheticDngDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void ensure_qt_core()
{
    static const bool logging = []
    {
        init_logging("ravo-dng-opcode-tests");
        return true;
    }();
    static_cast<void>(logging);
    if (QCoreApplication::instance() != nullptr)
    {
        return;
    }
    static int argc = 1;
    static char executable[] = "ravo-dng-opcode-tests";
    static char *argv[] = {executable, nullptr};
    static auto *application = new QCoreApplication(argc, argv);
    static_cast<void>(application);
}

TEST(DngOpcodeTest, LibRawOwnsAndExecutesSyntheticFileOpcodesWithoutSourceMutation)
{
    std::vector<TestOpcode> gain_maps;
    for (std::uint32_t parity = 0U; parity < 4U; ++parity)
    {
        gain_maps.push_back(
            {9U, 0U,
             gain_map_payload(parity / 2U, parity % 2U, 32U, 32U, {1.0F, 1.0F, 1.0F, 1.0F})});
    }
    const auto list2 = opcode_list(gain_maps);
    const auto list3 = opcode_list({{1U, 0U, warp_payload()}});
    SyntheticDngDirectory directory;
    const auto path = directory.path() / "valid.dng";
    ASSERT_TRUE(write_synthetic_dng(path, list2, list3));
    const auto source_size = std::filesystem::file_size(path);
    const auto source_hash = file_hash(path);

    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    auto inspection = engine.value().inspect(path.string(), CancellationToken{});
    ASSERT_TRUE(inspection) << inspection.error().message;
    EXPECT_TRUE(inspection.value().is_raw);
    EXPECT_TRUE(inspection.value().dng_opcode_list2_present);
    EXPECT_TRUE(inspection.value().dng_opcode_list3_present);
    EXPECT_EQ(inspection.value().dng_gain_map_count, 4U);
    EXPECT_TRUE(inspection.value().dng_has_warp_rectilinear);

    auto decoded = engine.value().decode_raw_frame(path.string(), CancellationToken{});
    ASSERT_TRUE(decoded) << decoded.error().message;
    ASSERT_NE(decoded.value().dng_opcodes, nullptr);
    EXPECT_EQ(dng_gain_map_count(*decoded.value().dng_opcodes), 4U);
    const auto decoded_pixels = decoded.value().pixels;
    auto working = working_from_raw(decoded.value(), decoded.value().width, decoded.value().height,
                                    {1.0F, 1.0F, 1.0F, 1.0F}, CancellationToken{});
    ASSERT_TRUE(working) << working.error().message;
    EXPECT_FALSE(working.value().rgb.empty());
    EXPECT_EQ(decoded.value().pixels, decoded_pixels);
    EXPECT_EQ(std::filesystem::file_size(path), source_size);
    EXPECT_EQ(file_hash(path), source_hash);
}

TEST(DngOpcodeTest, LibRawRejectsSyntheticFileWithUnknownMandatoryOpcode)
{
    const auto list2 = opcode_list({{42U, 0U, {1U, 2U, 3U}}});
    SyntheticDngDirectory directory;
    const auto path = directory.path() / "unsupported.dng";
    ASSERT_TRUE(write_synthetic_dng(path, list2, {}));
    const auto source_hash = file_hash(path);

    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    auto inspection = engine.value().inspect(path.string(), CancellationToken{});
    ASSERT_FALSE(inspection);
    EXPECT_EQ(inspection.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(inspection.error().context.at("reason"), "unsupported_mandatory_dng_opcode");
    EXPECT_EQ(file_hash(path), source_hash);
}

TEST(DngOpcodeTest, CliInspectAndRenderExposeSyntheticFileCorrections)
{
    std::vector<TestOpcode> gain_maps;
    for (std::uint32_t parity = 0U; parity < 4U; ++parity)
    {
        gain_maps.push_back(
            {9U, 0U,
             gain_map_payload(parity / 2U, parity % 2U, 32U, 32U, {1.0F, 1.0F, 1.0F, 1.0F})});
    }
    SyntheticDngDirectory directory;
    const auto input = directory.path() / "cli.dng";
    const auto recipe_path = directory.path() / "recipe.json";
    const auto output = directory.path() / "render.png";
    ASSERT_TRUE(write_synthetic_dng(input, opcode_list(gain_maps),
                                    opcode_list({{1U, 0U, warp_payload()}})));
    const auto source_hash = file_hash(input);

    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;
    std::ostringstream stdout_stream;
    std::ostringstream stderr_stream;
    const CliApplication application(engine.value(), stdout_stream, stderr_stream);
    const auto input_text = input.string();
    ASSERT_EQ(application.run(std::vector<std::string_view>{"inspect", input_text, "--json"}), 0)
        << stderr_stream.str();
    auto response = parse_json(stdout_stream.str());
    ASSERT_TRUE(response) << response.error().message;
    const auto *data = response.value().find("data");
    ASSERT_NE(data, nullptr);
    const auto *gain_count = data->find("dng_gain_map_count");
    const auto *has_warp = data->find("dng_has_warp_rectilinear");
    ASSERT_NE(gain_count, nullptr);
    ASSERT_NE(gain_count->number_if(), nullptr);
    EXPECT_EQ(gain_count->number_if()->text, "4");
    ASSERT_NE(has_warp, nullptr);
    ASSERT_NE(has_warp->boolean_if(), nullptr);
    EXPECT_TRUE(*has_warp->boolean_if());

    auto recipe = recipe_from_develop({"synthetic-dng", "file:///placeholder.dng", std::nullopt},
                                      DevelopParams{});
    ASSERT_TRUE(recipe) << recipe.error().message;
    auto serialized = serialize_recipe(recipe.value());
    ASSERT_TRUE(serialized) << serialized.error().message;
    {
        std::ofstream stream(recipe_path, std::ios::binary);
        ASSERT_TRUE(stream);
        stream << serialized.value();
    }
    stdout_stream.str({});
    stdout_stream.clear();
    stderr_stream.str({});
    stderr_stream.clear();
    const auto recipe_text = recipe_path.string();
    const auto output_text = output.string();
    ASSERT_EQ(application.run(std::vector<std::string_view>{
                  "render", input_text, "--recipe", recipe_text, "--output", output_text, "--width",
                  "32", "--height", "32", "--json"}),
              0)
        << stderr_stream.str();
    EXPECT_TRUE(std::filesystem::exists(output));
    EXPECT_GT(std::filesystem::file_size(output), 0U);
    EXPECT_EQ(file_hash(input), source_hash);
}

TEST(DngOpcodeTest, CatalogPreviewReopenAndExportPreserveSyntheticDngSource)
{
    ensure_qt_core();
    std::vector<TestOpcode> gain_maps;
    for (std::uint32_t parity = 0U; parity < 4U; ++parity)
    {
        gain_maps.push_back(
            {9U, 0U,
             gain_map_payload(parity / 2U, parity % 2U, 32U, 32U, {1.0F, 1.0F, 1.0F, 1.0F})});
    }
    SyntheticDngDirectory directory;
    const auto input = directory.path() / "catalog.dng";
    const auto database = (directory.path() / "library.sqlite").string();
    const auto output = directory.path() / "export.png";
    ASSERT_TRUE(write_synthetic_dng(input, opcode_list(gain_maps),
                                    opcode_list({{1U, 0U, warp_payload()}})));
    const auto source_hash = file_hash(input);
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine) << engine.error().message;

    auto make_service = [&](const bool create) -> Result<std::unique_ptr<CatalogService>>
    {
        auto repository = create ? SqliteCatalogRepository::create(database) :
                                   SqliteCatalogRepository::open(database);
        if (!repository)
        {
            return repository.error();
        }
        auto cache = FilesystemPreviewCache::create(database + ".preview");
        if (!cache)
        {
            return cache.error();
        }
        auto recovery = FilesystemRecoveryStore::create_for_catalog(database);
        if (!recovery)
        {
            return recovery.error();
        }
        return std::make_unique<CatalogService>(
            engine.value(), std::move(repository).value(), std::make_unique<QtRasterDecoder>(),
            std::move(cache).value(), std::move(recovery).value());
    };

    auto service_result = make_service(true);
    ASSERT_TRUE(service_result) << service_result.error().message;
    auto service = std::move(service_result).value();
    auto imported = service->import_one(input.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset)
        << (imported.value().error ? imported.value().error->message : "");
    const auto asset_id = imported.value().asset->id;
    PreviewRequest request;
    request.asset_id = asset_id;
    request.max_edge = 32U;
    request.request_revision = 1U;
    auto preview = service->request_preview(request);
    ASSERT_TRUE(preview) << preview.error().message;
    EXPECT_FALSE(preview.value().cache_path.empty());
    EXPECT_TRUE(std::filesystem::exists(preview.value().cache_path));
    ASSERT_TRUE(service->close());
    service.reset();

    service_result = make_service(false);
    ASSERT_TRUE(service_result) << service_result.error().message;
    service = std::move(service_result).value();
    request.request_revision = 2U;
    preview = service->request_preview(request);
    ASSERT_TRUE(preview) << preview.error().message;
    EXPECT_FALSE(preview.value().cache_path.empty());
    EXPECT_TRUE(std::filesystem::exists(preview.value().cache_path));
    ExportRequest export_request;
    export_request.asset_id = asset_id;
    export_request.output_path = output.string();
    export_request.format = ExportFormat::kPng;
    export_request.max_edge = 32U;
    auto exported = service->export_asset(export_request);
    ASSERT_TRUE(exported) << exported.error().message;
    EXPECT_TRUE(std::filesystem::exists(output));
    EXPECT_EQ(file_hash(input), source_hash);
    ASSERT_TRUE(service->close());
}

TEST(DngOpcodeTest, ParsesOwnedFourParityGainMapsAndInterpolates)
{
    std::vector<TestOpcode> operations;
    for (std::uint32_t parity = 0U; parity < 4U; ++parity)
    {
        const float gain = static_cast<float>(parity + 1U);
        operations.push_back(
            {9U, 0U, gain_map_payload(parity / 2U, parity % 2U, 4U, 4U, {gain, gain, gain, gain})});
    }
    auto bytes = opcode_list(operations);
    auto parsed = parse_dng_opcode_metadata({true, bytes}, {}, 4U, 4U);
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_NE(parsed.value(), nullptr);
    EXPECT_TRUE(parsed.value()->list2_present);
    EXPECT_FALSE(parsed.value()->list3_present);
    EXPECT_EQ(dng_gain_map_count(*parsed.value()), 4U);
    EXPECT_FLOAT_EQ(apply_dng_opcode_list2_sample(*parsed.value(), 0U, 0U, 4U, 4U, 0.2F), 0.2F);
    EXPECT_FLOAT_EQ(apply_dng_opcode_list2_sample(*parsed.value(), 1U, 0U, 4U, 4U, 0.2F), 0.4F);
    EXPECT_FLOAT_EQ(apply_dng_opcode_list2_sample(*parsed.value(), 0U, 1U, 4U, 4U, 0.2F), 0.6F);
    EXPECT_FLOAT_EQ(apply_dng_opcode_list2_sample(*parsed.value(), 3U, 3U, 4U, 4U, 0.2F), 0.8F);

    std::fill(bytes.begin(), bytes.end(), 0U);
    EXPECT_FLOAT_EQ(apply_dng_opcode_list2_sample(*parsed.value(), 1U, 0U, 4U, 4U, 0.2F), 0.4F);
    EXPECT_GT(estimate_dng_opcode_memory(*parsed.value()), sizeof(DngOpcodeMetadata));
}

TEST(DngOpcodeTest, GainMapsRunAfterBlackNormalizationBeforeDemosaic)
{
    std::vector<TestOpcode> operations;
    for (std::uint32_t parity = 0U; parity < 4U; ++parity)
    {
        const float gain = static_cast<float>(parity + 1U);
        operations.push_back(
            {9U, 0U, gain_map_payload(parity / 2U, parity % 2U, 4U, 4U, {gain, gain, gain, gain})});
    }
    const auto list2 = opcode_list(operations);
    auto metadata = parse_dng_opcode_metadata({true, list2}, {}, 4U, 4U);
    ASSERT_TRUE(metadata) << metadata.error().message;

    DecodedRaw raw;
    raw.width = 4U;
    raw.height = 4U;
    raw.cfa_width = 2U;
    raw.cfa_height = 2U;
    raw.cfa_channels = {0U, 1U, 1U, 2U};
    raw.black_level = 100;
    raw.white_level = 1100U;
    raw.pixels.resize(16U);
    for (std::size_t index = 0U; index < raw.pixels.size(); ++index)
    {
        raw.pixels[index] = static_cast<std::uint16_t>(200U + index * 10U);
    }
    raw.color_profile.kind = ColorProfileKind::kMatrix;
    raw.color_profile.model = ColorModel::kRgb;
    raw.color_profile.identifier = "camera";
    raw.dng_opcodes = metadata.value();
    const auto source_pixels = raw.pixels;

    auto working = working_from_raw(raw, 4U, 4U, {1.0F, 1.0F, 1.0F, 1.0F}, CancellationToken{});
    ASSERT_TRUE(working) << working.error().message;
    ASSERT_EQ(working.value().rgb.size(), 48U);

    for (std::uint32_t y = 0U; y < 4U; ++y)
    {
        for (std::uint32_t x = 0U; x < 4U; ++x)
        {
            const std::uint8_t channel = raw.cfa_channels[(y % 2U) * 2U + (x % 2U)];
            const float normalized =
                (static_cast<float>(raw.pixels[static_cast<std::size_t>(y) * 4U + x]) - 100.0F) /
                1000.0F;
            const float gain = static_cast<float>((y & 1U) * 2U + (x & 1U) + 1U);
            const std::size_t base = (static_cast<std::size_t>(y) * 4U + x) * 3U;
            EXPECT_FLOAT_EQ(working.value().rgb[base + channel], normalized * gain)
                << x << ',' << y;
        }
    }
    EXPECT_EQ(raw.pixels, source_pixels);
    EXPECT_EQ(raw.dng_opcodes, metadata.value());
}

TEST(DngOpcodeTest, AcceptsPartialMapsAndRejectsUnsafeOrOutOfBoundsGeometry)
{
    const auto single =
        opcode_list({{9U, 0U, gain_map_payload(0U, 0U, 4U, 4U, {2.0F, 2.0F, 2.0F, 2.0F})}});
    auto partial = parse_dng_opcode_metadata({true, single}, {}, 4U, 4U);
    ASSERT_TRUE(partial) << partial.error().message;
    EXPECT_EQ(dng_gain_map_count(*partial.value()), 1U);
    EXPECT_FLOAT_EQ(apply_dng_opcode_list2_sample(*partial.value(), 0U, 0U, 4U, 4U, 0.25F), 0.5F);
    EXPECT_FLOAT_EQ(apply_dng_opcode_list2_sample(*partial.value(), 1U, 0U, 4U, 4U, 0.25F), 0.25F);

    std::vector<TestOpcode> operations;
    for (std::uint32_t parity = 0U; parity < 4U; ++parity)
    {
        auto payload = gain_map_payload(parity / 2U, parity % 2U, 4U, 4U, {1.0F, 1.0F, 1.0F, 1.0F});
        if (parity == 3U)
        {
            const float invalid = std::numeric_limits<float>::infinity();
            const auto bits = std::bit_cast<std::uint32_t>(invalid);
            payload[payload.size() - 4U] = static_cast<std::uint8_t>(bits >> 24U);
            payload[payload.size() - 3U] = static_cast<std::uint8_t>(bits >> 16U);
            payload[payload.size() - 2U] = static_cast<std::uint8_t>(bits >> 8U);
            payload[payload.size() - 1U] = static_cast<std::uint8_t>(bits);
        }
        operations.push_back({9U, 0U, std::move(payload)});
    }
    const auto unsafe = opcode_list(operations);
    auto non_finite = parse_dng_opcode_metadata({true, unsafe}, {}, 4U, 4U);
    ASSERT_FALSE(non_finite);
    EXPECT_EQ(non_finite.error().context.at("reason"), "invalid_dng_gain_value");

    operations.clear();
    for (std::uint32_t parity = 0U; parity < 4U; ++parity)
    {
        operations.push_back(
            {9U, 0U, gain_map_payload(parity / 2U, parity % 2U, 5U, 4U, {1.0F, 1.0F, 1.0F, 1.0F})});
    }
    const auto wrong_width = opcode_list(operations);
    auto mismatched = parse_dng_opcode_metadata({true, wrong_width}, {}, 4U, 4U);
    ASSERT_FALSE(mismatched);
    EXPECT_EQ(mismatched.error().context.at("reason"), "unsupported_dng_gain_map_geometry");
}

TEST(DngOpcodeTest, FailsUnknownMandatoryAndRecordsUnknownOptional)
{
    const auto mandatory = opcode_list({{42U, 0U, {1U, 2U, 3U}}});
    auto rejected = parse_dng_opcode_metadata({true, mandatory}, {}, 4U, 4U);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(rejected.error().context.at("reason"), "unsupported_mandatory_dng_opcode");
    EXPECT_EQ(rejected.error().context.at("dng_opcode_id"), "42");

    const auto optional = opcode_list({{42U, 1U, {1U, 2U, 3U}}});
    auto parsed = parse_dng_opcode_metadata({true, optional}, {}, 4U, 4U);
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_NE(parsed.value(), nullptr);
    ASSERT_EQ(parsed.value()->skipped_optional.size(), 1U);
    EXPECT_EQ(parsed.value()->skipped_optional.front().list, 2U);
    EXPECT_EQ(parsed.value()->skipped_optional.front().id, 42U);
    EXPECT_EQ(dng_gain_map_count(*parsed.value()), 0U);
}

TEST(DngOpcodeTest, RejectsUnknownRequiredSemanticsAndSkipsThemOnlyWhenOptional)
{
    const auto future_required = opcode_list({{1U, 0U, warp_payload(), 0x02000000U}});
    auto parsed = parse_dng_opcode_metadata({}, {true, future_required}, 4U, 4U);
    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error().context.at("reason"), "unsupported_mandatory_dng_opcode");

    const auto future_optional = opcode_list({{1U, 1U, warp_payload(), 0x02000000U}});
    parsed = parse_dng_opcode_metadata({}, {true, future_optional}, 4U, 4U);
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_EQ(parsed.value()->skipped_optional.size(), 1U);
    EXPECT_TRUE(parsed.value()->list3_operations.empty());

    const auto unknown_flag_required = opcode_list({{1U, 4U, warp_payload()}});
    parsed = parse_dng_opcode_metadata({}, {true, unknown_flag_required}, 4U, 4U);
    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error().context.at("reason"), "unsupported_mandatory_dng_opcode");
}

TEST(DngOpcodeTest, RejectsTruncatedEnvelopeAndTrailingBytes)
{
    std::vector<std::uint8_t> truncated;
    append_u32(truncated, 1U);
    append_u32(truncated, 9U);
    auto parsed = parse_dng_opcode_metadata({true, truncated}, {}, 4U, 4U);
    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error().context.at("reason"), "invalid_dng_opcode_count");

    auto trailing = opcode_list({});
    trailing.push_back(0U);
    parsed = parse_dng_opcode_metadata({true, trailing}, {}, 4U, 4U);
    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error().context.at("reason"), "dng_opcode_trailing_bytes");
}

TEST(DngOpcodeTest, AppliesIdentityWarpAndVignetteInDeclaredOrder)
{
    const auto list3 = opcode_list({{3U, 0U, vignette_payload()}, {1U, 0U, warp_payload()}});
    auto parsed = parse_dng_opcode_metadata({}, {true, list3}, 3U, 3U);
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_NE(parsed.value(), nullptr);
    ASSERT_EQ(parsed.value()->list3_operations.size(), 2U);
    EXPECT_TRUE(
        std::holds_alternative<DngFixVignetteRadial>(parsed.value()->list3_operations.front()));
    EXPECT_TRUE(
        std::holds_alternative<DngWarpRectilinear>(parsed.value()->list3_operations.back()));

    const WorkingImage source = test_image();
    auto corrected = apply_dng_opcode_list3(source, *parsed.value(), CancellationToken{});
    ASSERT_TRUE(corrected) << corrected.error().message;
    EXPECT_EQ(corrected.value().rgb, source.rgb);
    EXPECT_EQ(corrected.value().color_profile.identifier, source.color_profile.identifier);
    EXPECT_EQ(source.rgb, test_image().rgb);
}

TEST(DngOpcodeTest, PreservesRepeatedOperationsAndAppliesList3RgbGainMapsInOrder)
{
    const std::array<float, 8> gains{2.0F, 3.0F, 2.0F, 3.0F, 2.0F, 3.0F, 2.0F, 3.0F};
    const auto gain = gain_map_payload_custom(0U, 0U, 3U, 3U, 1U, 2U, 1U, 1U, 2U, 2U, 2U, gains);
    const auto list3 =
        opcode_list({{3U, 0U, vignette_payload()}, {3U, 0U, vignette_payload()}, {9U, 0U, gain}});
    auto parsed = parse_dng_opcode_metadata({}, {true, list3}, 3U, 3U);
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_EQ(parsed.value()->list3_operations.size(), 3U);
    EXPECT_EQ(dng_gain_map_count(*parsed.value()), 1U);

    WorkingImage source = test_image();
    std::fill(source.rgb.begin(), source.rgb.end(), 0.2F);
    auto corrected = apply_dng_opcode_list3(source, *parsed.value(), CancellationToken{});
    ASSERT_TRUE(corrected) << corrected.error().message;
    for (std::size_t index = 0U; index < corrected.value().rgb.size(); index += 3U)
    {
        EXPECT_FLOAT_EQ(corrected.value().rgb[index], 0.2F);
        EXPECT_FLOAT_EQ(corrected.value().rgb[index + 1U], 0.4F);
        EXPECT_FLOAT_EQ(corrected.value().rgb[index + 2U], 0.6F);
    }
}

TEST(DngOpcodeTest, AppliesList3BeforeWhiteBalanceToPreserveCorrectedHeadroom)
{
    const auto list3 = opcode_list({{3U, 0U, vignette_payload({1.0, 0.0, 0.0, 0.0, 0.0})}});
    auto metadata = parse_dng_opcode_metadata({}, {true, list3}, 3U, 3U);
    ASSERT_TRUE(metadata) << metadata.error().message;

    DecodedRaw raw;
    raw.width = 3U;
    raw.height = 3U;
    raw.cfa_width = 2U;
    raw.cfa_height = 2U;
    raw.cfa_channels = {0U, 1U, 1U, 2U};
    raw.black_level = 100;
    raw.white_level = 1100U;
    raw.pixels.assign(9U, 500U);
    raw.color_profile.kind = ColorProfileKind::kMatrix;
    raw.color_profile.model = ColorModel::kRgb;
    raw.color_profile.identifier = "camera";
    raw.dng_opcodes = metadata.value();

    auto working = working_from_raw(raw, 3U, 3U, {2.0F, 1.0F, 1.0F, 1.0F}, CancellationToken{});
    ASSERT_TRUE(working) << working.error().message;
    EXPECT_FLOAT_EQ(working.value().rgb[0], 1.6F);
    EXPECT_FLOAT_EQ(working.value().rgb[1], 0.8F);
    EXPECT_FLOAT_EQ(working.value().rgb[2], 0.8F);
    const std::size_t center = (1U * 3U + 1U) * 3U;
    EXPECT_FLOAT_EQ(working.value().rgb[center], 0.8F);
    EXPECT_FLOAT_EQ(working.value().rgb[center + 1U], 0.4F);
}

TEST(DngOpcodeTest, RejectsNonMonotonicWarpAtTheParserBoundary)
{
    const auto list3 = opcode_list({{1U, 0U, warp_payload({1.0, -1.0, 0.0, 0.0, 0.0, 0.0})}});
    auto parsed = parse_dng_opcode_metadata({}, {true, list3}, 8U, 8U);
    ASSERT_FALSE(parsed);
    EXPECT_EQ(parsed.error().context.at("reason"), "non_monotonic_dng_warp");
}

TEST(DngOpcodeTest, VignetteUsesLastPixelRadiusAndClipsTheDngLogicalRange)
{
    const auto list3 = opcode_list({{3U, 0U, vignette_payload({1.0, 0.0, 0.0, 0.0, 0.0})}});
    auto parsed = parse_dng_opcode_metadata({}, {true, list3}, 3U, 3U);
    ASSERT_TRUE(parsed) << parsed.error().message;
    WorkingImage source = test_image();
    std::fill(source.rgb.begin(), source.rgb.end(), 0.4F);
    auto corrected = apply_dng_opcode_list3(source, *parsed.value(), CancellationToken{});
    ASSERT_TRUE(corrected) << corrected.error().message;
    EXPECT_FLOAT_EQ(corrected.value().rgb[0], 0.8F);
    const std::size_t center = (1U * 3U + 1U) * 3U;
    EXPECT_FLOAT_EQ(corrected.value().rgb[center], 0.4F);
    EXPECT_EQ(source.rgb.front(), 0.4F);

    std::fill(source.rgb.begin(), source.rgb.end(), 1.0F);
    corrected = apply_dng_opcode_list3(source, *parsed.value(), CancellationToken{});
    ASSERT_TRUE(corrected) << corrected.error().message;
    EXPECT_FLOAT_EQ(corrected.value().rgb[0], 1.0F);
}

TEST(DngOpcodeTest, WarpFailureAndCancellationPublishNothing)
{
    const auto list3 = opcode_list({{1U, 0U, warp_payload({2.0, 0.0, 0.0, 0.0, 0.0, 0.0})}});
    auto parsed = parse_dng_opcode_metadata({}, {true, list3}, 8U, 8U);
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_EQ(parsed.value()->list3_operations.size(), 1U);
    EXPECT_TRUE(
        std::holds_alternative<DngWarpRectilinear>(parsed.value()->list3_operations.front()));
    const WorkingImage source = test_image(8U, 8U);
    const auto source_pixels = source.rgb;
    auto corrected = apply_dng_opcode_list3(source, *parsed.value(), CancellationToken{});
    ASSERT_TRUE(corrected) << corrected.error().message;
    EXPECT_EQ(corrected.value().rgb, source_pixels);
    EXPECT_EQ(source.rgb, source_pixels);

    CancellationSource cancellation;
    ASSERT_TRUE(cancellation.cancel("test cancellation"));
    corrected = apply_dng_opcode_list3(source, *parsed.value(), cancellation.token());
    ASSERT_FALSE(corrected);
    EXPECT_EQ(corrected.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(source.rgb, source_pixels);
}

} // namespace
} // namespace ravo
