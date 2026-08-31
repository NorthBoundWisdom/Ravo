#include "ravo/adapters/filesystem_recovery_store.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <locale>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QString>
#include <QTemporaryFile>

#include "ravo/foundation/json.h"
#include "ravo/recipe/recipe.h"

#include "recovery_publication_internal.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace ravo
{
namespace
{

constexpr std::size_t kPublicationChunkBytes = 64U * 1024U;
constexpr std::size_t kIdentityMaximumBytes = 512U;
constexpr std::size_t kUriMaximumBytes = 32U * 1024U;

[[nodiscard]] QString qstring_from_utf8(const std::string_view value)
{
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

[[nodiscard]] std::string path_utf8(const std::filesystem::path &path)
{
    const auto encoded = path.generic_u8string();
    return {reinterpret_cast<const char *>(encoded.data()), encoded.size()};
}

[[nodiscard]] std::filesystem::path path_from_utf8(const std::string_view value)
{
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

[[nodiscard]] TaskError recovery_error(const ErrorCode code, std::string message,
                                       std::string reason, std::string path = {},
                                       std::string field = {},
                                       const std::error_code &filesystem_error = {})
{
    std::map<std::string, std::string, std::less<>> context{{"reason", std::move(reason)}};
    if (!path.empty())
        context.emplace("path", std::move(path));
    if (!field.empty())
        context.emplace("field", std::move(field));
    if (filesystem_error)
        context.emplace("detail", filesystem_error.message());
    return make_error(code, std::move(message), std::move(context));
}

[[nodiscard]] TaskError recovery_io_error(std::string message, std::string reason, std::string path,
                                          std::string detail)
{
    auto error =
        recovery_error(ErrorCode::kIo, std::move(message), std::move(reason), std::move(path));
    if (!detail.empty())
        error.context.emplace("detail", std::move(detail));
    return error;
}

[[nodiscard]] bool safe_asset_id(const std::string_view value)
{
    if (value.empty() || value.size() > 180U)
        return false;
    return std::all_of(value.begin(), value.end(),
                       [](const char character)
                       {
                           return (character >= 'a' && character <= 'z') ||
                                  (character >= 'A' && character <= 'Z') ||
                                  (character >= '0' && character <= '9') || character == '-' ||
                                  character == '_';
                       });
}

[[nodiscard]] std::string sidecar_filename(const std::string_view asset_id,
                                           const std::int64_t generation)
{
    return std::string(asset_id) + "." + std::to_string(generation) + ".ravo.json";
}

[[nodiscard]] std::string sha256(const std::string_view text)
{
    const auto bytes = QByteArrayView(text.data(), static_cast<qsizetype>(text.size()));
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString();
}

[[nodiscard]] bool valid_sha256(const std::string_view value)
{
    return value.size() == 64U && std::all_of(value.begin(), value.end(),
                                              [](const char character)
                                              {
                                                  return (character >= '0' && character <= '9') ||
                                                         (character >= 'a' && character <= 'f');
                                              });
}

[[nodiscard]] Result<std::string> number_text(const double value)
{
    if (!std::isfinite(value))
        return recovery_error(ErrorCode::kValidation,
                              "Recovery sidecar contains a non-finite number",
                              "nonfinite_recovery_number");
    std::array<char, 64U> buffer{};
    const auto result =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                      std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (result.ec != std::errc{})
        return recovery_error(ErrorCode::kInternal, "Unable to format recovery sidecar number",
                              "recovery_number_format_failed");
    return std::string(buffer.data(), result.ptr);
}

[[nodiscard]] bool parse_json_double(const std::string_view text, double &value)
{
    if (text.empty())
        return false;
    // Apple libc++ still lacks floating std::from_chars. JSON has a fixed '.'
    // decimal separator, so parse the already validated token with the classic
    // locale and require complete, finite consumption on every platform.
    std::istringstream stream{std::string(text)};
    stream.imbue(std::locale::classic());
    stream >> std::noskipws;
    double parsed = 0.0;
    if (!(stream >> parsed) || stream.peek() != std::char_traits<char>::eof() ||
        !std::isfinite(parsed))
        return false;
    value = parsed;
    return true;
}

[[nodiscard]] JsonValue optional_string_json(const std::optional<std::string> &value)
{
    return value ? JsonValue{*value} : JsonValue{nullptr};
}

template <typename Integer>
[[nodiscard]] JsonValue optional_integer_json(const std::optional<Integer> &value)
{
    return value ? JsonValue::number(std::to_string(*value)) : JsonValue{nullptr};
}

[[nodiscard]] Result<JsonValue> optional_double_json(const std::optional<double> &value)
{
    if (!value)
        return JsonValue{nullptr};
    auto text = number_text(*value);
    if (!text)
        return text.error();
    return JsonValue::number(std::move(text).value());
}

[[nodiscard]] Result<JsonValue> capture_json(const CaptureMetadata &capture)
{
    auto valid = validate_capture_metadata(capture);
    if (!valid)
        return valid.error();
    auto iso = optional_double_json(capture.iso);
    auto aperture = optional_double_json(capture.aperture);
    auto focal = optional_double_json(capture.focal_length_mm);
    auto shutter = optional_double_json(capture.shutter_s);
    if (!iso)
        return iso.error();
    if (!aperture)
        return aperture.error();
    if (!focal)
        return focal.error();
    if (!shutter)
        return shutter.error();

    JsonValue captured_datetime{nullptr};
    if (capture.captured_datetime)
    {
        captured_datetime = JsonValue::Object{
            {"local_exif", capture.captured_datetime->local_exif},
            {"subsecond_digits", optional_string_json(capture.captured_datetime->subsecond_digits)},
            {"utc_offset_minutes",
             optional_integer_json(capture.captured_datetime->utc_offset_minutes)},
        };
    }
    JsonValue location{nullptr};
    if (capture.location)
    {
        JsonValue altitude{nullptr};
        if (capture.location->altitude)
        {
            altitude = JsonValue::Object{
                {"magnitude_mm",
                 JsonValue::number(std::to_string(capture.location->altitude->magnitude_mm))},
                {"reference",
                 capture.location->altitude->reference == CaptureAltitudeReference::kBelowSeaLevel ?
                     "below_sea_level" :
                     "above_sea_level"},
            };
        }
        location = JsonValue::Object{
            {"altitude", std::move(altitude)},
            {"latitude_e6", JsonValue::number(std::to_string(capture.location->latitude_e6))},
            {"longitude_e6", JsonValue::number(std::to_string(capture.location->longitude_e6))},
        };
    }
    return JsonValue{JsonValue::Object{
        {"aperture", std::move(aperture).value()},
        {"camera_make", optional_string_json(capture.camera_make)},
        {"camera_model", optional_string_json(capture.camera_model)},
        {"captured_datetime", std::move(captured_datetime)},
        {"captured_unix_s", optional_integer_json(capture.captured_unix_s)},
        {"focal_length_mm", std::move(focal).value()},
        {"iso", std::move(iso).value()},
        {"location", std::move(location)},
        {"shutter_s", std::move(shutter).value()},
    }};
}

[[nodiscard]] Result<JsonValue> recovery_payload(const AssetRecoverySnapshot &snapshot)
{
    if (snapshot.catalog_id.empty() || snapshot.catalog_id.size() > kIdentityMaximumBytes ||
        !safe_asset_id(snapshot.asset.id) || snapshot.state.asset_id != snapshot.asset.id ||
        snapshot.state.generation <= 0 || snapshot.state.synchronized_generation < 0 ||
        snapshot.state.synchronized_generation > snapshot.state.generation ||
        snapshot.catalog_revision < 0 || snapshot.asset.normalized_uri.empty() ||
        snapshot.asset.normalized_uri.size() > kUriMaximumBytes ||
        snapshot.asset.media_type.empty() || !snapshot.asset.media_type.starts_with("image/") ||
        (snapshot.asset.import_state != kImportStateImported &&
         snapshot.asset.import_state != kImportStateFailed &&
         snapshot.asset.import_state != kImportStateMissing) ||
        snapshot.asset.width.has_value() != snapshot.asset.height.has_value() ||
        snapshot.asset.has_edits != snapshot.recipe_json.has_value() ||
        snapshot.asset.tags.size() > kRecoveryTagMaximumEntries ||
        snapshot.history.size() > kRecoveryHistoryMaximumEntries)
    {
        return recovery_error(ErrorCode::kValidation, "Recovery snapshot is invalid",
                              "invalid_recovery_snapshot");
    }
    auto review = validate_rating(snapshot.asset.review.rating);
    if (!review)
        return review.error();
    auto capture = capture_json(snapshot.asset.capture);
    if (!capture)
        return capture.error();

    JsonValue::Array tags;
    tags.reserve(snapshot.asset.tags.size());
    std::string previous_tag;
    for (const auto &tag : snapshot.asset.tags)
    {
        auto normalized = normalize_tag_name(tag);
        if (tag.empty() || tag.size() > kTagMaxLength ||
            (!previous_tag.empty() && tag <= previous_tag) || !normalized ||
            normalized.value() != tag)
            return recovery_error(ErrorCode::kValidation,
                                  "Recovery snapshot contains an invalid tag",
                                  "invalid_recovery_tag");
        tags.emplace_back(tag);
        previous_tag = tag;
    }

    for (const auto &[name, value] :
         {std::pair{"copyright", &snapshot.asset.metadata.copyright},
          std::pair{"creator", &snapshot.asset.metadata.creator},
          std::pair{"description", &snapshot.asset.metadata.description},
          std::pair{"title", &snapshot.asset.metadata.title}})
    {
        if (value->has_value())
        {
            auto valid = validate_metadata_field(name, **value);
            if (!valid)
                return valid.error();
        }
    }

    JsonValue::Array history;
    history.reserve(snapshot.history.size());
    std::int64_t previous_sequence = 0;
    for (const auto &entry : snapshot.history)
    {
        if (entry.asset_id != snapshot.asset.id || entry.seq <= previous_sequence ||
            (entry.kind != kRecipeHistoryKindHistory && entry.kind != kRecipeHistoryKindSnapshot) ||
            entry.created_unix_ms < 0 || (entry.label && entry.label->size() > kTagMaxLength))
        {
            return recovery_error(ErrorCode::kValidation, "Recovery snapshot history is invalid",
                                  "invalid_recovery_history");
        }
        if (!entry.recipe_json.empty())
        {
            auto recipe = parse_recipe_json(entry.recipe_json);
            if (!recipe)
                return recipe.error();
        }
        previous_sequence = entry.seq;
        history.emplace_back(JsonValue::Object{
            {"created_unix_ms", JsonValue::number(std::to_string(entry.created_unix_ms))},
            {"kind", entry.kind},
            {"label", optional_string_json(entry.label)},
            {"recipe_json", entry.recipe_json},
            {"seq", JsonValue::number(std::to_string(entry.seq))},
        });
    }
    if (snapshot.recipe_json)
    {
        auto recipe = parse_recipe_json(*snapshot.recipe_json);
        if (!recipe)
            return recipe.error();
    }

    JsonValue::Object asset{
        {"capture", std::move(capture).value()},
        {"color_label", std::string(color_label_name(snapshot.asset.review.color_label))},
        {"content_fingerprint", optional_string_json(snapshot.asset.content_fingerprint)},
        {"created_unix_ms", JsonValue::number(std::to_string(snapshot.asset.created_unix_ms))},
        {"error_code", optional_string_json(snapshot.asset.error_code)},
        {"error_message", optional_string_json(snapshot.asset.error_message)},
        {"has_edits", snapshot.asset.has_edits},
        {"height", optional_integer_json(snapshot.asset.height)},
        {"id", snapshot.asset.id},
        {"import_state", snapshot.asset.import_state},
        {"media_type", snapshot.asset.media_type},
        {"metadata",
         JsonValue::Object{
             {"copyright", optional_string_json(snapshot.asset.metadata.copyright)},
             {"creator", optional_string_json(snapshot.asset.metadata.creator)},
             {"description", optional_string_json(snapshot.asset.metadata.description)},
             {"title", optional_string_json(snapshot.asset.metadata.title)},
         }},
        {"mtime_unix_ms", JsonValue::number(std::to_string(snapshot.asset.mtime_unix_ms))},
        {"normalized_uri", snapshot.asset.normalized_uri},
        {"rating", JsonValue::number(std::to_string(snapshot.asset.review.rating))},
        {"rejected", snapshot.asset.review.rejected},
        {"size_bytes", JsonValue::number(std::to_string(snapshot.asset.size_bytes))},
        {"tags", std::move(tags)},
        {"width", optional_integer_json(snapshot.asset.width)},
    };
    return JsonValue{JsonValue::Object{
        {"asset", std::move(asset)},
        {"catalog_id", snapshot.catalog_id},
        {"catalog_revision", JsonValue::number(std::to_string(snapshot.catalog_revision))},
        {"generation", JsonValue::number(std::to_string(snapshot.state.generation))},
        {"history", std::move(history)},
        {"recipe_json", optional_string_json(snapshot.recipe_json)},
        {"schema", "ravo-asset-recovery"},
        {"version", JsonValue::number(std::to_string(kRecoverySidecarSchemaVersion))},
    }};
}

[[nodiscard]] Result<std::pair<std::string, std::string>>
serialize_recovery(const AssetRecoverySnapshot &snapshot)
{
    auto payload = recovery_payload(snapshot);
    if (!payload)
        return payload.error();
    const auto canonical_payload = serialize_json(payload.value());
    const auto checksum = sha256(canonical_payload);
    auto document = serialize_json(JsonValue::Object{
        {"checksum", JsonValue::Object{{"algorithm", "sha256"}, {"value", checksum}}},
        {"payload", std::move(payload).value()},
    });
    if (document.size() > kRecoverySidecarMaximumBytes)
        return recovery_error(ErrorCode::kValidation, "Recovery sidecar exceeds its byte limit",
                              "recovery_sidecar_too_large");
    const auto artifact_sha256 = sha256(document);
    return std::pair<std::string, std::string>{std::move(document), artifact_sha256};
}

[[nodiscard]] Result<void> expect_exact_keys(const JsonValue::Object &object,
                                             const std::initializer_list<std::string_view> keys,
                                             const std::string_view owner)
{
    std::set<std::string, std::less<>> expected;
    for (const auto key : keys)
        expected.emplace(key);
    for (const auto &[key, value] : object)
    {
        static_cast<void>(value);
        if (!expected.contains(key))
            return recovery_error(ErrorCode::kValidation,
                                  "Recovery sidecar contains an unknown field",
                                  "unknown_recovery_field", {}, std::string(owner) + "." + key);
    }
    for (const auto &key : expected)
        if (!object.contains(key))
            return recovery_error(ErrorCode::kValidation,
                                  "Recovery sidecar is missing a required field",
                                  "missing_recovery_field", {}, std::string(owner) + "." + key);
    return {};
}

[[nodiscard]] Result<const JsonValue::Object *> require_object(const JsonValue *value,
                                                               const std::string_view field)
{
    if (value == nullptr || value->object_if() == nullptr)
        return recovery_error(ErrorCode::kValidation, "Recovery sidecar field must be an object",
                              "recovery_type_mismatch", {}, std::string(field));
    return value->object_if();
}

[[nodiscard]] Result<std::string> require_string(const JsonValue::Object &object,
                                                 const std::string_view key,
                                                 const std::size_t maximum_bytes)
{
    const auto found = object.find(key);
    const auto *value = found == object.end() ? nullptr : found->second.string_if();
    if (value == nullptr || value->empty() || value->size() > maximum_bytes ||
        value->find('\0') != std::string::npos)
        return recovery_error(ErrorCode::kValidation, "Recovery sidecar string is invalid",
                              "invalid_recovery_string", {}, std::string(key));
    return *value;
}

template <typename Integer>
[[nodiscard]] Result<Integer> require_integer(const JsonValue::Object &object,
                                              const std::string_view key, const Integer minimum,
                                              const Integer maximum)
{
    const auto found = object.find(key);
    const auto *number = found == object.end() ? nullptr : found->second.number_if();
    if (number == nullptr)
        return recovery_error(ErrorCode::kValidation, "Recovery sidecar integer is missing",
                              "recovery_type_mismatch", {}, std::string(key));
    Integer value{};
    const auto parsed =
        std::from_chars(number->text.data(), number->text.data() + number->text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != number->text.data() + number->text.size() ||
        value < minimum || value > maximum)
        return recovery_error(ErrorCode::kValidation,
                              "Recovery sidecar integer is outside its supported bounds",
                              "invalid_recovery_integer", {}, std::string(key));
    return value;
}

[[nodiscard]] Result<const JsonValue::Array *> require_array(const JsonValue *value,
                                                             const std::string_view field)
{
    if (value == nullptr || value->array_if() == nullptr)
        return recovery_error(ErrorCode::kValidation, "Recovery sidecar field must be an array",
                              "recovery_type_mismatch", {}, std::string(field));
    return value->array_if();
}

[[nodiscard]] Result<bool> require_boolean(const JsonValue::Object &object,
                                           const std::string_view key)
{
    const auto found = object.find(key);
    const auto *value = found == object.end() ? nullptr : found->second.boolean_if();
    if (value == nullptr)
        return recovery_error(ErrorCode::kValidation, "Recovery sidecar boolean is missing",
                              "recovery_type_mismatch", {}, std::string(key));
    return *value;
}

[[nodiscard]] Result<std::optional<std::string>>
require_optional_string(const JsonValue::Object &object, const std::string_view key,
                        const std::size_t maximum_bytes)
{
    const auto found = object.find(key);
    if (found == object.end())
        return recovery_error(ErrorCode::kValidation, "Recovery sidecar optional string is missing",
                              "recovery_type_mismatch", {}, std::string(key));
    if (found->second.is_null())
        return std::optional<std::string>{};
    const auto *value = found->second.string_if();
    if (value == nullptr || value->size() > maximum_bytes || value->find('\0') != std::string::npos)
        return recovery_error(ErrorCode::kValidation, "Recovery sidecar optional string is invalid",
                              "invalid_recovery_string", {}, std::string(key));
    return std::optional<std::string>{*value};
}

template <typename Integer>
[[nodiscard]] Result<std::optional<Integer>>
require_optional_integer(const JsonValue::Object &object, const std::string_view key,
                         const Integer minimum, const Integer maximum)
{
    const auto found = object.find(key);
    if (found == object.end())
        return recovery_error(ErrorCode::kValidation,
                              "Recovery sidecar optional integer is missing",
                              "recovery_type_mismatch", {}, std::string(key));
    if (found->second.is_null())
        return std::optional<Integer>{};
    const auto *number = found->second.number_if();
    if (number == nullptr)
        return recovery_error(ErrorCode::kValidation,
                              "Recovery sidecar optional integer has the wrong type",
                              "recovery_type_mismatch", {}, std::string(key));
    Integer value{};
    const auto parsed =
        std::from_chars(number->text.data(), number->text.data() + number->text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != number->text.data() + number->text.size() ||
        value < minimum || value > maximum)
        return recovery_error(ErrorCode::kValidation,
                              "Recovery sidecar optional integer is outside its bounds",
                              "invalid_recovery_integer", {}, std::string(key));
    return std::optional<Integer>{value};
}

[[nodiscard]] Result<std::optional<double>> require_optional_double(const JsonValue::Object &object,
                                                                    const std::string_view key)
{
    const auto found = object.find(key);
    if (found == object.end())
        return recovery_error(ErrorCode::kValidation, "Recovery sidecar optional number is missing",
                              "recovery_type_mismatch", {}, std::string(key));
    if (found->second.is_null())
        return std::optional<double>{};
    const auto *number = found->second.number_if();
    if (number == nullptr)
        return recovery_error(ErrorCode::kValidation,
                              "Recovery sidecar optional number has the wrong type",
                              "recovery_type_mismatch", {}, std::string(key));
    double value = 0.0;
    if (!parse_json_double(number->text, value))
        return recovery_error(ErrorCode::kValidation, "Recovery sidecar optional number is invalid",
                              "invalid_recovery_number", {}, std::string(key));
    return std::optional<double>{value};
}

[[nodiscard]] Result<CaptureMetadata> validate_recovery_capture(const JsonValue::Object &object)
{
    auto keys =
        expect_exact_keys(object,
                          {"aperture", "camera_make", "camera_model", "captured_datetime",
                           "captured_unix_s", "focal_length_mm", "iso", "location", "shutter_s"},
                          "payload.asset.capture");
    if (!keys)
        return keys.error();

    CaptureMetadata capture;
    auto camera_make = require_optional_string(object, "camera_make", kExportCaptureFieldMaxLength);
    auto camera_model =
        require_optional_string(object, "camera_model", kExportCaptureFieldMaxLength);
    auto iso = require_optional_double(object, "iso");
    auto aperture = require_optional_double(object, "aperture");
    auto focal = require_optional_double(object, "focal_length_mm");
    auto shutter = require_optional_double(object, "shutter_s");
    auto captured_unix = require_optional_integer<std::int64_t>(
        object, "captured_unix_s", std::numeric_limits<std::int64_t>::lowest(),
        std::numeric_limits<std::int64_t>::max());
    if (!camera_make)
        return camera_make.error();
    if (!camera_model)
        return camera_model.error();
    if (!iso)
        return iso.error();
    if (!aperture)
        return aperture.error();
    if (!focal)
        return focal.error();
    if (!shutter)
        return shutter.error();
    if (!captured_unix)
        return captured_unix.error();
    capture.camera_make = std::move(camera_make).value();
    capture.camera_model = std::move(camera_model).value();
    capture.iso = iso.value();
    capture.aperture = aperture.value();
    capture.focal_length_mm = focal.value();
    capture.shutter_s = shutter.value();
    capture.captured_unix_s = captured_unix.value();

    const auto captured_datetime = object.find("captured_datetime");
    if (captured_datetime == object.end())
        return recovery_error(ErrorCode::kValidation, "Recovery capture datetime field is missing",
                              "recovery_type_mismatch", {}, "captured_datetime");
    if (!captured_datetime->second.is_null())
    {
        auto datetime =
            require_object(&captured_datetime->second, "payload.asset.capture.captured_datetime");
        if (!datetime)
            return datetime.error();
        auto datetime_keys = expect_exact_keys(
            *datetime.value(), {"local_exif", "subsecond_digits", "utc_offset_minutes"},
            "payload.asset.capture.captured_datetime");
        if (!datetime_keys)
            return datetime_keys.error();
        auto local = require_string(*datetime.value(), "local_exif", kCaptureLocalExifLength);
        auto subsecond = require_optional_string(*datetime.value(), "subsecond_digits",
                                                 kCaptureSubsecondDigitsMax);
        auto offset = require_optional_integer<std::int32_t>(
            *datetime.value(), "utc_offset_minutes", kCaptureUtcOffsetMinutesMin,
            kCaptureUtcOffsetMinutesMax);
        if (!local)
            return local.error();
        if (!subsecond)
            return subsecond.error();
        if (!offset)
            return offset.error();
        capture.captured_datetime =
            CaptureDateTime{std::move(local).value(), std::move(subsecond).value(), offset.value()};
    }

    const auto location = object.find("location");
    if (location == object.end())
        return recovery_error(ErrorCode::kValidation, "Recovery location field is missing",
                              "recovery_type_mismatch", {}, "location");
    if (!location->second.is_null())
    {
        auto location_object = require_object(&location->second, "payload.asset.capture.location");
        if (!location_object)
            return location_object.error();
        auto location_keys =
            expect_exact_keys(*location_object.value(), {"altitude", "latitude_e6", "longitude_e6"},
                              "payload.asset.capture.location");
        if (!location_keys)
            return location_keys.error();
        auto latitude = require_integer<std::int32_t>(*location_object.value(), "latitude_e6",
                                                      kCaptureLatitudeE6Min, kCaptureLatitudeE6Max);
        auto longitude =
            require_integer<std::int32_t>(*location_object.value(), "longitude_e6",
                                          kCaptureLongitudeE6Min, kCaptureLongitudeE6Max);
        if (!latitude)
            return latitude.error();
        if (!longitude)
            return longitude.error();
        CaptureLocation decoded_location{latitude.value(), longitude.value(), std::nullopt};
        const auto altitude = location_object.value()->find("altitude");
        if (altitude == location_object.value()->end())
            return recovery_error(ErrorCode::kValidation, "Recovery altitude field is missing",
                                  "recovery_type_mismatch", {}, "altitude");
        if (!altitude->second.is_null())
        {
            auto altitude_object =
                require_object(&altitude->second, "payload.asset.capture.location.altitude");
            if (!altitude_object)
                return altitude_object.error();
            auto altitude_keys =
                expect_exact_keys(*altitude_object.value(), {"magnitude_mm", "reference"},
                                  "payload.asset.capture.location.altitude");
            if (!altitude_keys)
                return altitude_keys.error();
            auto magnitude = require_integer<std::uint32_t>(
                *altitude_object.value(), "magnitude_mm", 0U, kCaptureAltitudeAboveSeaLevelMmMax);
            auto reference = require_string(*altitude_object.value(), "reference", 32U);
            if (!magnitude)
                return magnitude.error();
            if (!reference)
                return reference.error();
            CaptureAltitudeReference decoded_reference;
            if (reference.value() == "above_sea_level")
                decoded_reference = CaptureAltitudeReference::kAboveSeaLevel;
            else if (reference.value() == "below_sea_level")
                decoded_reference = CaptureAltitudeReference::kBelowSeaLevel;
            else
                return recovery_error(ErrorCode::kValidation,
                                      "Recovery altitude reference is invalid",
                                      "invalid_recovery_capture", {}, "reference");
            decoded_location.altitude = CaptureAltitude{magnitude.value(), decoded_reference};
        }
        capture.location = decoded_location;
    }

    auto valid = validate_capture_metadata(capture);
    if (!valid)
        return valid.error();
    return capture;
}

[[nodiscard]] Result<WritableMetadata> validate_recovery_metadata(const JsonValue::Object &object)
{
    auto keys = expect_exact_keys(object, {"copyright", "creator", "description", "title"},
                                  "payload.asset.metadata");
    if (!keys)
        return keys.error();
    WritableMetadata metadata;
    auto copyright = require_optional_string(object, "copyright", kMetadataFieldMaxLength);
    auto creator = require_optional_string(object, "creator", kMetadataFieldMaxLength);
    auto description = require_optional_string(object, "description", kMetadataFieldMaxLength);
    auto title = require_optional_string(object, "title", kMetadataFieldMaxLength);
    if (!copyright)
        return copyright.error();
    if (!creator)
        return creator.error();
    if (!description)
        return description.error();
    if (!title)
        return title.error();
    metadata.copyright = std::move(copyright).value();
    metadata.creator = std::move(creator).value();
    metadata.description = std::move(description).value();
    metadata.title = std::move(title).value();
    for (const auto &[name, value] :
         {std::pair{"copyright", &metadata.copyright}, std::pair{"creator", &metadata.creator},
          std::pair{"description", &metadata.description}, std::pair{"title", &metadata.title}})
    {
        if (value->has_value())
        {
            auto valid = validate_metadata_field(name, **value);
            if (!valid)
                return valid.error();
        }
    }
    return metadata;
}

struct VerifiedAsset
{
    std::string id;
    bool has_edits = false;
};

[[nodiscard]] Result<VerifiedAsset> validate_recovery_asset(const JsonValue::Object &object)
{
    auto keys = expect_exact_keys(
        object,
        {"capture", "color_label", "content_fingerprint", "created_unix_ms", "error_code",
         "error_message", "has_edits", "height", "id", "import_state", "media_type", "metadata",
         "mtime_unix_ms", "normalized_uri", "rating", "rejected", "size_bytes", "tags", "width"},
        "payload.asset");
    if (!keys)
        return keys.error();
    auto id = require_string(object, "id", 180U);
    auto normalized_uri = require_string(object, "normalized_uri", kUriMaximumBytes);
    auto media_type = require_string(object, "media_type", 128U);
    auto import_state = require_string(object, "import_state", 32U);
    auto created = require_integer<std::int64_t>(object, "created_unix_ms", 0,
                                                 std::numeric_limits<std::int64_t>::max());
    auto mtime = require_integer<std::int64_t>(object, "mtime_unix_ms",
                                               std::numeric_limits<std::int64_t>::lowest(),
                                               std::numeric_limits<std::int64_t>::max());
    auto size = require_integer<std::uint64_t>(object, "size_bytes", 0U,
                                               std::numeric_limits<std::uint64_t>::max());
    auto width = require_optional_integer<std::uint32_t>(object, "width", 1U,
                                                         std::numeric_limits<std::uint32_t>::max());
    auto height = require_optional_integer<std::uint32_t>(
        object, "height", 1U, std::numeric_limits<std::uint32_t>::max());
    auto content_fingerprint =
        require_optional_string(object, "content_fingerprint", kIdentityMaximumBytes);
    auto error_code = require_optional_string(object, "error_code", kIdentityMaximumBytes);
    auto error_message = require_optional_string(object, "error_message", kUriMaximumBytes);
    auto rating = require_integer<int>(object, "rating", 0, 5);
    auto color_label = require_string(object, "color_label", 16U);
    auto has_edits = require_boolean(object, "has_edits");
    auto rejected = require_boolean(object, "rejected");
    if (!id)
        return id.error();
    if (!normalized_uri)
        return normalized_uri.error();
    if (!media_type)
        return media_type.error();
    if (!import_state)
        return import_state.error();
    if (!created)
        return created.error();
    if (!mtime)
        return mtime.error();
    if (!size)
        return size.error();
    if (!width)
        return width.error();
    if (!height)
        return height.error();
    if (!content_fingerprint)
        return content_fingerprint.error();
    if (!error_code)
        return error_code.error();
    if (!error_message)
        return error_message.error();
    if (!rating)
        return rating.error();
    if (!color_label)
        return color_label.error();
    if (!has_edits)
        return has_edits.error();
    if (!rejected)
        return rejected.error();
    if (!safe_asset_id(id.value()) || !media_type.value().starts_with("image/") ||
        (import_state.value() != kImportStateImported &&
         import_state.value() != kImportStateFailed &&
         import_state.value() != kImportStateMissing) ||
        width.value().has_value() != height.value().has_value())
        return recovery_error(ErrorCode::kValidation, "Recovery asset state is invalid",
                              "invalid_recovery_asset");
    auto rating_valid = validate_rating(rating.value());
    if (!rating_valid)
        return rating_valid.error();
    auto parsed_label = parse_color_label(color_label.value());
    if (!parsed_label)
        return parsed_label.error();

    const auto capture_value = object.find("capture");
    const auto metadata_value = object.find("metadata");
    const auto tags_value = object.find("tags");
    auto capture = require_object(capture_value == object.end() ? nullptr : &capture_value->second,
                                  "payload.asset.capture");
    auto metadata =
        require_object(metadata_value == object.end() ? nullptr : &metadata_value->second,
                       "payload.asset.metadata");
    auto tags = require_array(tags_value == object.end() ? nullptr : &tags_value->second,
                              "payload.asset.tags");
    if (!capture)
        return capture.error();
    if (!metadata)
        return metadata.error();
    if (!tags)
        return tags.error();
    auto capture_valid = validate_recovery_capture(*capture.value());
    if (!capture_valid)
        return capture_valid.error();
    auto metadata_valid = validate_recovery_metadata(*metadata.value());
    if (!metadata_valid)
        return metadata_valid.error();
    if (tags.value()->size() > kRecoveryTagMaximumEntries)
        return recovery_error(ErrorCode::kValidation, "Recovery sidecar contains too many tags",
                              "recovery_tag_count_exceeded");
    std::string previous_tag;
    for (const auto &tag_value : *tags.value())
    {
        const auto *tag = tag_value.string_if();
        if (tag == nullptr || tag->empty() || tag->size() > kTagMaxLength ||
            (!previous_tag.empty() && *tag <= previous_tag))
            return recovery_error(ErrorCode::kValidation,
                                  "Recovery sidecar tag ordering is invalid",
                                  "invalid_recovery_tag");
        auto normalized = normalize_tag_name(*tag);
        if (!normalized || normalized.value() != *tag)
            return recovery_error(ErrorCode::kValidation, "Recovery sidecar tag is not canonical",
                                  "invalid_recovery_tag");
        previous_tag = *tag;
    }
    return VerifiedAsset{std::move(id).value(), has_edits.value()};
}

[[nodiscard]] Result<void> validate_recovery_history(const JsonValue::Array &history)
{
    if (history.size() > kRecoveryHistoryMaximumEntries)
        return recovery_error(ErrorCode::kValidation,
                              "Recovery sidecar contains too many history entries",
                              "recovery_history_count_exceeded");
    std::int64_t previous_sequence = 0;
    for (const auto &value : history)
    {
        auto entry = require_object(&value, "payload.history[]");
        if (!entry)
            return entry.error();
        auto keys = expect_exact_keys(*entry.value(),
                                      {"created_unix_ms", "kind", "label", "recipe_json", "seq"},
                                      "payload.history[]");
        if (!keys)
            return keys.error();
        auto sequence = require_integer<std::int64_t>(*entry.value(), "seq", 1,
                                                      std::numeric_limits<std::int64_t>::max());
        auto created = require_integer<std::int64_t>(*entry.value(), "created_unix_ms", 0,
                                                     std::numeric_limits<std::int64_t>::max());
        auto kind = require_string(*entry.value(), "kind", 16U);
        auto label = require_optional_string(*entry.value(), "label", kTagMaxLength);
        if (!sequence)
            return sequence.error();
        if (!created)
            return created.error();
        if (!kind)
            return kind.error();
        if (!label)
            return label.error();
        if (sequence.value() <= previous_sequence || (kind.value() != kRecipeHistoryKindHistory &&
                                                      kind.value() != kRecipeHistoryKindSnapshot))
            return recovery_error(ErrorCode::kValidation,
                                  "Recovery sidecar history ordering is invalid",
                                  "invalid_recovery_history");
        const auto recipe = entry.value()->find("recipe_json");
        const auto *recipe_text =
            recipe == entry.value()->end() ? nullptr : recipe->second.string_if();
        if (recipe_text == nullptr || recipe_text->size() > kRecoverySidecarMaximumBytes)
            return recovery_error(ErrorCode::kValidation,
                                  "Recovery sidecar history recipe is invalid",
                                  "invalid_recovery_history");
        if (!recipe_text->empty())
        {
            auto parsed = parse_recipe_json(*recipe_text);
            if (!parsed)
                return parsed.error();
        }
        previous_sequence = sequence.value();
    }
    return {};
}

struct VerifiedDocument
{
    std::string asset_id;
    std::int64_t generation = 0;
    std::string artifact_sha256;
    // catalog_revision is an observation of the whole catalog at serialization
    // time, not asset state. Excluding it keeps one asset generation immutable
    // across a crash between filesystem publication and database acknowledgement.
    std::string asset_state_checksum;
};

[[nodiscard]] Result<VerifiedDocument> verify_document(const std::string_view document)
{
    if (document.empty() || document.size() > kRecoverySidecarMaximumBytes)
        return recovery_error(ErrorCode::kValidation, "Recovery sidecar size is invalid",
                              "invalid_recovery_sidecar_size");
    auto parsed = parse_json(document);
    if (!parsed)
        return parsed.error();
    auto root = require_object(&parsed.value(), "root");
    if (!root)
        return root.error();
    auto root_keys = expect_exact_keys(*root.value(), {"checksum", "payload"}, "root");
    if (!root_keys)
        return root_keys.error();
    auto checksum_object = require_object(parsed.value().find("checksum"), "checksum");
    auto payload = require_object(parsed.value().find("payload"), "payload");
    if (!checksum_object)
        return checksum_object.error();
    if (!payload)
        return payload.error();
    auto checksum_keys =
        expect_exact_keys(*checksum_object.value(), {"algorithm", "value"}, "checksum");
    if (!checksum_keys)
        return checksum_keys.error();
    auto algorithm = require_string(*checksum_object.value(), "algorithm", 16U);
    auto checksum = require_string(*checksum_object.value(), "value", 64U);
    if (!algorithm)
        return algorithm.error();
    if (!checksum)
        return checksum.error();
    if (algorithm.value() != "sha256" || !valid_sha256(checksum.value()))
        return recovery_error(ErrorCode::kValidation,
                              "Recovery sidecar checksum descriptor is invalid",
                              "invalid_recovery_checksum");
    if (sha256(serialize_json(*parsed.value().find("payload"))) != checksum.value())
        return recovery_error(ErrorCode::kValidation, "Recovery sidecar checksum does not match",
                              "recovery_checksum_mismatch");

    auto payload_keys = expect_exact_keys(*payload.value(),
                                          {"asset", "catalog_id", "catalog_revision", "generation",
                                           "history", "recipe_json", "schema", "version"},
                                          "payload");
    if (!payload_keys)
        return payload_keys.error();
    auto schema = require_string(*payload.value(), "schema", 64U);
    auto version = require_integer<std::int64_t>(*payload.value(), "version", 1,
                                                 std::numeric_limits<std::int64_t>::max());
    auto generation = require_integer<std::int64_t>(*payload.value(), "generation", 1,
                                                    std::numeric_limits<std::int64_t>::max());
    auto catalog_id = require_string(*payload.value(), "catalog_id", kIdentityMaximumBytes);
    auto catalog_revision = require_integer<std::int64_t>(*payload.value(), "catalog_revision", 0,
                                                          std::numeric_limits<std::int64_t>::max());
    auto asset = require_object(parsed.value().find("payload")->find("asset"), "payload.asset");
    auto history =
        require_array(parsed.value().find("payload")->find("history"), "payload.history");
    if (!schema)
        return schema.error();
    if (!version)
        return version.error();
    if (!generation)
        return generation.error();
    if (!catalog_id)
        return catalog_id.error();
    if (!catalog_revision)
        return catalog_revision.error();
    if (!asset)
        return asset.error();
    if (!history)
        return history.error();
    if (schema.value() != "ravo-asset-recovery")
        return recovery_error(ErrorCode::kValidation,
                              "Recovery sidecar schema identifier is invalid",
                              "invalid_recovery_schema");
    if (version.value() > kRecoverySidecarSchemaVersion)
        return recovery_error(ErrorCode::kUnsupported,
                              "Recovery sidecar version is newer than this Ravo",
                              "newer_recovery_sidecar_version");
    if (version.value() != kRecoverySidecarSchemaVersion)
        return recovery_error(ErrorCode::kValidation, "Recovery sidecar version is invalid",
                              "invalid_recovery_sidecar_version");
    auto verified_asset = validate_recovery_asset(*asset.value());
    if (!verified_asset)
        return verified_asset.error();
    auto verified_history = validate_recovery_history(*history.value());
    if (!verified_history)
        return verified_history.error();

    const auto recipe = payload.value()->find("recipe_json");
    if (recipe == payload.value()->end())
        return recovery_error(ErrorCode::kValidation, "Recovery recipe field is missing",
                              "recovery_type_mismatch", {}, "recipe_json");
    bool has_recipe = false;
    if (!recipe->second.is_null())
    {
        const auto *recipe_text = recipe->second.string_if();
        if (recipe_text == nullptr || recipe_text->empty() ||
            recipe_text->size() > kRecoverySidecarMaximumBytes)
            return recovery_error(ErrorCode::kValidation, "Recovery recipe is invalid",
                                  "invalid_recovery_recipe");
        auto parsed_recipe = parse_recipe_json(*recipe_text);
        if (!parsed_recipe)
            return parsed_recipe.error();
        has_recipe = true;
    }
    if (verified_asset.value().has_edits != has_recipe)
        return recovery_error(ErrorCode::kValidation,
                              "Recovery edit state does not match its recipe",
                              "recovery_recipe_state_mismatch");

    JsonValue::Object stable_payload = *payload.value();
    stable_payload.erase("catalog_revision");
    const auto asset_state_checksum = sha256(serialize_json(JsonValue{std::move(stable_payload)}));
    return VerifiedDocument{std::move(verified_asset).value().id, generation.value(),
                            sha256(document), asset_state_checksum};
}

[[nodiscard]] Result<std::string> read_sidecar(const std::filesystem::path &path,
                                               const CancellationToken &cancellation)
{
    auto active = cancellation.check();
    if (!active)
        return active.error();
    std::error_code status_error;
    const auto status = std::filesystem::symlink_status(path, status_error);
    if (status_error == std::errc::no_such_file_or_directory ||
        status.type() == std::filesystem::file_type::not_found)
        return recovery_error(ErrorCode::kNotFound, "Recovery sidecar does not exist",
                              "recovery_sidecar_not_found", path_utf8(path));
    if (status_error || !std::filesystem::is_regular_file(status))
        return recovery_error(ErrorCode::kIo, "Recovery sidecar is not a regular file",
                              "recovery_sidecar_not_regular", path_utf8(path), {}, status_error);
    const auto bytes = std::filesystem::file_size(path, status_error);
    if (status_error)
        return recovery_error(ErrorCode::kIo, "Unable to measure recovery sidecar",
                              "recovery_sidecar_measure_failed", path_utf8(path), {}, status_error);
    if (bytes == 0U || bytes > kRecoverySidecarMaximumBytes)
        return recovery_error(ErrorCode::kValidation, "Recovery sidecar size is invalid",
                              "invalid_recovery_sidecar_size", path_utf8(path));
    QFile file(qstring_from_utf8(path_utf8(path)));
    if (!file.open(QIODevice::ReadOnly))
        return recovery_io_error("Unable to open recovery sidecar", "recovery_sidecar_open_failed",
                                 path_utf8(path), file.errorString().toUtf8().toStdString());
    const auto data = file.readAll();
    if (data.size() != static_cast<qsizetype>(bytes))
        return recovery_error(ErrorCode::kIo, "Unable to read complete recovery sidecar",
                              "recovery_sidecar_read_failed", path_utf8(path));
    active = cancellation.check();
    if (!active)
        return active.error();
    return std::string(data.constData(), static_cast<std::size_t>(data.size()));
}

[[nodiscard]] Result<void> validate_identity(const VerifiedDocument &verified,
                                             const std::string_view asset_id,
                                             const std::int64_t generation,
                                             const std::string_view path)
{
    if (verified.asset_id != asset_id || verified.generation != generation)
        return recovery_error(ErrorCode::kValidation,
                              "Recovery sidecar identity does not match its path",
                              "recovery_sidecar_identity_mismatch", std::string(path));
    return {};
}

} // namespace

namespace recovery_publication_internal
{
namespace
{

[[nodiscard]] std::error_code invoke_hook(const CheckpointHook &hook, const Checkpoint checkpoint,
                                          const std::string_view path,
                                          const std::uint64_t bytes_processed) noexcept
{
    return hook.callback == nullptr ?
               std::error_code{} :
               hook.callback(hook.context, checkpoint, path, bytes_processed);
}

[[nodiscard]] TaskError publication_error(const Checkpoint checkpoint,
                                          const std::string_view output,
                                          const std::error_code &error)
{
    std::string message = "Unable to publish recovery sidecar";
    std::string reason = "recovery_publish_failed";
    switch (checkpoint)
    {
    case Checkpoint::kBeforeTemporaryOpen:
    case Checkpoint::kTemporaryCreated:
        message = "Unable to create recovery sidecar temporary";
        reason = "recovery_temporary_open_failed";
        break;
    case Checkpoint::kBeforeTemporaryWrite:
    case Checkpoint::kTemporaryChunkWritten:
        message = "Unable to write recovery sidecar";
        reason = "recovery_temporary_write_failed";
        break;
    case Checkpoint::kBeforeTemporarySync:
        message = "Unable to synchronize recovery sidecar";
        reason = "recovery_temporary_sync_failed";
        break;
    case Checkpoint::kBeforePublish:
        break;
    }
    return recovery_io_error(std::move(message), std::move(reason), std::string(output),
                             error.message());
}

[[nodiscard]] Result<void> check_cancellation(const CancellationToken &cancellation,
                                              const std::string_view output)
{
    auto active = cancellation.check();
    if (active)
        return {};
    auto error = std::move(active).error();
    error.context.insert_or_assign("path", std::string(output));
    error.context.insert_or_assign("output", std::string(output));
    return error;
}

[[nodiscard]] std::error_code synchronize_file(QTemporaryFile &file) noexcept
{
    if (!file.flush())
        return std::make_error_code(std::errc::io_error);
#ifdef _WIN32
    if (file.handle() < 0 || ::_commit(static_cast<int>(file.handle())) != 0)
        return std::error_code(errno, std::generic_category());
#else
    if (file.handle() < 0 || ::fsync(static_cast<int>(file.handle())) != 0)
        return std::error_code(errno, std::generic_category());
#endif
    return {};
}

} // namespace

Result<void> publish_no_replace(const std::string_view output, const std::string_view document,
                                const CancellationToken &cancellation, const CheckpointHook hook)
{
    auto active = check_cancellation(cancellation, output);
    if (!active)
        return active.error();

    const auto output_path = path_from_utf8(output);
    std::error_code status_error;
    const auto output_status = std::filesystem::symlink_status(output_path, status_error);
    if (!status_error && output_status.type() != std::filesystem::file_type::not_found)
        return recovery_error(ErrorCode::kConflict, "Recovery sidecar already exists",
                              "recovery_sidecar_exists", std::string(output));
    if (status_error && status_error != std::errc::no_such_file_or_directory)
        return recovery_error(ErrorCode::kIo, "Unable to inspect recovery sidecar path",
                              "recovery_sidecar_inspect_failed", std::string(output), {},
                              status_error);

    auto hook_error = invoke_hook(hook, Checkpoint::kBeforeTemporaryOpen, output, 0U);
    if (hook_error)
        return publication_error(Checkpoint::kBeforeTemporaryOpen, output, hook_error);
    active = check_cancellation(cancellation, output);
    if (!active)
        return active.error();

    QTemporaryFile file(qstring_from_utf8(std::string(output) + ".tmp.XXXXXX"));
    file.setAutoRemove(true);
    if (!file.open())
        return recovery_io_error("Unable to create recovery sidecar temporary",
                                 "recovery_temporary_open_failed", std::string(output),
                                 file.errorString().toUtf8().toStdString());
    const auto temporary = file.fileName().toUtf8().toStdString();
    hook_error = invoke_hook(hook, Checkpoint::kTemporaryCreated, temporary, 0U);
    if (hook_error)
        return publication_error(Checkpoint::kTemporaryCreated, output, hook_error);
    active = check_cancellation(cancellation, output);
    if (!active)
        return active.error();

    std::size_t offset = 0U;
    while (offset < document.size())
    {
        active = check_cancellation(cancellation, output);
        if (!active)
            return active.error();
        hook_error = invoke_hook(hook, Checkpoint::kBeforeTemporaryWrite, temporary, offset);
        if (hook_error)
            return publication_error(Checkpoint::kBeforeTemporaryWrite, output, hook_error);
        active = check_cancellation(cancellation, output);
        if (!active)
            return active.error();
        const auto count = std::min(kPublicationChunkBytes, document.size() - offset);
        if (file.write(document.data() + offset, static_cast<qint64>(count)) !=
            static_cast<qint64>(count))
            return recovery_io_error("Unable to write recovery sidecar",
                                     "recovery_temporary_write_failed", std::string(output),
                                     file.errorString().toUtf8().toStdString());
        offset += count;
        hook_error = invoke_hook(hook, Checkpoint::kTemporaryChunkWritten, temporary, offset);
        if (hook_error)
            return publication_error(Checkpoint::kTemporaryChunkWritten, output, hook_error);
        active = check_cancellation(cancellation, output);
        if (!active)
            return active.error();
    }

    active = check_cancellation(cancellation, output);
    if (!active)
        return active.error();
    hook_error = invoke_hook(hook, Checkpoint::kBeforeTemporarySync, temporary, offset);
    if (hook_error)
        return publication_error(Checkpoint::kBeforeTemporarySync, output, hook_error);
    active = check_cancellation(cancellation, output);
    if (!active)
        return active.error();
    const auto sync_error = synchronize_file(file);
    if (sync_error)
        return publication_error(Checkpoint::kBeforeTemporarySync, output, sync_error);
    file.close();

    active = check_cancellation(cancellation, output);
    if (!active)
        return active.error();
    hook_error = invoke_hook(hook, Checkpoint::kBeforePublish, temporary, offset);
    if (hook_error)
        return publication_error(Checkpoint::kBeforePublish, output, hook_error);
    active = check_cancellation(cancellation, output);
    if (!active)
        return active.error();
    if (!QFile::rename(qstring_from_utf8(temporary), qstring_from_utf8(output)))
    {
        status_error.clear();
        const auto raced_status = std::filesystem::symlink_status(output_path, status_error);
        if (!status_error && raced_status.type() != std::filesystem::file_type::not_found)
            return recovery_error(ErrorCode::kConflict, "Recovery sidecar already exists",
                                  "recovery_sidecar_exists", std::string(output));
        return recovery_io_error("Unable to publish recovery sidecar", "recovery_publish_failed",
                                 std::string(output), "rename failed");
    }
    file.setAutoRemove(false);
    return {};
}

} // namespace recovery_publication_internal

FilesystemRecoveryStore::FilesystemRecoveryStore(std::string root)
    : root_(std::move(root))
{
}

std::string FilesystemRecoveryStore::default_root_for_catalog(const std::string_view database_path)
{
    return std::string(database_path) + ".ravo/sidecars";
}

Result<std::unique_ptr<FilesystemRecoveryStore>>
FilesystemRecoveryStore::create_for_catalog(const std::string_view database_path)
{
    if (database_path.empty())
        return recovery_error(ErrorCode::kInvalidArgument,
                              "Catalog database path must not be empty", "empty_catalog_path");
    return create(default_root_for_catalog(database_path));
}

Result<std::unique_ptr<FilesystemRecoveryStore>>
FilesystemRecoveryStore::create(const std::string_view root)
{
    if (root.empty())
        return recovery_error(ErrorCode::kInvalidArgument,
                              "Recovery sidecar root must not be empty", "empty_recovery_root");
    const auto path = path_from_utf8(root);
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error)
        return recovery_error(ErrorCode::kIo, "Unable to create recovery sidecar directory",
                              "recovery_root_create_failed", std::string(root), {}, error);
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_directory(status))
        return recovery_error(ErrorCode::kIo, "Recovery sidecar root is not a directory",
                              "recovery_root_not_directory", std::string(root), {}, error);
    return std::unique_ptr<FilesystemRecoveryStore>(new FilesystemRecoveryStore(std::string(root)));
}

Result<std::unique_ptr<FilesystemRecoveryStore>>
FilesystemRecoveryStore::open_existing(const std::string_view root)
{
    if (root.empty())
        return recovery_error(ErrorCode::kInvalidArgument,
                              "Recovery sidecar root must not be empty", "empty_recovery_root");
    const auto path = path_from_utf8(root);
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || !std::filesystem::is_directory(status))
        return recovery_error(ErrorCode::kValidation,
                              "Recovery sidecar root is not an existing directory",
                              "recovery_root_not_directory", std::string(root), {}, error);
    return std::unique_ptr<FilesystemRecoveryStore>(new FilesystemRecoveryStore(std::string(root)));
}

const std::string &FilesystemRecoveryStore::root() const noexcept
{
    return root_;
}

Result<RecoveryArtifact> FilesystemRecoveryStore::publish(const AssetRecoverySnapshot &snapshot,
                                                          const CancellationToken &cancellation)
{
    auto active = cancellation.check();
    if (!active)
        return active.error();
    auto serialized = serialize_recovery(snapshot);
    if (!serialized)
        return serialized.error();
    const auto output =
        path_from_utf8(root_) /
        path_from_utf8(sidecar_filename(snapshot.asset.id, snapshot.state.generation));
    const auto verify_existing = [&]() -> Result<RecoveryArtifact>
    {
        auto document = read_sidecar(output, CancellationToken{});
        if (!document)
            return document.error();
        auto existing = verify_document(document.value());
        if (!existing)
            return existing.error();
        auto identity = validate_identity(existing.value(), snapshot.asset.id,
                                          snapshot.state.generation, path_utf8(output));
        if (!identity)
            return identity.error();
        auto expected = verify_document(serialized.value().first);
        if (!expected)
            return expected.error();
        if (existing.value().asset_state_checksum != expected.value().asset_state_checksum)
            return recovery_error(ErrorCode::kConflict,
                                  "Recovery sidecar generation has different content",
                                  "recovery_generation_conflict", path_utf8(output));
        return RecoveryArtifact{snapshot.asset.id, snapshot.state.generation, path_utf8(output),
                                existing.value().artifact_sha256,
                                static_cast<std::uint64_t>(document.value().size())};
    };
    std::error_code status_error;
    const auto output_status = std::filesystem::symlink_status(output, status_error);
    if (status_error && status_error != std::errc::no_such_file_or_directory)
        return recovery_error(ErrorCode::kIo, "Unable to inspect recovery sidecar path",
                              "recovery_sidecar_inspect_failed", path_utf8(output), {},
                              status_error);
    const bool output_exists =
        !status_error && output_status.type() != std::filesystem::file_type::not_found;
    if (output_exists)
        return verify_existing();

    auto published = recovery_publication_internal::publish_no_replace(
        path_utf8(output), serialized.value().first, cancellation);
    if (!published)
    {
        if (published.error().code == ErrorCode::kConflict)
            return verify_existing();
        return published.error();
    }
    // Publication is the commit point. Finish bounded verification even if the
    // caller cancels immediately afterwards; returning a cancellation after
    // visibility would hide the durable artifact fact from acknowledgement.
    auto artifact = verify(snapshot.asset.id, snapshot.state.generation, CancellationToken{});
    if (!artifact)
        return artifact.error();
    if (artifact.value().sha256 != serialized.value().second)
        return recovery_error(ErrorCode::kValidation,
                              "Published recovery sidecar differs from committed state",
                              "published_recovery_checksum_mismatch", path_utf8(output));
    return artifact;
}

Result<RecoveryArtifact>
FilesystemRecoveryStore::verify(const std::string_view asset_id, const std::int64_t generation,
                                const CancellationToken &cancellation) const
{
    if (!safe_asset_id(asset_id) || generation <= 0)
        return recovery_error(ErrorCode::kInvalidArgument, "Recovery sidecar identity is invalid",
                              "invalid_recovery_identity");
    const auto path =
        path_from_utf8(root_) / path_from_utf8(sidecar_filename(asset_id, generation));
    return verify_artifact(path_utf8(path), asset_id, generation, cancellation);
}

Result<RecoveryArtifact> FilesystemRecoveryStore::verify_artifact(
    const std::string_view path, const std::string_view asset_id, const std::int64_t generation,
    const CancellationToken &cancellation) const
{
    if (path.empty() || !safe_asset_id(asset_id) || generation <= 0)
        return recovery_error(ErrorCode::kInvalidArgument, "Recovery sidecar identity is invalid",
                              "invalid_recovery_identity");
    const auto filesystem_path = path_from_utf8(path);
    auto document = read_sidecar(filesystem_path, cancellation);
    if (!document)
        return document.error();
    auto verified = verify_document(document.value());
    if (!verified)
        return verified.error();
    auto identity =
        validate_identity(verified.value(), asset_id, generation, path_utf8(filesystem_path));
    if (!identity)
        return identity.error();
    return RecoveryArtifact{std::string(asset_id), generation, path_utf8(filesystem_path),
                            verified.value().artifact_sha256,
                            static_cast<std::uint64_t>(document.value().size())};
}

Result<void> FilesystemRecoveryStore::remove_older(const std::string_view asset_id,
                                                   const std::int64_t keep_generation)
{
    if (!safe_asset_id(asset_id) || keep_generation <= 0)
        return recovery_error(ErrorCode::kInvalidArgument, "Recovery cleanup identity is invalid",
                              "invalid_recovery_identity");
    const auto root = path_from_utf8(root_);
    std::error_code iterator_error;
    std::filesystem::directory_iterator iterator(root, iterator_error);
    if (iterator_error)
        return recovery_error(ErrorCode::kIo, "Unable to enumerate recovery sidecars",
                              "recovery_enumeration_failed", root_, {}, iterator_error);
    const auto prefix = std::string(asset_id) + ".";
    constexpr std::string_view suffix = ".ravo.json";
    for (const std::filesystem::directory_iterator end; iterator != end;
         iterator.increment(iterator_error))
    {
        if (iterator_error)
            return recovery_error(ErrorCode::kIo, "Unable to enumerate recovery sidecars",
                                  "recovery_enumeration_failed", root_, {}, iterator_error);
        const auto name = path_utf8(iterator->path().filename());
        if (!name.starts_with(prefix) || !name.ends_with(suffix))
            continue;
        const auto generation_text = std::string_view(name).substr(
            prefix.size(), name.size() - prefix.size() - suffix.size());
        std::int64_t generation = 0;
        const auto parsed = std::from_chars(
            generation_text.data(), generation_text.data() + generation_text.size(), generation);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != generation_text.data() + generation_text.size() || generation <= 0 ||
            generation >= keep_generation)
            continue;
        std::error_code remove_error;
        std::filesystem::remove(iterator->path(), remove_error);
        if (remove_error)
            return recovery_error(ErrorCode::kIo, "Unable to remove obsolete recovery sidecar",
                                  "recovery_cleanup_failed", path_utf8(iterator->path()), {},
                                  remove_error);
    }
    return {};
}

Result<void> FilesystemRecoveryStore::remove_asset(const std::string_view asset_id)
{
    if (!safe_asset_id(asset_id))
        return recovery_error(ErrorCode::kInvalidArgument, "Recovery cleanup asset ID is invalid",
                              "invalid_recovery_asset_id");
    const auto root = path_from_utf8(root_);
    std::error_code iterator_error;
    std::filesystem::directory_iterator iterator(root, iterator_error);
    if (iterator_error)
        return recovery_error(ErrorCode::kIo, "Unable to enumerate recovery sidecars",
                              "recovery_enumeration_failed", root_, {}, iterator_error);
    const auto prefix = std::string(asset_id) + ".";
    constexpr std::string_view suffix = ".ravo.json";
    for (const std::filesystem::directory_iterator end; iterator != end;
         iterator.increment(iterator_error))
    {
        if (iterator_error)
            return recovery_error(ErrorCode::kIo, "Unable to enumerate recovery sidecars",
                                  "recovery_enumeration_failed", root_, {}, iterator_error);
        const auto name = path_utf8(iterator->path().filename());
        if (!name.starts_with(prefix) || !name.ends_with(suffix))
            continue;
        std::error_code status_error;
        const auto status = iterator->symlink_status(status_error);
        if (status_error || !std::filesystem::is_regular_file(status))
            return recovery_error(ErrorCode::kIo, "Recovery sidecar is not a regular file",
                                  "recovery_sidecar_not_regular", path_utf8(iterator->path()), {},
                                  status_error);
        std::error_code remove_error;
        std::filesystem::remove(iterator->path(), remove_error);
        if (remove_error)
            return recovery_error(ErrorCode::kIo, "Unable to remove recovery sidecar",
                                  "recovery_cleanup_failed", path_utf8(iterator->path()), {},
                                  remove_error);
    }
    return {};
}

} // namespace ravo
