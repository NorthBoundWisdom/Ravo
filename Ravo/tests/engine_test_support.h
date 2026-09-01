#pragma once

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <png.h>

#include "ravo/engine/engine.h"
#include "ravo/recipe/develop.h"

#include "image_ops.h"

namespace ravo::engine_test_support
{

[[nodiscard]] std::string mire1_path();
[[nodiscard]] std::string mire1_xtrans_path();

struct SourceFileSnapshot
{
    std::uintmax_t size = 0U;
    std::filesystem::file_time_type modified;
    std::uint64_t content_hash = 1469598103934665603ULL;

    [[nodiscard]] bool operator==(const SourceFileSnapshot &) const = default;
};

[[nodiscard]] std::optional<SourceFileSnapshot> source_file_snapshot(const std::string &path);

struct DecodedPng
{
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::vector<png_byte> pixels;
};

void declare_srgb(RasterBuffer &raster);
void declare_linear_srgb_matrix(DecodedRaw &raw);
void declare_input(Recipe &recipe);
[[nodiscard]] std::shared_ptr<const ExposureAnalysisContext>
exposure_analysis(std::initializer_list<std::pair<std::uint16_t, std::uint32_t>> bins,
                  std::uint32_t black_level, std::uint32_t white_level,
                  RawExposureMetadata metadata = {});
[[nodiscard]] std::optional<DecodedPng> read_rgb_png(const std::filesystem::path &path);
[[nodiscard]] std::optional<DecodedPng> read_rgb_png(const std::vector<std::uint8_t> &encoded);
[[nodiscard]] std::size_t png_chunk_count(const std::string &png_bytes,
                                          std::string_view chunk_type);
[[nodiscard]] RasterBuffer solid_raster(std::uint32_t width, std::uint32_t height, std::uint8_t r,
                                        std::uint8_t g, std::uint8_t b);
[[nodiscard]] RasterBuffer gradient_raster();
[[nodiscard]] std::uint64_t mean_luma(const RenderedImage &image);
[[nodiscard]] Result<RenderedImage> render_op(const EngineFacade &engine, RasterBuffer raster,
                                              OperationInstance operation);
[[nodiscard]] OperationInstance channel_mixer_operation(const ChannelMixerParams &params,
                                                        std::string instance_id = "calibration-1");
[[nodiscard]] OperationInstance
color_balance_rgb_operation(const ColorBalanceRgbParams &params,
                            std::string instance_id = "colorbalancergb-1");
[[nodiscard]] OperationInstance
legacy_color_balance_operation(const ColorBalanceParams &params,
                               std::string instance_id = "colorbalance-1");
[[nodiscard]] WorkingImage legacy_color_balance_working_fixture();
[[nodiscard]] OperationInstance temperature_operation(const TemperatureParams &params,
                                                      std::string instance_id = "temperature-1");
[[nodiscard]] OperationInstance hot_pixels_operation(bool permissive = false);
[[nodiscard]] OperationInstance raw_ca_operation(std::int64_t iterations = 2,
                                                 bool avoid_color_shift = false);
[[nodiscard]] DecodedRaw synthetic_bayer_raw();

} // namespace ravo::engine_test_support
