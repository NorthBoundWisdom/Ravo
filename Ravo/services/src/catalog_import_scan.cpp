#include "ravo/services/catalog_service.h"

#include <filesystem>
#include <map>
#include <set>
#include "catalog_internal.h"
#include "ravo/adapters/text_file.h"
#include "ravo/domain/uri.h"

namespace ravo
{
namespace
{
Result<std::string> stable_hash(const std::string &path, const FileIdentity &before,
                                const CancellationToken &cancellation)
{
    auto digest = sha256_file_hex(path, cancellation);
    if (!digest)
        return digest.error();
    auto after = read_file_identity(path);
    if (!after)
        return after.error();
    if (before.size_bytes != after.value().size_bytes ||
        before.mtime_unix_ms != after.value().mtime_unix_ms)
        return make_error(ErrorCode::kConflict, "Source changed while checking import content",
                          {{"path", path}, {"reason", "import_content_source_changed"}});
    return digest;
}
} // namespace

Result<ImportScanResult> CatalogService::scan_import_candidates(
    const std::vector<std::string> &inputs, const std::string_view source_root,
    const bool recursive, const CancellationToken &cancellation,
    const std::function<void(std::size_t, std::size_t, const ImportCandidate &)> &progress)
{
    if (!repository_)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    if (inputs.empty())
        return make_error(ErrorCode::kInvalidArgument, "Import scan requires at least one input",
                          {{"reason", "missing_import_scan_input"}});
    auto initial = repository_->snapshot();
    if (!initial)
        return initial.error();
    auto paths = enumerate_import_inputs(inputs, cancellation, recursive);
    if (!paths)
        return paths.error();
    ImportScanResult result;
    result.catalog_revision = initial.value().revision;
    std::set<std::uint64_t> indexed_sizes;
    std::set<std::string> batch_hashes;
    const auto index_size = [&](const std::uint64_t size) -> Result<void>
    {
        if (indexed_sizes.contains(size))
            return {};
        std::string after_id;
        for (;;)
        {
            auto page = repository_->import_content_sources(size, after_id);
            if (!page)
                return page.error();
            if (page.value().empty())
                break;
            for (const auto &source : page.value())
            {
                auto active = cancellation.check();
                if (!active)
                    return active.error();
                after_id = source.asset_id;
                auto location = normalize_local_input(source.normalized_uri);
                if (!location)
                    return location.error();
                auto identity = read_file_identity(location.value().path);
                // An indexed offline original still identifies the bytes imported earlier.
                if (!identity)
                {
                    if (source.sha256 && identity.error().code == ErrorCode::kNotFound)
                        continue;
                    return identity.error();
                }
                if (identity.value().size_bytes != source.size_bytes ||
                    identity.value().mtime_unix_ms != source.mtime_unix_ms)
                    return make_error(
                        ErrorCode::kConflict,
                        "Catalog original has changed; refresh its capture metadata before importing",
                        {{"path", location.value().path},
                         {"reason", "import_content_source_changed"}});
                if (source.sha256)
                    continue;
                auto digest = stable_hash(location.value().path, identity.value(), cancellation);
                if (!digest)
                    return digest.error();
                auto saved = repository_->cache_import_content(source, digest.value());
                if (!saved)
                    return saved.error();
            }
        }
        indexed_sizes.insert(size);
        return {};
    };
    for (const auto &path : paths.value())
    {
        auto active = cancellation.check();
        if (!active)
            return active.error();
        ImportCandidate candidate;
        candidate.source_path = path;
        candidate.display_name = uri_display_name(path);
        candidate.relative_path = candidate.display_name;
        if (!source_root.empty())
        {
            const auto relative = utf8_path(path).lexically_relative(utf8_path(source_root));
            if (!relative.empty())
            {
                const auto text = relative.generic_u8string();
                candidate.relative_path.assign(reinterpret_cast<const char *>(text.data()),
                                               text.size());
            }
        }
        const auto classify = [&]() -> Result<void>
        {
            auto location = normalize_local_input(path);
            if (!location)
                return location.error();
            candidate.source_path = location.value().path;
            auto existing = repository_->find_asset_by_uri(location.value().uri);
            if (!existing)
                return existing.error();
            if (existing.value())
            {
                candidate.duplicate = true;
                candidate.duplicate_reason = "catalog_path";
                candidate.duplicate_asset_id = existing.value()->id;
                return {};
            }
            auto identity = read_file_identity(path);
            if (!identity)
                return identity.error();
            candidate.size_bytes = identity.value().size_bytes;
            candidate.mtime_unix_ms = identity.value().mtime_unix_ms;
            auto digest = stable_hash(path, identity.value(), cancellation);
            if (!digest)
                return digest.error();
            candidate.content_sha256 = std::move(digest).value();
            auto indexed = index_size(candidate.size_bytes);
            if (!indexed)
                return indexed.error();
            auto match =
                repository_->find_import_content(candidate.size_bytes, candidate.content_sha256);
            if (!match)
                return match.error();
            if (match.value())
            {
                candidate.duplicate = true;
                candidate.duplicate_reason = "catalog_content";
                candidate.duplicate_asset_id = *match.value();
            }
            else if (batch_hashes.contains(candidate.content_sha256))
            {
                candidate.duplicate = true;
                candidate.duplicate_reason = "batch_content";
            }
            else
            {
                auto inspected = inspect_import_candidate(path, source_root, cancellation);
                if (!inspected)
                    return inspected.error();
                candidate.supported = inspected.value().supported;
                candidate.error = inspected.value().error;
                candidate.media_type = inspected.value().media_type;
                candidate.width = inspected.value().width;
                candidate.height = inspected.value().height;
                if (!candidate.supported)
                {
                    if (candidate.error)
                        return *candidate.error;
                    return make_error(ErrorCode::kUnsupported, "Import candidate is unsupported");
                }
                batch_hashes.insert(candidate.content_sha256);
            }
            return {};
        };
        auto classified = classify();
        if (!classified)
        {
            if (classified.error().code == ErrorCode::kCancelled)
                return classified.error();
            candidate.supported = false;
            candidate.error = classified.error();
            ++result.unavailable;
        }
        if (candidate.duplicate)
            ++result.duplicates;
        result.candidates.push_back(std::move(candidate));
        if (progress)
            progress(result.candidates.size(), paths.value().size(), result.candidates.back());
    }
    auto current = repository_->snapshot();
    if (!current)
        return current.error();
    if (current.value().revision != result.catalog_revision)
        return make_error(ErrorCode::kConflict, "Catalog changed during import scan; scan again",
                          {{"reason", "import_scan_stale"}});
    return result;
}
} // namespace ravo
