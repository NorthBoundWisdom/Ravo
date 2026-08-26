#include "input_color.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QUrl>

#include <lcms2.h>

namespace ravo
{
namespace
{

constexpr std::size_t kLutSamples = 0x10000U;
constexpr std::size_t kMaxIccBytes = 64U * 1024U * 1024U;

class CmsProfile final
{
public:
    CmsProfile() = default;
    explicit CmsProfile(cmsHPROFILE handle)
        : handle_(handle)
    {
    }
    ~CmsProfile()
    {
        if (handle_ != nullptr)
        {
            cmsCloseProfile(handle_);
        }
    }
    CmsProfile(const CmsProfile &) = delete;
    CmsProfile &operator=(const CmsProfile &) = delete;
    CmsProfile(CmsProfile &&other) noexcept
        : handle_(std::exchange(other.handle_, nullptr))
    {
    }
    CmsProfile &operator=(CmsProfile &&other) noexcept
    {
        if (this != &other)
        {
            if (handle_ != nullptr)
            {
                cmsCloseProfile(handle_);
            }
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    [[nodiscard]] cmsHPROFILE get() const noexcept
    {
        return handle_;
    }

private:
    cmsHPROFILE handle_ = nullptr;
};

class CmsTransform final
{
public:
    explicit CmsTransform(cmsHTRANSFORM handle)
        : handle_(handle)
    {
    }
    ~CmsTransform()
    {
        if (handle_ != nullptr)
        {
            cmsDeleteTransform(handle_);
        }
    }
    CmsTransform(const CmsTransform &) = delete;
    CmsTransform &operator=(const CmsTransform &) = delete;
    [[nodiscard]] cmsHTRANSFORM get() const noexcept
    {
        return handle_;
    }

private:
    cmsHTRANSFORM handle_ = nullptr;
};

struct ToneCurve
{
    bool linear = true;
    std::vector<float> lut;
    std::array<float, 3> unbounded{1.0F, 1.0F, 1.0F};
};

struct ProfileData
{
    ColorModel model = ColorModel::kRgb;
    CmsProfile profile;
    bool matrix_shaper = false;
    std::array<float, 9> matrix_to_xyz_d50{};
    std::array<ToneCurve, 3> curves;
    ColorProfileState state;
};

struct RgbChromaticities
{
    cmsCIExyY white{};
    cmsCIExyYTRIPLE primaries{};
};

[[nodiscard]] bool finite_matrix(const std::array<float, 9> &matrix) noexcept
{
    return std::all_of(matrix.begin(), matrix.end(),
                       [](const float value) { return std::isfinite(value); });
}

[[nodiscard]] Result<std::array<float, 9>> invert_matrix(const std::array<float, 9> &matrix)
{
    if (!finite_matrix(matrix))
    {
        return make_error(ErrorCode::kValidation, "Colour matrix contains a non-finite value");
    }
    const double a = matrix[0];
    const double b = matrix[1];
    const double c = matrix[2];
    const double d = matrix[3];
    const double e = matrix[4];
    const double f = matrix[5];
    const double g = matrix[6];
    const double h = matrix[7];
    const double i = matrix[8];
    const double determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-12)
    {
        return make_error(ErrorCode::kValidation, "Colour matrix is singular");
    }
    const double inverse = 1.0 / determinant;
    std::array<float, 9> result{
        static_cast<float>((e * i - f * h) * inverse),
        static_cast<float>((c * h - b * i) * inverse),
        static_cast<float>((b * f - c * e) * inverse),
        static_cast<float>((f * g - d * i) * inverse),
        static_cast<float>((a * i - c * g) * inverse),
        static_cast<float>((c * d - a * f) * inverse),
        static_cast<float>((d * h - e * g) * inverse),
        static_cast<float>((b * g - a * h) * inverse),
        static_cast<float>((a * e - b * d) * inverse),
    };
    if (!finite_matrix(result))
    {
        return make_error(ErrorCode::kValidation, "Inverted colour matrix is non-finite");
    }
    return result;
}

[[nodiscard]] std::array<float, 3> apply_matrix(const std::array<float, 9> &matrix,
                                                const std::array<float, 3> &value) noexcept
{
    return {matrix[0] * value[0] + matrix[1] * value[1] + matrix[2] * value[2],
            matrix[3] * value[0] + matrix[4] * value[1] + matrix[5] * value[2],
            matrix[6] * value[0] + matrix[7] * value[1] + matrix[8] * value[2]};
}

[[nodiscard]] RgbChromaticities chromaticities_for(const std::string_view profile)
{
    RgbChromaticities value;
    if (profile == kInputProfileProPhotoRgb)
    {
        value.white = {0.3457, 0.3585, 1.0};
        value.primaries = {{0.7347, 0.2653, 1.0}, {0.1596, 0.8404, 1.0}, {0.0366, 0.0001, 1.0}};
        return value;
    }

    value.white = {0.3127, 0.3290, 1.0};
    if (profile == kInputProfileAdobeRgb)
    {
        value.primaries = {{0.6400, 0.3300, 1.0}, {0.2100, 0.7100, 1.0}, {0.1500, 0.0600, 1.0}};
    }
    else if (profile == kInputProfileLinearRec2020 || profile == kInputProfilePqRec2020 ||
             profile == kInputProfileHlgRec2020)
    {
        value.primaries = {{0.7080, 0.2920, 1.0}, {0.1700, 0.7970, 1.0}, {0.1310, 0.0460, 1.0}};
    }
    else if (profile == kInputProfileDisplayP3 || profile == kInputProfilePqP3 ||
             profile == kInputProfileHlgP3)
    {
        value.primaries = {{0.6800, 0.3200, 1.0}, {0.2650, 0.6900, 1.0}, {0.1500, 0.0600, 1.0}};
    }
    else
    {
        value.primaries = {{0.6400, 0.3300, 1.0}, {0.3000, 0.6000, 1.0}, {0.1500, 0.0600, 1.0}};
    }
    return value;
}

[[nodiscard]] Result<CmsProfile> make_linear_rgb_profile(const std::string_view identifier)
{
    const auto chromaticities = chromaticities_for(identifier);
    std::array<cmsToneCurve *, 3> curves{};
    for (auto &curve : curves)
    {
        curve = cmsBuildGamma(nullptr, 1.0);
        if (curve == nullptr)
        {
            for (auto *owned : curves)
            {
                if (owned != nullptr)
                {
                    cmsFreeToneCurve(owned);
                }
            }
            return make_error(ErrorCode::kInternal,
                              "LittleCMS could not allocate a linear tone curve");
        }
    }
    CmsProfile profile(
        cmsCreateRGBProfile(&chromaticities.white, &chromaticities.primaries, curves.data()));
    for (auto *curve : curves)
    {
        cmsFreeToneCurve(curve);
    }
    if (profile.get() == nullptr)
    {
        return make_error(ErrorCode::kInternal,
                          "LittleCMS could not create a built-in RGB profile");
    }
    return std::move(profile);
}

[[nodiscard]] Result<std::array<float, 9>> matrix_from_profile(const cmsHPROFILE profile)
{
    const auto *red = static_cast<const cmsCIEXYZ *>(cmsReadTag(profile, cmsSigRedColorantTag));
    const auto *green = static_cast<const cmsCIEXYZ *>(cmsReadTag(profile, cmsSigGreenColorantTag));
    const auto *blue = static_cast<const cmsCIEXYZ *>(cmsReadTag(profile, cmsSigBlueColorantTag));
    if (red == nullptr || green == nullptr || blue == nullptr)
    {
        return make_error(ErrorCode::kUnsupported,
                          "ICC RGB profile does not expose matrix colorants");
    }
    std::array<float, 9> matrix{
        static_cast<float>(red->X), static_cast<float>(green->X), static_cast<float>(blue->X),
        static_cast<float>(red->Y), static_cast<float>(green->Y), static_cast<float>(blue->Y),
        static_cast<float>(red->Z), static_cast<float>(green->Z), static_cast<float>(blue->Z)};
    auto inverse = invert_matrix(matrix);
    if (!inverse)
    {
        return inverse.error();
    }
    return matrix;
}

[[nodiscard]] float srgb_decode(const float encoded) noexcept
{
    const float value = std::max(encoded, 0.0F);
    return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] float rec709_decode(const float encoded) noexcept
{
    const float value = std::max(encoded, 0.0F);
    return value < 0.081F ? value / 4.5F : std::pow((value + 0.099F) / 1.099F, 1.0F / 0.45F);
}

[[nodiscard]] float hlg_decode(const float encoded) noexcept
{
    constexpr float beta = 0.04F;
    constexpr float a = 0.17883277F;
    constexpr float b = 1.0F - 4.0F * a;
    const float c = 0.5F - a * std::log(4.0F * a);
    const float value = std::max(encoded * (1.0F - beta) + beta, 0.0F);
    return value <= 0.5F ? value * value / 3.0F : (std::exp((value - c) / a) + b) / 12.0F;
}

[[nodiscard]] float pq_decode(const float encoded) noexcept
{
    constexpr float m1 = 2610.0F / 16384.0F;
    constexpr float m2 = 2523.0F / 32.0F;
    constexpr float c1 = 3424.0F / 4096.0F;
    constexpr float c2 = 2413.0F / 128.0F;
    constexpr float c3 = 2392.0F / 128.0F;
    const float power = std::pow(std::max(encoded, 0.0F), 1.0F / m2);
    const float denominator = c2 - c3 * power;
    if (denominator <= 0.0F)
    {
        return 1.0F;
    }
    return std::pow(std::max(power - c1, 0.0F) / denominator, 1.0F / m1);
}

[[nodiscard]] Result<ToneCurve> sample_curve(const std::function<float(float)> &evaluator,
                                             const bool linear)
{
    ToneCurve curve;
    curve.linear = linear;
    if (linear)
    {
        return curve;
    }
    curve.lut.resize(kLutSamples);
    for (std::size_t index = 0; index < curve.lut.size(); ++index)
    {
        const float x = static_cast<float>(index) / static_cast<float>(kLutSamples - 1U);
        curve.lut[index] = evaluator(x);
        if (!std::isfinite(curve.lut[index]) || curve.lut[index] < 0.0F)
        {
            return make_error(ErrorCode::kValidation,
                              "Input profile tone curve contains an invalid sample");
        }
    }
    const std::array<float, 4> x{0.7F, 0.8F, 0.9F, 1.0F};
    std::array<float, 4> y{};
    for (std::size_t index = 0; index < x.size(); ++index)
    {
        y[index] = evaluator(x[index]);
    }
    const float x0 = x.back();
    const float y0 = y.back();
    if (!std::isfinite(y0) || y0 <= 0.0F)
    {
        return make_error(ErrorCode::kValidation,
                          "Input profile tone curve cannot be extrapolated");
    }
    float exponent = 0.0F;
    int count = 0;
    for (std::size_t index = 0; index + 1U < x.size(); ++index)
    {
        const float yy = y[index] / y0;
        const float xx = x[index] / x0;
        if (yy > 0.0F && xx > 0.0F)
        {
            exponent += std::log(yy) / std::log(xx);
            ++count;
        }
    }
    exponent = count > 0 ? exponent / static_cast<float>(count) : 1.0F;
    if (!std::isfinite(exponent))
    {
        return make_error(ErrorCode::kValidation,
                          "Input profile extrapolation exponent is non-finite");
    }
    curve.unbounded = {1.0F / x0, y0, exponent};
    return curve;
}

[[nodiscard]] Result<ToneCurve> sample_lcms_curve(const cmsToneCurve *const source)
{
    if (source == nullptr)
    {
        return make_error(ErrorCode::kValidation, "ICC matrix profile is missing a tone curve");
    }
    if (cmsIsToneCurveLinear(source) != 0)
    {
        return ToneCurve{};
    }
    return sample_curve([source](const float value)
                        { return cmsEvalToneCurveFloat(source, value); }, false);
}

[[nodiscard]] Result<ProfileData> make_builtin_profile(const std::string_view identifier)
{
    if (identifier == kInputProfileLab || identifier == kInputProfileXyz)
    {
        ProfileData result;
        result.model = identifier == kInputProfileLab ? ColorModel::kLab : ColorModel::kXyz;
        result.profile = CmsProfile(identifier == kInputProfileLab ? cmsCreateLab4Profile(nullptr) :
                                                                     cmsCreateXYZProfile());
        if (result.profile.get() == nullptr)
        {
            return make_error(ErrorCode::kInternal,
                              "LittleCMS could not create a PCS input profile");
        }
        result.state.kind = ColorProfileKind::kBuiltin;
        result.state.model = result.model;
        result.state.identifier = std::string(identifier);
        return result;
    }

    static const std::array<std::string_view, 13> rgb_profiles{
        kInputProfileSrgb,          kInputProfileAdobeRgb,   kInputProfileLinearRec709,
        kInputProfileLinearRec2020, kInputProfileRec709,     kInputProfileProPhotoRgb,
        kInputProfilePqRec2020,     kInputProfileHlgRec2020, kInputProfilePqP3,
        kInputProfileHlgP3,         kInputProfileDisplayP3,  kColorNormalizeSrgb,
        kColorNormalizeAdobeRgb};
    if (std::find(rgb_profiles.begin(), rgb_profiles.end(), identifier) == rgb_profiles.end())
    {
        return make_error(ErrorCode::kUnsupported, "Built-in input profile is unsupported",
                          {{"profile", std::string(identifier)}});
    }

    ProfileData result;
    result.model = ColorModel::kRgb;
    auto linear_profile = make_linear_rgb_profile(identifier);
    if (!linear_profile)
    {
        return linear_profile.error();
    }
    result.profile = std::move(linear_profile).value();
    auto matrix = matrix_from_profile(result.profile.get());
    if (!matrix)
    {
        return matrix.error();
    }
    result.matrix_to_xyz_d50 = matrix.value();
    if (identifier == kInputProfileSrgb || identifier == kInputProfileLinearRec709 ||
        identifier == kInputProfileRec709 || identifier == kColorNormalizeSrgb)
    {
        result.matrix_to_xyz_d50 = {0.4360747F, 0.3850649F, 0.1430804F, 0.2225045F, 0.7168786F,
                                    0.0606169F, 0.0139322F, 0.0971045F, 0.7141733F};
    }
    result.matrix_shaper = true;

    std::function<float(float)> evaluator = [](const float value) { return value; };
    bool linear = false;
    if (identifier == kInputProfileLinearRec709 || identifier == kInputProfileLinearRec2020 ||
        identifier == kInputProfileProPhotoRgb)
    {
        linear = true;
    }
    else if (identifier == kInputProfileSrgb || identifier == kInputProfileDisplayP3 ||
             identifier == kColorNormalizeSrgb)
    {
        evaluator = srgb_decode;
    }
    else if (identifier == kInputProfileRec709)
    {
        evaluator = rec709_decode;
    }
    else if (identifier == kInputProfileAdobeRgb || identifier == kColorNormalizeAdobeRgb)
    {
        evaluator = [](const float value)
        { return std::pow(std::max(value, 0.0F), 563.0F / 256.0F); };
    }
    else if (identifier == kInputProfilePqRec2020 || identifier == kInputProfilePqP3)
    {
        evaluator = pq_decode;
    }
    else if (identifier == kInputProfileHlgRec2020 || identifier == kInputProfileHlgP3)
    {
        evaluator = hlg_decode;
    }
    auto curve = sample_curve(evaluator, linear);
    if (!curve)
    {
        return curve.error();
    }
    result.curves = {curve.value(), curve.value(), curve.value()};
    result.state.kind = ColorProfileKind::kBuiltin;
    result.state.model = ColorModel::kRgb;
    result.state.identifier = std::string(identifier);
    result.state.matrix_to_xyz_d50 = result.matrix_to_xyz_d50;
    result.state.has_matrix = true;
    return result;
}

[[nodiscard]] Result<std::vector<std::uint8_t>> read_profile_file(const std::string_view filename)
{
    const QString text =
        QString::fromUtf8(filename.data(), static_cast<qsizetype>(filename.size()));
    const QUrl uri(text);
    const QString path = uri.isLocalFile() ? uri.toLocalFile() : text;
    QFile file(path);
    if (!file.exists())
    {
        return make_error(ErrorCode::kNotFound, "ICC profile file does not exist",
                          {{"path", std::string(filename)}});
    }
    if (!file.open(QIODevice::ReadOnly))
    {
        return make_error(ErrorCode::kIo, "ICC profile file could not be opened",
                          {{"path", std::string(filename)},
                           {"qt_error", file.errorString().toUtf8().toStdString()}});
    }
    const QByteArray bytes = file.read(static_cast<qint64>(kMaxIccBytes + 1U));
    if (bytes.isEmpty() || static_cast<std::size_t>(bytes.size()) > kMaxIccBytes)
    {
        return make_error(ErrorCode::kValidation, "ICC profile file is empty or too large",
                          {{"path", std::string(filename)}});
    }
    return std::vector<std::uint8_t>(bytes.cbegin(), bytes.cend());
}

[[nodiscard]] Result<ProfileData> open_icc_profile(std::vector<std::uint8_t> bytes,
                                                   std::string identifier)
{
    if (bytes.empty() || bytes.size() > kMaxIccBytes ||
        bytes.size() > static_cast<std::size_t>(std::numeric_limits<cmsUInt32Number>::max()))
    {
        return make_error(ErrorCode::kValidation, "ICC profile payload is empty or too large");
    }
    CmsProfile profile(
        cmsOpenProfileFromMem(bytes.data(), static_cast<cmsUInt32Number>(bytes.size())));
    if (profile.get() == nullptr)
    {
        return make_error(ErrorCode::kValidation, "ICC profile payload is corrupt");
    }

    ProfileData result;
    const cmsColorSpaceSignature signature = cmsGetColorSpace(profile.get());
    if (signature == cmsSigRgbData)
    {
        result.model = ColorModel::kRgb;
    }
    else if (signature == cmsSigXYZData)
    {
        result.model = ColorModel::kXyz;
    }
    else if (signature == cmsSigLabData)
    {
        result.model = ColorModel::kLab;
    }
    else
    {
        return make_error(ErrorCode::kUnsupported, "ICC input colorspace is unsupported");
    }
    result.profile = std::move(profile);
    result.state.kind = ColorProfileKind::kIcc;
    result.state.model = result.model;
    result.state.identifier = std::move(identifier);
    result.state.icc_bytes = std::move(bytes);

    if (result.model == ColorModel::kRgb && cmsIsMatrixShaper(result.profile.get()) != 0)
    {
        auto matrix = matrix_from_profile(result.profile.get());
        if (!matrix)
        {
            return matrix.error();
        }
        const std::array<cmsTagSignature, 3> tags{cmsSigRedTRCTag, cmsSigGreenTRCTag,
                                                  cmsSigBlueTRCTag};
        for (std::size_t channel = 0; channel < tags.size(); ++channel)
        {
            auto curve = sample_lcms_curve(
                static_cast<const cmsToneCurve *>(cmsReadTag(result.profile.get(), tags[channel])));
            if (!curve)
            {
                return curve.error();
            }
            result.curves[channel] = std::move(curve).value();
        }
        result.matrix_to_xyz_d50 = matrix.value();
        result.matrix_shaper = true;
        result.state.matrix_to_xyz_d50 = result.matrix_to_xyz_d50;
        result.state.has_matrix = true;
    }
    return result;
}

[[nodiscard]] Result<ProfileData> matrix_profile(const ColorProfileState &state)
{
    if (state.kind != ColorProfileKind::kMatrix || state.model != ColorModel::kRgb ||
        !state.has_matrix)
    {
        return make_error(ErrorCode::kUnsupported, "Requested camera input matrix is unavailable",
                          {{"profile", state.identifier}});
    }
    auto inverse = invert_matrix(state.matrix_to_xyz_d50);
    if (!inverse)
    {
        return inverse.error();
    }
    ProfileData result;
    result.model = ColorModel::kRgb;
    result.matrix_shaper = true;
    result.matrix_to_xyz_d50 = state.matrix_to_xyz_d50;
    result.state = state;
    return result;
}

[[nodiscard]] bool is_matrix_profile_name(const std::string_view name) noexcept
{
    return name == kInputProfileEmbeddedMatrix || name == kInputProfileStandardMatrix ||
           name == kInputProfileEnhancedMatrix || name == kInputProfileVendorMatrix ||
           name == kInputProfileAlternateMatrix;
}

[[nodiscard]] Result<ProfileData> source_profile(const ColorProfileState &state,
                                                 const InputColorParams &params)
{
    if (params.input_profile == kInputProfileSource)
    {
        if (state.kind == ColorProfileKind::kMissing)
        {
            return make_error(ErrorCode::kValidation,
                              "Decoded input has no declared colour profile");
        }
        if (state.kind == ColorProfileKind::kBuiltin)
        {
            return make_builtin_profile(state.identifier);
        }
        if (state.kind == ColorProfileKind::kMatrix)
        {
            return matrix_profile(state);
        }
        if (state.kind == ColorProfileKind::kIcc)
        {
            return open_icc_profile(state.icc_bytes, state.identifier);
        }
    }
    if (params.input_profile == kInputProfileFileIcc)
    {
        auto bytes = read_profile_file(params.input_profile_filename);
        if (!bytes)
        {
            return bytes.error();
        }
        return open_icc_profile(std::move(bytes).value(), params.input_profile_filename);
    }
    if (params.input_profile == kInputProfileEmbeddedIcc)
    {
        if (state.kind != ColorProfileKind::kIcc)
        {
            return make_error(ErrorCode::kValidation,
                              "Decoded input does not contain an embedded ICC profile");
        }
        return open_icc_profile(state.icc_bytes, state.identifier);
    }
    if (is_matrix_profile_name(params.input_profile))
    {
        if (state.identifier != params.input_profile)
        {
            return make_error(
                ErrorCode::kUnsupported, "Requested camera matrix type is unavailable",
                {{"available", state.identifier}, {"requested", params.input_profile}});
        }
        return matrix_profile(state);
    }
    return make_builtin_profile(params.input_profile);
}

[[nodiscard]] Result<ProfileData> working_profile(const InputColorParams &params)
{
    if (params.working_profile == kInputProfileFileIcc)
    {
        auto bytes = read_profile_file(params.working_profile_filename);
        if (!bytes)
        {
            return bytes.error();
        }
        auto profile = open_icc_profile(std::move(bytes).value(), params.working_profile_filename);
        if (!profile)
        {
            return profile.error();
        }
        if (profile.value().model != ColorModel::kRgb || !profile.value().matrix_shaper)
        {
            return make_error(ErrorCode::kUnsupported,
                              "Working ICC profile must be an RGB matrix/shaper profile");
        }
        return profile;
    }
    auto profile = make_builtin_profile(params.working_profile);
    if (!profile)
    {
        return profile.error();
    }
    if (profile.value().model != ColorModel::kRgb || !profile.value().matrix_shaper)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Working colour profile must define RGB primaries");
    }
    return profile;
}

[[nodiscard]] Result<std::optional<ProfileData>> normalize_profile(const std::string_view name)
{
    if (name == kColorNormalizeOff)
    {
        return std::optional<ProfileData>{};
    }
    const std::string_view profile =
        name == kColorNormalizeSrgb          ? kInputProfileSrgb :
        name == kColorNormalizeAdobeRgb      ? kInputProfileAdobeRgb :
        name == kColorNormalizeLinearRec709  ? kInputProfileLinearRec709 :
        name == kColorNormalizeLinearRec2020 ? kInputProfileLinearRec2020 :
                                               std::string_view{};
    if (profile.empty())
    {
        return make_error(ErrorCode::kUnsupported, "Gamut normalization profile is unsupported",
                          {{"normalize", std::string(name)}});
    }
    auto built = make_builtin_profile(profile);
    if (!built)
    {
        return built.error();
    }
    return std::optional<ProfileData>{std::move(built).value()};
}

[[nodiscard]] int lcms_intent(const std::string_view intent) noexcept
{
    return intent == kColorIntentRelative   ? INTENT_RELATIVE_COLORIMETRIC :
           intent == kColorIntentSaturation ? INTENT_SATURATION :
           intent == kColorIntentAbsolute   ? INTENT_ABSOLUTE_COLORIMETRIC :
                                              INTENT_PERCEPTUAL;
}

[[nodiscard]] cmsUInt32Number lcms_input_format(const ColorModel model) noexcept
{
    return model == ColorModel::kLab ? TYPE_Lab_FLT :
           model == ColorModel::kXyz ? TYPE_XYZ_FLT :
                                       TYPE_RGB_FLT;
}

[[nodiscard]] float evaluate_curve(const ToneCurve &curve, const float input) noexcept
{
    if (curve.linear)
    {
        return input;
    }
    if (input >= 1.0F)
    {
        return curve.unbounded[1] * std::pow(input * curve.unbounded[0], curve.unbounded[2]);
    }
    const float value = std::max(input, 0.0F);
    const float scaled = value * static_cast<float>(curve.lut.size() - 1U);
    const std::size_t lower = std::min(static_cast<std::size_t>(scaled), curve.lut.size() - 2U);
    const float fraction = scaled - static_cast<float>(lower);
    return curve.lut[lower] * (1.0F - fraction) + curve.lut[lower + 1U] * fraction;
}

void blue_map(std::array<float, 3> &value) noexcept
{
    const float luminance = value[0] + value[1] + value[2];
    if (luminance <= 0.0F)
    {
        return;
    }
    const float z = value[2] / luminance;
    if (z <= 0.5F)
    {
        return;
    }
    const float t = (z - 0.5F) / 0.5F * std::min(1.0F, luminance / 0.5F);
    value[1] += t * 0.11F;
    value[2] -= t * 0.11F;
}

[[nodiscard]] Result<void> validate_input_buffer(const ProfiledColorBuffer &input)
{
    const std::uint64_t expected = static_cast<std::uint64_t>(input.width) * input.height * 3U;
    if (input.width == 0 || input.height == 0 || input.channels.size() != expected)
    {
        return make_error(ErrorCode::kValidation,
                          "Input colour buffer does not match its dimensions");
    }
    if (!std::all_of(input.channels.begin(), input.channels.end(),
                     [](const float value) { return std::isfinite(value); }))
    {
        return make_error(ErrorCode::kValidation, "Input colour buffer contains NaN or infinity");
    }
    return {};
}

[[nodiscard]] Result<LinearWorkingBuffer>
apply_matrix_profile(const ProfiledColorBuffer &input, const ProfileData &source,
                     const ProfileData &working, const std::optional<ProfileData> &normalize,
                     const bool map_blue, const CancellationToken &cancellation)
{
    auto working_inverse = invert_matrix(working.matrix_to_xyz_d50);
    if (!working_inverse)
    {
        return working_inverse.error();
    }
    std::optional<std::array<float, 9>> normalize_inverse;
    if (normalize)
    {
        auto inverse = invert_matrix(normalize->matrix_to_xyz_d50);
        if (!inverse)
        {
            return inverse.error();
        }
        normalize_inverse = inverse.value();
    }

    LinearWorkingBuffer output;
    output.width = input.width;
    output.height = input.height;
    output.rgb.resize(input.channels.size());
    output.color_profile = working.state;
    output.color_profile.matrix_to_xyz_d50 = working.matrix_to_xyz_d50;
    output.color_profile.has_matrix = true;

    for (std::uint32_t y = 0; y < input.height; ++y)
    {
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        for (std::uint32_t x = 0; x < input.width; ++x)
        {
            const std::size_t offset = (static_cast<std::size_t>(y) * input.width + x) * 3U;
            std::array<float, 3> linear{};
            for (std::size_t channel = 0; channel < 3U; ++channel)
            {
                linear[channel] =
                    evaluate_curve(source.curves[channel], input.channels[offset + channel]);
            }
            if (map_blue)
            {
                blue_map(linear);
            }
            auto xyz = apply_matrix(source.matrix_to_xyz_d50, linear);
            if (normalize && normalize_inverse)
            {
                auto normalized = apply_matrix(*normalize_inverse, xyz);
                for (auto &value : normalized)
                {
                    value = std::clamp(value, 0.0F, 1.0F);
                }
                xyz = apply_matrix(normalize->matrix_to_xyz_d50, normalized);
            }
            const auto result = apply_matrix(working_inverse.value(), xyz);
            if (!std::all_of(result.begin(), result.end(),
                             [](const float value) { return std::isfinite(value); }))
            {
                return make_error(ErrorCode::kValidation,
                                  "Input colour transform produced a non-finite pixel");
            }
            std::copy(result.begin(), result.end(),
                      output.rgb.begin() + static_cast<std::ptrdiff_t>(offset));
        }
    }
    return output;
}

[[nodiscard]] Result<LinearWorkingBuffer>
apply_lcms_profile(const ProfiledColorBuffer &input, const ProfileData &source,
                   const ProfileData &working, const std::optional<ProfileData> &normalize,
                   const bool map_blue, const int intent, const CancellationToken &cancellation)
{
    if (source.profile.get() == nullptr)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Input profile has no LittleCMS transform representation");
    }
    if (map_blue && source.model != ColorModel::kRgb)
    {
        return make_error(ErrorCode::kUnsupported, "Blue mapping requires an RGB input profile");
    }
    auto working_inverse = invert_matrix(working.matrix_to_xyz_d50);
    if (!working_inverse)
    {
        return working_inverse.error();
    }

    CmsProfile xyz_profile;
    cmsHPROFILE output_profile = nullptr;
    cmsUInt32Number output_format = TYPE_XYZ_FLT;
    if (normalize)
    {
        output_profile = normalize->profile.get();
        output_format = TYPE_RGB_FLT;
    }
    else
    {
        xyz_profile = CmsProfile(cmsCreateXYZProfile());
        output_profile = xyz_profile.get();
    }
    if (output_profile == nullptr)
    {
        return make_error(ErrorCode::kInternal,
                          "LittleCMS could not create the transform output profile");
    }
    CmsTransform transform(cmsCreateTransform(
        source.profile.get(), lcms_input_format(source.model), output_profile, output_format,
        static_cast<cmsUInt32Number>(intent), cmsFLAGS_NOCACHE | cmsFLAGS_NOOPTIMIZE));
    if (transform.get() == nullptr)
    {
        return make_error(ErrorCode::kUnsupported,
                          "LittleCMS could not create the requested ICC transform");
    }

    LinearWorkingBuffer output;
    output.width = input.width;
    output.height = input.height;
    output.rgb.resize(input.channels.size());
    output.color_profile = working.state;
    output.color_profile.matrix_to_xyz_d50 = working.matrix_to_xyz_d50;
    output.color_profile.has_matrix = true;
    std::vector<float> source_row(static_cast<std::size_t>(input.width) * 3U);
    std::vector<float> transformed_row(source_row.size());

    for (std::uint32_t y = 0; y < input.height; ++y)
    {
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        const std::size_t row_offset = static_cast<std::size_t>(y) * input.width * 3U;
        std::copy_n(input.channels.begin() + static_cast<std::ptrdiff_t>(row_offset),
                    source_row.size(), source_row.begin());
        if (map_blue)
        {
            for (std::size_t offset = 0; offset < source_row.size(); offset += 3U)
            {
                std::array<float, 3> pixel{source_row[offset], source_row[offset + 1U],
                                           source_row[offset + 2U]};
                blue_map(pixel);
                std::copy(pixel.begin(), pixel.end(),
                          source_row.begin() + static_cast<std::ptrdiff_t>(offset));
            }
        }
        cmsDoTransform(transform.get(), source_row.data(), transformed_row.data(), input.width);
        for (std::uint32_t x = 0; x < input.width; ++x)
        {
            const std::size_t offset = static_cast<std::size_t>(x) * 3U;
            std::array<float, 3> xyz{};
            if (normalize)
            {
                std::array<float, 3> normalized{transformed_row[offset],
                                                transformed_row[offset + 1U],
                                                transformed_row[offset + 2U]};
                for (auto &value : normalized)
                {
                    value = std::clamp(value, 0.0F, 1.0F);
                }
                xyz = apply_matrix(normalize->matrix_to_xyz_d50, normalized);
            }
            else
            {
                xyz = {transformed_row[offset], transformed_row[offset + 1U],
                       transformed_row[offset + 2U]};
            }
            const auto result = apply_matrix(working_inverse.value(), xyz);
            if (!std::all_of(result.begin(), result.end(),
                             [](const float value) { return std::isfinite(value); }))
            {
                return make_error(ErrorCode::kValidation,
                                  "ICC transform produced a non-finite pixel");
            }
            const std::size_t output_offset = row_offset + offset;
            std::copy(result.begin(), result.end(),
                      output.rgb.begin() + static_cast<std::ptrdiff_t>(output_offset));
        }
    }
    return output;
}

} // namespace

Result<InputColorParams> resolve_input_color(const Recipe &recipe)
try
{
    std::optional<InputColorParams> resolved;
    for (const auto &operation : recipe.operations)
    {
        if (!operation.enabled || operation.id != "ravo.color.input")
        {
            continue;
        }
        if (resolved)
        {
            return make_error(ErrorCode::kConflict,
                              "Recipe contains more than one input colour operation");
        }
        auto parsed = input_color_from_parameters(operation.parameters);
        if (!parsed)
        {
            return parsed.error();
        }
        resolved = std::move(parsed).value();
    }
    if (!resolved)
    {
        return make_error(ErrorCode::kValidation,
                          "Render recipe must declare an input colour operation");
    }
    return *resolved;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Input colour parameter allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<std::string> input_color_cache_fingerprint(const Recipe &recipe)
try
{
    auto params = resolve_input_color(recipe);
    if (!params)
    {
        return params.error();
    }
    std::string result = "builtin";
    if (params.value().input_profile == kInputProfileFileIcc)
    {
        auto bytes = read_profile_file(params.value().input_profile_filename);
        if (!bytes)
        {
            return bytes.error();
        }
        auto profile =
            open_icc_profile(std::move(bytes).value(), params.value().input_profile_filename);
        if (!profile)
        {
            return profile.error();
        }
        result = "input-" + color_profile_fingerprint(profile.value().state);
    }
    if (params.value().working_profile == kInputProfileFileIcc)
    {
        auto bytes = read_profile_file(params.value().working_profile_filename);
        if (!bytes)
        {
            return bytes.error();
        }
        auto profile =
            open_icc_profile(std::move(bytes).value(), params.value().working_profile_filename);
        if (!profile)
        {
            return profile.error();
        }
        result += "_working-" + color_profile_fingerprint(profile.value().state);
    }
    return result;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Input colour cache fingerprint allocation failed",
                      {{"reason", "allocation_failed"}});
}

namespace
{

Result<LinearWorkingBuffer> apply_input_color_impl(const ProfiledColorBuffer &input,
                                                   const InputColorParams &params,
                                                   const CancellationToken &cancellation)
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    auto valid = validate_input_buffer(input);
    if (!valid)
    {
        return valid.error();
    }
    if (params.blue_mapping && !input.color_profile.camera_input)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Blue mapping requires a declared RAW camera input profile");
    }
    auto source = source_profile(input.color_profile, params);
    if (!source)
    {
        return source.error();
    }
    auto working = working_profile(params);
    if (!working)
    {
        return working.error();
    }
    auto normalize = normalize_profile(params.gamut_normalize);
    if (!normalize)
    {
        return normalize.error();
    }

    if (source.value().matrix_shaper)
    {
        return apply_matrix_profile(input, source.value(), working.value(), normalize.value(),
                                    params.blue_mapping, cancellation);
    }
    return apply_lcms_profile(input, source.value(), working.value(), normalize.value(),
                              params.blue_mapping, lcms_intent(params.rendering_intent),
                              cancellation);
}

} // namespace

Result<LinearWorkingBuffer> apply_input_color(const ProfiledColorBuffer &input,
                                              const InputColorParams &params,
                                              const CancellationToken &cancellation)
try
{
    return apply_input_color_impl(input, params, cancellation);
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Input colour transform allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<LinearWorkingBuffer> convert_working_profile(const LinearWorkingBuffer &input,
                                                    const std::string_view target_profile,
                                                    const CancellationToken &cancellation)
try
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    if (!input.color_profile.has_matrix || input.color_profile.model != ColorModel::kRgb)
    {
        return make_error(ErrorCode::kValidation,
                          "Linear working buffer has no RGB profile matrix");
    }
    if (input.color_profile.identifier == target_profile)
    {
        return input;
    }
    ProfiledColorBuffer source;
    source.width = input.width;
    source.height = input.height;
    source.channels = input.rgb;
    source.color_profile = input.color_profile;
    source.color_profile.kind = ColorProfileKind::kMatrix;
    source.color_profile.icc_bytes.clear();

    InputColorParams params;
    params.input_profile = std::string(kInputProfileSource);
    params.working_profile = std::string(target_profile);
    return apply_input_color(source, params, cancellation);
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Working colour conversion allocation failed",
                      {{"reason", "allocation_failed"}});
}

} // namespace ravo
