#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "lut3d.h"
#include "ravo/adapters/legacy_xmp.h"
#include "ravo/cli/application.h"
#include "ravo/recipe/color_input.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/lut3d.h"

namespace ravo
{
namespace
{
class LutTempDirectory
{
public:
    LutTempDirectory()
    {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("ravo-lut3d-test-" + std::to_string(nonce));
        std::filesystem::create_directories(path_);
    }
    ~LutTempDirectory()
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

void write_text(const std::filesystem::path &path, const std::string_view text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    ASSERT_TRUE(output);
}

[[nodiscard]] std::string identity_cube(const std::string_view header = {})
{
    std::string result(header);
    result += "LUT_3D_SIZE 2\n";
    for (int blue = 0; blue < 2; ++blue)
        for (int green = 0; green < 2; ++green)
            for (int red = 0; red < 2; ++red)
                result += std::to_string(red) + " " + std::to_string(green) + " " +
                          std::to_string(blue) + "\n";
    return result;
}

[[nodiscard]] std::string product_cube()
{
    std::string result = "TITLE \"cross term\"\nLUT_3D_SIZE 2\n";
    for (int blue = 0; blue < 2; ++blue)
        for (int green = 0; green < 2; ++green)
            for (int red = 0; red < 2; ++red)
                result += std::to_string(red * green) + " " + std::to_string(green) + " " +
                          std::to_string(blue) + "\n";
    return result;
}

[[nodiscard]] WorkingImage linear_image(std::vector<float> rgb, const std::uint32_t width = 1U)
{
    WorkingImage image;
    image.width = width;
    image.height = static_cast<std::uint32_t>(rgb.size() / 3U) / width;
    image.rgb = std::move(rgb);
    image.color_profile.kind = ColorProfileKind::kBuiltin;
    image.color_profile.model = ColorModel::kRgb;
    image.color_profile.identifier = std::string(kInputProfileLinearRec709);
    image.color_profile.has_matrix = true;
    image.color_profile.matrix_to_xyz_d50 = {0.4360747F, 0.3850649F, 0.1430804F,
                                              0.2225045F, 0.7168786F, 0.0606169F,
                                              0.0139322F, 0.0971045F, 0.7141733F};
    image.canonical_roi_scale = CanonicalRoiScale::from_scaled_dimensions(
        image.width, image.height, image.width, image.height);
    return image;
}

[[nodiscard]] Lut3dParams linear_params(const std::filesystem::path &path,
                                        const std::string_view interpolation)
{
    return {path.string(), std::string(kLut3dSpaceLinearRec709),
            std::string(kLut3dSpaceLinearRec709), std::string(interpolation), 1.0};
}

TEST(Lut3dRecipeTest, SchemaAndDevelopRoundTripAreStrict)
{
    LutTempDirectory temporary;
    const auto path = temporary.path() / "look.cube";
    write_text(path, identity_cube());
    const Lut3dParams expected{path.string(), std::string(kLut3dSpaceAdobeRgb),
                               std::string(kLut3dSpaceLinearRec2020),
                               std::string(kLut3dInterpolationTrilinear), 0.75};
    auto parameters = lut3d_to_parameters(expected);
    ASSERT_TRUE(parameters) << parameters.error().message;
    EXPECT_EQ(lut3d_from_parameters(parameters.value()).value(), expected);

    auto unknown = parameters.value();
    unknown.emplace("future", ParameterValue{true});
    EXPECT_FALSE(lut3d_from_parameters(unknown));
    auto invalid_space = expected;
    invalid_space.input_space = "implicit-current-display";
    EXPECT_FALSE(lut3d_to_parameters(invalid_space));
    auto invalid_strength = expected;
    invalid_strength.strength = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(lut3d_to_parameters(invalid_strength));

    DevelopParams develop;
    ASSERT_TRUE(apply_develop_text_field_strict(develop, "lut3dFile", path.string()));
    ASSERT_TRUE(apply_develop_field_strict(develop, "lut3dInputSpaceIndex", 1.0));
    ASSERT_TRUE(apply_develop_field_strict(develop, "lut3dOutputSpaceIndex", 4.0));
    ASSERT_TRUE(apply_develop_field_strict(develop, "lut3dInterpolationIndex", 1.0));
    ASSERT_TRUE(apply_develop_field_strict(develop, "lut3dStrength", 0.75));
    auto recipe = recipe_from_develop({"asset", "file:///fixture.raw", std::nullopt}, develop);
    ASSERT_TRUE(recipe) << recipe.error().message;
    const auto operation =
        std::find_if(recipe.value().operations.begin(), recipe.value().operations.end(),
                     [](const auto &item) { return item.id == kLut3dOperationId; });
    ASSERT_NE(operation, recipe.value().operations.end());
    EXPECT_EQ(operation->schema_version, kLut3dOperationSchemaVersion);
    auto roundtrip = develop_from_recipe(recipe.value());
    ASSERT_TRUE(roundtrip) << roundtrip.error().message;
    EXPECT_TRUE(roundtrip.value().lut3d_present);
    EXPECT_TRUE(roundtrip.value().lut3d_enabled);
    EXPECT_EQ(roundtrip.value().lut3d, expected);
    ASSERT_TRUE(reset_develop_field(roundtrip.value(), "lut3d"));
    EXPECT_TRUE(roundtrip.value().is_identity());

    DevelopParams disabled_identity;
    ASSERT_TRUE(apply_develop_field_strict(disabled_identity, "lut3dEnabled", 0.0));
    EXPECT_FALSE(disabled_identity.lut3d_present);
    EXPECT_TRUE(disabled_identity.is_identity());
}

TEST(Lut3dTest, IndependentTrilinearAndTetrahedralGoldensUseRedFastestOrder)
{
    LutTempDirectory temporary;
    const auto path = temporary.path() / "cross.cube";
    write_text(path, product_cube());
    const auto source = linear_image({0.25F, 0.5F, 0.75F});
    Lut3dCache cache;
    auto trilinear = apply_lut3d(
        source, linear_params(path, kLut3dInterpolationTrilinear), cache, CancellationToken{});
    auto tetrahedral = apply_lut3d(
        source, linear_params(path, kLut3dInterpolationTetrahedral), cache, CancellationToken{});
    ASSERT_TRUE(trilinear) << trilinear.error().message;
    ASSERT_TRUE(tetrahedral) << tetrahedral.error().message;
    EXPECT_NEAR(trilinear.value().rgb[0], 0.125F, 1.0e-6F);
    EXPECT_NEAR(tetrahedral.value().rgb[0], 0.25F, 1.0e-6F);
    EXPECT_NEAR(trilinear.value().rgb[1], 0.5F, 1.0e-6F);
    EXPECT_NEAR(tetrahedral.value().rgb[2], 0.75F, 1.0e-6F);
    EXPECT_EQ(source.rgb, (std::vector<float>{0.25F, 0.5F, 0.75F}));
}

TEST(Lut3dTest, DomainStrengthAndUnboundedLinearBlendAvoidOutputClipping)
{
    LutTempDirectory temporary;
    const auto path = temporary.path() / "domain.cube";
    write_text(path, identity_cube("DOMAIN_MIN -1 -1 -1\nDOMAIN_MAX 1 1 1\n"));
    Lut3dCache cache;
    auto params = linear_params(path, kLut3dInterpolationTetrahedral);
    params.strength = 0.5;
    auto result = apply_lut3d(linear_image({0.0F, 2.0F, -0.5F}), params, cache, {});
    ASSERT_TRUE(result) << result.error().message;
    // Domain maps 0 -> .5, 2 clamps to the last node, and -.5 -> .25.
    // The linear-strength blend remains unbounded rather than clipping to [0,1].
    EXPECT_NEAR(result.value().rgb[0], 0.25F, 1.0e-6F);
    EXPECT_NEAR(result.value().rgb[1], 1.5F, 1.0e-6F);
    EXPECT_NEAR(result.value().rgb[2], -0.125F, 1.0e-6F);
}

TEST(Lut3dTest, ContentFingerprintInvalidatesCacheAndNeverFallsBackToStaleData)
{
    LutTempDirectory temporary;
    const auto path = temporary.path() / "changing.cube";
    write_text(path, identity_cube());
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine);
    auto first = engine.value().inspect_lut3d(path.string(), {});
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_EQ(first.value().size, 2U);

    write_text(path, product_cube());
    auto second = engine.value().inspect_lut3d(path.string(), {});
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_NE(second.value().fingerprint, first.value().fingerprint);
    EXPECT_EQ(second.value().title, "cross term");

    write_text(path, "LUT_3D_SIZE 2\n0 0 0\n");
    auto corrupt = engine.value().inspect_lut3d(path.string(), {});
    ASSERT_FALSE(corrupt);
    EXPECT_EQ(corrupt.error().context.at("reason"), "invalid_lut_sample_count");
}

TEST(Lut3dTest, MalformedUnsupportedMissingAndCancelledInputsFailStructurally)
{
    LutTempDirectory temporary;
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine);
    const auto one_d = temporary.path() / "one.cube";
    write_text(one_d, "LUT_1D_SIZE 2\n0 0 0\n1 1 1\n");
    auto unsupported = engine.value().inspect_lut3d(one_d.string(), {});
    ASSERT_FALSE(unsupported);
    EXPECT_EQ(unsupported.error().code, ErrorCode::kUnsupported);

    const auto nonfinite = temporary.path() / "nonfinite.cube";
    write_text(nonfinite, "LUT_3D_SIZE 2\nnan 0 0\n");
    auto rejected = engine.value().inspect_lut3d(nonfinite.string(), {});
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "invalid_lut_sample");

    auto missing = engine.value().inspect_lut3d((temporary.path() / "missing.cube").string(), {});
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::kNotFound);

    Lut3dCache cache;
    auto zero_strength = linear_params(temporary.path() / "missing.cube",
                                       kLut3dInterpolationTetrahedral);
    zero_strength.strength = 0.0;
    auto missing_zero = apply_lut3d(linear_image({0.2F, 0.3F, 0.4F}), zero_strength, cache, {});
    ASSERT_FALSE(missing_zero);
    EXPECT_EQ(missing_zero.error().code, ErrorCode::kNotFound);

    CancellationSource source;
    ASSERT_TRUE(source.cancel("lut-test"));
    auto cancelled = engine.value().inspect_lut3d(one_d.string(), source.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, ErrorCode::kCancelled);
}

TEST(Lut3dCliTest, InspectAndTextFieldDiscoveryAreMachineReadable)
{
    LutTempDirectory temporary;
    const auto path = temporary.path() / "identity.cube";
    write_text(path, identity_cube("TITLE \"identity\"\n"));
    auto engine = EngineFacade::create_phase1();
    ASSERT_TRUE(engine);
    std::ostringstream output;
    std::ostringstream errors;
    CliApplication cli(engine.value(), output, errors);
    const std::array<std::string, 4> owned{"lut", "inspect", path.string(), "--json"};
    std::vector<std::string_view> arguments;
    for (const auto &item : owned)
        arguments.push_back(item);
    EXPECT_EQ(cli.run(arguments), 0) << errors.str();
    auto json = parse_json(output.str());
    ASSERT_TRUE(json) << json.error().message;
    EXPECT_NE(output.str().find("\"title\":\"identity\""), std::string::npos);

    output.str({});
    output.clear();
    EXPECT_EQ(cli.run(std::array<std::string_view, 2>{"develop-fields", "--json"}), 0);
    EXPECT_NE(output.str().find("\"name\":\"lut3dFile\""), std::string::npos);
    EXPECT_NE(output.str().find("--set-text name=value"), std::string::npos);
}

TEST(Lut3dLegacyXmpTest, ExternalLegacyResourceRejectsWithAStableReason)
{
    constexpr std::string_view xmp = R"(<?xml version="1.0"?>
<rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
         xmlns:darktable="http://darktable.sf.net/">
  <rdf:Description darktable:xmp_version="6"><darktable:history><rdf:Seq><rdf:li
    darktable:num="8" darktable:operation="lut3d" darktable:enabled="1"
    darktable:modversion="3" darktable:params="unversioned-external-path"
    darktable:multi_name="" darktable:multi_priority="0"
    darktable:blendop_version="13" darktable:blendop_params="opaque"/>
  </rdf:Seq></darktable:history></rdf:Description></rdf:RDF>)";
    auto imported = import_legacy_xmp({std::string(xmp),
                                       {"asset", "file:///fixture.raw", std::nullopt}});
    ASSERT_FALSE(imported);
    EXPECT_EQ(imported.error().code, ErrorCode::kUnsupported);
    EXPECT_EQ(imported.error().context.at("reason"),
              "unsupported_legacy_lut3d_resource");
}

} // namespace
} // namespace ravo
