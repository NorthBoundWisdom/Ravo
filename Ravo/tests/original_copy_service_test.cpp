#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>
#endif

#include <QByteArray>
#include <QColorSpace>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QImage>
#include <QIODevice>
#include <gtest/gtest.h>

#include "catalog_internal.h"
#include "ravo/adapters/filesystem_preview_cache.h"
#include "ravo/adapters/qt_raster_decoder.h"
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/domain/types.h"
#include "ravo/domain/uri.h"
#include "ravo/engine/engine.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/log.h"
#include "ravo/services/catalog_service.h"

namespace ravo
{
namespace
{

constexpr std::size_t kCopyChunkBytes = 64U * 1024U;
#if defined(__APPLE__) || defined(__linux__)
constexpr std::string_view kTestXattr = "user.ravo.original_copy";
#endif

class OriginalCopyTempDirectory
{
public:
    OriginalCopyTempDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("ravo-original-copy-" + generate_catalog_id()))
    {
        std::filesystem::create_directories(path_);
    }

    ~OriginalCopyTempDirectory()
    {
        std::error_code ignored;
        std::filesystem::permissions(path_, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, ignored);
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path &path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] QByteArray patterned_bytes(const std::size_t size)
{
    QByteArray bytes(static_cast<qsizetype>(size), Qt::Uninitialized);
    std::uint32_t state = 0xA341316CU;
    for (qsizetype index = 0; index < bytes.size(); ++index)
    {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        bytes[index] = static_cast<char>(state & 0xFFU);
    }
    return bytes;
}

void write_file(const std::filesystem::path &path, const QByteArray &bytes)
{
    QFile file(QString::fromStdString(path.string()));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(file.write(bytes), bytes.size());
    file.close();
}

[[nodiscard]] QByteArray read_file(const std::filesystem::path &path)
{
    QFile file(QString::fromStdString(path.string()));
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    return file.readAll();
}

[[nodiscard]] QByteArray sha256(const std::filesystem::path &path)
{
    return QCryptographicHash::hash(read_file(path), QCryptographicHash::Sha256);
}

void write_large_png(const std::filesystem::path &path)
{
    QImage image(512, 512, QImage::Format_RGB888);
    std::uint32_t state = 0x9E3779B9U;
    for (int y = 0; y < image.height(); ++y)
    {
        auto *row = image.scanLine(y);
        for (int x = 0; x < image.width() * 3; ++x)
        {
            state = state * 1664525U + 1013904223U;
            row[x] = static_cast<std::uint8_t>(state >> 24U);
        }
    }
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    ASSERT_TRUE(image.save(QString::fromStdString(path.string()), "PNG"));
    ASSERT_GT(std::filesystem::file_size(path), 3U * kCopyChunkBytes);
}

template <typename Value>
void expect_copy_error(const Result<Value> &result, const ErrorCode code,
                       const std::string_view reason, const std::filesystem::path &source,
                       const std::filesystem::path &output)
{
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, code);
    const auto reason_entry = result.error().context.find("reason");
    ASSERT_NE(reason_entry, result.error().context.end());
    EXPECT_EQ(reason_entry->second, reason);
    const auto source_entry = result.error().context.find("source");
    ASSERT_NE(source_entry, result.error().context.end());
    EXPECT_EQ(source_entry->second, source.string());
    const auto output_entry = result.error().context.find("output");
    ASSERT_NE(output_entry, result.error().context.end());
    EXPECT_EQ(output_entry->second, output.string());
}

template <typename Value>
void expect_disk_full_error(const Result<Value> &result, const std::string_view reason,
                            const std::filesystem::path &source,
                            const std::filesystem::path &output)
{
    expect_copy_error(result, ErrorCode::kIo, reason, source, output);
    const auto disk_full_entry = result.error().context.find("disk_full");
    ASSERT_NE(disk_full_entry, result.error().context.end());
    EXPECT_EQ(disk_full_entry->second, "true");
}

enum class HookMutation : std::uint8_t
{
    kNone,
    kCancel,
    kCreateCompetitor,
    kTruncateSource,
    kCloseObservedFile,
    kMakeParentReadOnly,
    kRemoveObservedFile,
};

struct CopyHookFixture
{
    OriginalCopyCheckpoint target = OriginalCopyCheckpoint::kSourceOpened;
    std::uint64_t threshold = 0U;
    HookMutation mutation = HookMutation::kNone;
    std::error_code injected_error;
    CancellationSource *cancellation = nullptr;
    const char *cancellation_reason = nullptr;
    std::string source;
    std::string output;
    std::string parent;
    bool invoked = false;
    bool mutation_succeeded = false;
    std::array<char, 4096> temporary{};
    std::size_t temporary_size = 0U;
};

void remember_temporary(CopyHookFixture &fixture, const std::string_view path) noexcept
{
    if (path.empty() || path.size() >= fixture.temporary.size())
    {
        return;
    }
    std::memcpy(fixture.temporary.data(), path.data(), path.size());
    fixture.temporary[path.size()] = '\0';
    fixture.temporary_size = path.size();
}

[[nodiscard]] std::filesystem::path observed_temporary(const CopyHookFixture &fixture)
{
    return std::filesystem::path(
        std::string_view(fixture.temporary.data(), fixture.temporary_size));
}

#ifndef _WIN32
[[nodiscard]] bool close_descriptor_for_path(const std::string_view expected) noexcept
{
    std::array<char, 4096> actual{};
    for (int descriptor = 3; descriptor < 4096; ++descriptor)
    {
#ifdef __APPLE__
        if (::fcntl(descriptor, F_GETPATH, actual.data()) != 0)
        {
            continue;
        }
        const std::string_view path(actual.data());
#else
        std::array<char, 64> descriptor_path{};
        const int length = std::snprintf(descriptor_path.data(), descriptor_path.size(),
                                         "/proc/self/fd/%d", descriptor);
        if (length <= 0 || static_cast<std::size_t>(length) >= descriptor_path.size())
        {
            continue;
        }
        const auto count = ::readlink(descriptor_path.data(), actual.data(), actual.size() - 1U);
        if (count <= 0)
        {
            continue;
        }
        actual[static_cast<std::size_t>(count)] = '\0';
        const std::string_view path(actual.data(), static_cast<std::size_t>(count));
#endif
        if (path == expected)
        {
            return ::close(descriptor) == 0;
        }
    }
    return false;
}
#endif

[[nodiscard]] std::error_code run_copy_hook(void *const context,
                                            const OriginalCopyCheckpoint checkpoint,
                                            const std::string_view path,
                                            const std::uint64_t bytes_processed) noexcept
{
    auto &fixture = *static_cast<CopyHookFixture *>(context);
    if (checkpoint == OriginalCopyCheckpoint::kBeforeTemporaryOpen ||
        checkpoint == OriginalCopyCheckpoint::kTemporaryCreated ||
        checkpoint == OriginalCopyCheckpoint::kBeforeTemporaryWrite ||
        checkpoint == OriginalCopyCheckpoint::kTemporaryChunkWritten ||
        checkpoint == OriginalCopyCheckpoint::kBeforeTemporaryFinish ||
        checkpoint == OriginalCopyCheckpoint::kBeforePublish)
    {
        remember_temporary(fixture, path);
    }
    if (fixture.invoked || checkpoint != fixture.target || bytes_processed < fixture.threshold)
    {
        return {};
    }
    fixture.invoked = true;
    switch (fixture.mutation)
    {
    case HookMutation::kNone:
        fixture.mutation_succeeded = true;
        break;
    case HookMutation::kCancel:
        fixture.mutation_succeeded = fixture.cancellation != nullptr &&
                                     fixture.cancellation_reason != nullptr &&
                                     fixture.cancellation->cancel(fixture.cancellation_reason);
        break;
    case HookMutation::kCreateCompetitor:
    {
        std::ofstream competitor(fixture.output, std::ios::binary | std::ios::trunc);
        competitor << "competitor-wins";
        competitor.close();
        fixture.mutation_succeeded = static_cast<bool>(competitor);
        break;
    }
    case HookMutation::kTruncateSource:
    {
        std::error_code error;
        std::filesystem::resize_file(fixture.source, 1U, error);
        fixture.mutation_succeeded = !error;
        break;
    }
    case HookMutation::kCloseObservedFile:
#ifndef _WIN32
        fixture.mutation_succeeded = close_descriptor_for_path(path);
#endif
        break;
    case HookMutation::kMakeParentReadOnly:
    {
        std::error_code error;
        std::filesystem::permissions(
            fixture.parent, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
            std::filesystem::perm_options::replace, error);
        fixture.mutation_succeeded = !error;
        break;
    }
    case HookMutation::kRemoveObservedFile:
    {
        std::error_code error;
        fixture.mutation_succeeded = std::filesystem::remove(path, error) && !error;
        break;
    }
    }
    return fixture.injected_error;
}

[[nodiscard]] OriginalCopyCheckpointHook hook_for(CopyHookFixture &fixture) noexcept
{
    return {run_copy_hook, &fixture};
}

void expect_owned_temporary_removed(const CopyHookFixture &fixture)
{
    if (fixture.temporary_size != 0U)
    {
        EXPECT_FALSE(std::filesystem::exists(observed_temporary(fixture)));
    }
}

#if defined(__APPLE__) || defined(__linux__)
[[nodiscard]] int set_test_xattr(const std::filesystem::path &path, const std::string_view value)
{
#ifdef __APPLE__
    return ::setxattr(path.c_str(), kTestXattr.data(), value.data(), value.size(), 0, 0);
#else
    return ::setxattr(path.c_str(), kTestXattr.data(), value.data(), value.size(), 0);
#endif
}

[[nodiscard]] ssize_t get_test_xattr(const std::filesystem::path &path, void *const value,
                                     const std::size_t size)
{
#ifdef __APPLE__
    return ::getxattr(path.c_str(), kTestXattr.data(), value, size, 0, 0);
#else
    return ::getxattr(path.c_str(), kTestXattr.data(), value, size);
#endif
}
#endif

class OriginalCopyServiceTest : public ::testing::Test
{
protected:
    EngineFacade engine_ = []
    {
        auto created = EngineFacade::create_phase1();
        return std::move(created).value();
    }();
};

TEST_F(OriginalCopyServiceTest, CopiesMultipleChunksWithoutChangingSourceOrCopyingMetadata)
{
    OriginalCopyTempDirectory temporary;
    const auto source = temporary.path() / "source.png";
    const auto output = temporary.path() / "copy.png";
    write_large_png(source);

    const auto database = (temporary.path() / "library.sqlite").string();
    auto repository = SqliteCatalogRepository::create(database);
    ASSERT_TRUE(repository) << repository.error().message;
    auto cache = FilesystemPreviewCache::create(database + ".preview");
    ASSERT_TRUE(cache) << cache.error().message;
    CatalogService service(engine_, std::move(repository).value(),
                           std::make_unique<QtRasterDecoder>(), std::move(cache).value());
    const auto imported = service.import_one(source.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);

#if defined(__APPLE__) || defined(__linux__)
    ASSERT_EQ(set_test_xattr(source, "source-only"), 0) << std::strerror(errno);
#endif
    const auto old_time = std::filesystem::file_time_type::clock::now() - std::chrono::hours(48);
    std::error_code metadata_error;
    std::filesystem::last_write_time(source, old_time, metadata_error);
    ASSERT_FALSE(metadata_error);
#ifndef _WIN32
    std::filesystem::permissions(
        source, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace, metadata_error);
    ASSERT_FALSE(metadata_error);
#endif
    const auto bytes_before = read_file(source);
    const auto hash_before = sha256(source);
    const auto size_before = std::filesystem::file_size(source);
    const auto mtime_before = std::filesystem::last_write_time(source);
    const auto mode_before = std::filesystem::status(source).permissions();

    ExportRequest request;
    request.asset_id = imported.value().asset->id;
    request.output_path = output.string();
    request.format = ExportFormat::kOriginalCopy;
    const auto copied = service.export_asset(request);
    ASSERT_TRUE(copied) << copied.error().message;
    EXPECT_EQ(copied.value().bytes_written, size_before);
    EXPECT_EQ(read_file(output), bytes_before);
    EXPECT_EQ(sha256(source), hash_before);
    EXPECT_EQ(std::filesystem::file_size(source), size_before);
    EXPECT_EQ(std::filesystem::last_write_time(source), mtime_before);
    EXPECT_EQ(std::filesystem::status(source).permissions(), mode_before);
    EXPECT_NE(std::filesystem::last_write_time(output), mtime_before);
#ifndef _WIN32
    EXPECT_NE(std::filesystem::status(output).permissions(), mode_before);
#endif
#if defined(__APPLE__) || defined(__linux__)
    std::array<char, 32> xattr{};
    EXPECT_EQ(get_test_xattr(source, xattr.data(), xattr.size()), 11);
    errno = 0;
    EXPECT_EQ(get_test_xattr(output, xattr.data(), xattr.size()), -1);
#ifdef __APPLE__
    EXPECT_EQ(errno, ENOATTR);
#else
    EXPECT_EQ(errno, ENODATA);
#endif
#endif
    EXPECT_FALSE(std::filesystem::exists(output.string() + ".xmp"));
}

TEST_F(OriginalCopyServiceTest, CatalogServiceClassifiesOriginalSourceFailuresWithoutPublication)
{
    OriginalCopyTempDirectory temporary;
    const auto source = temporary.path() / "source.png";
    write_large_png(source);
    const QByteArray valid_png = read_file(source);

    const auto database = (temporary.path() / "library.sqlite").string();
    auto repository = SqliteCatalogRepository::create(database);
    ASSERT_TRUE(repository) << repository.error().message;
    auto cache = FilesystemPreviewCache::create(database + ".preview");
    ASSERT_TRUE(cache) << cache.error().message;
    CatalogService service(engine_, std::move(repository).value(),
                           std::make_unique<QtRasterDecoder>(), std::move(cache).value());
    const auto imported = service.import_one(source.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    ASSERT_TRUE(imported.value().asset);
    const auto normalized_source = normalize_local_input(source.string());
    ASSERT_TRUE(normalized_source) << normalized_source.error().message;
    const std::filesystem::path expected_source(normalized_source.value().path);

    const auto export_original = [&](const std::filesystem::path &output)
    {
        ExportRequest request;
        request.asset_id = imported.value().asset->id;
        request.output_path = output.string();
        request.format = ExportFormat::kOriginalCopy;
        return service.export_asset(request);
    };
    const auto expected_output = [](const std::filesystem::path &output)
    {
        auto normalized = normalize_local_input(output.string());
        EXPECT_TRUE(normalized) << normalized.error().message;
        return std::filesystem::path(normalized ? normalized.value().path : output.string());
    };

    std::error_code filesystem_error;
    ASSERT_TRUE(std::filesystem::remove(source, filesystem_error));
    ASSERT_FALSE(filesystem_error);
    const auto missing_output = temporary.path() / "missing-source.bin";
    expect_copy_error(export_original(missing_output), ErrorCode::kNotFound,
                      "original_copy_source_missing", expected_source,
                      expected_output(missing_output));
    EXPECT_FALSE(std::filesystem::exists(missing_output));
    write_file(source, valid_png);

    ASSERT_TRUE(std::filesystem::remove(source, filesystem_error));
    ASSERT_FALSE(filesystem_error);
    ASSERT_TRUE(std::filesystem::create_directory(source, filesystem_error));
    ASSERT_FALSE(filesystem_error);
    const auto directory_output = temporary.path() / "directory-source.bin";
    expect_copy_error(export_original(directory_output), ErrorCode::kIo,
                      "original_copy_source_not_regular", expected_source,
                      expected_output(directory_output));
    EXPECT_FALSE(std::filesystem::exists(directory_output));
    ASSERT_EQ(std::filesystem::remove_all(source, filesystem_error), 1U);
    ASSERT_FALSE(filesystem_error);
    write_file(source, valid_png);

#ifndef _WIN32
    ASSERT_TRUE(std::filesystem::remove(source, filesystem_error));
    ASSERT_FALSE(filesystem_error);
    ASSERT_EQ(::mkfifo(source.c_str(), 0600), 0) << std::strerror(errno);
    const auto nonregular_output = temporary.path() / "nonregular-source.bin";
    expect_copy_error(export_original(nonregular_output), ErrorCode::kIo,
                      "original_copy_source_not_regular", expected_source,
                      expected_output(nonregular_output));
    EXPECT_FALSE(std::filesystem::exists(nonregular_output));
    ASSERT_EQ(::unlink(source.c_str()), 0) << std::strerror(errno);
    write_file(source, valid_png);

    std::filesystem::permissions(source, std::filesystem::perms::none,
                                 std::filesystem::perm_options::replace, filesystem_error);
    ASSERT_FALSE(filesystem_error);
    const auto unreadable_output = temporary.path() / "unreadable-source.bin";
    const auto unreadable_result = export_original(unreadable_output);
    std::filesystem::permissions(source, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, filesystem_error);
    ASSERT_FALSE(filesystem_error);
    expect_copy_error(unreadable_result, ErrorCode::kIo, "original_copy_source_open_failed",
                      expected_source, expected_output(unreadable_output));
    EXPECT_FALSE(std::filesystem::exists(unreadable_output));
#endif
}

TEST(OriginalCopyInternalTest, UsesUniqueOwnedTemporaryAndPreservesLegacySentinel)
{
    OriginalCopyTempDirectory temporary;
    const auto source = temporary.path() / "source.bin";
    const auto output = temporary.path() / "output.bin";
    const auto legacy_sentinel = std::filesystem::path(output.string() + ".ravo-export-tmp");
    const QByteArray payload = patterned_bytes(3U * kCopyChunkBytes + 17U);
    write_file(source, payload);
    write_file(legacy_sentinel, QByteArray("do-not-delete"));
    CopyHookFixture fixture;

    const auto copied = copy_file_atomically(source.string(), output.string(), CancellationToken{},
                                             hook_for(fixture));
    ASSERT_TRUE(copied) << copied.error().message;
    EXPECT_EQ(copied.value(), static_cast<std::uint64_t>(payload.size()));
    EXPECT_EQ(read_file(output), payload);
    EXPECT_EQ(read_file(legacy_sentinel), QByteArray("do-not-delete"));
    ASSERT_NE(fixture.temporary_size, 0U);
    EXPECT_NE(observed_temporary(fixture), legacy_sentinel);
    expect_owned_temporary_removed(fixture);
}

TEST(OriginalCopyInternalTest, PrioritizesEntryAndLoopCancellationAndCleansTemporaryFiles)
{
    OriginalCopyTempDirectory temporary;
    const auto source = temporary.path() / "source.bin";
    write_file(source, patterned_bytes(5U * kCopyChunkBytes + 31U));

    {
        const auto output = temporary.path() / "entry.bin";
        write_file(output, QByteArray("winner"));
        CancellationSource cancellation;
        ASSERT_TRUE(cancellation.cancel("entry_cancel"));
        CopyHookFixture fixture;
        fixture.target = OriginalCopyCheckpoint::kSourceOpened;
        fixture.mutation = HookMutation::kNone;
        const auto result = copy_file_atomically(temporary.path().string(), output.string(),
                                                 cancellation.token(), hook_for(fixture));
        expect_copy_error(result, ErrorCode::kCancelled, "entry_cancel", temporary.path(), output);
        EXPECT_FALSE(fixture.invoked);
        EXPECT_EQ(read_file(output), QByteArray("winner"));
    }

    struct CancellationCase
    {
        OriginalCopyCheckpoint checkpoint;
        std::uint64_t threshold;
        const char *reason;
        const char *filename;
    };
    const std::array cases{
        CancellationCase{OriginalCopyCheckpoint::kSourceChunkRead, kCopyChunkBytes, "mid_read",
                         "mid-read.bin"},
        CancellationCase{OriginalCopyCheckpoint::kTemporaryChunkWritten, kCopyChunkBytes,
                         "mid_write", "mid-write.bin"},
        CancellationCase{OriginalCopyCheckpoint::kBeforePublish, 0U, "before_publish",
                         "before-publish.bin"},
    };
    for (const auto &test_case : cases)
    {
        const auto output = temporary.path() / test_case.filename;
        CancellationSource cancellation;
        CopyHookFixture fixture;
        fixture.target = test_case.checkpoint;
        fixture.threshold = test_case.threshold;
        fixture.mutation = HookMutation::kCancel;
        fixture.cancellation = &cancellation;
        fixture.cancellation_reason = test_case.reason;
        fixture.source = source.string();
        fixture.output = output.string();
        const auto result = copy_file_atomically(source.string(), output.string(),
                                                 cancellation.token(), hook_for(fixture));
        expect_copy_error(result, ErrorCode::kCancelled, test_case.reason, source, output);
        EXPECT_TRUE(fixture.invoked);
        EXPECT_TRUE(fixture.mutation_succeeded);
        EXPECT_FALSE(std::filesystem::exists(output));
        expect_owned_temporary_removed(fixture);
    }
}

TEST(OriginalCopyInternalTest, NeverOverwritesPreexistingSamePathOrLateCompetitor)
{
    OriginalCopyTempDirectory temporary;
    const auto source = temporary.path() / "source.bin";
    write_file(source, patterned_bytes(2U * kCopyChunkBytes + 3U));

    const auto preexisting = temporary.path() / "preexisting.bin";
    write_file(preexisting, QByteArray("preexisting-winner"));
    const auto existing_result =
        copy_file_atomically(source.string(), preexisting.string(), CancellationToken{}, {});
    expect_copy_error(existing_result, ErrorCode::kConflict, "original_copy_output_exists", source,
                      preexisting);
    EXPECT_EQ(read_file(preexisting), QByteArray("preexisting-winner"));

    const auto same_result =
        copy_file_atomically(source.string(), source.string(), CancellationToken{}, {});
    expect_copy_error(same_result, ErrorCode::kConflict, "original_copy_source_equals_output",
                      source, source);

    const auto late = temporary.path() / "late.bin";
    CopyHookFixture fixture;
    fixture.target = OriginalCopyCheckpoint::kBeforePublish;
    fixture.mutation = HookMutation::kCreateCompetitor;
    fixture.source = source.string();
    fixture.output = late.string();
    const auto late_result = copy_file_atomically(source.string(), late.string(),
                                                  CancellationToken{}, hook_for(fixture));
    expect_copy_error(late_result, ErrorCode::kConflict, "original_copy_output_exists", source,
                      late);
    EXPECT_TRUE(fixture.invoked);
    EXPECT_TRUE(fixture.mutation_succeeded);
    EXPECT_EQ(read_file(late), QByteArray("competitor-wins"));
    expect_owned_temporary_removed(fixture);
}

TEST(OriginalCopyInternalTest, ClassifiesMissingNonRegularOpenReadAndSourceChangeFailures)
{
    OriginalCopyTempDirectory temporary;
    const auto output = temporary.path() / "output.bin";

    const auto missing = temporary.path() / "missing.bin";
    expect_copy_error(
        copy_file_atomically(missing.string(), output.string(), CancellationToken{}, {}),
        ErrorCode::kNotFound, "original_copy_source_missing", missing, output);
    expect_copy_error(
        copy_file_atomically(temporary.path().string(), output.string(), CancellationToken{}, {}),
        ErrorCode::kIo, "original_copy_source_not_regular", temporary.path(), output);

#ifndef _WIN32
    const std::filesystem::path nonregular("/dev/null");
    expect_copy_error(
        copy_file_atomically(nonregular.string(), output.string(), CancellationToken{}, {}),
        ErrorCode::kIo, "original_copy_source_not_regular", nonregular, output);

    const auto unreadable = temporary.path() / "unreadable.bin";
    write_file(unreadable, patterned_bytes(kCopyChunkBytes));
    std::error_code permission_error;
    std::filesystem::permissions(unreadable, std::filesystem::perms::none,
                                 std::filesystem::perm_options::replace, permission_error);
    ASSERT_FALSE(permission_error);
    const auto open_failed =
        copy_file_atomically(unreadable.string(), output.string(), CancellationToken{}, {});
    std::filesystem::permissions(unreadable, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, permission_error);
    ASSERT_FALSE(permission_error);
    expect_copy_error(open_failed, ErrorCode::kIo, "original_copy_source_open_failed", unreadable,
                      output);

    const auto read_failed = temporary.path() / "read-failed.bin";
    write_file(read_failed, patterned_bytes(2U * kCopyChunkBytes));
    CopyHookFixture read_fixture;
    read_fixture.target = OriginalCopyCheckpoint::kBeforeSourceRead;
    read_fixture.mutation = HookMutation::kCloseObservedFile;
    read_fixture.source = read_failed.string();
    read_fixture.output = output.string();
    const auto read_result = copy_file_atomically(read_failed.string(), output.string(),
                                                  CancellationToken{}, hook_for(read_fixture));
    expect_copy_error(read_result, ErrorCode::kIo, "original_copy_source_read_failed", read_failed,
                      output);
    EXPECT_TRUE(read_fixture.invoked);
    EXPECT_TRUE(read_fixture.mutation_succeeded);
#endif

    const auto changed = temporary.path() / "changed.bin";
    write_file(changed, patterned_bytes(4U * kCopyChunkBytes));
    CopyHookFixture changed_fixture;
    changed_fixture.target = OriginalCopyCheckpoint::kSourceChunkRead;
    changed_fixture.threshold = kCopyChunkBytes;
    changed_fixture.mutation = HookMutation::kTruncateSource;
    changed_fixture.source = changed.string();
    changed_fixture.output = output.string();
    const auto changed_result = copy_file_atomically(
        changed.string(), output.string(), CancellationToken{}, hook_for(changed_fixture));
    expect_copy_error(changed_result, ErrorCode::kIo, "original_copy_source_changed", changed,
                      output);
    EXPECT_TRUE(changed_fixture.invoked);
    EXPECT_TRUE(changed_fixture.mutation_succeeded);
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST(OriginalCopyInternalTest, ClassifiesOutputDirectoryAndTemporaryOpenFailures)
{
    OriginalCopyTempDirectory temporary;
    const auto source = temporary.path() / "source.bin";
    write_file(source, patterned_bytes(kCopyChunkBytes + 1U));

    const auto missing_parent_output = temporary.path() / "missing" / "output.bin";
    expect_copy_error(copy_file_atomically(source.string(), missing_parent_output.string(),
                                           CancellationToken{}, {}),
                      ErrorCode::kIo, "original_copy_output_parent_missing", source,
                      missing_parent_output);

    const auto parent_file = temporary.path() / "parent-file";
    write_file(parent_file, QByteArray("not-a-directory"));
    const auto parent_file_output = parent_file / "output.bin";
    expect_copy_error(
        copy_file_atomically(source.string(), parent_file_output.string(), CancellationToken{}, {}),
        ErrorCode::kIo, "original_copy_output_parent_not_directory", source, parent_file_output);

#ifndef _WIN32
    const auto unwritable_parent = temporary.path() / "unwritable";
    std::filesystem::create_directory(unwritable_parent);
    std::error_code permission_error;
    std::filesystem::permissions(
        unwritable_parent, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace, permission_error);
    ASSERT_FALSE(permission_error);
    const auto unwritable_output = unwritable_parent / "output.bin";
    const auto unwritable_result =
        copy_file_atomically(source.string(), unwritable_output.string(), CancellationToken{}, {});
    std::filesystem::permissions(unwritable_parent, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, permission_error);
    ASSERT_FALSE(permission_error);
    expect_copy_error(unwritable_result, ErrorCode::kIo, "original_copy_output_parent_unwritable",
                      source, unwritable_output);

    const auto late_unwritable_parent = temporary.path() / "late-unwritable";
    std::filesystem::create_directory(late_unwritable_parent);
    const auto late_unwritable_output = late_unwritable_parent / "output.bin";
    CopyHookFixture fixture;
    fixture.target = OriginalCopyCheckpoint::kBeforeTemporaryOpen;
    fixture.mutation = HookMutation::kMakeParentReadOnly;
    fixture.source = source.string();
    fixture.output = late_unwritable_output.string();
    fixture.parent = late_unwritable_parent.string();
    const auto temporary_open_result = copy_file_atomically(
        source.string(), late_unwritable_output.string(), CancellationToken{}, hook_for(fixture));
    std::filesystem::permissions(late_unwritable_parent, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, permission_error);
    ASSERT_FALSE(permission_error);
    expect_copy_error(temporary_open_result, ErrorCode::kIo, "original_copy_temporary_open_failed",
                      source, late_unwritable_output);
    EXPECT_TRUE(fixture.invoked);
    EXPECT_TRUE(fixture.mutation_succeeded);
    expect_owned_temporary_removed(fixture);
#endif
}

TEST(OriginalCopyInternalTest, ClassifiesTemporaryWriteFinishAndPublishFailuresAndCleansUp)
{
#ifdef _WIN32
    GTEST_SKIP() << "Deterministic descriptor failure injection is POSIX-only";
#else
    OriginalCopyTempDirectory temporary;
    const auto source = temporary.path() / "source.bin";
    write_file(source, patterned_bytes(3U * kCopyChunkBytes + 7U));

    struct FailureCase
    {
        OriginalCopyCheckpoint checkpoint;
        HookMutation mutation;
        const char *reason;
        const char *filename;
    };
    const std::array cases{
        FailureCase{OriginalCopyCheckpoint::kBeforeTemporaryWrite, HookMutation::kCloseObservedFile,
                    "original_copy_temporary_write_failed", "write-failed.bin"},
        FailureCase{OriginalCopyCheckpoint::kBeforeTemporaryFinish,
                    HookMutation::kCloseObservedFile, "original_copy_temporary_finish_failed",
                    "finish-failed.bin"},
        FailureCase{OriginalCopyCheckpoint::kBeforePublish, HookMutation::kRemoveObservedFile,
                    "original_copy_publish_failed", "publish-failed.bin"},
    };
    for (const auto &test_case : cases)
    {
        const auto output = temporary.path() / test_case.filename;
        CopyHookFixture fixture;
        fixture.target = test_case.checkpoint;
        fixture.mutation = test_case.mutation;
        fixture.source = source.string();
        fixture.output = output.string();
        const auto result = copy_file_atomically(source.string(), output.string(),
                                                 CancellationToken{}, hook_for(fixture));
        expect_copy_error(result, ErrorCode::kIo, test_case.reason, source, output);
        EXPECT_TRUE(fixture.invoked);
        EXPECT_TRUE(fixture.mutation_succeeded);
        EXPECT_FALSE(std::filesystem::exists(output));
        expect_owned_temporary_removed(fixture);
    }
#endif
}

TEST(OriginalCopyInternalTest, MapsInjectedDiskFullAtWriteAndFinishAndCleansUp)
{
    OriginalCopyTempDirectory temporary;
    const auto source = temporary.path() / "source.bin";
    write_file(source, patterned_bytes(3U * kCopyChunkBytes + 7U));

    struct FailureCase
    {
        OriginalCopyCheckpoint checkpoint;
        const char *reason;
        const char *filename;
    };
    const std::array cases{
        FailureCase{OriginalCopyCheckpoint::kBeforeTemporaryWrite,
                    "original_copy_temporary_write_failed", "write-disk-full.bin"},
        FailureCase{OriginalCopyCheckpoint::kBeforeTemporaryFinish,
                    "original_copy_temporary_finish_failed", "finish-disk-full.bin"},
    };
    for (const auto &test_case : cases)
    {
        const auto output = temporary.path() / test_case.filename;
        CopyHookFixture fixture;
        fixture.target = test_case.checkpoint;
        fixture.injected_error = std::make_error_code(std::errc::no_space_on_device);
        fixture.source = source.string();
        fixture.output = output.string();

        const auto result = copy_file_atomically(source.string(), output.string(),
                                                 CancellationToken{}, hook_for(fixture));
        expect_disk_full_error(result, test_case.reason, source, output);
        EXPECT_TRUE(fixture.invoked);
        EXPECT_TRUE(fixture.mutation_succeeded);
        EXPECT_FALSE(std::filesystem::exists(output));
        expect_owned_temporary_removed(fixture);
    }
}

} // namespace
} // namespace ravo

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    ravo::init_logging("ravo-original-copy-service-tests");
    ::testing::InitGoogleTest(&argc, argv);
    const int result = RUN_ALL_TESTS();
    ravo::shutdown_logging();
    return result;
}
