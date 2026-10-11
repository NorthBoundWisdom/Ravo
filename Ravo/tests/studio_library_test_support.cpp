#include "studio_library_test_support.h"
#include "studio_automation_test_control.h"
#include <gtest/gtest.h>
#include <QColorSpace>
#include <QImage>
#include "ravo/adapters/sqlite_catalog.h"
#include "ravo/domain/uri.h"
namespace ravo::studio_test_support
{
void make_paged_catalog(const QTemporaryDir &root, const int count,
                        const bool include_unloaded_video)
{
    auto repository =
        SqliteCatalogRepository::create(root.filePath("library.sqlite").toStdString());
    ASSERT_TRUE(repository);
    QImage image(16, 12, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    for (int i = 0; i < count; ++i)
    {
        const auto file = root.filePath(QString("photo-%1.png").arg(i));
        image.fill(QColor(i % 256, i / 256, 160));
        ASSERT_TRUE(image.save(file, "PNG"));
        auto location = normalize_local_input(file.toStdString());
        ASSERT_TRUE(location);
        auto identity = read_file_identity(location.value().path);
        ASSERT_TRUE(identity);
        AssetRecord asset;
        asset.id = "ast_page_" + std::to_string(i);
        asset.normalized_uri = location.value().uri;
        asset.media_type = "image/png";
        if (include_unloaded_video && i == 500)
        {
            asset.media_type = "video/mp4";
            VideoInfo video;
            video.container = "mp4";
            video.codec = "h264";
            video.width = 16;
            video.height = 12;
            asset.video = video;
        }
        asset.width = 16;
        asset.height = 12;
        asset.size_bytes = identity.value().size_bytes;
        asset.mtime_unix_ms = identity.value().mtime_unix_ms;
        asset.content_fingerprint = make_content_fingerprint(identity.value());
        asset.created_unix_ms = 1000 - i;
        ASSERT_TRUE(repository.value()->commit_imported_asset(asset));
    }
}
} // namespace ravo::studio_test_support
namespace ravo::testing
{
std::uint64_t StudioAutomationTestControl::generation(StudioPresenter &p)
{
    return p.library_query_generation_;
}
Result<LibraryPage> StudioAutomationTestControl::query(StudioPresenter &p,
                                                       LibraryPageRequest request)
{
    return p.executor_.submit([&] { return p.service_->library().list_assets_page(request); });
}
Result<Recipe> StudioAutomationTestControl::saved(StudioPresenter &p)
{
    return p.executor_.submit(
        [&] { return p.service_->develop().load_recipe(p.selected_asset_id_.toStdString()); });
}
bool StudioAutomationTestControl::block(StudioPresenter &p,
                                        std::shared_ptr<std::promise<void>> entered,
                                        std::shared_future<void> release)
{
    return p.executor_.post(
        [entered, release]
        {
            entered->set_value();
            release.wait();
        });
}
} // namespace ravo::testing
