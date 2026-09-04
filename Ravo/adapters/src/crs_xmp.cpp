#include "ravo/adapters/crs_xmp.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <cstdint>
#include <limits>
#include <locale>
#include <map>
#include <numbers>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QStringView>
#include <QtCore/QXmlStreamReader>

#include "ravo/recipe/primaries.h"

namespace ravo
{
namespace
{

constexpr QLatin1String kCrsNs("http://ns.adobe.com/camera-raw-settings/1.0/");
constexpr double kCrsSlider = 100.0;
constexpr double kCrsCurveMax = 255.0;
constexpr double kCrsHueToRadians = (std::numbers::pi / 2.0) / kCrsSlider;
constexpr double kCrsShadowTintGreenHue = 120.0 * std::numbers::pi / 180.0;
constexpr double kCrsShadowTintMagentaHue = -60.0 * std::numbers::pi / 180.0;
constexpr double kCrsPositiveContrastSigmoid = 3.25;
constexpr double kCrsReferenceToneGamma = 0.62;
constexpr double kCrsCurveDeltaScale = 1.4;
constexpr std::size_t kCrsComposedCurvePoints = 20;

[[nodiscard]] constexpr auto supported_crs_process_versions()
{
    return std::array<std::string_view, 12>{"6.7",  "8.0",  "9.0",  "10.0", "11.0", "15.0",
                                            "15.1", "15.2", "15.3", "15.4", "16.0", "17.0"};
}

[[nodiscard]] std::string utf8(const QStringView value)
{
    return value.toString().toUtf8().toStdString();
}

[[nodiscard]] std::string utf8(const QString &value)
{
    return value.toUtf8().toStdString();
}

[[nodiscard]] bool near(const double a, const double b) noexcept
{
    return std::abs(a - b) <= 1.0e-9;
}

[[nodiscard]] TaskError crs_error(const std::string_view message, const std::string_view reason,
                                  const std::string_view key = {},
                                  const std::string_view value = {})
{
    std::map<std::string, std::string, std::less<>> context{{"reason", std::string(reason)}};
    if (!key.empty())
        context.emplace("key", std::string(key));
    if (!value.empty())
        context.emplace("value", std::string(value));
    return make_error(ErrorCode::kUnsupported, std::string(message), std::move(context));
}

[[nodiscard]] bool equals_ignore_case(const std::string_view text, const std::string_view expected)
{
    if (text.size() != expected.size())
        return false;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const auto a = static_cast<unsigned char>(text[i]);
        const auto b = static_cast<unsigned char>(expected[i]);
        if (std::tolower(a) != std::tolower(b))
            return false;
    }
    return true;
}

[[nodiscard]] std::string_view trim_view(std::string_view text) noexcept
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

[[nodiscard]] Result<double> parse_crs_number(const std::string_view text,
                                              const std::string_view key)
{
    auto body = trim_view(text);
    if (body.empty())
        return crs_error("CRS numeric field is empty", "invalid_crs_number", key, text);
    if (body.front() == '+')
        body.remove_prefix(1);
    body = trim_view(body);
    std::istringstream stream{std::string(body)};
    stream.imbue(std::locale::classic());
    stream >> std::noskipws;
    double value = 0.0;
    if (!(stream >> value) || stream.peek() != std::char_traits<char>::eof() ||
        !std::isfinite(value))
        return crs_error("CRS numeric field is invalid", "invalid_crs_number", key, text);
    return value;
}

[[nodiscard]] bool numeric_equals(const std::string_view actual, const std::string_view expected)
{
    auto a = parse_crs_number(actual, {});
    auto b = parse_crs_number(expected, {});
    return static_cast<bool>(a) && static_cast<bool>(b) && near(a.value(), b.value());
}

[[nodiscard]] bool identity_matches(const std::string_view actual, const std::string_view expected)
{
    if (equals_ignore_case(actual, expected))
        return true;
    if (equals_ignore_case(expected, "false") || equals_ignore_case(expected, "true"))
        return equals_ignore_case(actual, expected);
    return numeric_equals(actual, expected);
}

struct ParsedCrs
{
    std::map<std::string, std::string, std::less<>> attributes;
    std::string name;
    std::vector<std::string> master_curve;
    std::vector<std::string> red_curve;
    std::vector<std::string> green_curve;
    std::vector<std::string> blue_curve;
    std::vector<std::string> point_colors;
    std::string look_name;
};

[[nodiscard]] bool is_crs_node(const QXmlStreamReader &reader)
{
    return reader.namespaceUri() == kCrsNs;
}

[[nodiscard]] Result<std::vector<std::string>> read_rdf_seq(QXmlStreamReader &reader)
{
    std::vector<std::string> items;
    while (!reader.atEnd())
    {
        reader.readNext();
        if (reader.isEndElement() && is_crs_node(reader))
            break;
        if (!reader.isStartElement() || reader.name() != QLatin1String("li"))
            continue;
        items.push_back(utf8(reader.readElementText()));
    }
    if (reader.hasError())
        return crs_error("CRS XMP sequence is malformed", "invalid_crs_xml");
    return items;
}

[[nodiscard]] Result<std::string> read_crs_look(QXmlStreamReader &reader)
{
    std::string name;
    const auto take_name = [&](const QXmlStreamAttributes &attributes)
    {
        for (const auto &attribute : attributes)
        {
            if (attribute.namespaceUri() == kCrsNs && attribute.name() == QLatin1String("Name") &&
                !attribute.value().isEmpty())
            {
                name = utf8(attribute.value());
            }
        }
    };
    take_name(reader.attributes());
    int depth = 1;
    while (!reader.atEnd() && depth > 0)
    {
        reader.readNext();
        if (reader.isStartElement())
        {
            ++depth;
            take_name(reader.attributes());
        }
        else if (reader.isEndElement())
        {
            --depth;
        }
    }
    if (reader.hasError())
        return crs_error("CRS Look is malformed", "invalid_crs_xml");
    return name;
}

[[nodiscard]] Result<ParsedCrs> parse_crs_document(const std::string_view xmp_utf8)
{
    if (xmp_utf8.size() > static_cast<std::size_t>(std::numeric_limits<qsizetype>::max()))
        return crs_error("CRS XMP document is too large", "invalid_crs_xml");
    const QByteArray source(xmp_utf8.data(), static_cast<qsizetype>(xmp_utf8.size()));
    QXmlStreamReader reader(source);
    ParsedCrs parsed;
    bool found_crs = false;
    while (!reader.atEnd())
    {
        reader.readNext();
        if (!reader.isStartElement())
            continue;
        if (reader.name() == QLatin1String("Description"))
        {
            for (const auto &attribute : reader.attributes())
            {
                if (attribute.namespaceUri() != kCrsNs)
                    continue;
                found_crs = true;
                parsed.attributes.emplace(utf8(attribute.name()), utf8(attribute.value()));
            }
            continue;
        }
        if (!is_crs_node(reader))
            continue;
        found_crs = true;
        const auto name = utf8(reader.name());
        if (name == "Name" || name == "ShortName" || name == "SortName" || name == "Group" ||
            name == "Description")
        {
            QString text;
            while (!reader.atEnd())
            {
                reader.readNext();
                if (reader.isEndElement() && is_crs_node(reader) && utf8(reader.name()) == name)
                    break;
                if (reader.isStartElement() && reader.name() == QLatin1String("li"))
                    text = reader.readElementText();
                else if (reader.isCharacters() && text.isEmpty())
                    text += reader.text().toString();
            }
            if (name == "Name")
                parsed.name = utf8(text.trimmed());
            continue;
        }
        if (name == "ToneCurvePV2012")
        {
            auto items = read_rdf_seq(reader);
            if (!items)
                return items.error();
            parsed.master_curve = std::move(items).value();
            continue;
        }
        if (name == "ToneCurvePV2012Red")
        {
            auto items = read_rdf_seq(reader);
            if (!items)
                return items.error();
            parsed.red_curve = std::move(items).value();
            continue;
        }
        if (name == "ToneCurvePV2012Green")
        {
            auto items = read_rdf_seq(reader);
            if (!items)
                return items.error();
            parsed.green_curve = std::move(items).value();
            continue;
        }
        if (name == "ToneCurvePV2012Blue")
        {
            auto items = read_rdf_seq(reader);
            if (!items)
                return items.error();
            parsed.blue_curve = std::move(items).value();
            continue;
        }
        if (name == "PointColors")
        {
            auto items = read_rdf_seq(reader);
            if (!items)
                return items.error();
            parsed.point_colors = std::move(items).value();
            continue;
        }
        if (name == "Look")
        {
            auto look_name = read_crs_look(reader);
            if (!look_name)
                return look_name.error();
            parsed.look_name = std::move(look_name).value();
            continue;
        }
        parsed.attributes.emplace(name, utf8(reader.readElementText()));
    }
    if (reader.hasError())
        return crs_error("CRS XMP is malformed", "invalid_crs_xml");
    if (!found_crs)
        return crs_error("XMP document has no Camera Raw settings", "missing_crs_namespace");
    return parsed;
}

[[nodiscard]] const std::string *find_attr(const ParsedCrs &parsed, const std::string_view key)
{
    const auto found = parsed.attributes.find(std::string(key));
    return found == parsed.attributes.end() ? nullptr : &found->second;
}

[[nodiscard]] Result<std::optional<double>> optional_number(const ParsedCrs &parsed,
                                                            const std::string_view key)
{
    const auto *value = find_attr(parsed, key);
    if (value == nullptr)
        return std::optional<double>{};
    auto parsed_value = parse_crs_number(*value, key);
    if (!parsed_value)
        return parsed_value.error();
    return std::optional<double>{parsed_value.value()};
}

[[nodiscard]] Result<std::vector<ToneCurvePoint>>
map_crs_curve(const std::vector<std::string> &items, const std::string_view key)
{
    if (items.empty())
        return std::vector<ToneCurvePoint>{};
    if (items.size() < kToneCurveMinPoints || items.size() > kToneCurveMaxPoints)
        return crs_error("CRS tone curve has an unsupported node count", "unsupported_crs_curve",
                         key, std::to_string(items.size()));
    std::vector<ToneCurvePoint> points;
    points.reserve(items.size());
    for (const auto &item : items)
    {
        const auto comma = item.find(',');
        if (comma == std::string::npos)
            return crs_error("CRS tone curve node is not an x,y pair", "invalid_crs_curve", key,
                             item);
        auto x = parse_crs_number(std::string_view(item).substr(0, comma), key);
        auto y = parse_crs_number(std::string_view(item).substr(comma + 1), key);
        if (!x || !y)
            return !x ? x.error() : y.error();
        if (x.value() < 0.0 || x.value() > kCrsCurveMax || y.value() < 0.0 ||
            y.value() > kCrsCurveMax)
            return crs_error("CRS tone curve node is outside 0–255", "invalid_crs_curve", key,
                             item);
        points.push_back({x.value() / kCrsCurveMax, y.value() / kCrsCurveMax});
        if (points.size() > 1U && !(points.back().x > points[points.size() - 2U].x))
            return crs_error("CRS tone curve nodes must be strictly increasing",
                             "invalid_crs_curve", key, item);
    }
    return points;
}

[[nodiscard]] bool curve_is_linear(const std::vector<ToneCurvePoint> &points) noexcept
{
    return tone_curve_is_identity(points);
}

[[nodiscard]] RgbCurveParams
compose_crs_display_curves(const std::vector<ToneCurvePoint> &master,
                           const std::array<std::vector<ToneCurvePoint>, 3> &channels)
{
    RgbCurveParams result;
    result.mode = std::string(kRgbLevelsModeIndependent);
    result.preserve_colors = std::string(kToneCurvePreserveColorsNone);
    result.interpolation = std::string(kToneCurveInterpolationMonotoneHermite);
    result.application_space = std::string(kRgbCurveApplicationSpaceDisplaySrgb);
    for (std::size_t channel = 0; channel < result.channels.size(); ++channel)
    {
        auto &composed = result.channels[channel];
        composed.clear();
        composed.reserve(kCrsComposedCurvePoints);
        for (std::size_t index = 0; index < kCrsComposedCurvePoints; ++index)
        {
            const double input =
                static_cast<double>(index) / static_cast<double>(kCrsComposedCurvePoints - 1U);
            // Adobe's omitted profile/look places the same scene higher on its encoded tone
            // axis. Transfer the measured curve delta from that reference axis instead of
            // baking the unavailable Adobe baseline into every Ravo pixel.
            const double reference = std::pow(input, kCrsReferenceToneGamma);
            const double master_value = evaluate_tone_curve(master, reference);
            const double output = evaluate_tone_curve(channels[channel], master_value);
            composed.push_back(
                {input, std::clamp(input + kCrsCurveDeltaScale * (output - reference), 0.0, 1.0)});
        }
    }
    return result;
}

constexpr std::array<std::string_view, 8> kHslBands = {"Red",  "Orange", "Yellow", "Green",
                                                       "Aqua", "Blue",   "Purple", "Magenta"};

constexpr std::array<std::string_view, 28> kMetadataKeys = {
    "PresetType",
    "Cluster",
    "UUID",
    "SupportsAmount",
    "SupportsColor",
    "SupportsMonochrome",
    "SupportsHighDynamicRange",
    "SupportsNormalDynamicRange",
    "SupportsSceneReferred",
    "SupportsOutputReferred",
    "CameraModelRestriction",
    "Copyright",
    "ContactInfo",
    "Version",
    "HasSettings",
    "AlreadyApplied",
    "RawFileName",
    "CameraProfileDigest",
    "Temperature",
    "Tint",
    "OverrideLookVignette",
    "ToneCurveName2012",
    "Name",
    "ShortName",
    "SortName",
    "Group",
    "Description",
    "Look",
};

constexpr std::array<std::pair<std::string_view, std::string_view>, 33> kIdentityKeys = {{
    {"Texture", "0"},
    {"GrainSize", "25"},
    {"GrainFrequency", "50"},
    {"GrainSeed", "0"},
    {"PerspectiveVertical", "0"},
    {"PerspectiveHorizontal", "0"},
    {"PerspectiveRotate", "0"},
    {"PerspectiveScale", "100"},
    {"PerspectiveAspect", "0"},
    {"PerspectiveUpright", "0"},
    {"PerspectiveX", "0"},
    {"PerspectiveY", "0"},
    {"LuminanceNoiseReductionDetail", "50"},
    {"LuminanceNoiseReductionContrast", "0"},
    {"ColorNoiseReductionDetail", "50"},
    {"ColorNoiseReductionSmoothness", "50"},
    {"SharpenDetail", "25"},
    {"PostCropVignetteHighlightContrast", "0"},
    {"AutoLateralCA", "0"},
    {"LensProfileEnable", "0"},
    {"LensManualDistortionAmount", "0"},
    {"DefringePurpleAmount", "0"},
    {"DefringePurpleHueLo", "30"},
    {"DefringePurpleHueHi", "70"},
    {"DefringeGreenAmount", "0"},
    {"DefringeGreenHueLo", "40"},
    {"DefringeGreenHueHi", "60"},
    {"VignetteAmount", "0"},
    {"HDREditMode", "0"},
    {"AutoTone", "False"},
    {"ToggleStyleAmount", "0"},
    {"CurveRefineSaturation", "0"},
    {"Amount", "0"},
}};

[[nodiscard]] bool in_list(const std::string_view key,
                           const std::span<const std::string_view> keys) noexcept
{
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

[[nodiscard]] Result<void> validate_identity_point_colors(const ParsedCrs &parsed)
{
    for (const auto &item : parsed.point_colors)
    {
        std::string_view remaining = item;
        while (!remaining.empty())
        {
            const auto comma = remaining.find(',');
            const auto field = remaining.substr(0, comma);
            auto value = parse_crs_number(field, "PointColors");
            if (!value)
                return value.error();
            if (!near(value.value(), -1.0))
                return crs_error("CRS Point Colors has no Ravo mapping at a non-default value",
                                 "unsupported_crs_key", "PointColors", item);
            if (comma == std::string_view::npos)
                break;
            remaining.remove_prefix(comma + 1U);
        }
    }
    return {};
}

[[nodiscard]] Result<void> reject_unknown_and_identity(const ParsedCrs &parsed,
                                                       std::vector<CrsOmission> &omitted)
{
    static constexpr auto kAllowedProcess = supported_crs_process_versions();
    static constexpr std::array<std::string_view, 5> kDefaultProfiles = {
        "Adobe Standard", "Adobe Color", "Embedded", "Camera Settings", "Default"};
    static constexpr std::array<std::string_view, 1> kDefaultLooks = {"Adobe Color"};
    static constexpr std::array<std::string_view, 72> kMappedKeys = {
        "ProcessVersion",
        "WhiteBalance",
        "CameraProfile",
        "Saturation",
        "Vibrance",
        "Sharpness",
        "SharpenRadius",
        "SharpenEdgeMasking",
        "LuminanceSmoothing",
        "ColorNoiseReduction",
        "ShadowTint",
        "RedHue",
        "RedSaturation",
        "GreenHue",
        "GreenSaturation",
        "BlueHue",
        "BlueSaturation",
        "HueAdjustmentRed",
        "HueAdjustmentOrange",
        "HueAdjustmentYellow",
        "HueAdjustmentGreen",
        "HueAdjustmentAqua",
        "HueAdjustmentBlue",
        "HueAdjustmentPurple",
        "HueAdjustmentMagenta",
        "SaturationAdjustmentRed",
        "SaturationAdjustmentOrange",
        "SaturationAdjustmentYellow",
        "SaturationAdjustmentGreen",
        "SaturationAdjustmentAqua",
        "SaturationAdjustmentBlue",
        "SaturationAdjustmentPurple",
        "SaturationAdjustmentMagenta",
        "LuminanceAdjustmentRed",
        "LuminanceAdjustmentOrange",
        "LuminanceAdjustmentYellow",
        "LuminanceAdjustmentGreen",
        "LuminanceAdjustmentAqua",
        "LuminanceAdjustmentBlue",
        "LuminanceAdjustmentPurple",
        "LuminanceAdjustmentMagenta",
        "SplitToningShadowHue",
        "SplitToningShadowSaturation",
        "SplitToningHighlightHue",
        "SplitToningHighlightSaturation",
        "SplitToningBalance",
        "ParametricShadows",
        "ParametricDarks",
        "ParametricLights",
        "ParametricHighlights",
        "ParametricShadowSplit",
        "ParametricMidtoneSplit",
        "ParametricHighlightSplit",
        "PostCropVignetteAmount",
        "PostCropVignetteMidpoint",
        "PostCropVignetteFeather",
        "PostCropVignetteRoundness",
        "PostCropVignetteStyle",
        "GrainAmount",
        "Exposure2012",
        "Contrast2012",
        "Highlights2012",
        "Shadows2012",
        "Whites2012",
        "Blacks2012",
        "Clarity2012",
        "Dehaze",
        "ToneCurvePV2012",
        "ToneCurvePV2012Red",
        "ToneCurvePV2012Green",
        "ToneCurvePV2012Blue",
        "ConvertToGrayscale",
    };

    if (const auto *process = find_attr(parsed, "ProcessVersion"); process != nullptr)
    {
        if (!in_list(*process, kAllowedProcess))
            return crs_error("CRS process version is not the mapped PV2012 dialect",
                             "unsupported_crs_process_version", "ProcessVersion", *process);
    }

    if (const auto *wb = find_attr(parsed, "WhiteBalance"); wb != nullptr)
    {
        if (*wb != "As Shot")
            return crs_error("CRS white balance is not As Shot; Kelvin/tint is not stored",
                             "unsupported_crs_white_balance", "WhiteBalance", *wb);
    }
    for (const auto key : {std::string_view{"Temperature"}, std::string_view{"Tint"}})
    {
        if (const auto *value = find_attr(parsed, key); value != nullptr)
            omitted.push_back(
                {std::string(key), *value, "as_shot_white_balance_metadata_not_applied"});
    }

    if (const auto *profile = find_attr(parsed, "CameraProfile"); profile != nullptr)
    {
        if (!in_list(*profile, kDefaultProfiles))
            return crs_error("CRS camera profile has no Ravo DCP owner",
                             "unsupported_crs_camera_profile", "CameraProfile", *profile);
        omitted.push_back({"CameraProfile", *profile, "adobe_camera_profile_not_applied"});
    }

    if (!parsed.look_name.empty())
    {
        if (!in_list(parsed.look_name, kDefaultLooks))
            return crs_error("CRS Look name requires an Adobe look that Ravo does not own",
                             "unsupported_crs_look", "Look.Name", parsed.look_name);
        omitted.push_back({"Look.Name", parsed.look_name, "adobe_look_not_applied"});
    }

    if (auto point_colors = validate_identity_point_colors(parsed); !point_colors)
        return point_colors.error();

    if (const auto *curve_name = find_attr(parsed, "ToneCurveName2012"); curve_name != nullptr)
    {
        if (*curve_name != "Custom" && *curve_name != "Linear" && *curve_name != "Linear Contrast")
            return crs_error("Named CRS tone curve is not imported without PV2012 point lists",
                             "unsupported_crs_named_curve", "ToneCurveName2012", *curve_name);
    }

    for (const auto &[key, value] : parsed.attributes)
    {
        if (in_list(key, kMetadataKeys) || in_list(key, kMappedKeys))
            continue;
        if (key == "CurveRefineSaturation" &&
            (identity_matches(value, "0") || identity_matches(value, "100")))
            continue;
        bool identity = false;
        for (const auto &[name, expected] : kIdentityKeys)
        {
            if (key != name)
                continue;
            if (!identity_matches(value, expected))
                return crs_error("CRS field has no Ravo mapping at a non-default value",
                                 "unsupported_crs_key", key, value);
            identity = true;
            break;
        }
        if (identity)
            continue;
        if (key.rfind("ColorGrade", 0) == 0)
        {
            if (!identity_matches(value, "0") && !identity_matches(value, "50") &&
                !identity_matches(value, "False"))
                return crs_error("CRS Color Grading has no Ravo mapping at a non-default value",
                                 "unsupported_crs_key", key, value);
            continue;
        }
        return crs_error("CRS field has no Ravo mapping", "unsupported_crs_key", key, value);
    }
    return {};
}

[[nodiscard]] Result<double> map_signed_slider(const double value, const std::string_view key,
                                               const double scale = 1.0 / kCrsSlider)
{
    const double mapped = value * scale;
    if (!std::isfinite(mapped))
        return crs_error("CRS slider maps to a non-finite Ravo value", "invalid_crs_number", key);
    return mapped;
}

[[nodiscard]] Result<void>
assign_hsl(DevelopParams &look, CrsLookMask &mask, const ParsedCrs &parsed,
           const std::string_view prefix,
           std::array<double, kColorEqualizerBandCount> DevelopParams::*member,
           bool CrsLookMask::*flag, const double scale)
{
    bool any = false;
    std::array<double, kColorEqualizerBandCount> bands{};
    for (std::size_t index = 0; index < kHslBands.size(); ++index)
    {
        const std::string key = std::string(prefix) + std::string(kHslBands[index]);
        auto value = optional_number(parsed, key);
        if (!value)
            return value.error();
        if (!value.value())
            continue;
        auto mapped = map_signed_slider(*value.value(), key, scale);
        if (!mapped)
            return mapped.error();
        bands[index] = mapped.value();
        any = true;
    }
    if (any)
    {
        look.*member = bands;
        mask.*flag = true;
    }
    return {};
}

[[nodiscard]] Result<void> map_document(const ParsedCrs &parsed, DevelopParams &look,
                                        CrsLookMask &mask, std::vector<CrsOmission> &omitted)
{
    if (find_attr(parsed, "WhiteBalance") != nullptr)
    {
        look.temperature.mode = std::string(kTemperatureModeAsShot);
        look.temperature.coefficients.reset();
        mask.white_balance = true;
    }

    const auto assign_slider = [&](const std::string_view key, double DevelopParams::*field,
                                   bool CrsLookMask::*flag, const double scale) -> Result<void>
    {
        auto value = optional_number(parsed, key);
        if (!value)
            return value.error();
        if (!value.value())
            return {};
        auto mapped = map_signed_slider(*value.value(), key, scale);
        if (!mapped)
            return mapped.error();
        look.*field = mapped.value();
        mask.*flag = true;
        return {};
    };

    if (auto status =
            assign_slider("Exposure2012", &DevelopParams::exposure_ev, &CrsLookMask::exposure, 1.0);
        !status)
        return status.error();
    if (auto contrast = optional_number(parsed, "Contrast2012"); !contrast)
    {
        return contrast.error();
    }
    else if (contrast.value())
    {
        auto mapped = map_signed_slider(*contrast.value(), "Contrast2012");
        if (!mapped)
            return mapped.error();
        look.contrast = mapped.value();
        look.sigmoid_contrast =
            kSigmoidContrastDefault *
            std::pow(kCrsPositiveContrastSigmoid / kSigmoidContrastDefault, mapped.value());
        mask.contrast = true;
    }
    if (auto status = assign_slider("Highlights2012", &DevelopParams::highlights,
                                    &CrsLookMask::highlights, 1.0 / kCrsSlider);
        !status)
        return status.error();
    if (auto status = assign_slider("Shadows2012", &DevelopParams::shadows, &CrsLookMask::shadows,
                                    1.0 / kCrsSlider);
        !status)
        return status.error();
    if (auto status = assign_slider("Whites2012", &DevelopParams::whites, &CrsLookMask::whites,
                                    1.0 / kCrsSlider);
        !status)
        return status.error();
    if (auto status = assign_slider("Blacks2012", &DevelopParams::blacks, &CrsLookMask::blacks,
                                    1.0 / kCrsSlider);
        !status)
        return status.error();
    if (auto status = assign_slider("Vibrance", &DevelopParams::vibrance, &CrsLookMask::vibrance,
                                    1.0 / kCrsSlider);
        !status)
        return status.error();
    if (auto status = assign_slider("Saturation", &DevelopParams::saturation,
                                    &CrsLookMask::saturation, 1.0 / kCrsSlider);
        !status)
        return status.error();
    if (auto status = assign_slider("Clarity2012", &DevelopParams::clarity, &CrsLookMask::clarity,
                                    1.0 / kCrsSlider);
        !status)
        return status.error();
    if (auto status =
            assign_slider("Dehaze", &DevelopParams::dehaze, &CrsLookMask::dehaze, 1.0 / kCrsSlider);
        !status)
        return status.error();
    if (auto status = assign_slider("GrainAmount", &DevelopParams::grain, &CrsLookMask::grain,
                                    1.0 / kCrsSlider);
        !status)
        return status.error();

    if (auto status = assign_hsl(look, mask, parsed, "HueAdjustment", &DevelopParams::color_eq_hue,
                                 &CrsLookMask::color_eq_hue, 0.5 / kCrsSlider);
        !status)
        return status.error();
    if (auto status =
            assign_hsl(look, mask, parsed, "SaturationAdjustment", &DevelopParams::color_eq_sat,
                       &CrsLookMask::color_eq_sat, 1.0 / kCrsSlider);
        !status)
        return status.error();
    if (auto status =
            assign_hsl(look, mask, parsed, "LuminanceAdjustment", &DevelopParams::color_eq_light,
                       &CrsLookMask::color_eq_light, 1.0 / kCrsSlider);
        !status)
        return status.error();

    const bool has_split = find_attr(parsed, "SplitToningShadowHue") != nullptr ||
                           find_attr(parsed, "SplitToningShadowSaturation") != nullptr ||
                           find_attr(parsed, "SplitToningHighlightHue") != nullptr ||
                           find_attr(parsed, "SplitToningHighlightSaturation") != nullptr ||
                           find_attr(parsed, "SplitToningBalance") != nullptr;
    if (has_split)
    {
        auto shadow_sat = optional_number(parsed, "SplitToningShadowSaturation");
        auto highlight_sat = optional_number(parsed, "SplitToningHighlightSaturation");
        if (!shadow_sat || !highlight_sat)
            return !shadow_sat ? shadow_sat.error() : highlight_sat.error();
        const double ss = shadow_sat.value().value_or(0.0) / kCrsSlider;
        const double hs = highlight_sat.value().value_or(0.0) / kCrsSlider;
        if (!near(ss, 0.0) || !near(hs, 0.0))
        {
            auto shadow_hue = optional_number(parsed, "SplitToningShadowHue");
            auto highlight_hue = optional_number(parsed, "SplitToningHighlightHue");
            auto balance = optional_number(parsed, "SplitToningBalance");
            if (!shadow_hue || !highlight_hue || !balance)
                return !shadow_hue    ? shadow_hue.error() :
                       !highlight_hue ? highlight_hue.error() :
                                        balance.error();
            look.split_toning_present = true;
            look.split_toning_enabled = true;
            look.split_toning.shadow_hue = shadow_hue.value().value_or(0.0) / 360.0;
            look.split_toning.highlight_hue = highlight_hue.value().value_or(0.0) / 360.0;
            look.split_toning.shadow_saturation = ss;
            look.split_toning.highlight_saturation = hs;
            const double crs_balance = balance.value().value_or(0.0);
            look.split_toning.balance = std::clamp(0.5 - crs_balance / 200.0, 0.0, 1.0);
            mask.split_toning = true;
        }
    }

    auto parametric_shadows = optional_number(parsed, "ParametricShadows");
    auto parametric_darks = optional_number(parsed, "ParametricDarks");
    auto parametric_lights = optional_number(parsed, "ParametricLights");
    auto parametric_highlights = optional_number(parsed, "ParametricHighlights");
    if (!parametric_shadows || !parametric_darks || !parametric_lights || !parametric_highlights)
        return !parametric_shadows ? parametric_shadows.error() :
               !parametric_darks   ? parametric_darks.error() :
               !parametric_lights  ? parametric_lights.error() :
                                     parametric_highlights.error();
    const bool parametric_active =
        (parametric_shadows.value() && !near(*parametric_shadows.value(), 0.0)) ||
        (parametric_darks.value() && !near(*parametric_darks.value(), 0.0)) ||
        (parametric_lights.value() && !near(*parametric_lights.value(), 0.0)) ||
        (parametric_highlights.value() && !near(*parametric_highlights.value(), 0.0));

    auto master = map_crs_curve(parsed.master_curve, "ToneCurvePV2012");
    auto red = map_crs_curve(parsed.red_curve, "ToneCurvePV2012Red");
    auto green = map_crs_curve(parsed.green_curve, "ToneCurvePV2012Green");
    auto blue = map_crs_curve(parsed.blue_curve, "ToneCurvePV2012Blue");
    if (!master || !red || !green || !blue)
        return !master ? master.error() :
               !red    ? red.error() :
               !green  ? green.error() :
                         blue.error();
    const bool rgb_custom = !curve_is_linear(red.value()) || !curve_is_linear(green.value()) ||
                            !curve_is_linear(blue.value());
    const bool master_custom = !curve_is_linear(master.value());
    if (parametric_active && rgb_custom)
        return crs_error(
            "CRS parametric curve cannot map onto independent RGB point curves together",
            "unsupported_crs_parametric_with_channel_curves");

    if (rgb_custom || (master_custom && !parametric_active))
    {
        const std::array<std::vector<ToneCurvePoint>, 3> channels{
            red.value().empty() ? std::vector<ToneCurvePoint>{{0.0, 0.0}, {1.0, 1.0}} : red.value(),
            green.value().empty() ? std::vector<ToneCurvePoint>{{0.0, 0.0}, {1.0, 1.0}} :
                                    green.value(),
            blue.value().empty() ? std::vector<ToneCurvePoint>{{0.0, 0.0}, {1.0, 1.0}} :
                                   blue.value(),
        };
        look.rgb_curve = compose_crs_display_curves(master.value(), channels);
        mask.rgb_curve = true;
    }
    else if (master_custom || parametric_active)
    {
        look.rgb_curve.mode = std::string(kRgbLevelsModeLinked);
        if (master_custom)
            look.rgb_curve.channels[0] = std::move(master).value();
        if (parametric_active)
        {
            look.rgb_curve.parametric_shadows =
                parametric_shadows.value().value_or(0.0) / kCrsSlider;
            look.rgb_curve.parametric_darks = parametric_darks.value().value_or(0.0) / kCrsSlider;
            look.rgb_curve.parametric_lights = parametric_lights.value().value_or(0.0) / kCrsSlider;
            look.rgb_curve.parametric_highlights =
                parametric_highlights.value().value_or(0.0) / kCrsSlider;
        }
        auto split0 = optional_number(parsed, "ParametricShadowSplit");
        auto split1 = optional_number(parsed, "ParametricMidtoneSplit");
        auto split2 = optional_number(parsed, "ParametricHighlightSplit");
        if (!split0 || !split1 || !split2)
            return !split0 ? split0.error() : !split1 ? split1.error() : split2.error();
        if (split0.value())
            look.rgb_curve.parametric_split_shadows = *split0.value() / kCrsSlider;
        if (split1.value())
            look.rgb_curve.parametric_split_mid = *split1.value() / kCrsSlider;
        if (split2.value())
            look.rgb_curve.parametric_split_highlights = *split2.value() / kCrsSlider;
        mask.rgb_curve = true;
    }

    const bool has_cal =
        find_attr(parsed, "RedHue") != nullptr || find_attr(parsed, "RedSaturation") != nullptr ||
        find_attr(parsed, "GreenHue") != nullptr ||
        find_attr(parsed, "GreenSaturation") != nullptr ||
        find_attr(parsed, "BlueHue") != nullptr || find_attr(parsed, "BlueSaturation") != nullptr ||
        find_attr(parsed, "ShadowTint") != nullptr;
    if (has_cal)
    {
        const auto hue = [&](const std::string_view key) -> Result<double>
        {
            auto value = optional_number(parsed, key);
            if (!value)
                return value.error();
            return value.value() ? *value.value() * kCrsHueToRadians : 0.0;
        };
        const auto purity = [&](const std::string_view key) -> Result<double>
        {
            auto value = optional_number(parsed, key);
            if (!value)
                return value.error();
            return value.value() ? 1.0 + *value.value() / kCrsSlider : 1.0;
        };
        auto red_hue = hue("RedHue");
        auto green_hue = hue("GreenHue");
        auto blue_hue = hue("BlueHue");
        auto red_purity = purity("RedSaturation");
        auto green_purity = purity("GreenSaturation");
        auto blue_purity = purity("BlueSaturation");
        if (!red_hue || !green_hue || !blue_hue || !red_purity || !green_purity || !blue_purity)
            return !red_hue      ? red_hue.error() :
                   !green_hue    ? green_hue.error() :
                   !blue_hue     ? blue_hue.error() :
                   !red_purity   ? red_purity.error() :
                   !green_purity ? green_purity.error() :
                                   blue_purity.error();
        look.primaries.red_hue = red_hue.value();
        look.primaries.green_hue = green_hue.value();
        look.primaries.blue_hue = blue_hue.value();
        look.primaries.red_purity = red_purity.value();
        look.primaries.green_purity = green_purity.value();
        look.primaries.blue_purity = blue_purity.value();
        auto tint = optional_number(parsed, "ShadowTint");
        if (!tint)
            return tint.error();
        if (tint.value() && !near(*tint.value(), 0.0))
        {
            look.primaries.achromatic_tint_purity = std::min(
                kPrimariesAchromaticTintPurityMax, std::abs(*tint.value()) / kCrsSlider * 0.5);
            look.primaries.achromatic_tint_hue =
                *tint.value() < 0.0 ? kCrsShadowTintGreenHue : kCrsShadowTintMagentaHue;
        }
        mask.primaries = true;
    }

    auto sharpness = optional_number(parsed, "Sharpness");
    if (!sharpness)
        return sharpness.error();
    if (sharpness.value() && !near(*sharpness.value(), 0.0))
    {
        look.sharpen = *sharpness.value() / kCrsSlider;
        auto radius = optional_number(parsed, "SharpenRadius");
        auto masking = optional_number(parsed, "SharpenEdgeMasking");
        if (!radius || !masking)
            return !radius ? radius.error() : masking.error();
        if (radius.value())
            look.sharpen_radius = *radius.value();
        if (masking.value())
            look.sharpen_threshold = *masking.value();
        mask.sharpen = true;
    }

    auto luma_nr = optional_number(parsed, "LuminanceSmoothing");
    auto chroma_nr = optional_number(parsed, "ColorNoiseReduction");
    if (!luma_nr || !chroma_nr)
        return !luma_nr ? luma_nr.error() : chroma_nr.error();
    if ((luma_nr.value() && !near(*luma_nr.value(), 0.0)) ||
        (chroma_nr.value() && !near(*chroma_nr.value(), 0.0)))
    {
        look.denoise = luma_nr.value().value_or(0.0) / kCrsSlider;
        look.denoise_chroma = chroma_nr.value().value_or(0.0) / kCrsSlider;
        mask.denoise = true;
    }

    auto vig_amount = optional_number(parsed, "PostCropVignetteAmount");
    if (!vig_amount)
        return vig_amount.error();
    if (vig_amount.value() && !near(*vig_amount.value(), 0.0))
    {
        look.vignette = -(*vig_amount.value()) / kCrsSlider;
        auto midpoint = optional_number(parsed, "PostCropVignetteMidpoint");
        auto feather = optional_number(parsed, "PostCropVignetteFeather");
        auto roundness = optional_number(parsed, "PostCropVignetteRoundness");
        if (!midpoint || !feather || !roundness)
            return !midpoint ? midpoint.error() : !feather ? feather.error() : roundness.error();
        if (midpoint.value())
            look.vignette_midpoint = *midpoint.value() / kCrsSlider;
        if (feather.value())
            look.vignette_falloff = std::max(0.05, *feather.value() / kCrsSlider);
        if (roundness.value())
            look.vignette_shape = std::clamp(1.0 + *roundness.value() / kCrsSlider, 0.5, 5.0);
        if (const auto *style = find_attr(parsed, "PostCropVignetteStyle"); style != nullptr)
            omitted.push_back({"PostCropVignetteStyle", *style, "crs_vignette_style_approximated"});
        mask.vignette = true;
    }

    if (const auto *gray = find_attr(parsed, "ConvertToGrayscale");
        gray != nullptr && equals_ignore_case(*gray, "true"))
    {
        look.monochrome_present = true;
        look.monochrome_enabled = true;
        mask.grayscale = true;
    }

    return {};
}

} // namespace

bool is_crs_xmp_document(const std::string_view xmp_utf8) noexcept
{
    return xmp_utf8.find(kCrsNamespaceUri) != std::string_view::npos;
}

Result<std::string> crs_xmp_preset_name(const std::string_view xmp_utf8)
{
    auto parsed = parse_crs_document(xmp_utf8);
    if (!parsed)
        return parsed.error();
    return parsed.value().name;
}

void apply_crs_look(DevelopParams &dest, const DevelopParams &look, const CrsLookMask &mask)
{
    if (mask.white_balance)
    {
        dest.temperature = look.temperature;
        dest.white_balance_effect_enabled = true;
    }
    if (mask.exposure)
        dest.exposure_ev = look.exposure_ev;
    if (mask.contrast)
    {
        if (dest.sigmoid_enabled)
        {
            dest.sigmoid_contrast = look.sigmoid_contrast;
            dest.contrast = 0.0;
        }
        else
        {
            dest.contrast = look.contrast;
        }
    }
    if (mask.highlights)
        dest.highlights = look.highlights;
    if (mask.shadows)
        dest.shadows = look.shadows;
    if (mask.whites)
        dest.whites = look.whites;
    if (mask.blacks)
        dest.blacks = look.blacks;
    if (mask.exposure || mask.contrast || mask.highlights || mask.shadows || mask.whites ||
        mask.blacks)
        dest.light_effect_enabled = true;
    if (mask.vibrance)
        dest.vibrance = look.vibrance;
    if (mask.saturation)
        dest.saturation = look.saturation;
    if (mask.clarity)
        dest.clarity = look.clarity;
    if (mask.dehaze)
    {
        dest.dehaze = look.dehaze;
        dest.effects_effect_enabled = true;
    }
    if (mask.vibrance || mask.saturation)
        dest.color_effect_enabled = true;
    if (mask.color_eq_hue)
        dest.color_eq_hue = look.color_eq_hue;
    if (mask.color_eq_sat)
        dest.color_eq_sat = look.color_eq_sat;
    if (mask.color_eq_light)
        dest.color_eq_light = look.color_eq_light;
    if (mask.color_eq_hue || mask.color_eq_sat || mask.color_eq_light)
        dest.color_eq_effect_enabled = true;
    if (mask.split_toning)
    {
        dest.split_toning_present = look.split_toning_present;
        dest.split_toning_enabled = look.split_toning_enabled;
        dest.split_toning = look.split_toning;
        dest.color_effect_enabled = true;
    }
    if (mask.rgb_curve)
        dest.rgb_curve = look.rgb_curve;
    if (mask.tone_curve)
    {
        dest.tone_curve = look.tone_curve;
        dest.tone_curve_working_space = look.tone_curve_working_space;
        dest.tone_curve_interpolation = look.tone_curve_interpolation;
        dest.tone_curve_channel_mode = look.tone_curve_channel_mode;
        dest.tone_curve_preserve_colors = look.tone_curve_preserve_colors;
    }
    if (mask.rgb_curve || mask.tone_curve)
        dest.curves_effect_enabled = true;
    if (mask.primaries)
    {
        dest.primaries = look.primaries;
        dest.primaries_effect_enabled = true;
    }
    if (mask.sharpen)
    {
        dest.sharpen = look.sharpen;
        dest.sharpen_radius = look.sharpen_radius;
        dest.sharpen_threshold = look.sharpen_threshold;
        dest.detail_effect_enabled = true;
    }
    if (mask.denoise)
    {
        dest.denoise = look.denoise;
        dest.denoise_chroma = look.denoise_chroma;
        dest.detail_effect_enabled = true;
    }
    if (mask.vignette)
    {
        dest.vignette = look.vignette;
        dest.vignette_midpoint = look.vignette_midpoint;
        dest.vignette_falloff = look.vignette_falloff;
        dest.vignette_shape = look.vignette_shape;
        dest.vignette_center_x = look.vignette_center_x;
        dest.vignette_center_y = look.vignette_center_y;
        dest.effects_effect_enabled = true;
    }
    if (mask.grain)
    {
        dest.grain = look.grain;
        dest.detail_effect_enabled = true;
    }
    if (mask.grayscale)
    {
        dest.monochrome_present = look.monochrome_present;
        dest.monochrome_enabled = look.monochrome_enabled;
        dest.monochrome = look.monochrome;
        dest.color_effect_enabled = true;
    }
    if (mask.clarity)
        dest.detail_effect_enabled = true;
    clamp_develop(dest);
}

bool crs_process_version_is_supported(const std::string_view process_version) noexcept
{
    for (const auto allowed : supported_crs_process_versions())
    {
        if (process_version == allowed)
            return true;
    }
    return false;
}

Result<CrsProcessVersionInfo> classify_crs_process_version(const std::string_view xmp_utf8)
{
    auto parsed = parse_crs_document(xmp_utf8);
    if (!parsed)
        return parsed.error();
    CrsProcessVersionInfo info;
    if (const auto *process = find_attr(parsed.value(), "ProcessVersion"); process != nullptr)
    {
        info.process_version = *process;
        if (crs_process_version_is_supported(*process))
        {
            info.version_class = CrsProcessVersionClass::kSupportedPv2012;
        }
        else
        {
            info.version_class = CrsProcessVersionClass::kUnsupported;
            info.reason = "unsupported_crs_process_version";
        }
    }
    else
    {
        info.version_class = CrsProcessVersionClass::kAbsent;
    }
    return info;
}

Result<CrsImportResult> import_crs_xmp(const LegacyXmpImportRequest &request)
{
    if (request.asset.id.empty() || request.asset.input_uri.empty())
    {
        return make_error(ErrorCode::kValidation,
                          "CRS XMP import requires an explicit asset ID and input URI");
    }
    if (request.xmp_utf8.find("http://darktable.sf.net/") != std::string_view::npos &&
        request.xmp_utf8.find("darktable:history") != std::string_view::npos)
    {
        return crs_error("XMP mixes Camera Raw settings with leftover darktable history",
                         "unsupported_mixed_xmp_dialect");
    }
    auto parsed = parse_crs_document(request.xmp_utf8);
    if (!parsed)
        return parsed.error();
    CrsImportResult result;
    result.name = parsed.value().name;
    auto allowed = reject_unknown_and_identity(parsed.value(), result.omitted);
    if (!allowed)
        return allowed.error();
    auto mapped = map_document(parsed.value(), result.look, result.mask, result.omitted);
    if (!mapped)
        return mapped.error();
    clamp_develop(result.look);
    DevelopParams clamped = result.look;
    if (clamped != result.look)
        return crs_error("CRS values map outside Ravo Develop bounds",
                         "crs_value_outside_ravo_range");
    auto recipe = recipe_from_develop(request.asset, result.look);
    if (!recipe)
        return recipe.error();
    result.recipe = std::move(recipe).value();
    return result;
}

namespace
{

[[nodiscard]] std::string format_crs_number(const double value)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(12) << value;
    return stream.str();
}

[[nodiscard]] std::string xml_attr_escape(const std::string_view value)
{
    std::string out;
    out.reserve(value.size());
    for (const char character : value)
    {
        switch (character)
        {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&apos;";
            break;
        default:
            out.push_back(static_cast<char>(character));
            break;
        }
    }
    return out;
}

} // namespace

std::optional<std::string>
crs_xmp_unrepresentable_multi_instance_reason(const DevelopParams &look) noexcept
{
    if (look.exposure_instances.size() > 1U || look.color_balance_rgb_instances.size() > 1U)
    {
        return std::string{"unrepresentable_multi_instance_local_adjustments"};
    }
    return std::nullopt;
}

Result<CrsExportResult> export_crs_xmp(const CrsExportRequest &request)
{
    if (const auto reason = crs_xmp_unrepresentable_multi_instance_reason(request.look))
    {
        return make_error(ErrorCode::kUnsupported,
                          "CRS/XMP cannot represent multi-instance local adjustments",
                          {{"reason", *reason}});
    }
    CrsExportResult result;
    const DevelopParams &look = request.look;
    auto omit = [&](const std::string_view field)
    {
        if (std::find(result.omitted_catalog_fields.begin(), result.omitted_catalog_fields.end(),
                      field) == result.omitted_catalog_fields.end())
            result.omitted_catalog_fields.emplace_back(field);
    };

    if (!look.masks.empty())
        omit("masks");
    if (look.crop_x != 0.0 || look.crop_y != 0.0 || look.crop_width != 1.0 ||
        look.crop_height != 1.0)
        omit("crop");
    if (look.rotate_quarters != 0 || look.flip_horizontal != 0 || look.flip_vertical != 0 ||
        std::abs(look.straighten_degrees) > 1.0e-12)
        omit("geometry");
    if (look.canvas_present || look.canvas_enabled)
        omit("canvas");
    if (!look.retouch.is_identity())
        omit("retouch");
    if (!look.rgb_curve.is_identity() || !tone_curve_is_identity(look.tone_curve) ||
        !tone_curve_is_identity(look.tone_curve_a) || !tone_curve_is_identity(look.tone_curve_b))
        omit("curves");
    if (look.color_eq_effect_enabled)
        omit("color_eq");
    if (look.split_toning_present || look.split_toning_enabled)
        omit("split_toning");
    if (look.monochrome_present || look.monochrome_enabled)
        omit("monochrome");
    if (look.profile_gamma_enabled)
        omit("profile_gamma");
    if (std::abs(look.sharpen) > 1.0e-12 || std::abs(look.denoise) > 1.0e-12)
        omit("detail");
    if (std::abs(look.vignette) > 1.0e-12)
        omit("vignette");
    if (std::abs(look.grain) > 1.0e-12)
        omit("grain");

    const double contrast_slider = look.sigmoid_enabled ?
                                       [&]()
    {
        // Inverse of import: sigmoid_contrast =
        //   kSigmoidContrastDefault * pow(kCrsPositiveContrastSigmoid / kSigmoidContrastDefault, mapped)
        // where mapped = Contrast2012 / 100.
        if (look.sigmoid_contrast <= 0.0 || !std::isfinite(look.sigmoid_contrast))
            return 0.0;
        const double ratio = look.sigmoid_contrast / kSigmoidContrastDefault;
        const double base = kCrsPositiveContrastSigmoid / kSigmoidContrastDefault;
        if (base <= 0.0 || ratio <= 0.0)
            return 0.0;
        return std::log(ratio) / std::log(base) * kCrsSlider;
    }() :
                                       look.contrast * kCrsSlider;

    const std::string name =
        request.preset_name.empty() ? "Ravo" : std::string(request.preset_name);
    std::ostringstream xml;
    xml.imbue(std::locale::classic());
    xml << "<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
        << "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
        << " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
        << "  <rdf:Description rdf:about=\"\"\n"
        << "    xmlns:crs=\"" << kCrsNamespaceUri << "\"\n"
        << "   crs:Version=\"15.4\"\n"
        << "   crs:ProcessVersion=\"11.0\"\n"
        << "   crs:HasSettings=\"True\"\n"
        << "   crs:WhiteBalance=\"As Shot\"\n"
        << "   crs:Exposure2012=\"" << format_crs_number(look.exposure_ev) << "\"\n"
        << "   crs:Contrast2012=\"" << format_crs_number(contrast_slider) << "\"\n"
        << "   crs:Highlights2012=\"" << format_crs_number(look.highlights * kCrsSlider) << "\"\n"
        << "   crs:Shadows2012=\"" << format_crs_number(look.shadows * kCrsSlider) << "\"\n"
        << "   crs:Whites2012=\"" << format_crs_number(look.whites * kCrsSlider) << "\"\n"
        << "   crs:Blacks2012=\"" << format_crs_number(look.blacks * kCrsSlider) << "\"\n"
        << "   crs:Vibrance=\"" << format_crs_number(look.vibrance * kCrsSlider) << "\"\n"
        << "   crs:Saturation=\"" << format_crs_number(look.saturation * kCrsSlider) << "\"\n"
        << "   crs:Clarity2012=\"" << format_crs_number(look.clarity * kCrsSlider) << "\"\n"
        << "   crs:Dehaze=\"" << format_crs_number(look.dehaze * kCrsSlider) << "\"\n"
        << "   crs:ConvertToGrayscale=\"False\">\n"
        << "   <crs:Name>" << xml_attr_escape(name) << "</crs:Name>\n"
        << "  </rdf:Description>\n"
        << " </rdf:RDF>\n"
        << "</x:xmpmeta>\n"
        << "<?xpacket end=\"w\"?>\n";
    result.xmp_utf8 = xml.str();
    return result;
}

} // namespace ravo
