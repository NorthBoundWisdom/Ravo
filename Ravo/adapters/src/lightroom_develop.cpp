#include "ravo/adapters/lightroom_develop.h"

#include <QXmlStreamWriter>

#include <cctype>
#include <cmath>
#include <charconv>
#include <set>

#include "ravo/foundation/json.h"

// Serialized-table parsing informed by RAWmakase lr_develop.rs.
// Copyright (c) 2026 RAWmakase contributors, MIT; see THIRD_PARTY_NOTICES.md.
namespace ravo
{
namespace
{
std::string_view trim(std::string_view value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return value;
}

TaskError invalid(const std::string_view detail)
{
    return make_error(ErrorCode::kValidation, "Malformed Lightroom Develop settings",
                      {{"reason", "invalid_lightroom_develop"}, {"detail", std::string(detail)}});
}

Result<std::string> scalar(std::string_view text)
{
    text = trim(text);
    if (text.empty())
        return invalid("Empty scalar");
    if (text.front() == '"')
    {
        auto parsed = parse_json(text);
        if (!parsed || !parsed.value().string_if())
            return invalid("Unsupported quoted string");
        return *parsed.value().string_if();
    }
    if (text == "true" || text == "false")
        return std::string(text == "true" ? "True" : "False");
    // Numeric lexical validation is independent of locale and rejects NaN/Inf,
    // expressions and Lua calls. The shared CRS owner checks field ranges.
    auto number = parse_json(text);
    if (!number || !number.value().number_if())
        return invalid("Unsupported scalar literal");
    return std::string(text);
}

Result<std::string> packet(const std::map<std::string, std::string, std::less<>> &fields)
{
    QString xml;
    QXmlStreamWriter writer(&xml);
    writer.writeStartElement("x:xmpmeta");
    writer.writeNamespace("adobe:ns:meta/", "x");
    writer.writeStartElement("rdf:RDF");
    writer.writeNamespace("http://www.w3.org/1999/02/22-rdf-syntax-ns#", "rdf");
    writer.writeStartElement("rdf:Description");
    writer.writeNamespace(QString::fromUtf8(kCrsNamespaceUri.data(), kCrsNamespaceUri.size()),
                          "crs");
    for (const auto &[key, raw] : fields)
    {
        if (key.starts_with("ToneCurve") && key.find("Name") == std::string::npos)
            continue;
        if (raw.starts_with('{'))
            return make_error(ErrorCode::kUnsupported,
                              "Nested Lightroom field has no admitted mapping",
                              {{"reason", "unsupported_lightroom_nested_field"}, {"field", key}});
        auto value = scalar(raw);
        if (!value)
            return value.error();
        writer.writeAttribute(QString::fromStdString("crs:" + key),
                              QString::fromStdString(value.value()));
    }
    for (const auto &[key, raw] : fields)
    {
        if (!key.starts_with("ToneCurve") || key.find("Name") != std::string::npos)
            continue;
        auto body = trim(raw);
        if (body.size() < 2 || body.front() != '{' || body.back() != '}')
            return invalid("Invalid curve table");
        body.remove_prefix(1);
        body.remove_suffix(1);
        std::vector<std::string> numbers;
        while (!trim(body).empty())
        {
            const auto end = body.find(',');
            auto value = scalar(trim(body.substr(0, end)));
            if (!value)
                return value.error();
            numbers.push_back(value.value());
            if (numbers.size() > 512)
                return invalid("Curve exceeds point bound");
            if (end == std::string_view::npos)
                break;
            body.remove_prefix(end + 1);
        }
        if (numbers.size() < 4 || numbers.size() % 2 != 0)
            return invalid("Invalid curve point count");
        writer.writeStartElement(QString::fromStdString("crs:" + key));
        writer.writeStartElement("rdf:Seq");
        for (std::size_t i = 0; i < numbers.size(); i += 2)
            writer.writeTextElement("rdf:li",
                                    QString::fromStdString(numbers[i] + ", " + numbers[i + 1]));
        writer.writeEndElement();
        writer.writeEndElement();
    }
    writer.writeEndElement();
    writer.writeEndElement();
    writer.writeEndElement();
    return xml.toStdString();
}

std::string group(const std::string &key)
{
    if (key == "CropLeft" || key == "CropTop" || key == "CropRight" || key == "CropBottom" ||
        key == "CropAngle")
        return "crop";
    if (key.starts_with("ToneCurve") || key.starts_with("Parametric"))
        return "curves";
    if (key.starts_with("Sharpen") || key == "Sharpness")
        return "sharpen";
    if ((key.starts_with("Luminance") && !key.starts_with("LuminanceAdjustment")) ||
        key.starts_with("ColorNoise"))
        return "denoise";
    if (key.starts_with("PostCropVignette"))
        return "vignette";
    if (key.starts_with("SplitToning"))
        return "split_toning";
    if (key.starts_with("HueAdjustment") || key.starts_with("SaturationAdjustment") ||
        key.starts_with("LuminanceAdjustment"))
        return "hsl";
    if (key == "WhiteBalance" || key == "Temperature" || key == "Tint")
        return "white_balance";
    if (key == "RedHue" || key == "RedSaturation" || key == "GreenHue" ||
        key == "GreenSaturation" || key == "BlueHue" || key == "BlueSaturation" ||
        key == "ShadowTint")
        return "primaries";
    return key;
}
} // namespace

Result<std::map<std::string, std::string, std::less<>>>
parse_lightroom_develop_fields(std::string_view text)
{
    if (text.size() > 16000000)
        return invalid("Settings exceed 16 MB");
    text = trim(text);
    if (text.empty() || text.front() != 's')
        return invalid("Expected settings assignment");
    text.remove_prefix(1);
    text = trim(text);
    if (text.empty() || text.front() != '=')
        return invalid("Expected equals");
    text.remove_prefix(1);
    text = trim(text);
    if (text.size() < 2 || text.front() != '{' || text.back() != '}')
        return invalid("Expected table");
    text.remove_prefix(1);
    text.remove_suffix(1);
    std::map<std::string, std::string, std::less<>> fields;
    const auto insert = [&](std::string_view part) -> Result<void>
    {
        part = trim(part);
        if (part.empty())
            return {};
        const auto equal = part.find('=');
        if (equal == std::string_view::npos)
            return invalid("Expected field assignment");
        const auto key = trim(part.substr(0, equal));
        const auto value = trim(part.substr(equal + 1));
        if (key.empty() || value.empty() || fields.size() >= 4096)
            return invalid("Invalid field or field count");
        for (const auto c : key)
            if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') &&
                c != '_')
                return invalid("Invalid field key");
        if (!fields.emplace(std::string(key), std::string(value)).second)
            return invalid("Duplicate field");
        return {};
    };
    std::size_t start = 0;
    int depth = 0;
    char quote = 0;
    bool escape = false;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const auto c = text[i];
        if (quote)
        {
            if (escape)
                escape = false;
            else if (c == '\\')
                escape = true;
            else if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '"' || c == '\'')
            quote = c;
        else if (c == '{' && ++depth > 128)
            return invalid("Nesting exceeds 128");
        else if (c == '}' && --depth < 0)
            return invalid("Unbalanced table");
        else if (c == ',' && depth == 0)
        {
            auto added = insert(text.substr(start, i - start));
            if (!added)
                return added.error();
            start = i + 1;
        }
    }
    if (quote || depth)
        return invalid("Unterminated value");
    auto added = insert(text.substr(start));
    if (!added)
        return added.error();
    return fields;
}

Result<LightroomDevelopResult> import_lightroom_develop(const std::string_view text,
                                                        const AssetDescriptor &asset)
{
    auto parsed = parse_lightroom_develop_fields(text);
    if (!parsed)
        return parsed.error();
    auto &fields = parsed.value();
    auto empty = import_crs_xmp(
        {"<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"><rdf:Description xmlns:crs=\"http://ns.adobe.com/camera-raw-settings/1.0/\" crs:Exposure2012=\"0\"/></rdf:RDF></x:xmpmeta>",
         asset});
    if (!empty)
        return empty.error();
    LightroomDevelopResult result;
    result.look = empty.value().look;
    bool has_2012 = false;
    for (const auto &[key, value] : fields)
        has_2012 |= key.ends_with("2012");
    double process = 0;
    if (const auto version = fields.find("ProcessVersion"); version != fields.end())
    {
        auto value = scalar(version->second);
        if (value)
        {
            const auto parsed_process = std::from_chars(
                value.value().data(), value.value().data() + value.value().size(), process);
            if (parsed_process.ec != std::errc{} ||
                parsed_process.ptr != value.value().data() + value.value().size() ||
                !std::isfinite(process))
                process = 0;
        }
    }
    const bool legacy = process > 0 && process < 6.7 && !has_2012;
    const auto is_zero = [&](const char *key)
    {
        const auto found = fields.find(key);
        if (found == fields.end())
            return false;
        double number = 1;
        const auto parsed_number = std::from_chars(
            found->second.data(), found->second.data() + found->second.size(), number);
        return parsed_number.ec == std::errc{} &&
               parsed_number.ptr == found->second.data() + found->second.size() && number == 0;
    };
    const std::set<std::string> local_tables{"Look",
                                             "RetouchAreas",
                                             "RetouchInfo",
                                             "RedEyeInfo",
                                             "MaskGroupBasedCorrections",
                                             "PaintBasedCorrections",
                                             "GradientBasedCorrections",
                                             "CircularGradientBasedCorrections"};
    const std::set<std::string> inactive_lens_metadata{
        "LensProfileSetup",          "LensProfileName",       "LensProfileFilename",
        "LensProfileDigest",         "LensProfileIsEmbedded", "LensProfileDistortionScale",
        "LensProfileVignettingScale"};
    const std::map<std::string, double> old_controls{
        {"Exposure", 0}, {"Contrast", 25}, {"Brightness", 50},      {"Shadows", 5},
        {"Clarity", 0},  {"FillLight", 0}, {"HighlightRecovery", 0}};
    std::map<std::string, std::map<std::string, std::string, std::less<>>> groups;
    for (const auto &[key, value] : fields)
    {
        if (local_tables.contains(key) && value.size() >= 2 && value.front() == '{' &&
            value.back() == '}' &&
            std::string_view(value).substr(1, value.size() - 2).find_first_not_of(" \t\r\n") ==
                std::string_view::npos)
            continue;
        if ((key == "IncrementalTemperature" || key == "IncrementalTint") && is_zero(key.c_str()))
            continue;
        if (key.starts_with("Upright") && is_zero("PerspectiveUpright"))
            continue;
        if (inactive_lens_metadata.contains(key) && is_zero("LensProfileEnable"))
            continue;
        if (const auto old = old_controls.find(key); old != old_controls.end())
        {
            // PV2012 records retain obsolete parallel controls. They are not
            // another active edit and must never be applied twice.
            if (has_2012)
                continue;
            if (legacy)
            {
                if (key == "Exposure")
                    groups["Exposure2012"].emplace("Exposure2012", value);
                else
                {
                    double number = 0;
                    const auto parsed_number =
                        std::from_chars(value.data(), value.data() + value.size(), number);
                    if (parsed_number.ec != std::errc{} ||
                        parsed_number.ptr != value.data() + value.size() ||
                        !std::isfinite(number) || number != old->second)
                        result.omitted.push_back(
                            {key, value, "unsupported_legacy_process_control"});
                }
                continue;
            }
        }
        if (key != "ProcessVersion")
            groups[group(key)].emplace(key, value);
    }
    for (auto &[name, values] : groups)
    {
        if (name == "Texture")
        {
            const auto &value = values.at("Texture");
            double strength = 0;
            const auto parsed_strength =
                std::from_chars(value.data(), value.data() + value.size(), strength);
            if (parsed_strength.ec != std::errc{} ||
                parsed_strength.ptr != value.data() + value.size() || !std::isfinite(strength) ||
                strength < -100 || strength > 100)
                result.omitted.push_back({"Texture", value, "invalid_lightroom_texture"});
            else
            {
                result.look.texture.strength = strength * 0.01;
                result.texture = true;
                ++result.compatible_groups;
            }
            continue;
        }
        if (name == "crop")
        {
            std::map<std::string, double> numbers;
            bool valid = true;
            for (const auto &[key, value] : values)
            {
                double number = 0;
                const auto parsed_number =
                    std::from_chars(value.data(), value.data() + value.size(), number);
                if (parsed_number.ec != std::errc{} ||
                    parsed_number.ptr != value.data() + value.size() || !std::isfinite(number) ||
                    (key == "CropAngle" ? std::abs(number) > 45 : number < 0 || number > 1))
                    valid = false;
                numbers.emplace(key, number);
            }
            const auto number = [&](const char *key, const double fallback)
            {
                const auto found = numbers.find(key);
                return found == numbers.end() ? fallback : found->second;
            };
            const double left = number("CropLeft", 0), top = number("CropTop", 0);
            const double right = number("CropRight", 1), bottom = number("CropBottom", 1);
            if (valid && right > left && bottom > top)
            {
                result.look.crop_x = left;
                result.look.crop_y = top;
                result.look.crop_width = right - left;
                result.look.crop_height = bottom - top;
                result.look.straighten_degrees = number("CropAngle", 0);
                result.geometry = true;
                ++result.compatible_groups;
            }
            else
                for (const auto &[key, value] : values)
                    result.omitted.push_back({key, value, "invalid_lightroom_crop"});
            continue;
        }
        if (!legacy)
            if (const auto version = fields.find("ProcessVersion"); version != fields.end())
                values.emplace(*version);
        auto xml = packet(values);
        Result<CrsImportResult> converted =
            xml ? import_crs_xmp({xml.value(), asset}) : Result<CrsImportResult>(xml.error());
        if (!converted)
        {
            const auto reason = converted.error().context.find("reason");
            for (const auto &[key, value] : values)
                if (key != "ProcessVersion")
                    result.omitted.push_back({key, value,
                                              reason == converted.error().context.end() ?
                                                  "unsupported_lightroom_field" :
                                                  reason->second});
            continue;
        }
        apply_crs_look(result.look, converted.value().look, converted.value().mask);
        bool mapped = false;
        for (bool CrsLookMask::*member :
             {&CrsLookMask::white_balance, &CrsLookMask::exposure,       &CrsLookMask::contrast,
              &CrsLookMask::highlights,    &CrsLookMask::shadows,        &CrsLookMask::whites,
              &CrsLookMask::blacks,        &CrsLookMask::vibrance,       &CrsLookMask::saturation,
              &CrsLookMask::clarity,       &CrsLookMask::dehaze,         &CrsLookMask::color_eq_hue,
              &CrsLookMask::color_eq_sat,  &CrsLookMask::color_eq_light, &CrsLookMask::split_toning,
              &CrsLookMask::rgb_curve,     &CrsLookMask::tone_curve,     &CrsLookMask::primaries,
              &CrsLookMask::sharpen,       &CrsLookMask::denoise,        &CrsLookMask::vignette,
              &CrsLookMask::grain,         &CrsLookMask::grayscale})
        {
            mapped |= converted.value().mask.*member;
            result.mask.*member = result.mask.*member || converted.value().mask.*member;
        }
        if (mapped)
            ++result.compatible_groups;
        result.omitted.insert(result.omitted.end(), converted.value().omitted.begin(),
                              converted.value().omitted.end());
    }
    return result;
}

void apply_lightroom_develop(DevelopParams &destination, const LightroomDevelopResult &converted)
{
    apply_crs_look(destination, converted.look, converted.mask);
    if (converted.texture)
        destination.texture = converted.look.texture;
    if (converted.geometry)
    {
        destination.crop_x = converted.look.crop_x;
        destination.crop_y = converted.look.crop_y;
        destination.crop_width = converted.look.crop_width;
        destination.crop_height = converted.look.crop_height;
        destination.straighten_degrees = converted.look.straighten_degrees;
    }
}
} // namespace ravo
