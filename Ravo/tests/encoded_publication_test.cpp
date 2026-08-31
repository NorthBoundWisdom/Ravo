#include <array>
#include <barrier>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include "catalog_internal.h"
#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"

namespace ravo
{
namespace
{

constexpr std::size_t kPublicationChunkBytes = 64U * 1024U;

class PublicationTempDirectory
{
public:
    PublicationTempDirectory()
        : path_(std::filesystem::temp_directory_path() /
                ("ravo-encoded-publication-" + generate_catalog_id()))
    {
        std::filesystem::create_directories(path_);
    }

    ~PublicationTempDirectory()
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

[[nodiscard]] std::vector<std::uint8_t> patterned_bytes(const std::size_t size,
                                                        std::uint32_t state = 0xA341316CU)
{
    std::vector<std::uint8_t> bytes(size);
    for (auto &byte : bytes)
    {
        state ^= state << 13U;
        state ^= state >> 17U;
        state ^= state << 5U;
        byte = static_cast<std::uint8_t>(state & 0xFFU);
    }
    return bytes;
}

void write_file(const std::filesystem::path &path, const std::string_view bytes)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    ASSERT_TRUE(output);
}

[[nodiscard]] std::vector<std::uint8_t> read_file(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    EXPECT_TRUE(input);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void expect_publication_error(const Result<void> &result, const ErrorCode code,
                              const std::string_view reason, const std::filesystem::path &output)
{
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, code);
    const auto reason_entry = result.error().context.find("reason");
    ASSERT_NE(reason_entry, result.error().context.end());
    EXPECT_EQ(reason_entry->second, reason);
    const auto output_entry = result.error().context.find("output");
    ASSERT_NE(output_entry, result.error().context.end());
    EXPECT_EQ(output_entry->second, output.string());
    const auto path_entry = result.error().context.find("path");
    ASSERT_NE(path_entry, result.error().context.end());
    EXPECT_EQ(path_entry->second, output.string());
}

void expect_disk_full_error(const Result<void> &result, const std::string_view reason,
                            const std::filesystem::path &output)
{
    expect_publication_error(result, ErrorCode::kIo, reason, output);
    const auto disk_full_entry = result.error().context.find("disk_full");
    ASSERT_NE(disk_full_entry, result.error().context.end());
    EXPECT_EQ(disk_full_entry->second, "true");
}

enum class HookMutation : std::uint8_t
{
    kNone,
    kCancel,
    kCreateCompetitor,
    kMakeParentReadOnly,
    kReplaceTemporaryWithForeignDirectory,
};

struct PublicationHookFixture
{
    EncodedPublicationCheckpoint target = EncodedPublicationCheckpoint::kTemporaryCreated;
    std::uint64_t threshold = 0U;
    HookMutation mutation = HookMutation::kNone;
    std::error_code injected_error;
    CancellationSource *cancellation = nullptr;
    const char *cancellation_reason = nullptr;
    std::string output;
    std::string parent;
    bool invoked = false;
    bool mutation_succeeded = false;
    std::array<char, 4096> temporary{};
    std::size_t temporary_size = 0U;
};

void remember_temporary(PublicationHookFixture &fixture, const std::string_view path) noexcept
{
    if (path.empty() || path.size() >= fixture.temporary.size())
    {
        return;
    }
    std::memcpy(fixture.temporary.data(), path.data(), path.size());
    fixture.temporary[path.size()] = '\0';
    fixture.temporary_size = path.size();
}

[[nodiscard]] std::filesystem::path observed_temporary(const PublicationHookFixture &fixture)
{
    return std::filesystem::path(
        std::string_view(fixture.temporary.data(), fixture.temporary_size));
}

[[nodiscard]] std::error_code run_publication_hook(void *const context,
                                                   const EncodedPublicationCheckpoint checkpoint,
                                                   const std::string_view path,
                                                   const std::uint64_t bytes_processed) noexcept
{
    auto &fixture = *static_cast<PublicationHookFixture *>(context);
    remember_temporary(fixture, path);
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
    case HookMutation::kMakeParentReadOnly:
    {
        std::error_code error;
        std::filesystem::permissions(
            fixture.parent, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
            std::filesystem::perm_options::replace, error);
        fixture.mutation_succeeded = !error;
        break;
    }
    case HookMutation::kReplaceTemporaryWithForeignDirectory:
    {
        std::error_code error;
        const auto temporary = std::filesystem::path(path);
        const bool removed = std::filesystem::remove(temporary, error) && !error;
        error.clear();
        const bool created =
            removed && std::filesystem::create_directory(temporary, error) && !error;
        if (created)
        {
            std::ofstream marker(temporary / "foreign", std::ios::binary | std::ios::trunc);
            marker << "keep";
            marker.close();
            fixture.mutation_succeeded = static_cast<bool>(marker);
        }
        break;
    }
    }
    return fixture.injected_error;
}

[[nodiscard]] EncodedPublicationCheckpointHook hook_for(PublicationHookFixture &fixture) noexcept
{
    return {run_publication_hook, &fixture};
}

void expect_owned_temporary_removed(const PublicationHookFixture &fixture)
{
    if (fixture.temporary_size != 0U)
    {
        EXPECT_FALSE(std::filesystem::exists(observed_temporary(fixture)));
    }
}

TEST(EncodedPublicationTest, PublishesLargeAndEmptyPayloadsAndPreservesLegacySentinel)
{
    PublicationTempDirectory temporary;
    const auto output = temporary.path() / "large.bin";
    const auto sentinel = std::filesystem::path(output.string() + ".ravo-export-tmp");
    write_file(sentinel, "do-not-delete");
    const auto payload = patterned_bytes(3U * kPublicationChunkBytes + 17U);
    const auto before = payload;
    PublicationHookFixture fixture;

    const auto published =
        write_bytes_atomically(output.string(), payload, CancellationToken{}, hook_for(fixture));
    ASSERT_TRUE(published) << published.error().message;
    EXPECT_EQ(read_file(output), payload);
    EXPECT_EQ(payload, before);
    EXPECT_EQ(read_file(sentinel), std::vector<std::uint8_t>({'d', 'o', '-', 'n', 'o', 't', '-',
                                                              'd', 'e', 'l', 'e', 't', 'e'}));
    ASSERT_NE(fixture.temporary_size, 0U);
    EXPECT_NE(observed_temporary(fixture), sentinel);
    expect_owned_temporary_removed(fixture);

    const auto empty_output = temporary.path() / "empty.bin";
    const std::vector<std::uint8_t> empty;
    ASSERT_TRUE(write_bytes_atomically(empty_output.string(), empty, CancellationToken{}));
    EXPECT_TRUE(std::filesystem::is_regular_file(empty_output));
    EXPECT_EQ(std::filesystem::file_size(empty_output), 0U);
}

TEST(EncodedPublicationTest, KeepsTemporaryLeafBoundedForLongDestinations)
{
    PublicationTempDirectory temporary;
    const auto output = temporary.path() / (std::string(120U, 'x') + ".bin");
    const auto payload = patterned_bytes(kPublicationChunkBytes + 17U);
    PublicationHookFixture fixture;

    const auto published =
        write_bytes_atomically(output.string(), payload, CancellationToken{}, hook_for(fixture));

    ASSERT_TRUE(published) << published.error().message;
    EXPECT_EQ(read_file(output), payload);
    ASSERT_NE(fixture.temporary_size, 0U);
    const auto observed = observed_temporary(fixture);
    std::error_code path_error;
    EXPECT_TRUE(
        std::filesystem::equivalent(observed.parent_path(), output.parent_path(), path_error));
    EXPECT_FALSE(path_error);
    EXPECT_LE(observed.filename().string().size(), 96U);
    EXPECT_EQ(observed.filename().string().find(output.filename().string()), std::string::npos);
    expect_owned_temporary_removed(fixture);
}

TEST(EncodedPublicationTest, PrioritizesEntryLoopSyncAndPrepublishCancellation)
{
    PublicationTempDirectory temporary;
    const auto payload = patterned_bytes(4U * kPublicationChunkBytes + 31U);

    {
        const auto output = temporary.path() / "entry.bin";
        write_file(output, "winner");
        CancellationSource cancellation;
        ASSERT_TRUE(cancellation.cancel("entry_cancel"));
        PublicationHookFixture fixture;
        const auto result = write_bytes_atomically(output.string(), payload, cancellation.token(),
                                                   hook_for(fixture));
        expect_publication_error(result, ErrorCode::kCancelled, "entry_cancel", output);
        EXPECT_FALSE(fixture.invoked);
        EXPECT_EQ(read_file(output), std::vector<std::uint8_t>({'w', 'i', 'n', 'n', 'e', 'r'}));
    }

    struct CancellationCase
    {
        EncodedPublicationCheckpoint checkpoint;
        std::uint64_t threshold;
        const char *reason;
        const char *filename;
    };
    const std::array cases{
        CancellationCase{EncodedPublicationCheckpoint::kTemporaryChunkWritten,
                         kPublicationChunkBytes, "mid_write", "mid-write.bin"},
        CancellationCase{EncodedPublicationCheckpoint::kBeforeTemporarySync, 0U, "before_sync",
                         "before-sync.bin"},
        CancellationCase{EncodedPublicationCheckpoint::kBeforeTemporaryClose, 0U, "before_close",
                         "before-close.bin"},
        CancellationCase{EncodedPublicationCheckpoint::kBeforePublish, 0U, "before_publish",
                         "before-publish.bin"},
    };
    for (const auto &test_case : cases)
    {
        const auto output = temporary.path() / test_case.filename;
        CancellationSource cancellation;
        PublicationHookFixture fixture;
        fixture.target = test_case.checkpoint;
        fixture.threshold = test_case.threshold;
        fixture.mutation = HookMutation::kCancel;
        fixture.cancellation = &cancellation;
        fixture.cancellation_reason = test_case.reason;
        const auto result = write_bytes_atomically(output.string(), payload, cancellation.token(),
                                                   hook_for(fixture));
        expect_publication_error(result, ErrorCode::kCancelled, test_case.reason, output);
        EXPECT_TRUE(fixture.invoked);
        EXPECT_TRUE(fixture.mutation_succeeded);
        EXPECT_FALSE(std::filesystem::exists(output));
        expect_owned_temporary_removed(fixture);
    }
}

TEST(EncodedPublicationTest, RejectsEveryPreexistingOutputKindWithoutClobbering)
{
    PublicationTempDirectory temporary;
    const auto payload = patterned_bytes(19U);

    const auto regular = temporary.path() / "regular.bin";
    write_file(regular, "winner");
    const auto regular_result =
        write_bytes_atomically(regular.string(), payload, CancellationToken{}, {});
    expect_publication_error(regular_result, ErrorCode::kConflict, "encoded_output_exists",
                             regular);
    EXPECT_EQ(read_file(regular), std::vector<std::uint8_t>({'w', 'i', 'n', 'n', 'e', 'r'}));

    const auto directory = temporary.path() / "directory";
    std::filesystem::create_directory(directory);
    expect_publication_error(
        write_bytes_atomically(directory.string(), payload, CancellationToken{}, {}),
        ErrorCode::kConflict, "encoded_output_exists", directory);

#ifndef _WIN32
    const auto symlink_target = temporary.path() / "symlink-target";
    write_file(symlink_target, "winner");
    const auto symlink = temporary.path() / "symlink";
    std::filesystem::create_symlink(symlink_target, symlink);
    expect_publication_error(
        write_bytes_atomically(symlink.string(), payload, CancellationToken{}, {}),
        ErrorCode::kConflict, "encoded_output_exists", symlink);
    EXPECT_EQ(read_file(symlink_target), std::vector<std::uint8_t>({'w', 'i', 'n', 'n', 'e', 'r'}));

    const auto dangling = temporary.path() / "dangling";
    std::filesystem::create_symlink(temporary.path() / "missing-target", dangling);
    expect_publication_error(
        write_bytes_atomically(dangling.string(), payload, CancellationToken{}, {}),
        ErrorCode::kConflict, "encoded_output_exists", dangling);

    const auto fifo = temporary.path() / "fifo";
    ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
    expect_publication_error(
        write_bytes_atomically(fifo.string(), payload, CancellationToken{}, {}),
        ErrorCode::kConflict, "encoded_output_exists", fifo);
#endif
}

TEST(EncodedPublicationTest, ClassifiesParentAndTemporaryOpenFailures)
{
    PublicationTempDirectory temporary;
    const auto payload = patterned_bytes(31U);
    const auto missing = temporary.path() / "missing" / "output.bin";
    expect_publication_error(
        write_bytes_atomically(missing.string(), payload, CancellationToken{}, {}), ErrorCode::kIo,
        "encoded_output_parent_missing", missing);

    const auto parent_file = temporary.path() / "parent-file";
    write_file(parent_file, "not-a-directory");
    const auto not_directory = parent_file / "output.bin";
    expect_publication_error(
        write_bytes_atomically(not_directory.string(), payload, CancellationToken{}, {}),
        ErrorCode::kIo, "encoded_output_parent_not_directory", not_directory);

#ifndef _WIN32
    const auto unwritable_parent = temporary.path() / "unwritable";
    std::filesystem::create_directory(unwritable_parent);
    std::error_code permission_error;
    std::filesystem::permissions(
        unwritable_parent, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace, permission_error);
    ASSERT_FALSE(permission_error);
    const auto unwritable = unwritable_parent / "output.bin";
    const auto unwritable_result =
        write_bytes_atomically(unwritable.string(), payload, CancellationToken{}, {});
    std::filesystem::permissions(unwritable_parent, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, permission_error);
    ASSERT_FALSE(permission_error);
    expect_publication_error(unwritable_result, ErrorCode::kIo, "encoded_output_parent_unwritable",
                             unwritable);

    const auto late_parent = temporary.path() / "late-unwritable";
    std::filesystem::create_directory(late_parent);
    const auto late_output = late_parent / "output.bin";
    PublicationHookFixture fixture;
    fixture.target = EncodedPublicationCheckpoint::kBeforeTemporaryOpen;
    fixture.mutation = HookMutation::kMakeParentReadOnly;
    fixture.parent = late_parent.string();
    const auto late_result = write_bytes_atomically(late_output.string(), payload,
                                                    CancellationToken{}, hook_for(fixture));
    std::filesystem::permissions(late_parent, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, permission_error);
    ASSERT_FALSE(permission_error);
    expect_publication_error(late_result, ErrorCode::kIo, "encoded_temporary_open_failed",
                             late_output);
    EXPECT_TRUE(fixture.invoked);
    EXPECT_TRUE(fixture.mutation_succeeded);
#endif
}

TEST(EncodedPublicationTest, MapsWriteSyncCloseAndPublishFailuresWithoutPublishing)
{
    PublicationTempDirectory temporary;
    const auto payload = patterned_bytes(3U * kPublicationChunkBytes + 7U);
    struct FailureCase
    {
        EncodedPublicationCheckpoint checkpoint;
        std::uint64_t threshold;
        const char *reason;
        const char *filename;
    };
    const std::array cases{
        FailureCase{EncodedPublicationCheckpoint::kTemporaryChunkWritten, kPublicationChunkBytes,
                    "encoded_temporary_write_failed", "write.bin"},
        FailureCase{EncodedPublicationCheckpoint::kBeforeTemporarySync, 0U,
                    "encoded_temporary_sync_failed", "sync.bin"},
        FailureCase{EncodedPublicationCheckpoint::kBeforeTemporaryClose, 0U,
                    "encoded_temporary_close_failed", "close.bin"},
        FailureCase{EncodedPublicationCheckpoint::kBeforePublish, 0U, "encoded_publish_failed",
                    "publish.bin"},
    };
    for (const auto &test_case : cases)
    {
        const auto output = temporary.path() / test_case.filename;
        PublicationHookFixture fixture;
        fixture.target = test_case.checkpoint;
        fixture.threshold = test_case.threshold;
        fixture.injected_error = std::make_error_code(std::errc::io_error);
        const auto result = write_bytes_atomically(output.string(), payload, CancellationToken{},
                                                   hook_for(fixture));
        expect_publication_error(result, ErrorCode::kIo, test_case.reason, output);
        EXPECT_TRUE(fixture.invoked);
        EXPECT_TRUE(fixture.mutation_succeeded);
        EXPECT_FALSE(std::filesystem::exists(output));
        expect_owned_temporary_removed(fixture);
    }
}

TEST(EncodedPublicationTest, RetainsStageAndDiskFullMarkerForWriteSyncAndClose)
{
    PublicationTempDirectory temporary;
    const auto payload = patterned_bytes(kPublicationChunkBytes + 7U);
    struct FailureCase
    {
        EncodedPublicationCheckpoint checkpoint;
        const char *reason;
        const char *filename;
    };
    const std::array cases{
        FailureCase{EncodedPublicationCheckpoint::kBeforeTemporaryWrite,
                    "encoded_temporary_write_failed", "write.bin"},
        FailureCase{EncodedPublicationCheckpoint::kBeforeTemporarySync,
                    "encoded_temporary_sync_failed", "sync.bin"},
        FailureCase{EncodedPublicationCheckpoint::kBeforeTemporaryClose,
                    "encoded_temporary_close_failed", "close.bin"},
    };
    for (const auto &test_case : cases)
    {
        const auto output = temporary.path() / test_case.filename;
        PublicationHookFixture fixture;
        fixture.target = test_case.checkpoint;
        fixture.injected_error = std::make_error_code(std::errc::no_space_on_device);
        const auto result = write_bytes_atomically(output.string(), payload, CancellationToken{},
                                                   hook_for(fixture));
        expect_disk_full_error(result, test_case.reason, output);
        EXPECT_TRUE(fixture.invoked);
        EXPECT_TRUE(fixture.mutation_succeeded);
        EXPECT_FALSE(std::filesystem::exists(output));
        expect_owned_temporary_removed(fixture);
    }
}

struct RaceHookContext
{
    std::barrier<> *ready = nullptr;
    bool invoked = false;
};

[[nodiscard]] std::error_code race_hook(void *const context,
                                        const EncodedPublicationCheckpoint checkpoint,
                                        std::string_view, std::uint64_t) noexcept
{
    auto &fixture = *static_cast<RaceHookContext *>(context);
    if (checkpoint == EncodedPublicationCheckpoint::kBeforePublish)
    {
        fixture.invoked = true;
        fixture.ready->arrive_and_wait();
    }
    return {};
}

TEST(EncodedPublicationTest, AtomicallyChoosesOneWinnerForParallelSameDestination)
{
    PublicationTempDirectory temporary;
    const auto output = temporary.path() / "race.bin";
    const auto first = patterned_bytes(2U * kPublicationChunkBytes + 3U, 0x11111111U);
    const auto second = patterned_bytes(2U * kPublicationChunkBytes + 3U, 0x22222222U);
    std::barrier ready(2);
    RaceHookContext first_hook{&ready};
    RaceHookContext second_hook{&ready};
    std::optional<Result<void>> first_result;
    std::optional<Result<void>> second_result;
    std::thread first_thread(
        [&]
        {
            first_result = write_bytes_atomically(output.string(), first, CancellationToken{},
                                                  {race_hook, &first_hook});
        });
    std::thread second_thread(
        [&]
        {
            second_result = write_bytes_atomically(output.string(), second, CancellationToken{},
                                                   {race_hook, &second_hook});
        });
    first_thread.join();
    second_thread.join();

    ASSERT_TRUE(first_result.has_value());
    ASSERT_TRUE(second_result.has_value());
    EXPECT_TRUE(first_hook.invoked);
    EXPECT_TRUE(second_hook.invoked);
    EXPECT_NE(static_cast<bool>(*first_result), static_cast<bool>(*second_result));
    const auto winner = read_file(output);
    EXPECT_TRUE(winner == first || winner == second);
    const auto &loser = *first_result ? *second_result : *first_result;
    expect_publication_error(loser, ErrorCode::kConflict, "encoded_output_exists", output);
}

TEST(EncodedPublicationTest, PreservesPrimaryFailureWhenCleanupCannotRemoveReplacedPath)
{
    PublicationTempDirectory temporary;
    const auto output = temporary.path() / "output.bin";
    const auto payload = patterned_bytes(71U);
    PublicationHookFixture fixture;
    fixture.target = EncodedPublicationCheckpoint::kBeforePublish;
    fixture.mutation = HookMutation::kReplaceTemporaryWithForeignDirectory;
    fixture.injected_error = std::make_error_code(std::errc::io_error);

    const auto result =
        write_bytes_atomically(output.string(), payload, CancellationToken{}, hook_for(fixture));
    expect_publication_error(result, ErrorCode::kIo, "encoded_publish_failed", output);
    EXPECT_TRUE(fixture.invoked);
    ASSERT_TRUE(fixture.mutation_succeeded);
    EXPECT_FALSE(std::filesystem::exists(output));
    const auto replaced = observed_temporary(fixture);
    EXPECT_TRUE(std::filesystem::is_directory(replaced));
    EXPECT_EQ(read_file(replaced / "foreign"), std::vector<std::uint8_t>({'k', 'e', 'e', 'p'}));
}

} // namespace
} // namespace ravo
