#include "input_color.h"
#include "output_color.h"
#include "profile_gamma.h"

#include <algorithm>
#include <array>
#include <bit>
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

class CmsContext final
{
public:
    CmsContext()
        : handle_(cmsCreateContext(nullptr, nullptr))
    {
    }
    ~CmsContext()
    {
        if (handle_ != nullptr)
        {
            cmsDeleteContext(handle_);
        }
    }
    CmsContext(const CmsContext &) = delete;
    CmsContext &operator=(const CmsContext &) = delete;
    [[nodiscard]] cmsContext get() const noexcept
    {
        return handle_;
    }

private:
    cmsContext handle_ = nullptr;
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

class CmsToneCurve final
{
public:
    CmsToneCurve() = default;
    explicit CmsToneCurve(cmsToneCurve *handle)
        : handle_(handle)
    {
    }
    ~CmsToneCurve()
    {
        if (handle_ != nullptr)
        {
            cmsFreeToneCurve(handle_);
        }
    }
    CmsToneCurve(const CmsToneCurve &) = delete;
    CmsToneCurve &operator=(const CmsToneCurve &) = delete;
    CmsToneCurve(CmsToneCurve &&other) noexcept
        : handle_(std::exchange(other.handle_, nullptr))
    {
    }
    CmsToneCurve &operator=(CmsToneCurve &&other) noexcept
    {
        if (this != &other)
        {
            if (handle_ != nullptr)
            {
                cmsFreeToneCurve(handle_);
            }
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    [[nodiscard]] cmsToneCurve *get() const noexcept
    {
        return handle_;
    }

private:
    cmsToneCurve *handle_ = nullptr;
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
    std::array<ToneCurve, 3> output_curves;
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

[[nodiscard]] float srgb_encode_curve(const float linear) noexcept
{
    const float value = std::max(linear, 0.0F);
    return value <= 0.0031308F ? 12.92F * value : 1.055F * std::pow(value, 1.0F / 2.4F) - 0.055F;
}

[[nodiscard]] float rec709_encode_curve(const float linear) noexcept
{
    const float value = std::max(linear, 0.0F);
    return value < 0.018F ? 4.5F * value : 1.099F * std::pow(value, 0.45F) - 0.099F;
}

[[nodiscard]] float hlg_encode_curve(const float linear) noexcept
{
    constexpr float beta = 0.04F;
    constexpr float a = 0.17883277F;
    constexpr float b = 1.0F - 4.0F * a;
    const float c = 0.5F - a * std::log(4.0F * a);
    const float value = std::max(linear, 0.0F);
    const float encoded =
        value <= (1.0F / 12.0F) ? std::sqrt(3.0F * value) : a * std::log(12.0F * value - b) + c;
    return std::max((encoded - beta) / (1.0F - beta), 0.0F);
}

[[nodiscard]] float pq_encode_curve(const float linear) noexcept
{
    constexpr float m1 = 2610.0F / 16384.0F;
    constexpr float m2 = 2523.0F / 32.0F;
    constexpr float c1 = 3424.0F / 4096.0F;
    constexpr float c2 = 2413.0F / 128.0F;
    constexpr float c3 = 2392.0F / 128.0F;
    const float power = std::pow(std::max(linear, 0.0F), m1);
    return std::pow((c1 + c2 * power) / (1.0F + c3 * power), m2);
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
                              "Colour profile tone curve contains an invalid sample");
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
                          "Colour profile tone curve cannot be extrapolated");
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
                          "Colour profile extrapolation exponent is non-finite");
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

[[nodiscard]] Result<CmsProfile> make_rgb_profile(const std::string_view identifier,
                                                  const ToneCurve &decode_curve,
                                                  const bool force_tabulated = false)
{
    std::array<CmsToneCurve, 3> owned_curves;
    for (auto &owned : owned_curves)
    {
        cmsToneCurve *curve = nullptr;
        if (decode_curve.linear)
        {
            curve = cmsBuildGamma(nullptr, 1.0);
        }
        else if (force_tabulated)
        {
            curve = cmsBuildTabulatedToneCurveFloat(
                nullptr, static_cast<cmsUInt32Number>(decode_curve.lut.size()),
                decode_curve.lut.data());
        }
        else if (identifier == kInputProfileSrgb || identifier == kInputProfileDisplayP3 ||
                 identifier == kColorNormalizeSrgb)
        {
            const std::array<cmsFloat64Number, 5> params{2.4, 1.0 / 1.055, 0.055 / 1.055,
                                                         1.0 / 12.92, 0.04045};
            curve = cmsBuildParametricToneCurve(nullptr, 4, params.data());
        }
        else if (identifier == kInputProfileRec709)
        {
            const std::array<cmsFloat64Number, 5> params{1.0 / 0.45, 1.0 / 1.099, 0.099 / 1.099,
                                                         1.0 / 4.5, 0.081};
            curve = cmsBuildParametricToneCurve(nullptr, 4, params.data());
        }
        else if (identifier == kInputProfileAdobeRgb || identifier == kColorNormalizeAdobeRgb)
        {
            curve = cmsBuildGamma(nullptr, 563.0 / 256.0);
        }
        else
        {
            curve = cmsBuildTabulatedToneCurveFloat(
                nullptr, static_cast<cmsUInt32Number>(decode_curve.lut.size()),
                decode_curve.lut.data());
        }
        if (curve == nullptr)
        {
            return make_error(ErrorCode::kInternal,
                              "LittleCMS could not allocate a built-in tone curve");
        }
        owned = CmsToneCurve(curve);
    }
    std::array<cmsToneCurve *, 3> curves{owned_curves[0].get(), owned_curves[1].get(),
                                         owned_curves[2].get()};
    const auto chromaticities = chromaticities_for(identifier);
    CmsProfile profile(
        cmsCreateRGBProfile(&chromaticities.white, &chromaticities.primaries, curves.data()));
    if (profile.get() == nullptr)
    {
        return make_error(ErrorCode::kInternal,
                          "LittleCMS could not create a built-in RGB profile");
    }
    return std::move(profile);
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

    ProfileData result;
    result.model = ColorModel::kRgb;
    auto profile = make_rgb_profile(identifier, ToneCurve{});
    if (!profile)
    {
        return profile.error();
    }
    result.profile = std::move(profile).value();
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
    result.curves = {curve.value(), curve.value(), curve.value()};

    std::function<float(float)> output_evaluator = [](const float value) { return value; };
    if (identifier == kInputProfileSrgb || identifier == kInputProfileDisplayP3 ||
        identifier == kColorNormalizeSrgb)
    {
        output_evaluator = srgb_encode_curve;
    }
    else if (identifier == kInputProfileRec709)
    {
        output_evaluator = rec709_encode_curve;
    }
    else if (identifier == kInputProfileAdobeRgb || identifier == kColorNormalizeAdobeRgb)
    {
        output_evaluator = [](const float value)
        { return std::pow(std::max(value, 0.0F), 256.0F / 563.0F); };
    }
    else if (identifier == kInputProfilePqRec2020 || identifier == kInputProfilePqP3)
    {
        output_evaluator = pq_encode_curve;
    }
    else if (identifier == kInputProfileHlgRec2020 || identifier == kInputProfileHlgP3)
    {
        output_evaluator = hlg_encode_curve;
    }
    auto output_curve = sample_curve(output_evaluator, linear);
    if (!output_curve)
    {
        return output_curve.error();
    }
    result.output_curves = {output_curve.value(), output_curve.value(), output_curve.value()};
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
            const auto *source_curve =
                static_cast<const cmsToneCurve *>(cmsReadTag(result.profile.get(), tags[channel]));
            auto curve = sample_lcms_curve(source_curve);
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

[[nodiscard]] Result<void> prepare_icc_output_curves(ProfileData &profile)
{
    if (profile.model != ColorModel::kRgb || !profile.matrix_shaper)
    {
        return {};
    }
    const std::array<cmsTagSignature, 3> tags{cmsSigRedTRCTag, cmsSigGreenTRCTag, cmsSigBlueTRCTag};
    for (std::size_t channel = 0; channel < tags.size(); ++channel)
    {
        const auto *source_curve =
            static_cast<const cmsToneCurve *>(cmsReadTag(profile.profile.get(), tags[channel]));
        if (source_curve == nullptr)
        {
            return make_error(ErrorCode::kValidation,
                              "ICC output matrix profile is missing a tone curve");
        }
        CmsToneCurve inverse(
            cmsReverseToneCurveEx(static_cast<cmsUInt32Number>(kLutSamples), source_curve));
        if (inverse.get() == nullptr)
        {
            return make_error(ErrorCode::kUnsupported, "ICC output tone curve cannot be inverted");
        }
        auto output_curve = sample_lcms_curve(inverse.get());
        if (!output_curve)
        {
            return output_curve.error();
        }
        profile.output_curves[channel] = std::move(output_curve).value();
    }
    return {};
}

[[nodiscard]] Result<std::vector<std::uint8_t>> profile_bytes(const cmsHPROFILE profile)
{
    if (profile == nullptr)
    {
        return make_error(ErrorCode::kValidation,
                          "Output colour profile has no serializable ICC representation");
    }
    cmsUInt32Number size = 0;
    if (cmsSaveProfileToMem(profile, nullptr, &size) == 0 || size == 0U || size > kMaxIccBytes)
    {
        return make_error(ErrorCode::kValidation,
                          "Output colour profile cannot be serialized as bounded ICC data");
    }
    std::vector<std::uint8_t> bytes(size);
    cmsUInt32Number written = size;
    if (cmsSaveProfileToMem(profile, bytes.data(), &written) == 0 || written == 0U ||
        written > size)
    {
        return make_error(ErrorCode::kValidation, "Output colour profile ICC serialization failed");
    }
    bytes.resize(written);
    constexpr std::size_t kIccHeaderBytes = 128U;
    constexpr std::size_t kCreatedOffset = 24U;
    constexpr std::size_t kProfileIdOffset = 84U;
    if (bytes.size() < kIccHeaderBytes)
    {
        return make_error(ErrorCode::kValidation,
                          "Generated output ICC profile has a truncated header");
    }
    const auto write_u16 = [&bytes](const std::size_t offset, const std::uint16_t value)
    {
        bytes[offset] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
        bytes[offset + 1U] = static_cast<std::uint8_t>(value & 0xffU);
    };
    write_u16(kCreatedOffset, 2000U);
    write_u16(kCreatedOffset + 2U, 1U);
    write_u16(kCreatedOffset + 4U, 1U);
    write_u16(kCreatedOffset + 6U, 0U);
    write_u16(kCreatedOffset + 8U, 0U);
    write_u16(kCreatedOffset + 10U, 0U);
    std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(kProfileIdOffset), 16U, 0U);
    return bytes;
}

[[nodiscard]] Result<ProfileData> declared_output_profile(const OutputColorParams &params)
{
    Result<ProfileData> profile =
        make_error(ErrorCode::kInternal, "Output colour profile was not resolved");
    if (params.output_profile == kInputProfileFileIcc)
    {
        auto bytes = read_profile_file(params.output_profile_filename);
        if (!bytes)
        {
            return bytes.error();
        }
        profile = open_icc_profile(std::move(bytes).value(), params.output_profile_filename);
    }
    else
    {
        profile = make_builtin_profile(params.output_profile);
    }
    if (!profile)
    {
        return profile.error();
    }
    if (params.output_profile != kInputProfileFileIcc && profile.value().model == ColorModel::kRgb)
    {
        auto encoded = make_rgb_profile(params.output_profile, profile.value().curves[0]);
        if (!encoded)
        {
            return encoded.error();
        }
        profile.value().profile = std::move(encoded).value();
    }
    if (params.output_profile == kInputProfileFileIcc)
    {
        auto curves = prepare_icc_output_curves(profile.value());
        if (!curves)
        {
            return curves.error();
        }
    }
    if (profile.value().state.icc_bytes.empty())
    {
        auto bytes = profile_bytes(profile.value().profile.get());
        if (!bytes)
        {
            return bytes.error();
        }
        profile.value().state.icc_bytes = std::move(bytes).value();
    }
    return profile;
}

[[nodiscard]] Result<ProfileData> declared_proof_profile(const OutputColorParams &params)
{
    Result<ProfileData> profile =
        make_error(ErrorCode::kInternal, "Proof colour profile was not resolved");
    if (params.proof_profile == kInputProfileFileIcc)
    {
        auto bytes = read_profile_file(params.proof_profile_filename);
        if (!bytes)
        {
            return bytes.error();
        }
        profile = open_icc_profile(std::move(bytes).value(), params.proof_profile_filename);
    }
    else
    {
        profile = make_builtin_profile(params.proof_profile);
        if (profile && profile.value().model == ColorModel::kRgb && profile.value().matrix_shaper)
        {
            auto quantized =
                make_rgb_profile(params.proof_profile, profile.value().curves[0], true);
            if (!quantized)
            {
                return quantized.error();
            }
            profile.value().profile = std::move(quantized).value();
        }
    }
    if (!profile)
    {
        return profile.error();
    }
    if (profile.value().model != ColorModel::kRgb)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Proof colour profile must use an RGB colour model",
                          {{"profile", params.proof_profile}});
    }
    return profile;
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
    output.canonical_roi_scale = input.canonical_roi_scale;
    output.mask_attached_frame = input.mask_attached_frame;

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
    output.canonical_roi_scale = input.canonical_roi_scale;
    output.mask_attached_frame = input.mask_attached_frame;
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
    bool saw_input = false;
    for (const auto &operation : recipe.operations)
    {
        if (operation.id != "ravo.color.input")
        {
            continue;
        }
        saw_input = true;
        if (!operation.enabled)
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
    if (resolved)
    {
        return *resolved;
    }
    if (saw_input)
    {
        return InputColorParams{};
    }
    return make_error(ErrorCode::kValidation,
                      "Render recipe must declare an input colour operation");
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
    auto profile_gamma = resolve_profile_gamma(recipe);
    if (!profile_gamma)
    {
        return profile_gamma.error();
    }
    if (profile_gamma.value())
    {
        const auto &gamma = *profile_gamma.value();
        const auto bits = [](const double value)
        { return std::to_string(std::bit_cast<std::uint64_t>(value)); };
        result += "_profilegamma-mode=" + gamma.mode + ":linear_bits=" + bits(gamma.linear) +
                  ":gamma_bits=" + bits(gamma.gamma) +
                  ":dynamic_range_bits=" + bits(gamma.dynamic_range) +
                  ":grey_point_bits=" + bits(gamma.grey_point) +
                  ":shadows_range_bits=" + bits(gamma.shadows_range) +
                  ":security_factor_bits=" + bits(gamma.security_factor);
    }
    return result;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Input colour cache fingerprint allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<OutputColorParams> resolve_output_color(const Recipe &recipe)
try
{
    std::optional<OutputColorParams> resolved;
    bool saw_output = false;
    for (const auto &operation : recipe.operations)
    {
        if (operation.id != "ravo.color.output")
        {
            continue;
        }
        saw_output = true;
        if (!operation.enabled)
        {
            continue;
        }
        if (resolved)
        {
            return make_error(ErrorCode::kConflict,
                              "Recipe contains more than one output colour operation");
        }
        auto parsed = output_color_from_parameters(operation.parameters);
        if (!parsed)
        {
            return parsed.error();
        }
        resolved = std::move(parsed).value();
    }
    if (resolved)
    {
        return *resolved;
    }
    if (saw_output)
    {
        return OutputColorParams{};
    }
    return make_error(ErrorCode::kValidation,
                      "Render recipe must declare an output colour operation");
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Output colour parameter allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<std::string> output_color_cache_fingerprint(const Recipe &recipe)
try
{
    auto params = resolve_output_color(recipe);
    if (!params)
    {
        return params.error();
    }
    std::string result = "builtin";
    if (params.value().output_profile == kInputProfileFileIcc)
    {
        auto profile = declared_output_profile(params.value());
        if (!profile)
        {
            return profile.error();
        }
        result = "output-" + color_profile_fingerprint(profile.value().state);
    }
    if (params.value().proof_mode != kProofModeOff &&
        params.value().proof_profile == kInputProfileFileIcc)
    {
        auto profile = declared_proof_profile(params.value());
        if (!profile)
        {
            return profile.error();
        }
        result += "_proof-" + color_profile_fingerprint(profile.value().state);
    }
    return result;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Output colour cache fingerprint allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<void> validate_output_profile_state(const ColorProfileState &profile)
{
    if (profile.model != ColorModel::kRgb || profile.kind == ColorProfileKind::kMissing ||
        profile.icc_bytes.empty() || profile.icc_bytes.size() > kMaxIccBytes ||
        profile.icc_bytes.size() >
            static_cast<std::size_t>(std::numeric_limits<cmsUInt32Number>::max()))
    {
        return make_error(ErrorCode::kUnsupported,
                          "Encoded output requires bounded owned RGB ICC state",
                          {{"profile", profile.identifier}});
    }
    CmsProfile parsed(cmsOpenProfileFromMem(
        profile.icc_bytes.data(), static_cast<cmsUInt32Number>(profile.icc_bytes.size())));
    if (parsed.get() == nullptr)
    {
        return make_error(ErrorCode::kValidation, "Encoded output ICC profile is corrupt",
                          {{"profile", profile.identifier}});
    }
    if (cmsGetColorSpace(parsed.get()) != cmsSigRgbData)
    {
        return make_error(ErrorCode::kUnsupported,
                          "Encoded output ICC profile does not use an RGB colour model",
                          {{"profile", profile.identifier}});
    }
    return {};
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
    source.canonical_roi_scale = input.canonical_roi_scale;
    source.mask_attached_frame = input.mask_attached_frame;
    source.color_profile.kind = ColorProfileKind::kMatrix;
    source.color_profile.icc_bytes.clear();

    InputColorParams params;
    params.input_profile = std::string(kInputProfileSource);
    params.working_profile = std::string(target_profile);
    auto converted = apply_input_color(source, params, cancellation);
    if (!converted)
    {
        return converted.error();
    }
    converted.value().exposure_analysis = input.exposure_analysis;
    converted.value().canonical_roi_scale = input.canonical_roi_scale;
    converted.value().mask_attached_frame = input.mask_attached_frame;
    return converted;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Working colour conversion allocation failed",
                      {{"reason", "allocation_failed"}});
}

Result<ColorProfileState>
builtin_linear_working_profile_state(const std::string_view target_profile)
try
{
    auto profile = make_builtin_profile(target_profile);
    if (!profile)
        return profile.error();
    if (profile.value().model != ColorModel::kRgb || !profile.value().matrix_shaper)
        return make_error(ErrorCode::kUnsupported,
                          "3D LUT colour space must define RGB primaries",
                          {{"profile", std::string(target_profile)}});
    auto state = profile.value().state;
    state.kind = ColorProfileKind::kBuiltin;
    state.model = ColorModel::kRgb;
    state.identifier = std::string(target_profile);
    state.matrix_to_xyz_d50 = profile.value().matrix_to_xyz_d50;
    state.has_matrix = true;
    state.icc_bytes.clear();
    return state;
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "3D LUT profile allocation failed",
                      {{"reason", "allocation_failed"}});
}

namespace
{

[[nodiscard]] Result<ProfiledOutputBuffer>
apply_matrix_output(const LinearWorkingBuffer &input, const ProfileData &output_profile,
                    const CancellationToken &cancellation)
{
    auto output_inverse = invert_matrix(output_profile.matrix_to_xyz_d50);
    if (!output_inverse)
    {
        return output_inverse.error();
    }
    const bool same_primaries =
        input.color_profile.matrix_to_xyz_d50 == output_profile.matrix_to_xyz_d50;

    ProfiledOutputBuffer output;
    output.width = input.width;
    output.height = input.height;
    output.channels.resize(input.rgb.size());
    output.color_profile = output_profile.state;
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
            std::array<float, 3> linear{input.rgb[offset], input.rgb[offset + 1U],
                                        input.rgb[offset + 2U]};
            if (!same_primaries)
            {
                const auto xyz = apply_matrix(input.color_profile.matrix_to_xyz_d50, linear);
                linear = apply_matrix(output_inverse.value(), xyz);
            }
            for (std::size_t channel = 0; channel < 3U; ++channel)
            {
                const float encoded =
                    evaluate_curve(output_profile.output_curves[channel], linear[channel]);
                if (!std::isfinite(encoded))
                {
                    return make_error(ErrorCode::kValidation,
                                      "Output colour transform produced a non-finite pixel");
                }
                output.channels[offset + channel] = encoded;
            }
        }
    }
    return output;
}

[[nodiscard]] Result<ProfiledOutputBuffer> apply_lcms_output(const LinearWorkingBuffer &input,
                                                             const ProfileData &output_profile,
                                                             const ProfileData *const proof_profile,
                                                             const OutputColorParams &params,
                                                             const CancellationToken &cancellation)
{
    CmsContext context;
    if (context.get() == nullptr)
    {
        return make_error(ErrorCode::kInternal,
                          "LittleCMS could not allocate an output transform context");
    }
    CmsProfile xyz_profile(cmsCreateXYZProfileTHR(context.get()));
    if (xyz_profile.get() == nullptr)
    {
        return make_error(ErrorCode::kInternal,
                          "LittleCMS could not create the working XYZ profile");
    }
    cmsUInt32Number flags = cmsFLAGS_NOCACHE | cmsFLAGS_NOOPTIMIZE;
    if (params.black_point_compensation)
    {
        flags |= cmsFLAGS_BLACKPOINTCOMPENSATION;
    }
    cmsHTRANSFORM handle = nullptr;
    if (proof_profile != nullptr)
    {
        flags |= cmsFLAGS_SOFTPROOFING;
        if (params.proof_mode == kProofModeGamutCheck)
        {
            if (output_profile.model != ColorModel::kRgb)
            {
                return make_error(ErrorCode::kUnsupported,
                                  "Gamut warning requires an RGB output profile");
            }
            std::array<cmsUInt16Number, cmsMAXCHANNELS> alarms{};
            alarms[1] = std::numeric_limits<cmsUInt16Number>::max();
            alarms[2] = std::numeric_limits<cmsUInt16Number>::max();
            cmsSetAlarmCodesTHR(context.get(), alarms.data());
            flags |= cmsFLAGS_GAMUTCHECK;
        }
        handle = cmsCreateProofingTransformTHR(
            context.get(), xyz_profile.get(), TYPE_XYZ_FLT, output_profile.profile.get(),
            lcms_input_format(output_profile.model), proof_profile->profile.get(),
            static_cast<cmsUInt32Number>(lcms_intent(params.rendering_intent)),
            static_cast<cmsUInt32Number>(lcms_intent(params.proof_intent)), flags);
    }
    else
    {
        handle = cmsCreateTransformTHR(
            context.get(), xyz_profile.get(), TYPE_XYZ_FLT, output_profile.profile.get(),
            lcms_input_format(output_profile.model),
            static_cast<cmsUInt32Number>(lcms_intent(params.rendering_intent)), flags);
    }
    CmsTransform transform(handle);
    if (transform.get() == nullptr)
    {
        return make_error(
            ErrorCode::kUnsupported, "LittleCMS could not create the requested output transform",
            {{"output_profile", params.output_profile}, {"proof_mode", params.proof_mode}});
    }

    ProfiledOutputBuffer output;
    output.width = input.width;
    output.height = input.height;
    output.channels.resize(input.rgb.size());
    output.color_profile = output_profile.state;
    std::vector<float> xyz_row(static_cast<std::size_t>(input.width) * 3U);
    for (std::uint32_t y = 0; y < input.height; ++y)
    {
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        const std::size_t row_offset = static_cast<std::size_t>(y) * input.width * 3U;
        for (std::uint32_t x = 0; x < input.width; ++x)
        {
            const std::size_t offset = static_cast<std::size_t>(x) * 3U;
            const std::array<float, 3> linear{input.rgb[row_offset + offset],
                                              input.rgb[row_offset + offset + 1U],
                                              input.rgb[row_offset + offset + 2U]};
            const auto xyz = apply_matrix(input.color_profile.matrix_to_xyz_d50, linear);
            std::copy(xyz.begin(), xyz.end(),
                      xyz_row.begin() + static_cast<std::ptrdiff_t>(offset));
        }
        cmsDoTransform(transform.get(), xyz_row.data(), output.channels.data() + row_offset,
                       input.width);
        const auto row_begin = output.channels.begin() + static_cast<std::ptrdiff_t>(row_offset);
        const auto row_end = row_begin + static_cast<std::ptrdiff_t>(xyz_row.size());
        if (!std::all_of(row_begin, row_end,
                         [](const float value) { return std::isfinite(value); }))
        {
            return make_error(ErrorCode::kValidation,
                              "ICC output transform produced a non-finite pixel");
        }
    }
    return output;
}

Result<ProfiledOutputBuffer> apply_output_color_impl(const LinearWorkingBuffer &input,
                                                     const OutputColorParams &params,
                                                     const CancellationToken &cancellation)
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    auto valid_params = validate_output_color_parameters(output_color_to_parameters(params));
    if (!valid_params)
    {
        return valid_params.error();
    }
    const std::uint64_t pixels = static_cast<std::uint64_t>(input.width) * input.height;
    if (input.width == 0 || input.height == 0 ||
        pixels > std::numeric_limits<std::size_t>::max() / 3U ||
        input.rgb.size() != static_cast<std::size_t>(pixels * 3U) ||
        !input.color_profile.has_matrix || input.color_profile.model != ColorModel::kRgb ||
        !finite_matrix(input.color_profile.matrix_to_xyz_d50))
    {
        return make_error(ErrorCode::kValidation,
                          "Output colour input has invalid dimensions or working profile state");
    }
    if (!std::all_of(input.rgb.begin(), input.rgb.end(),
                     [](const float value) { return std::isfinite(value); }))
    {
        return make_error(ErrorCode::kValidation, "Output colour input contains NaN or infinity");
    }
    auto working_inverse = invert_matrix(input.color_profile.matrix_to_xyz_d50);
    if (!working_inverse)
    {
        return working_inverse.error();
    }
    auto output_profile = declared_output_profile(params);
    if (!output_profile)
    {
        return output_profile.error();
    }
    if (params.proof_mode == kProofModeOff && output_profile.value().model == ColorModel::kRgb &&
        output_profile.value().matrix_shaper)
    {
        return apply_matrix_output(input, output_profile.value(), cancellation);
    }

    std::optional<ProfileData> proof_profile;
    if (params.proof_mode != kProofModeOff)
    {
        auto resolved = declared_proof_profile(params);
        if (!resolved)
        {
            return resolved.error();
        }
        proof_profile = std::move(resolved).value();
    }
    return apply_lcms_output(input, output_profile.value(),
                             proof_profile ? &*proof_profile : nullptr, params, cancellation);
}

} // namespace

Result<ProfiledOutputBuffer> apply_output_color(const LinearWorkingBuffer &input,
                                                const OutputColorParams &params,
                                                const CancellationToken &cancellation)
try
{
    return apply_output_color_impl(input, params, cancellation);
}
catch (const std::bad_alloc &)
{
    return make_error(ErrorCode::kIo, "Output colour transform allocation failed",
                      {{"reason", "allocation_failed"}});
}

} // namespace ravo
