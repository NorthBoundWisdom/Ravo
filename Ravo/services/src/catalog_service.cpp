#include "ravo/services/catalog_service.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <set>
#include <system_error>
#include <utility>

#include "ravo/domain/uri.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{
namespace
{

[[nodiscard]] std::int64_t now_unix_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] ImportItemResult failed_item(std::string path, TaskError error)
{
    ImportItemResult result;
    result.status = ImportItemStatus::kFailed;
    result.input_path = std::move(path);
    result.error = std::move(error);
    return result;
}

[[nodiscard]] ImportItemResult unsupported_item(std::string path, TaskError error)
{
    ImportItemResult result;
    result.status = ImportItemStatus::kUnsupported;
    result.input_path = std::move(path);
    result.error = std::move(error);
    return result;
}

[[nodiscard]] std::string lower_ascii(std::string value)
{
    for (char &character : value)
    {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

[[nodiscard]] std::string extension_lower(const std::filesystem::path &path)
{
    const auto extension = path.extension().generic_u8string();
    return lower_ascii({reinterpret_cast<const char *>(extension.data()), extension.size()});
}

[[nodiscard]] bool is_raw_extension(const std::filesystem::path &path)
{
    static const std::set<std::string> raw{".arw", ".cr2", ".cr3", ".crw", ".nef", ".nrw", ".dng",
                                           ".raf", ".orf", ".rw2", ".raw", ".sr2", ".srf", ".pef",
                                           ".3fr", ".mrw", ".kdc", ".dcr", ".erf"};
    return raw.contains(extension_lower(path));
}

[[nodiscard]] bool is_import_candidate(const std::filesystem::path &path)
{
    static const std::set<std::string> raster{".png",  ".jpg", ".jpeg", ".tif",
                                              ".tiff", ".bmp", ".gif",  ".webp"};
    const auto name = path.filename().generic_u8string();
    if (name.empty() || name.front() == u8'.')
    {
        return false;
    }
    return raster.contains(extension_lower(path)) || is_raw_extension(path);
}

[[nodiscard]] Result<std::vector<std::string>>
collect_import_paths(const std::vector<std::string> &inputs, const CancellationToken &cancellation)
{
    std::vector<std::string> files;
    for (const auto &input : inputs)
    {
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        auto location = normalize_local_input(input);
        if (!location)
        {
            continue;
        }
        std::error_code error;
        const std::filesystem::path path(
            std::u8string(location.value().path.begin(), location.value().path.end()));
        if (std::filesystem::is_regular_file(path, error) && !error)
        {
            files.push_back(location.value().path);
            continue;
        }
        if (!std::filesystem::is_directory(path, error) || error)
        {
            continue;
        }
        const auto options = std::filesystem::directory_options::skip_permission_denied;
        for (std::filesystem::recursive_directory_iterator iterator(path, options, error), end;
             iterator != end && !error; iterator.increment(error))
        {
            cancelled = cancellation.check();
            if (!cancelled)
            {
                return cancelled.error();
            }
            if (!iterator->is_regular_file(error) || error)
            {
                continue;
            }
            if (is_import_candidate(iterator->path()))
            {
                const auto utf8 = iterator->path().generic_u8string();
                files.emplace_back(reinterpret_cast<const char *>(utf8.data()), utf8.size());
            }
        }
        if (error)
        {
            return make_error(ErrorCode::kIo, "Unable to enumerate import directory",
                              {{"path", location.value().path}, {"detail", error.message()}});
        }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    LOG_INFO(ravo::logger(), "import enumeration collected {} files from {} inputs", files.size(),
             inputs.size());
    return files;
}

[[nodiscard]] std::string fnv1a64_hex(const std::string_view text)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char character : text)
    {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ULL;
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int index = 15; index >= 0; --index)
    {
        out[static_cast<std::size_t>(index)] = hex[hash & 0xfU];
        hash >>= 4U;
    }
    return out;
}

[[nodiscard]] Recipe identity_recipe_for(const AssetRecord &asset, const std::string &path)
{
    Recipe recipe;
    recipe.asset = {asset.id, path, asset.content_fingerprint};
    return recipe;
}

} // namespace

CatalogService::CatalogService(const EngineFacade &engine,
                               std::unique_ptr<CatalogRepository> repository,
                               std::unique_ptr<RasterDecoder> raster,
                               std::unique_ptr<PreviewCache> cache)
    : engine_(&engine)
    , repository_(std::move(repository))
    , raster_(std::move(raster))
    , cache_(std::move(cache))
{
}

CatalogService::~CatalogService()
{
    static_cast<void>(close());
}

Result<void> CatalogService::close()
{
    if (repository_ == nullptr)
    {
        return {};
    }
    const auto closed = repository_->close();
    repository_.reset();
    raster_.reset();
    cache_.reset();
    engine_ = nullptr;
    return closed;
}

Result<CatalogSnapshot> CatalogService::snapshot() const
{
    if (repository_ == nullptr || cache_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto snapshot = repository_->snapshot();
    if (!snapshot)
    {
        return snapshot.error();
    }
    snapshot.value().cache_root = cache_->root();
    return snapshot;
}

Result<std::vector<AssetRecord>> CatalogService::list_assets() const
{
    return list_assets(LibraryQuery{});
}

Result<std::vector<AssetRecord>> CatalogService::list_assets(const LibraryQuery &query) const
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto listed = repository_->list_assets();
    if (!listed)
    {
        return listed.error();
    }
    return filter_and_sort_assets(std::move(listed).value(), query);
}

Result<std::vector<FolderRecord>> CatalogService::list_folders() const
{
    auto listed = list_assets();
    if (!listed)
    {
        return listed.error();
    }
    return library_folders(listed.value());
}

Result<AssetRecord> CatalogService::set_rating(const std::string_view asset_id, const int rating)
{
    auto valid = validate_rating(rating);
    if (!valid)
    {
        return valid.error();
    }
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    ReviewState review = asset.value()->review;
    review.rating = rating;
    const auto updated = repository_->update_review(asset_id, review);
    if (!updated)
    {
        return updated.error();
    }
    const auto revision = repository_->bump_revision();
    if (!revision)
    {
        return revision.error();
    }
    asset.value()->review = review;
    return *asset.value();
}

Result<AssetRecord> CatalogService::set_color_label(const std::string_view asset_id,
                                                    const ColorLabel label)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    ReviewState review = asset.value()->review;
    review.color_label = label;
    const auto updated = repository_->update_review(asset_id, review);
    if (!updated)
    {
        return updated.error();
    }
    const auto revision = repository_->bump_revision();
    if (!revision)
    {
        return revision.error();
    }
    asset.value()->review = review;
    return *asset.value();
}

Result<AssetRecord> CatalogService::set_rejected(const std::string_view asset_id,
                                                 const bool rejected)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    ReviewState review = asset.value()->review;
    review.rejected = rejected;
    const auto updated = repository_->update_review(asset_id, review);
    if (!updated)
    {
        return updated.error();
    }
    const auto revision = repository_->bump_revision();
    if (!revision)
    {
        return revision.error();
    }
    asset.value()->review = review;
    return *asset.value();
}

Result<void> CatalogService::remove_from_catalog(const std::string_view asset_id)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    const auto removed = repository_->remove_asset(asset_id);
    if (!removed)
    {
        return removed.error();
    }
    const auto revision = repository_->bump_revision();
    if (!revision)
    {
        return revision.error();
    }
    if (cache_ != nullptr)
    {
        const auto removed_cache = cache_->remove_for_asset(asset_id);
        if (!removed_cache)
        {
            return removed_cache.error();
        }
    }
    return {};
}

Result<void> CatalogService::remove_original_and_catalog(const std::string_view asset_id)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    auto location = normalize_local_input(asset.value()->normalized_uri);
    if (!location)
    {
        return location.error();
    }
    const auto path = std::filesystem::path(
        std::u8string(location.value().path.begin(), location.value().path.end()));
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(path, exists_error);
    if (exists_error)
    {
        return make_error(ErrorCode::kIo, "Unable to inspect original file",
                          {{"path", location.value().path},
                           {"asset_id", std::string(asset_id)},
                           {"detail", exists_error.message()}});
    }
    if (!exists)
    {
        return make_error(ErrorCode::kNotFound, "Original file is missing",
                          {{"path", location.value().path}, {"asset_id", std::string(asset_id)}});
    }
    std::error_code remove_error;
    if (!std::filesystem::remove(path, remove_error) || remove_error)
    {
        return make_error(ErrorCode::kIo, "Unable to delete original file",
                          {{"path", location.value().path},
                           {"asset_id", std::string(asset_id)},
                           {"detail", remove_error.message()}});
    }
    return remove_from_catalog(asset_id);
}

Result<Recipe> CatalogService::load_recipe(const std::string_view asset_id) const
{
    if (repository_ == nullptr || engine_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    auto location = normalize_local_input(asset.value()->normalized_uri);
    if (!location)
    {
        return location.error();
    }
    auto stored = repository_->load_recipe_json(asset_id);
    if (!stored)
    {
        return stored.error();
    }
    if (!stored.value())
    {
        return identity_recipe_for(*asset.value(), location.value().path);
    }
    auto parsed = parse_recipe_json(*stored.value());
    if (!parsed)
    {
        return parsed.error();
    }
    parsed.value().asset = {asset.value()->id, location.value().path,
                            asset.value()->content_fingerprint};
    auto valid = engine_->validate(parsed.value());
    if (!valid)
    {
        return valid.error();
    }
    return parsed;
}

Result<AssetRecord> CatalogService::save_recipe(const std::string_view asset_id,
                                                const Recipe &recipe)
{
    if (repository_ == nullptr || engine_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    auto location = normalize_local_input(asset.value()->normalized_uri);
    if (!location)
    {
        return location.error();
    }
    Recipe stored = recipe;
    stored.asset = {asset.value()->id, location.value().path, asset.value()->content_fingerprint};
    auto valid = engine_->validate(stored);
    if (!valid)
    {
        return valid.error();
    }
    auto params = develop_from_recipe(stored);
    if (!params)
    {
        return params.error();
    }
    if (params.value().is_identity())
    {
        const auto cleared = repository_->clear_recipe(asset_id);
        if (!cleared)
        {
            return cleared.error();
        }
        asset.value()->has_edits = false;
    }
    else
    {
        auto json = serialize_recipe(stored);
        if (!json)
        {
            return json.error();
        }
        const auto saved =
            repository_->save_recipe_json(asset_id, stored.schema_version, json.value());
        if (!saved)
        {
            return saved.error();
        }
        asset.value()->has_edits = true;
    }
    const auto revision = repository_->bump_revision();
    if (!revision)
    {
        return revision.error();
    }
    return *asset.value();
}

Result<AssetRecord> CatalogService::save_develop(const std::string_view asset_id,
                                                 const DevelopParams &params)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    auto location = normalize_local_input(asset.value()->normalized_uri);
    if (!location)
    {
        return location.error();
    }
    auto recipe = recipe_from_develop(
        {asset.value()->id, location.value().path, asset.value()->content_fingerprint}, params);
    if (!recipe)
    {
        return recipe.error();
    }
    return save_recipe(asset_id, recipe.value());
}

Result<AssetRecord> CatalogService::reset_recipe(const std::string_view asset_id)
{
    return save_develop(asset_id, DevelopParams{});
}

Result<ImportItemResult> CatalogService::import_one(const std::string_view path,
                                                    const CancellationToken &cancellation)
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return failed_item(std::string(path), cancelled.error());
    }
    if (repository_ == nullptr || raster_ == nullptr || engine_ == nullptr || cache_ == nullptr)
    {
        return failed_item(std::string(path),
                           make_error(ErrorCode::kIo, "Catalog session is closed"));
    }

    LOG_INFO(ravo::logger(), "import one path={}", path);
    auto location = normalize_local_input(path);
    if (!location)
    {
        LOG_ERROR(ravo::logger(), "import path normalize failed path={} error={}", path,
                  location.error().message);
        return failed_item(std::string(path), location.error());
    }
    LOG_DEBUG(ravo::logger(), "import normalized path={} uri={}", location.value().path,
              location.value().uri);

    std::error_code exists_error;
    if (!std::filesystem::is_regular_file(
            std::filesystem::path(
                std::u8string(location.value().path.begin(), location.value().path.end())),
            exists_error) ||
        exists_error)
    {
        return failed_item(location.value().path,
                           make_error(ErrorCode::kNotFound, "Import input does not exist",
                                      {{"path", location.value().path}}));
    }

    auto existing = repository_->find_asset_by_uri(location.value().uri);
    if (!existing)
    {
        return failed_item(location.value().path, existing.error());
    }
    if (existing.value())
    {
        ImportItemResult duplicate;
        duplicate.status = ImportItemStatus::kDuplicate;
        duplicate.input_path = location.value().path;
        duplicate.asset = *existing.value();
        return duplicate;
    }

    auto identity = read_file_identity(location.value().path);
    if (!identity)
    {
        return failed_item(location.value().path, identity.error());
    }

    AssetRecord asset;
    asset.id = generate_asset_id();
    asset.normalized_uri = location.value().uri;
    asset.size_bytes = identity.value().size_bytes;
    asset.mtime_unix_ms = identity.value().mtime_unix_ms;
    asset.content_fingerprint = make_content_fingerprint(identity.value());
    asset.created_unix_ms = now_unix_ms();
    asset.import_state = std::string(kImportStateImported);

    const std::filesystem::path file_path(
        std::u8string(location.value().path.begin(), location.value().path.end()));
    Result<RasterInfo> raster = make_error(ErrorCode::kUnsupported, "Not probed as raster");
    if (!is_raw_extension(file_path))
    {
        raster = raster_->probe(location.value().path);
    }
    if (raster)
    {
        asset.media_type = raster.value().media_type;
        asset.width = raster.value().width;
        asset.height = raster.value().height;
    }
    else if (raster.error().code == ErrorCode::kUnsupported)
    {
        const auto inspected = engine_->inspect(location.value().path, cancellation);
        if (!inspected)
        {
            if (inspected.error().code == ErrorCode::kUnsupported)
            {
                return unsupported_item(location.value().path, inspected.error());
            }
            if (inspected.error().code == ErrorCode::kCancelled)
            {
                return failed_item(location.value().path, inspected.error());
            }
            if (inspected.error().code == ErrorCode::kValidation)
            {
                return unsupported_item(location.value().path, inspected.error());
            }
            return failed_item(location.value().path, inspected.error());
        }
        if (!inspected.value().is_raw)
        {
            return unsupported_item(location.value().path,
                                    make_error(ErrorCode::kUnsupported,
                                               "Input is not a supported RAW file",
                                               {{"path", location.value().path}}));
        }
        asset.media_type = std::string(kMediaTypeRaw);
        asset.width = inspected.value().width;
        asset.height = inspected.value().height;
    }
    else
    {
        return failed_item(location.value().path, raster.error());
    }

    const auto inserted = repository_->insert_asset(asset);
    if (!inserted)
    {
        return failed_item(location.value().path, inserted.error());
    }
    const auto revision = repository_->bump_revision();
    if (!revision)
    {
        return failed_item(location.value().path, revision.error());
    }

    auto preview = generate_preview(asset, kDefaultPreviewMaxEdge, cancellation, 0);
    if (!preview)
    {
        LOG_ERROR(ravo::logger(), "preview failed asset={} path={} error={}", asset.id,
                  location.value().path, preview.error().message);
        PreviewRecord failed;
        failed.asset_id = asset.id;
        failed.state = std::string(kPreviewStateFailed);
        failed.cache_key =
            make_preview_cache_key(asset.id, asset.width.value_or(0), asset.height.value_or(0),
                                   asset.content_fingerprint.value_or("none"));
        static_cast<void>(repository_->upsert_preview(failed));
    }
    else
    {
        LOG_INFO(ravo::logger(), "preview ready asset={} cache={}", asset.id,
                 preview.value().cache_path);
    }

    ImportItemResult result;
    result.status = ImportItemStatus::kImported;
    result.input_path = location.value().path;
    result.asset = asset;
    return result;
}

Result<std::vector<ImportItemResult>>
CatalogService::import_inputs(const std::vector<std::string> &paths,
                              const CancellationToken &cancellation,
                              const std::function<void(std::size_t, std::size_t)> &progress)
{
    auto files = collect_import_paths(paths, cancellation);
    if (!files)
    {
        return files.error();
    }
    std::vector<ImportItemResult> results;
    results.reserve(files.value().size());
    if (files.value().empty())
    {
        if (progress)
        {
            progress(0, 0);
        }
        return results;
    }
    for (std::size_t index = 0; index < files.value().size(); ++index)
    {
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            ImportItemResult stopped;
            stopped.status = ImportItemStatus::kFailed;
            stopped.input_path = files.value()[index];
            stopped.error = cancelled.error();
            results.push_back(std::move(stopped));
            break;
        }
        auto item = import_one(files.value()[index], cancellation);
        if (!item)
        {
            results.push_back(failed_item(files.value()[index], item.error()));
        }
        else
        {
            results.push_back(std::move(item).value());
        }
        if (progress)
        {
            progress(index + 1U, files.value().size());
        }
    }
    return results;
}

Result<PreviewResult> CatalogService::request_preview(const PreviewRequest &request)
{
    auto cancelled = request.cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(request.asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", request.asset_id}});
    }
    return generate_preview(*asset.value(), request.max_edge, request.cancellation,
                            request.request_revision, request.ignore_edits, request.ignore_crop);
}

Result<PreviewResult> CatalogService::generate_preview(const AssetRecord &asset,
                                                       const std::uint32_t max_edge,
                                                       const CancellationToken &cancellation,
                                                       const std::uint64_t request_revision,
                                                       const bool ignore_edits,
                                                       const bool ignore_crop)
{
    if (engine_ == nullptr || raster_ == nullptr || cache_ == nullptr || repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }

    auto location = normalize_local_input(asset.normalized_uri);
    if (!location)
    {
        LOG_ERROR(ravo::logger(), "preview normalize failed asset={} uri={} error={}", asset.id,
                  asset.normalized_uri, location.error().message);
        return location.error();
    }
    std::error_code exists_error;
    const bool original_exists =
        std::filesystem::is_regular_file(
            std::filesystem::path(
                std::u8string(location.value().path.begin(), location.value().path.end())),
            exists_error) &&
        !exists_error;
    AssetRecord working = asset;
    if (!original_exists)
    {
        if (working.import_state != kImportStateMissing)
        {
            LOG_INFO(ravo::logger(), "original file missing asset={} path={}", asset.id,
                     location.value().path);
            working.import_state = std::string(kImportStateMissing);
            working.error_code = std::string(error_code_name(ErrorCode::kNotFound));
            working.error_message = "Original file is missing";
            static_cast<void>(repository_->update_asset(working));
        }
    }
    else if (working.import_state == kImportStateMissing)
    {
        working.import_state = std::string(kImportStateImported);
        working.error_code.reset();
        working.error_message.reset();
        static_cast<void>(repository_->update_asset(working));
    }

    const auto source_width = asset.width.value_or(0);
    const auto source_height = asset.height.value_or(0);
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    fit_within_max_edge(source_width, source_height, max_edge, width, height);
    const auto fingerprint = asset.content_fingerprint.value_or("none");
    std::string edit_digest = "identity";
    Recipe edit_recipe = identity_recipe_for(working, location.value().path);
    if (!ignore_edits)
    {
        auto stored = repository_->load_recipe_json(asset.id);
        if (!stored)
        {
            return stored.error();
        }
        if (stored.value())
        {
            auto parsed = parse_recipe_json(*stored.value());
            if (!parsed)
            {
                return parsed.error();
            }
            parsed.value().asset = edit_recipe.asset;
            auto valid = engine_->validate(parsed.value());
            if (!valid)
            {
                return valid.error();
            }
            edit_recipe = std::move(parsed).value();
            edit_digest = fnv1a64_hex(*stored.value());
            if (ignore_crop)
            {
                strip_crop_operations(edit_recipe);
                edit_digest += "_nocrop";
            }
        }
    }
    const auto cache_key =
        make_preview_cache_key(asset.id, width, height, fingerprint, edit_digest);

    auto existing = cache_->existing_png(cache_key);
    if (!existing)
    {
        return existing.error();
    }

    PreviewResult result;
    result.asset_id = asset.id;
    result.request_revision = request_revision;
    result.cache_key = cache_key;
    result.original_missing = !original_exists;

    if (existing.value())
    {
        result.cache_path = *existing.value();
        result.width = width;
        result.height = height;
        PreviewRecord record;
        record.asset_id = asset.id;
        record.cache_key = cache_key;
        record.width = width;
        record.height = height;
        record.state = std::string(kPreviewStateReady);
        record.cache_relpath = cache_->relative_png_path(cache_key);
        record.last_success_unix_ms = now_unix_ms();
        static_cast<void>(repository_->upsert_preview(record));
        return result;
    }

    if (!original_exists)
    {
        return make_error(ErrorCode::kNotFound, "Original file is missing",
                          {{"path", location.value().path}, {"asset_id", asset.id}});
    }

    const bool apply_edits = edit_digest != "identity";
    if (is_raster_media_type(asset.media_type))
    {
        auto decoded = raster_->decode(location.value().path, max_edge, cancellation);
        if (!decoded)
        {
            return decoded.error();
        }
        if (!apply_edits)
        {
            auto committed = cache_->commit_png_bytes(cache_key, decoded.value().bytes);
            if (!committed)
            {
                return committed.error();
            }
            result.cache_path = committed.value();
            result.width = decoded.value().width;
            result.height = decoded.value().height;
        }
        else
        {
            auto raster = engine_->decode_png(decoded.value().bytes);
            if (!raster)
            {
                return raster.error();
            }
            RenderRequest render;
            render.asset = edit_recipe.asset;
            render.recipe = edit_recipe;
            render.cancellation = cancellation;
            render.correlation_id = asset.id;
            auto rendered = engine_->render_to_image(render, &raster.value());
            if (!rendered)
            {
                return rendered.error();
            }
            auto encoded = engine_->encode_png(rendered.value());
            if (!encoded)
            {
                return encoded.error();
            }
            auto committed = cache_->commit_png_bytes(cache_key, encoded.value());
            if (!committed)
            {
                return committed.error();
            }
            result.cache_path = committed.value();
            result.width = rendered.value().width;
            result.height = rendered.value().height;
        }
    }
    else if (is_raw_media_type(asset.media_type))
    {
        if (!apply_edits)
        {
            auto embedded =
                engine_->extract_embedded_preview(location.value().path, max_edge, cancellation);
            if (embedded)
            {
                auto decoded =
                    raster_->decode_memory(embedded.value().bytes, max_edge, cancellation);
                if (!decoded)
                {
                    return decoded.error();
                }
                auto committed = cache_->commit_png_bytes(cache_key, decoded.value().bytes);
                if (!committed)
                {
                    return committed.error();
                }
                result.cache_path = committed.value();
                result.width = decoded.value().width;
                result.height = decoded.value().height;
            }
            else
            {
                RenderRequest render;
                render.asset = edit_recipe.asset;
                render.recipe = edit_recipe;
                render.output_uri = cache_->absolute_png_path(cache_key);
                render.output_width = width;
                render.output_height = height;
                render.cancellation = cancellation;
                render.correlation_id = asset.id;
                const auto rendered = engine_->render(render);
                if (!rendered)
                {
                    return rendered.error();
                }
                result.cache_path = render.output_uri;
                result.width = rendered.value().width;
                result.height = rendered.value().height;
            }
        }
        else
        {
            RenderRequest render;
            render.asset = edit_recipe.asset;
            render.recipe = edit_recipe;
            render.output_width = width;
            render.output_height = height;
            render.cancellation = cancellation;
            render.correlation_id = asset.id;
            auto rendered = engine_->render_to_image(render, nullptr);
            if (!rendered)
            {
                return rendered.error();
            }
            auto encoded = engine_->encode_png(rendered.value());
            if (!encoded)
            {
                return encoded.error();
            }
            auto committed = cache_->commit_png_bytes(cache_key, encoded.value());
            if (!committed)
            {
                return committed.error();
            }
            result.cache_path = committed.value();
            result.width = rendered.value().width;
            result.height = rendered.value().height;
        }
    }
    else
    {
        return make_error(ErrorCode::kUnsupported, "Asset media type cannot be previewed",
                          {{"media_type", asset.media_type}, {"asset_id", asset.id}});
    }

    PreviewRecord record;
    record.asset_id = asset.id;
    record.cache_key = cache_key;
    record.width = result.width;
    record.height = result.height;
    record.state = std::string(kPreviewStateReady);
    record.cache_relpath = cache_->relative_png_path(cache_key);
    record.last_success_unix_ms = now_unix_ms();
    const auto stored = repository_->upsert_preview(record);
    if (!stored)
    {
        return stored.error();
    }
    return result;
}

} // namespace ravo
