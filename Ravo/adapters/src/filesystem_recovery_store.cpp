#include "ravo/adapters/filesystem_recovery_store.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
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
#include <QSaveFile>
#include <QString>

#include "ravo/foundation/json.h"
#include "ravo/recipe/recipe.h"

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
        snapshot.asset.media_type.empty() ||
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
    for (const auto &tag : snapshot.asset.tags)
    {
        if (tag.empty() || tag.size() > kTagMaxLength)
            return recovery_error(ErrorCode::kValidation,
                                  "Recovery snapshot contains an invalid tag",
                                  "invalid_recovery_tag");
        tags.emplace_back(tag);
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
    if (value == nullptr || value->empty() || value->size() > maximum_bytes)
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
    JsonValue::Object stable_payload = *payload.value();
    stable_payload.erase("catalog_revision");
    const auto asset_state_checksum = sha256(serialize_json(JsonValue{std::move(stable_payload)}));
    auto schema = require_string(*payload.value(), "schema", 64U);
    auto version = require_integer<std::int64_t>(*payload.value(), "version", 1,
                                                 std::numeric_limits<std::int64_t>::max());
    auto generation = require_integer<std::int64_t>(*payload.value(), "generation", 1,
                                                    std::numeric_limits<std::int64_t>::max());
    auto asset = require_object(parsed.value().find("payload")->find("asset"), "payload.asset");
    if (!schema)
        return schema.error();
    if (!version)
        return version.error();
    if (!generation)
        return generation.error();
    if (!asset)
        return asset.error();
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
    auto asset_id = require_string(*asset.value(), "id", 180U);
    if (!asset_id)
        return asset_id.error();
    if (!safe_asset_id(asset_id.value()))
        return recovery_error(ErrorCode::kValidation, "Recovery sidecar asset ID is invalid",
                              "invalid_recovery_asset_id");
    return VerifiedDocument{std::move(asset_id).value(), generation.value(), sha256(document),
                            asset_state_checksum};
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
    std::error_code status_error;
    const auto output_status = std::filesystem::symlink_status(output, status_error);
    if (status_error && status_error != std::errc::no_such_file_or_directory)
        return recovery_error(ErrorCode::kIo, "Unable to inspect recovery sidecar path",
                              "recovery_sidecar_inspect_failed", path_utf8(output), {},
                              status_error);
    const bool output_exists =
        !status_error && output_status.type() != std::filesystem::file_type::not_found;
    if (output_exists)
    {
        auto document = read_sidecar(output, cancellation);
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
    }

    QSaveFile file(qstring_from_utf8(path_utf8(output)));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return recovery_io_error("Unable to create recovery sidecar temporary",
                                 "recovery_temporary_open_failed", path_utf8(output),
                                 file.errorString().toUtf8().toStdString());
    std::size_t offset = 0U;
    while (offset < serialized.value().first.size())
    {
        active = cancellation.check();
        if (!active)
        {
            file.cancelWriting();
            return active.error();
        }
        const auto count =
            std::min(kPublicationChunkBytes, serialized.value().first.size() - offset);
        if (file.write(serialized.value().first.data() + offset, static_cast<qint64>(count)) !=
            static_cast<qint64>(count))
        {
            const auto detail = file.errorString().toUtf8().toStdString();
            file.cancelWriting();
            return recovery_io_error("Unable to write recovery sidecar",
                                     "recovery_temporary_write_failed", path_utf8(output), detail);
        }
        offset += count;
    }
    active = cancellation.check();
    if (!active)
    {
        file.cancelWriting();
        return active.error();
    }
    if (!file.commit())
        return recovery_io_error("Unable to publish recovery sidecar", "recovery_publish_failed",
                                 path_utf8(output), file.errorString().toUtf8().toStdString());
    active = cancellation.check();
    if (!active)
        return active.error();
    auto artifact = verify(snapshot.asset.id, snapshot.state.generation, cancellation);
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
