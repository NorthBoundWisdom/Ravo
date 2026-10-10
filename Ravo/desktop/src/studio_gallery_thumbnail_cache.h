#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>

#include <QByteArray>
#include <QString>

#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo
{
class FilesystemPreviewCache;

// Used only on the presenter's serial presentation worker. The publish lock
// protects the shared disk budget; an epoch invalidates other windows' indexes.
class StudioGalleryThumbnailCache
{
public:
    explicit StudioGalleryThumbnailCache(std::uint64_t max_bytes = 512ULL * 1024ULL * 1024ULL);
    ~StudioGalleryThumbnailCache();
    [[nodiscard]] Result<QString> publish(const QString &root, const QString &key,
                                          const QByteArray &png, bool replace,
                                          const CancellationToken &cancellation);
    [[nodiscard]] std::size_t indexBuildCount() const noexcept
    {
        return index_build_count_;
    }

private:
    std::uint64_t max_bytes_;
    std::size_t index_build_count_ = 0;
    std::unique_ptr<FilesystemPreviewCache> cache_;
    QByteArray epoch_;
};
} // namespace ravo
