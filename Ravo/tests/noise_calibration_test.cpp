#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <QProcess>

#include "ravo/adapters/camera_noise_profile.h"
#include "ravo/engine/noise_calibration.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"
#include "ravo/services/artifact_publication.h"

namespace ravo
{
namespace
{

class NoiseCalibrationTempDirectory
{
public:
    NoiseCalibrationTempDirectory()
    {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("ravo-noise-calibration-test-" + std::to_string(nonce));
        std::filesystem::create_directories(path_);
    }

    ~NoiseCalibrationTempDirectory()
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

[[nodiscard]] std::string read_text(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

[[nodiscard]] CameraNoiseCalibrationDocument synthetic_document(const bool add_outliers = false)
{
    CameraNoiseCalibrationDocument document{{"Sony", "Synthetic Camera", 800U}, {}};
    for (std::uint32_t index = 0U; index < 16U; ++index)
    {
        const double signal = 256.0 + static_cast<double>(index) * 2048.0;
        double variance = 25.0 + 0.5 * signal;
        if (add_outliers && (index == 3U || index == 12U))
            variance += 4000.0;
        document.samples.push_back({signal, variance, 1000U + index});
    }
    return document;
}

TEST(CameraNoiseCalibrationTest, RobustFitRecoversKnownGaussianPoissonModel)
{
    const auto document = synthetic_document(true);
    auto fit = fit_camera_noise(document.samples);
    ASSERT_TRUE(fit) << fit.error().message;
    EXPECT_NEAR(fit.value().gaussian_variance, 25.0, 1.0e-9);
    EXPECT_NEAR(fit.value().poisson_slope, 0.5, 1.0e-12);
    EXPECT_NEAR(fit.value().weighted_rmse, 0.0, 1.0e-9);
    EXPECT_NEAR(fit.value().weighted_r_squared, 1.0, 1.0e-12);
    EXPECT_EQ(fit.value().input_sample_count, 16U);
    EXPECT_EQ(fit.value().retained_sample_count, 14U);
}

TEST(CameraNoiseCalibrationTest, InvalidSamplesSpanAndCancellationFailExplicitly)
{
    auto too_few = synthetic_document().samples;
    too_few.resize(kCameraNoiseMinimumSamples - 1U);
    auto result = fit_camera_noise(too_few);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().context.at("reason"), "sample_count_out_of_range");

    auto too_many = synthetic_document().samples;
    too_many.resize(kCameraNoiseMaximumSamples + 1U, too_many.back());
    result = fit_camera_noise(too_many);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().context.at("reason"), "sample_count_out_of_range");

    auto narrow = synthetic_document().samples;
    for (std::size_t index = 0U; index < narrow.size(); ++index)
        narrow[index].signal_mean = static_cast<double>(index);
    result = fit_camera_noise(narrow);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().context.at("reason"), "insufficient_signal_span");

    auto invalid = synthetic_document().samples;
    invalid[4].variance = -1.0;
    result = fit_camera_noise(invalid);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().context.at("reason"), "invalid_sample");
    EXPECT_EQ(result.error().context.at("index"), "4");

    CancellationSource source;
    ASSERT_TRUE(source.cancel("test cancellation"));
    result = fit_camera_noise(synthetic_document().samples, source.token());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::kCancelled);
    EXPECT_EQ(result.error().context.at("stage"), "validation");
}

TEST(CameraNoiseCalibrationTest, CancelledPublicationLeavesNoDestination)
{
    NoiseCalibrationTempDirectory temporary;
    const auto output = temporary.path() / "cancelled.json";
    CancellationSource source;
    ASSERT_TRUE(source.cancel("cancel before publication"));
    const auto result =
        publish_text_artifact_no_replace(output.string(), "complete", source.token());
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, ErrorCode::kCancelled);
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST(CameraNoiseProfileTest, CanonicalDocumentsAreReproducibleAndTamperingIsRejected)
{
    const auto document = synthetic_document(true);
    auto input = serialize_camera_noise_calibration_json(document);
    ASSERT_TRUE(input) << input.error().message;
    auto parsed_input = parse_camera_noise_calibration_json(input.value());
    ASSERT_TRUE(parsed_input) << parsed_input.error().message;
    EXPECT_EQ(parsed_input.value(), document);
    auto source_hash = camera_noise_calibration_sha256(document);
    ASSERT_TRUE(source_hash) << source_hash.error().message;
    EXPECT_EQ(camera_noise_calibration_sha256(parsed_input.value()).value(), source_hash.value());

    auto fit = fit_camera_noise(document.samples);
    ASSERT_TRUE(fit) << fit.error().message;
    auto first =
        serialize_camera_noise_profile_json(document.identity, fit.value(), source_hash.value());
    auto second =
        serialize_camera_noise_profile_json(document.identity, fit.value(), source_hash.value());
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(first.value(), second.value());
    auto profile = parse_camera_noise_profile_json(first.value());
    ASSERT_TRUE(profile) << profile.error().message;
    EXPECT_EQ(profile.value().identity, document.identity);
    EXPECT_EQ(profile.value().fit, fit.value());
    EXPECT_EQ(profile.value().source_samples_sha256, source_hash.value());

    auto tampered = first.value();
    const auto position = tampered.find("\"gaussian_variance\":25");
    ASSERT_NE(position, std::string::npos);
    tampered.replace(position, std::string_view("\"gaussian_variance\":25").size(),
                     "\"gaussian_variance\":26");
    auto rejected = parse_camera_noise_profile_json(tampered);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().context.at("reason"), "checksum_mismatch");
}

TEST(CameraNoiseProfileTest, StrictJsonRejectsUnknownFieldsAndUnsupportedUnits)
{
    auto serialized = serialize_camera_noise_calibration_json(synthetic_document());
    ASSERT_TRUE(serialized) << serialized.error().message;
    auto unknown = serialized.value();
    const auto closing = unknown.rfind('}');
    ASSERT_NE(closing, std::string::npos);
    unknown.insert(closing, ",\"future\":true");
    auto result = parse_camera_noise_calibration_json(unknown);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().context.at("reason"), "unknown_field");

    auto units = serialized.value();
    const auto expected = std::string(kCameraNoiseSignalUnits);
    const auto offset = units.find(expected);
    ASSERT_NE(offset, std::string::npos);
    units.replace(offset, expected.size(), "normalized_float");
    result = parse_camera_noise_calibration_json(units);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().context.at("reason"), "unsupported_units");
}

TEST(CameraNoiseCliTest, RealCliPublishesInspectsAndNeverReplacesOrMutatesInput)
{
    NoiseCalibrationTempDirectory temporary;
    const auto input_path = temporary.path() / "samples.json";
    const auto output_path = temporary.path() / "profile.json";
    auto input = serialize_camera_noise_calibration_json(synthetic_document(true));
    ASSERT_TRUE(input) << input.error().message;
    write_text(input_path, input.value());
    const auto input_before = read_text(input_path);

    const auto run = [&](const QStringList &arguments)
    {
        QProcess process;
        process.start(QString::fromUtf8(RAVO_CLI_EXECUTABLE), arguments);
        EXPECT_TRUE(process.waitForStarted());
        EXPECT_TRUE(process.waitForFinished(30000));
        return std::pair{process.exitCode(), process.readAllStandardOutput().toStdString()};
    };
    const auto calibrated =
        run({QStringLiteral("noise"), QStringLiteral("calibrate"),
             QString::fromStdString(input_path.string()), QStringLiteral("--output"),
             QString::fromStdString(output_path.string()), QStringLiteral("--json")});
    EXPECT_EQ(calibrated.first, 0) << calibrated.second;
    EXPECT_TRUE(std::filesystem::is_regular_file(output_path));
    EXPECT_EQ(read_text(input_path), input_before);
    auto profile = parse_camera_noise_profile_json(read_text(output_path));
    ASSERT_TRUE(profile) << profile.error().message;
    EXPECT_NEAR(profile.value().fit.gaussian_variance, 25.0, 1.0e-9);
    EXPECT_NEAR(profile.value().fit.poisson_slope, 0.5, 1.0e-12);

    const auto output_before = read_text(output_path);
    const auto conflict =
        run({QStringLiteral("noise"), QStringLiteral("calibrate"),
             QString::fromStdString(input_path.string()), QStringLiteral("--output"),
             QString::fromStdString(output_path.string()), QStringLiteral("--json")});
    EXPECT_EQ(conflict.first, cli_exit_code(ErrorCode::kConflict));
    EXPECT_NE(conflict.second.find("artifact_output_exists"), std::string::npos);
    EXPECT_EQ(read_text(output_path), output_before);
    EXPECT_EQ(read_text(input_path), input_before);

    const auto malformed_path = temporary.path() / "malformed.json";
    const auto malformed_output = temporary.path() / "must-not-exist.json";
    write_text(malformed_path, "{\"schema\":false}\n");
    const auto malformed =
        run({QStringLiteral("noise"), QStringLiteral("calibrate"),
             QString::fromStdString(malformed_path.string()), QStringLiteral("--output"),
             QString::fromStdString(malformed_output.string()), QStringLiteral("--json")});
    EXPECT_NE(malformed.first, 0);
    EXPECT_FALSE(std::filesystem::exists(malformed_output));

    const auto inspected =
        run({QStringLiteral("noise"), QStringLiteral("inspect"),
             QString::fromStdString(output_path.string()), QStringLiteral("--json")});
    EXPECT_EQ(inspected.first, 0) << inspected.second;
    EXPECT_NE(inspected.second.find(profile.value().payload_sha256), std::string::npos);
}

} // namespace
} // namespace ravo
