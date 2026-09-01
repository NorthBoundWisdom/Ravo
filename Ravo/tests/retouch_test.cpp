#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "ravo/adapters/legacy_xmp.h"
#include "ravo/recipe/color_input.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/operation.h"
#include "ravo/recipe/retouch.h"

#include "retouch.h"

namespace ravo
{
namespace
{

[[nodiscard]] std::filesystem::path repository_root()
{
    return std::filesystem::path(RAVO_REPOSITORY_ROOT);
}

[[nodiscard]] std::optional<std::string> read_file(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

[[nodiscard]] std::string focused_retouch_xmp(const std::string &document)
{
    const auto masks_begin = document.find("<darktable:masks_history>");
    const auto masks_end = document.find("</darktable:masks_history>", masks_begin);
    EXPECT_NE(masks_begin, std::string::npos);
    EXPECT_NE(masks_end, std::string::npos);
    std::string entries;
    std::size_t search = 0U;
    while ((search = document.find("darktable:operation=\"retouch\"", search)) !=
           std::string::npos)
    {
        const auto begin = document.rfind("<rdf:li", search);
        const auto end = document.find("/>", search);
        EXPECT_NE(begin, std::string::npos);
        EXPECT_NE(end, std::string::npos);
        entries.append(document, begin, end + 2U - begin);
        search = end + 2U;
    }
    std::string result = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
<rdf:Description darktable:xmp_version="6">)";
    result.append(document, masks_begin,
                  masks_end + std::string("</darktable:masks_history>").size() - masks_begin);
    result += "<darktable:history><rdf:Seq>" + entries +
              "</rdf:Seq></darktable:history></rdf:Description></rdf:RDF>";
    return result;
}

[[nodiscard]] WorkingImage gradient_image(const std::uint32_t width, const std::uint32_t height)
{
    WorkingImage image;
    image.width = width;
    image.height = height;
    image.color_profile.kind = ColorProfileKind::kBuiltin;
    image.color_profile.model = ColorModel::kRgb;
    image.color_profile.identifier = std::string(kInputProfileLinearRec709);
    image.canonical_roi_scale =
        CanonicalRoiScale::from_scaled_dimensions(width, height, width, height);
    image.rgb.resize(static_cast<std::size_t>(width) * height * 3U);
    for (std::uint32_t y = 0U; y < height; ++y)
    {
        for (std::uint32_t x = 0U; x < width; ++x)
        {
            const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 3U;
            image.rgb[index] = 0.05F * static_cast<float>(x) + 0.01F * static_cast<float>(y);
            image.rgb[index + 1U] = 0.03F * static_cast<float>(x) + 0.02F;
            image.rgb[index + 2U] = 0.04F * static_cast<float>(y) + 0.01F;
        }
    }
    return image;
}

[[nodiscard]] Mask circle_mask(std::string id, const double x, const double y,
                               const double radius, const double feather = 0.0)
{
    Mask mask{std::move(id), kCanonicalMaskSchemaVersion, MaskKind::kCircle};
    mask.payload = CircleMask{x, y, radius, feather};
    return mask;
}

[[nodiscard]] OperationInstance operation_for(const RetouchParams &params)
{
    return {std::string(kRetouchOperationId), kRetouchOperationSchemaVersion, "retouch-test", true,
            retouch_to_parameters(params), std::nullopt};
}

TEST(RetouchRecipeTest, StrictNestedSchemaRoundTripsAndDevelopPreservesMasks)
{
    RetouchParams params;
    params.num_scales = 3;
    params.merge_from_scale = 2;
    params.regions.push_back({"spot", RetouchMode::kHeal, 1, 0.75, 0.2, 0.3,
                              RetouchBlurType::kGaussian, 12.0, RetouchFillMode::kErase,
                              {0.0, 0.0, 0.0}, 0.1});
    const auto encoded = retouch_to_parameters(params);
    auto decoded = retouch_from_parameters(encoded);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded.value(), params);

    Recipe recipe;
    recipe.asset = {"asset", "file:///fixture.raw", std::nullopt};
    recipe.masks.push_back(circle_mask("spot", 0.5, 0.5, 0.2));
    recipe.operations.push_back(operation_for(params));
    const auto registry = make_phase1_registry();
    ASSERT_TRUE(registry) << registry.error().message;
    ASSERT_TRUE(validate_recipe(recipe, registry.value()));

    auto develop = develop_from_recipe(recipe);
    ASSERT_TRUE(develop) << develop.error().message;
    EXPECT_EQ(develop.value().retouch, params);
    auto round_trip = recipe_from_develop(recipe.asset, develop.value());
    ASSERT_TRUE(round_trip) << round_trip.error().message;
    ASSERT_TRUE(validate_recipe(round_trip.value(), registry.value()));
    const auto found = std::find_if(round_trip.value().operations.begin(),
                                    round_trip.value().operations.end(),
                                    [](const OperationInstance &operation)
                                    { return operation.id == kRetouchOperationId; });
    ASSERT_NE(found, round_trip.value().operations.end());
    EXPECT_EQ(retouch_from_parameters(found->parameters).value(), params);
    auto serialized = serialize_recipe(round_trip.value());
    ASSERT_TRUE(serialized) << serialized.error().message;
    auto reparsed = parse_recipe_json(serialized.value());
    ASSERT_TRUE(reparsed) << reparsed.error().message;
    ASSERT_TRUE(validate_recipe(reparsed.value(), registry.value()));
    EXPECT_EQ(reparsed.value().masks, round_trip.value().masks);
    const auto reparsed_operation =
        std::find_if(reparsed.value().operations.begin(), reparsed.value().operations.end(),
                     [](const OperationInstance &operation)
                     { return operation.id == kRetouchOperationId; });
    ASSERT_NE(reparsed_operation, reparsed.value().operations.end());
    EXPECT_EQ(retouch_from_parameters(reparsed_operation->parameters).value(), params);

    auto unknown = encoded;
    auto &regions = std::get<ParameterValue::Array>(unknown.at("regions").value);
    std::get<ParameterValue::Object>(regions.front().value).emplace("future", ParameterValue{1.0});
    EXPECT_FALSE(retouch_from_parameters(unknown));
    auto duplicate = params;
    duplicate.regions.push_back(params.regions.front());
    EXPECT_FALSE(validate_retouch_operation(operation_for(duplicate), recipe.masks));
    EXPECT_FALSE(validate_retouch_operation(operation_for(params), {}));
}

TEST(RetouchTest, LaterCloneReadsAnEarlierFilledRegionAndOutsidePixelsStayBitExact)
{
    constexpr std::uint32_t width = 9U;
    constexpr std::uint32_t height = 9U;
    constexpr std::uint32_t source_x = 2U;
    constexpr std::uint32_t target_x = 6U;
    constexpr std::uint32_t row = 4U;
    const WorkingImage input = gradient_image(width, height);
    Recipe recipe;
    recipe.asset = {"asset", "file:///fixture.raw", std::nullopt};
    recipe.masks = {
        circle_mask("source", (source_x + 0.5) / width, (row + 0.5) / height, 0.04),
        circle_mask("target", (target_x + 0.5) / width, (row + 0.5) / height, 0.04)};
    RetouchParams params;
    params.regions = {
        {"source", RetouchMode::kFill, 0, 1.0, 0.5, 0.5, RetouchBlurType::kGaussian, 10.0,
         RetouchFillMode::kColor, {0.8, 0.3, 0.1}, 0.0},
        {"target", RetouchMode::kClone, 0, 1.0, (source_x + 0.5) / width,
         (row + 0.5) / height, RetouchBlurType::kGaussian, 10.0,
         RetouchFillMode::kErase, {}, 0.0},
    };
    recipe.operations.push_back(operation_for(params));
    auto output = apply_retouch(input, recipe, recipe.operations.front(), CancellationToken{});
    ASSERT_TRUE(output) << output.error().message;
    const std::size_t source = (static_cast<std::size_t>(row) * width + source_x) * 3U;
    const std::size_t target = (static_cast<std::size_t>(row) * width + target_x) * 3U;
    constexpr std::array<float, 3> fill{0.8F, 0.3F, 0.1F};
    for (std::size_t channel = 0U; channel < 3U; ++channel)
    {
        EXPECT_FLOAT_EQ(output.value().rgb[source + channel], fill[channel]);
        EXPECT_FLOAT_EQ(output.value().rgb[target + channel],
                        output.value().rgb[source + channel]);
    }
    EXPECT_EQ(output.value().rgb.front(), input.rgb.front());
    EXPECT_EQ(output.value().rgb.back(), input.rgb.back());
}

TEST(RetouchTest, CloneFillBlurHealAndWaveletRegionsAreOrderedAndBounded)
{
    const WorkingImage input = gradient_image(17U, 13U);
    Recipe recipe;
    recipe.asset = {"asset", "file:///fixture.raw", std::nullopt};
    recipe.masks = {circle_mask("clone", 0.50, 0.50, 0.12),
                    circle_mask("fill", 0.75, 0.50, 0.10),
                    circle_mask("blur", 0.25, 0.50, 0.13, 0.05),
                    circle_mask("heal", 0.50, 0.75, 0.10),
                    circle_mask("wavelet", 0.50, 0.25, 0.11)};
    RetouchParams params;
    params.num_scales = 2;
    params.regions = {
        {"clone", RetouchMode::kClone, 0, 1.0, 0.30, 0.50, RetouchBlurType::kGaussian, 10.0,
         RetouchFillMode::kErase, {}, 0.0},
        {"fill", RetouchMode::kFill, 0, 0.8, 0.5, 0.5, RetouchBlurType::kGaussian, 10.0,
         RetouchFillMode::kColor, {0.9, 0.2, 0.1}, -0.05},
        {"blur", RetouchMode::kBlur, 0, 1.0, 0.5, 0.5, RetouchBlurType::kGaussian, 1.5,
         RetouchFillMode::kErase, {}, 0.0},
        {"heal", RetouchMode::kHeal, 0, 1.0, 0.30, 0.75, RetouchBlurType::kGaussian, 10.0,
         RetouchFillMode::kErase, {}, 0.0},
        {"wavelet", RetouchMode::kFill, 1, 0.5, 0.5, 0.5, RetouchBlurType::kGaussian, 10.0,
         RetouchFillMode::kErase, {}, 0.02},
    };
    recipe.operations.push_back(operation_for(params));
    auto output = apply_retouch(input, recipe, recipe.operations.front(), CancellationToken{});
    ASSERT_TRUE(output) << output.error().message;
    EXPECT_NE(output.value().rgb, input.rgb);
    EXPECT_EQ(output.value().color_profile, input.color_profile);
    EXPECT_EQ(input.rgb.front(), gradient_image(17U, 13U).rgb.front());
    const std::size_t untouched = (12U * 17U + 16U) * 3U;
    EXPECT_EQ(output.value().rgb[untouched], input.rgb[untouched]);

    RetouchParams bilateral_params;
    bilateral_params.regions.push_back(
        {"blur", RetouchMode::kBlur, 0, 1.0, 0.5, 0.5, RetouchBlurType::kBilateral, 1.0,
         RetouchFillMode::kErase, {}, 0.0});
    auto bilateral_operation = operation_for(bilateral_params);
    auto bilateral = apply_retouch(input, recipe, bilateral_operation, CancellationToken{});
    ASSERT_TRUE(bilateral) << bilateral.error().message;
    EXPECT_NE(bilateral.value().rgb, input.rgb);

    CancellationSource cancelled;
    ASSERT_TRUE(cancelled.cancel("retouch-test"));
    const auto rejected = apply_retouch(input, recipe, recipe.operations.front(), cancelled.token());
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, ErrorCode::kCancelled);
    EXPECT_GT(detail::retouch_working_bytes(17U, 13U, params), input.rgb.size() * sizeof(float));
}

TEST(RetouchLegacyXmpTest, FourFrozenFixtureFamiliesHaveOneStrictCanonicalWinner)
{
    const auto root = repository_root() / "Ravo" / "tests" / "fixtures" / "frozen";
    const std::array<std::string_view, 4> names{"0021-retouch-wavelets", "0074-retouch-clone",
                                               "0075-retouch-heal",
                                               "0076-retouch-blur-fill"};
    std::size_t imported = 0U;
    for (const auto name : names)
    {
        SCOPED_TRACE(name);
        const auto directory = root / name;
        const auto stem = directory.filename().string().substr(5U);
        const auto file = directory / (stem + ".xmp");
        const auto xmp = read_file(file);
        ASSERT_TRUE(xmp) << file;
        auto result = import_legacy_xmp({focused_retouch_xmp(*xmp),
                                         {std::string(name), "file:///frozen-retouch.raw",
                                          std::nullopt}});
        ASSERT_TRUE(result) << result.error().message;
        const auto operation = std::find_if(result.value().operations.begin(),
                                            result.value().operations.end(),
                                            [](const OperationInstance &candidate)
                                            { return candidate.id == kRetouchOperationId; });
        ASSERT_NE(operation, result.value().operations.end());
        auto params = retouch_from_parameters(operation->parameters);
        ASSERT_TRUE(params) << params.error().message;
        const std::size_t expected_regions =
            name == std::string_view("0021-retouch-wavelets") ? 1U :
            name == std::string_view("0076-retouch-blur-fill") ? 5U :
                                                                  4U;
        EXPECT_EQ(params.value().regions.size(), expected_regions);
        EXPECT_EQ(params.value().num_scales,
                  name == std::string_view("0021-retouch-wavelets") ? 6 : 0);
        EXPECT_EQ(result.value().masks.size(), params.value().regions.size());
        ++imported;
    }
    EXPECT_EQ(imported, 4U);

    const auto fixture = read_file(root / "0074-retouch-clone" / "retouch-clone.xmp");
    ASSERT_TRUE(fixture);
    const std::string focused = focused_retouch_xmp(*fixture);
    auto custom_blend = focused;
    const auto blend = custom_blend.find(
        "gz09eJxjYGBgYAFiCQYYOOEEIk8zn46HiTAyEAsa7CF4pPKxg4pZVw6AMC4+IQAAgt0lmA==");
    ASSERT_NE(blend, std::string::npos);
    custom_blend.replace(blend, 4U, "gz00");
    EXPECT_FALSE(import_legacy_xmp(
        {custom_blend, {"asset", "file:///fixture.raw", std::nullopt}}));

    auto unknown_attribute = focused;
    const auto operation = unknown_attribute.find("darktable:operation=\"retouch\"");
    ASSERT_NE(operation, std::string::npos);
    unknown_attribute.insert(operation, "darktable:future=\"1\" ");
    const auto unknown = import_legacy_xmp(
        {unknown_attribute, {"asset", "file:///fixture.raw", std::nullopt}});
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().context.at("reason"), "unsupported_legacy_retouch_attribute");

    auto unknown_mask = focused;
    const auto version = unknown_mask.find("darktable:mask_version=\"6\"");
    ASSERT_NE(version, std::string::npos);
    unknown_mask.replace(version, std::string("darktable:mask_version=\"6\"").size(),
                         "darktable:mask_version=\"5\"");
    const auto rejected_mask = import_legacy_xmp(
        {unknown_mask, {"asset", "file:///fixture.raw", std::nullopt}});
    ASSERT_FALSE(rejected_mask);
    EXPECT_EQ(rejected_mask.error().context.at("reason"), "unsupported_legacy_retouch_mask");
}

} // namespace
} // namespace ravo
