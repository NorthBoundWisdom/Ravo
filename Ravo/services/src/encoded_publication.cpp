#include "catalog_internal.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <system_error>

#include "atomic_publication_internal.h"

namespace ravo
{
namespace
{

constexpr std::size_t kEncodedPublicationChunkBytes = 64U * 1024U;
constexpr int kEncodedPublicationTemporaryAttempts = 32;

[[nodiscard]] TaskError encoded_publication_error(const ErrorCode code, std::string message,
                                                  const std::string_view reason,
                                                  const std::string_view output,
                                                  const std::error_code &error = {})
{
    std::map<std::string, std::string, std::less<>> context{
        {"output", std::string(output)},
        {"path", std::string(output)},
        {"reason", std::string(reason)},
    };
    if (error)
    {
        context.emplace("detail", error.message());
        if (is_disk_full(error))
        {
            context.emplace("disk_full", "true");
        }
    }
    return make_error(code, std::move(message), std::move(context));
}

[[nodiscard]] Result<void>
check_encoded_publication_cancellation(const CancellationToken &cancellation,
                                       const std::string_view output)
{
    auto active = cancellation.check();
    if (active)
    {
        return {};
    }
    auto error = std::move(active).error();
    error.context.insert_or_assign("output", std::string(output));
    error.context.insert_or_assign("path", std::string(output));
    return error;
}

[[nodiscard]] std::error_code invoke_encoded_publication_hook(
    const EncodedPublicationCheckpointHook &hook, const EncodedPublicationCheckpoint checkpoint,
    const std::string_view path, const std::uint64_t bytes_processed) noexcept
{
    return hook.callback == nullptr ?
               std::error_code{} :
               hook.callback(hook.context, checkpoint, path, bytes_processed);
}

[[nodiscard]] TaskError encoded_hook_stage_error(const EncodedPublicationCheckpoint checkpoint,
                                                 const std::string_view output,
                                                 const std::error_code &error)
{
    switch (checkpoint)
    {
    case EncodedPublicationCheckpoint::kBeforeTemporaryOpen:
    case EncodedPublicationCheckpoint::kTemporaryCreated:
        return encoded_publication_error(ErrorCode::kIo, "Unable to open temporary export file",
                                         "encoded_temporary_open_failed", output, error);
    case EncodedPublicationCheckpoint::kBeforeTemporaryWrite:
    case EncodedPublicationCheckpoint::kTemporaryChunkWritten:
        return encoded_publication_error(ErrorCode::kIo, "Unable to write temporary export file",
                                         "encoded_temporary_write_failed", output, error);
    case EncodedPublicationCheckpoint::kBeforeTemporarySync:
        return encoded_publication_error(ErrorCode::kIo,
                                         "Unable to synchronize temporary export file",
                                         "encoded_temporary_sync_failed", output, error);
    case EncodedPublicationCheckpoint::kBeforeTemporaryClose:
        return encoded_publication_error(ErrorCode::kIo, "Unable to close temporary export file",
                                         "encoded_temporary_close_failed", output, error);
    case EncodedPublicationCheckpoint::kBeforePublish:
        return encoded_publication_error(ErrorCode::kIo, "Unable to publish encoded output",
                                         "encoded_publish_failed", output, error);
    }
    return encoded_publication_error(ErrorCode::kIo, "Unable to publish encoded output",
                                     "encoded_publication_internal_error", output, error);
}

} // namespace

Result<void> write_bytes_atomically(const std::string_view dest_utf8,
                                    const std::vector<std::uint8_t> &bytes,
                                    const CancellationToken &cancellation)
{
    return write_bytes_atomically(dest_utf8, bytes, cancellation, {});
}

Result<void> write_bytes_atomically(const std::string_view dest_utf8,
                                    const std::vector<std::uint8_t> &bytes,
                                    const CancellationToken &cancellation,
                                    const EncodedPublicationCheckpointHook checkpoint_hook)
{
    using atomic_publication_internal::FileDescriptor;
    using atomic_publication_internal::OwnedTemporaryPath;

    auto active = check_encoded_publication_cancellation(cancellation, dest_utf8);
    if (!active)
    {
        return active.error();
    }

    const auto output = utf8_path(dest_utf8);
    std::error_code error;
    const auto output_status = std::filesystem::symlink_status(output, error);
    if (!error && output_status.type() != std::filesystem::file_type::not_found)
    {
        return encoded_publication_error(ErrorCode::kConflict, "Export output already exists",
                                         "encoded_output_exists", dest_utf8);
    }
    if (error && error != std::errc::no_such_file_or_directory &&
        error != std::errc::not_a_directory)
    {
        return encoded_publication_error(ErrorCode::kIo, "Unable to inspect export output path",
                                         "encoded_output_inspect_failed", dest_utf8, error);
    }
    error.clear();

    const auto parent =
        output.parent_path().empty() ? std::filesystem::path(".") : output.parent_path();
    const auto parent_status = std::filesystem::status(parent, error);
    if (error == std::errc::no_such_file_or_directory ||
        parent_status.type() == std::filesystem::file_type::not_found)
    {
        return encoded_publication_error(ErrorCode::kIo, "Export directory does not exist",
                                         "encoded_output_parent_missing", dest_utf8);
    }
    if (error || !std::filesystem::is_directory(parent_status))
    {
        return encoded_publication_error(ErrorCode::kIo, "Export parent is not a directory",
                                         "encoded_output_parent_not_directory", dest_utf8, error);
    }
    if (!atomic_publication_internal::has_write_permission(parent_status.permissions()))
    {
        return encoded_publication_error(ErrorCode::kIo, "Export directory is not writable",
                                         "encoded_output_parent_unwritable", dest_utf8);
    }

    OwnedTemporaryPath owned_temporary;
    FileDescriptor temporary_file;
    std::string temporary_utf8;
    for (int attempt = 0; attempt < kEncodedPublicationTemporaryAttempts; ++attempt)
    {
        const auto candidate =
            atomic_publication_internal::temporary_candidate(output, "encoded-output");
        temporary_utf8 = atomic_publication_internal::checkpoint_path_utf8(candidate);
        auto hook_error = invoke_encoded_publication_hook(
            checkpoint_hook, EncodedPublicationCheckpoint::kBeforeTemporaryOpen, temporary_utf8,
            0U);
        active = check_encoded_publication_cancellation(cancellation, dest_utf8);
        if (!active)
        {
            return active.error();
        }
        if (hook_error)
        {
            return encoded_hook_stage_error(EncodedPublicationCheckpoint::kBeforeTemporaryOpen,
                                            dest_utf8, hook_error);
        }

        const int descriptor = atomic_publication_internal::open_temporary_descriptor(candidate);
        if (descriptor >= 0)
        {
            temporary_file = FileDescriptor(descriptor);
            owned_temporary.reset(candidate);
            break;
        }
        const std::error_code open_error(errno, std::generic_category());
        if (open_error == std::errc::file_exists)
        {
            continue;
        }
        return encoded_hook_stage_error(EncodedPublicationCheckpoint::kBeforeTemporaryOpen,
                                        dest_utf8, open_error);
    }
    if (temporary_file.get() < 0)
    {
        return encoded_hook_stage_error(EncodedPublicationCheckpoint::kBeforeTemporaryOpen,
                                        dest_utf8, std::make_error_code(std::errc::file_exists));
    }

    auto hook_error = invoke_encoded_publication_hook(
        checkpoint_hook, EncodedPublicationCheckpoint::kTemporaryCreated, temporary_utf8, 0U);
    active = check_encoded_publication_cancellation(cancellation, dest_utf8);
    if (!active)
    {
        return active.error();
    }
    if (hook_error)
    {
        return encoded_hook_stage_error(EncodedPublicationCheckpoint::kTemporaryCreated, dest_utf8,
                                        hook_error);
    }

    std::size_t bytes_written = 0U;
    while (bytes_written < bytes.size())
    {
        active = check_encoded_publication_cancellation(cancellation, dest_utf8);
        if (!active)
        {
            return active.error();
        }
        hook_error = invoke_encoded_publication_hook(
            checkpoint_hook, EncodedPublicationCheckpoint::kBeforeTemporaryWrite, temporary_utf8,
            bytes_written);
        active = check_encoded_publication_cancellation(cancellation, dest_utf8);
        if (!active)
        {
            return active.error();
        }
        if (hook_error)
        {
            return encoded_hook_stage_error(EncodedPublicationCheckpoint::kBeforeTemporaryWrite,
                                            dest_utf8, hook_error);
        }

        const auto chunk_size =
            std::min(kEncodedPublicationChunkBytes, bytes.size() - bytes_written);
        std::size_t chunk_offset = 0U;
        while (chunk_offset < chunk_size)
        {
            const auto written = atomic_publication_internal::write_descriptor(
                temporary_file.get(), bytes.data() + bytes_written + chunk_offset,
                chunk_size - chunk_offset);
            if (written <= 0)
            {
                const std::error_code write_error =
                    written < 0 ? std::error_code(errno, std::generic_category()) :
                                  std::make_error_code(std::errc::io_error);
                active = check_encoded_publication_cancellation(cancellation, dest_utf8);
                if (!active)
                {
                    return active.error();
                }
                return encoded_hook_stage_error(EncodedPublicationCheckpoint::kBeforeTemporaryWrite,
                                                dest_utf8, write_error);
            }
            chunk_offset += static_cast<std::size_t>(written);
        }
        bytes_written += chunk_size;
        hook_error = invoke_encoded_publication_hook(
            checkpoint_hook, EncodedPublicationCheckpoint::kTemporaryChunkWritten, temporary_utf8,
            bytes_written);
        active = check_encoded_publication_cancellation(cancellation, dest_utf8);
        if (!active)
        {
            return active.error();
        }
        if (hook_error)
        {
            return encoded_hook_stage_error(EncodedPublicationCheckpoint::kTemporaryChunkWritten,
                                            dest_utf8, hook_error);
        }
    }

    hook_error = invoke_encoded_publication_hook(checkpoint_hook,
                                                 EncodedPublicationCheckpoint::kBeforeTemporarySync,
                                                 temporary_utf8, bytes_written);
    active = check_encoded_publication_cancellation(cancellation, dest_utf8);
    if (!active)
    {
        return active.error();
    }
    if (hook_error)
    {
        return encoded_hook_stage_error(EncodedPublicationCheckpoint::kBeforeTemporarySync,
                                        dest_utf8, hook_error);
    }
    const auto sync_error = temporary_file.sync();
    if (sync_error)
    {
        active = check_encoded_publication_cancellation(cancellation, dest_utf8);
        if (!active)
        {
            return active.error();
        }
        return encoded_hook_stage_error(EncodedPublicationCheckpoint::kBeforeTemporarySync,
                                        dest_utf8, sync_error);
    }

    hook_error = invoke_encoded_publication_hook(
        checkpoint_hook, EncodedPublicationCheckpoint::kBeforeTemporaryClose, temporary_utf8,
        bytes_written);
    active = check_encoded_publication_cancellation(cancellation, dest_utf8);
    if (!active)
    {
        return active.error();
    }
    if (hook_error)
    {
        return encoded_hook_stage_error(EncodedPublicationCheckpoint::kBeforeTemporaryClose,
                                        dest_utf8, hook_error);
    }
    const auto close_error = temporary_file.close();
    if (close_error)
    {
        active = check_encoded_publication_cancellation(cancellation, dest_utf8);
        if (!active)
        {
            return active.error();
        }
        return encoded_hook_stage_error(EncodedPublicationCheckpoint::kBeforeTemporaryClose,
                                        dest_utf8, close_error);
    }

    hook_error = invoke_encoded_publication_hook(checkpoint_hook,
                                                 EncodedPublicationCheckpoint::kBeforePublish,
                                                 temporary_utf8, bytes_written);
    active = check_encoded_publication_cancellation(cancellation, dest_utf8);
    if (!active)
    {
        return active.error();
    }
    if (hook_error)
    {
        return encoded_hook_stage_error(EncodedPublicationCheckpoint::kBeforePublish, dest_utf8,
                                        hook_error);
    }

    error = atomic_publication_internal::publish_no_replace(owned_temporary.path(), output);
    if (error)
    {
        std::error_code inspect_error;
        const auto status = std::filesystem::symlink_status(output, inspect_error);
        if (!inspect_error && status.type() != std::filesystem::file_type::not_found)
        {
            return encoded_publication_error(ErrorCode::kConflict, "Export output already exists",
                                             "encoded_output_exists", dest_utf8);
        }
        return encoded_hook_stage_error(EncodedPublicationCheckpoint::kBeforePublish, dest_utf8,
                                        error);
    }
    owned_temporary.release();
    return {};
}

} // namespace ravo
