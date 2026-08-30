#include "ravo/services/artifact_publication.h"

#include <cstdint>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "catalog_internal.h"

namespace ravo
{

Result<void> publish_text_artifact_no_replace(const std::string_view destination,
                                              const std::string_view utf8_text,
                                              const CancellationToken &cancellation)
{
    try
    {
        const std::vector<std::uint8_t> bytes(utf8_text.begin(), utf8_text.end());
        auto published = write_bytes_atomically(destination, bytes, cancellation);
        if (published)
            return {};
        auto error = std::move(published).error();
        const auto reason = error.context.find("reason");
        if (reason != error.context.end() && reason->second.starts_with("encoded_"))
            reason->second.replace(0U, std::string_view("encoded").size(), "artifact");
        if (error.code == ErrorCode::kConflict)
            error.message = "Artifact destination already exists";
        else if (error.code == ErrorCode::kIo)
            error.message = "Unable to publish artifact";
        return error;
    }
    catch (const std::bad_alloc &)
    {
        return make_error(ErrorCode::kIo, "Unable to allocate artifact publication buffer",
                          {{"output", std::string(destination)},
                           {"path", std::string(destination)},
                           {"reason", "allocation_failed"}});
    }
}

} // namespace ravo
