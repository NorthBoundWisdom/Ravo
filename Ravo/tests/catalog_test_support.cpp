#include "catalog_test_support.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QFile>
#include <QIODevice>

#include "ravo/foundation/json.h"

#include <system_error>
#include <utility>

#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/filesystem_recovery_store.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/domain/types.h"

namespace ravo
{

std::filesystem::path make_catalog_test_temp_root()
{
    const auto root =
        std::filesystem::temp_directory_path() / ("ravo-catalog-" + generate_catalog_id());
    std::filesystem::create_directories(root);
    return root;
}

CatalogServiceTest::CatalogServiceTest()
    : engine(
          []
          {
              auto created = EngineFacade::create_phase1();
              return std::move(created).value();
          }())
{
}

CatalogServiceTest::~CatalogServiceTest() = default;

void CatalogServiceTest::SetUp()
{
    auto created = EngineFacade::create_phase1();
    ASSERT_TRUE(created) << created.error().message;
    engine = std::move(created).value();
    root = make_catalog_test_temp_root();
    database_path = (root / "library.sqlite").string();
}

void CatalogServiceTest::TearDown()
{
    service.reset();
    sqlite_repository = nullptr;
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

Result<void> CatalogServiceTest::open_service(const bool create, const bool resume_recovery)
{
    auto repository = create ? SqliteCatalogRepository::create(database_path) :
                               SqliteCatalogRepository::open(database_path);
    if (!repository)
    {
        return repository.error();
    }
    auto cache = FilesystemPreviewCache::create(database_path + ".preview");
    if (!cache)
    {
        return cache.error();
    }
    auto recovery = FilesystemRecoveryStore::create_for_catalog(database_path);
    if (!recovery)
    {
        return recovery.error();
    }
    auto owned_repository = std::move(repository).value();
    sqlite_repository = owned_repository.get();
    service = std::make_unique<CatalogService>(
        engine, std::move(owned_repository), std::make_unique<QtRasterDecoder>(),
        std::move(cache).value(), std::move(recovery).value());
    if (resume_recovery)
    {
        auto resumed = service->sync_recovery(std::nullopt);
        if (!resumed)
        {
            return resumed.error();
        }
    }
    return {};
}

[[nodiscard]] std::string repository_path(const std::filesystem::path &relative)
{
    const auto path = std::filesystem::path(RAVO_REPOSITORY_ROOT) / relative;
    const auto utf8 = path.generic_u8string();
    return {utf8.begin(), utf8.end()};
}

[[nodiscard]] std::string png_fixture_path()
{
    return repository_path(std::filesystem::path("Ravo") / "tests" / "fixtures" / "frozen" /
                           "0000-nop" / "expected.png");
}

[[nodiscard]] std::string raw_fixture_path()
{
    return repository_path(std::filesystem::path("Ravo") / "tests" / "fixtures" / "frozen" /
                           "images" / "mire1.cr2");
}

[[nodiscard]] std::string xtrans_fixture_path()
{
    return repository_path(std::filesystem::path("Ravo") / "tests" / "fixtures" / "frozen" /
                           "images" / "mire1-xtrans.raf");
}

[[nodiscard]] QByteArray file_sha256(const std::string &path)
{
    QFile file(QString::fromStdString(path));
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&file);
    return hash.result();
}

[[nodiscard]] std::string sha256_text(const std::string_view text)
{
    return QCryptographicHash::hash(
               QByteArrayView(text.data(), static_cast<qsizetype>(text.size())),
               QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}

[[nodiscard]] std::string recovery_document_with_mutated_payload(
    const std::filesystem::path &source,
    const std::function<void(JsonValue::Object &)> &mutate_payload)
{
    QFile input(QString::fromStdString(source.string()));
    EXPECT_TRUE(input.open(QIODevice::ReadOnly));
    const auto bytes = input.readAll();
    auto parsed =
        parse_json(std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
    EXPECT_TRUE(parsed) << parsed.error().message;
    if (!parsed || parsed.value().object_if() == nullptr)
        return {};
    auto root = *parsed.value().object_if();
    const auto payload_value = root.find("payload");
    EXPECT_NE(payload_value, root.end());
    if (payload_value == root.end() || payload_value->second.object_if() == nullptr)
        return {};
    auto payload = *payload_value->second.object_if();
    mutate_payload(payload);
    const auto canonical_payload = serialize_json(JsonValue{payload});
    root.insert_or_assign("checksum", JsonValue::Object{{"algorithm", "sha256"},
                                                        {"value", sha256_text(canonical_payload)}});
    root.insert_or_assign("payload", JsonValue{std::move(payload)});
    return serialize_json(JsonValue{std::move(root)});
}

[[nodiscard]] std::error_code
recovery_publication_hook(void *context, const recovery_publication_internal::Checkpoint checkpoint,
                          const std::string_view path, const std::uint64_t bytes_processed) noexcept
{
    static_cast<void>(bytes_processed);
    auto &state = *static_cast<RecoveryPublicationHookState *>(context);
    state.observed_paths.emplace_back(path);
    if (checkpoint != state.target)
        return {};
    if (state.cancellation != nullptr)
        static_cast<void>(state.cancellation->cancel("recovery-publication-test"));
    if (state.probe_temporary_rename)
    {
        const auto source = std::filesystem::path(path);
        auto probe = source;
        probe += ".ownership-probe";
        std::error_code error;
        std::filesystem::rename(source, probe, error);
        if (error)
            return error;
        std::filesystem::rename(probe, source, error);
        if (error)
        {
            std::error_code ignored;
            std::filesystem::remove(probe, ignored);
            return error;
        }
    }
    if (!state.competitor_output.empty())
    {
        QFile competitor(QString::fromStdString(state.competitor_output));
        if (!competitor.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
            competitor.write("winner", 6) != 6)
            return std::make_error_code(std::errc::io_error);
        competitor.close();
    }
    return state.injected_error;
}

} // namespace ravo
