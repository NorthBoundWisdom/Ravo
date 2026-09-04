#include "catalog_restore_uri.h"

#include <map>
#include <string>
#include <utility>

#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/adapters/text_file.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/json.h"
#include "ravo/domain/recovery_store.h"

namespace ravo
{
namespace
{

constexpr std::string_view kSupportMarker = ".ravo/";
constexpr const char *kKnownSupportPrefixes[] = {"derived/", "external-editor/", "sidecars/",
                                                 "dng-conversion/", "smart-previews/"};

[[nodiscard]] TaskError uri_error(const ErrorCode code, std::string message, std::string reason,
                                  std::string path = {}, std::string detail = {})
{
    std::map<std::string, std::string, std::less<>> context{{"reason", std::move(reason)}};
    if (!path.empty())
        context.emplace("path", std::move(path));
    if (!detail.empty())
        context.emplace("detail", std::move(detail));
    return make_error(code, std::move(message), std::move(context));
}

[[nodiscard]] bool known_support_suffix(const std::string_view suffix) noexcept
{
    for (const char *prefix : kKnownSupportPrefixes)
    {
        const std::string_view known(prefix);
        if (suffix.size() >= known.size() && suffix.compare(0, known.size(), known) == 0)
            return true;
    }
    return false;
}

[[nodiscard]] std::string path_utf8(const std::filesystem::path &path)
{
    const auto encoded = path.generic_u8string();
    return {reinterpret_cast<const char *>(encoded.data()), encoded.size()};
}

[[nodiscard]] Result<std::string>
rewrite_path_or_uri(const std::string_view value, const std::string_view destination_support_root)
{
    if (value.empty())
        return std::string{};

    const auto marker = value.find(kSupportMarker);
    if (marker == std::string_view::npos)
        return std::string(value);

    const auto suffix = value.substr(marker + kSupportMarker.size());
    if (!known_support_suffix(suffix))
    {
        return uri_error(ErrorCode::kValidation,
                         "Support-rooted URI points outside known catalog support roots",
                         "restore_support_uri_outside_known_roots", std::string(value));
    }

    // Prefer path form when the value is a filesystem path; keep file:// when present.
    if (value.starts_with("file:"))
    {
        auto location = normalize_local_input(value);
        if (!location)
            return location.error();
        const auto path_marker = location.value().path.find(kSupportMarker);
        if (path_marker == std::string::npos)
        {
            return uri_error(ErrorCode::kValidation,
                             "Support-rooted URI could not be resolved to a path prefix",
                             "restore_support_uri_unresolved", std::string(value));
        }
        const auto path_suffix = location.value().path.substr(path_marker + kSupportMarker.size());
        if (!known_support_suffix(path_suffix))
        {
            return uri_error(ErrorCode::kValidation,
                             "Support-rooted URI points outside known catalog support roots",
                             "restore_support_uri_outside_known_roots", std::string(value));
        }
        const std::string rewritten_path =
            std::string(destination_support_root) + "/" + std::string(path_suffix);
        auto rewritten = normalize_local_input(rewritten_path);
        if (!rewritten)
            return rewritten.error();
        return rewritten.value().uri;
    }

    const std::string rewritten = std::string(destination_support_root) + "/" + std::string(suffix);
    return rewritten;
}

[[nodiscard]] Result<bool>
rewrite_json_string_field(JsonValue::Object &object, const char *key,
                          const std::string_view destination_support_root)
{
    auto found = object.find(key);
    if (found == object.end() || found->second.string_if() == nullptr)
        return false;
    auto rewritten = rewrite_path_or_uri(*found->second.string_if(), destination_support_root);
    if (!rewritten)
        return rewritten.error();
    if (rewritten.value() == *found->second.string_if())
        return false;
    found->second = JsonValue{std::move(rewritten).value()};
    return true;
}

[[nodiscard]] Result<std::size_t>
rewrite_json_file(const std::filesystem::path &path,
                  const std::string_view destination_support_root,
                  const std::initializer_list<const char *> fields)
{
    const auto path_text = path_utf8(path);
    auto text = read_utf8_text_file(path_text);
    if (!text)
        return text.error();
    auto parsed = parse_json(text.value());
    if (!parsed)
        return parsed.error();
    const auto *object = parsed.value().object_if();
    if (object == nullptr)
    {
        return uri_error(ErrorCode::kValidation, "Support JSON is not an object",
                         "restore_support_json_not_object", path_text);
    }
    JsonValue::Object mutable_object = *object;
    bool changed = false;
    for (const char *field : fields)
    {
        auto field_changed =
            rewrite_json_string_field(mutable_object, field, destination_support_root);
        if (!field_changed)
            return field_changed.error();
        changed = changed || field_changed.value();
    }
    if (!changed)
        return 0U;
    auto written = write_utf8_text_file_replace_atomically(
        path_text, serialize_json(JsonValue{std::move(mutable_object)}));
    if (!written)
        return written.error();
    return 1U;
}

} // namespace

Result<std::string>
catalog_restore_rewrite_support_rooted_value(const std::string_view value,
                                             const std::string_view destination_support_root)
{
    if (destination_support_root.empty())
    {
        return uri_error(ErrorCode::kInvalidArgument, "Destination support root must not be empty",
                         "invalid_restore_support_root");
    }
    return rewrite_path_or_uri(value, destination_support_root);
}

Result<std::size_t>
catalog_restore_rewrite_support_json_tree(const std::filesystem::path &support_root,
                                          const std::string_view destination_support_root,
                                          const CancellationToken &cancellation)
{
    std::size_t rewritten = 0U;
    std::error_code error;
    if (!std::filesystem::exists(support_root, error))
        return 0U;

    const auto external_root = support_root / "external-editor";
    if (std::filesystem::is_directory(external_root, error))
    {
        for (std::filesystem::directory_iterator it(external_root, error), end; it != end;
             it.increment(error))
        {
            auto active = cancellation.check();
            if (!active)
                return active.error();
            if (error)
            {
                return uri_error(ErrorCode::kIo, "Unable to enumerate external-editor tree",
                                 "restore_support_json_enumeration_failed",
                                 path_utf8(external_root), error.message());
            }
            if (!it->is_regular_file() || it->path().extension() != ".json")
                continue;
            // Top-level provenance files only (open-intents live in a subdirectory).
            if (it->path().parent_path() != external_root)
                continue;
            auto count = rewrite_json_file(it->path(), destination_support_root,
                                           {"derived_path", "source_original_path"});
            if (!count)
                return count.error();
            rewritten += count.value();
        }
        if (error)
        {
            return uri_error(ErrorCode::kIo, "Unable to enumerate external-editor tree",
                             "restore_support_json_enumeration_failed", path_utf8(external_root),
                             error.message());
        }

        const auto intents_root = external_root / "open-intents";
        if (std::filesystem::is_directory(intents_root, error))
        {
            for (std::filesystem::directory_iterator it(intents_root, error), end; it != end;
                 it.increment(error))
            {
                auto active = cancellation.check();
                if (!active)
                    return active.error();
                if (error)
                {
                    return uri_error(ErrorCode::kIo, "Unable to enumerate open-intent tree",
                                     "restore_support_json_enumeration_failed",
                                     path_utf8(intents_root), error.message());
                }
                if (!it->is_regular_file() || it->path().extension() != ".json")
                    continue;
                auto count = rewrite_json_file(it->path(), destination_support_root,
                                               {"open_path", "open_uri"});
                if (!count)
                    return count.error();
                rewritten += count.value();
            }
            if (error)
            {
                return uri_error(ErrorCode::kIo, "Unable to enumerate open-intent tree",
                                 "restore_support_json_enumeration_failed", path_utf8(intents_root),
                                 error.message());
            }
        }
    }
    return rewritten;
}

Result<std::size_t> catalog_restore_rewrite_recovery_sidecars(
    const std::filesystem::path &sidecar_root, const std::string_view destination_support_root,
    std::vector<RecoveryArtifact> &sidecars, const RecoveryStore &recovery_verifier,
    const CancellationToken &cancellation)
{
    std::size_t rewritten = 0U;
    for (auto &artifact : sidecars)
    {
        auto active = cancellation.check();
        if (!active)
            return active.error();

        const auto filename =
            artifact.asset_id + "." + std::to_string(artifact.generation) + ".ravo.json";
        const auto staged_path = sidecar_root / filename;
        const auto staged_path_text = path_utf8(staged_path);
        auto text = read_utf8_text_file(staged_path_text);
        if (!text)
            return text.error();
        auto parsed = parse_json(text.value());
        if (!parsed)
            return parsed.error();
        const auto *root = parsed.value().object_if();
        if (root == nullptr)
        {
            return uri_error(ErrorCode::kValidation, "Recovery sidecar is not an object",
                             "restore_recovery_sidecar_not_object", staged_path_text);
        }
        const auto *payload = parsed.value().find("payload");
        if (payload == nullptr || payload->object_if() == nullptr)
        {
            return uri_error(ErrorCode::kValidation, "Recovery sidecar payload is missing",
                             "restore_recovery_sidecar_payload_missing", staged_path_text);
        }
        const auto *asset = payload->find("asset");
        if (asset == nullptr || asset->object_if() == nullptr)
        {
            return uri_error(ErrorCode::kValidation, "Recovery sidecar asset is missing",
                             "restore_recovery_sidecar_asset_missing", staged_path_text);
        }
        JsonValue::Object mutable_asset = *asset->object_if();
        auto field_changed =
            rewrite_json_string_field(mutable_asset, "normalized_uri", destination_support_root);
        if (!field_changed)
            return field_changed.error();
        if (!field_changed.value())
            continue;

        JsonValue::Object mutable_payload = *payload->object_if();
        mutable_payload["asset"] = JsonValue{std::move(mutable_asset)};
        const auto payload_canonical = serialize_json(JsonValue{mutable_payload});
        const auto checksum = sha256_utf8_hex(payload_canonical);
        const auto *checksum_object = parsed.value().find("checksum");
        if (checksum_object == nullptr || checksum_object->object_if() == nullptr)
        {
            return uri_error(ErrorCode::kValidation, "Recovery sidecar checksum is missing",
                             "restore_recovery_sidecar_checksum_missing", staged_path_text);
        }
        JsonValue::Object mutable_checksum = *checksum_object->object_if();
        mutable_checksum["value"] = JsonValue{checksum};
        JsonValue::Object mutable_root = *root;
        mutable_root["payload"] = JsonValue{std::move(mutable_payload)};
        mutable_root["checksum"] = JsonValue{std::move(mutable_checksum)};
        auto written = write_utf8_text_file_replace_atomically(
            staged_path_text, serialize_json(JsonValue{std::move(mutable_root)}));
        if (!written)
            return written.error();

        auto verified = recovery_verifier.verify_artifact(staged_path_text, artifact.asset_id,
                                                          artifact.generation, cancellation);
        if (!verified)
            return verified.error();
        artifact = std::move(verified).value();
        ++rewritten;
    }
    return rewritten;
}

Result<std::size_t>
catalog_restore_rewrite_catalog_uris(const std::string_view catalog_path,
                                     const std::string_view destination_support_root,
                                     const CancellationToken &cancellation)
{
    return sqlite_rewrite_support_rooted_uris(catalog_path, destination_support_root, cancellation);
}

} // namespace ravo
