#include <algorithm>
#include <string>
#include <vector>

#include <QColor>
#include <QColorSpace>
#include <QImage>
#include <QString>
#include <gtest/gtest.h>

#include "ravo/foundation/cancellation.h"
#include "ravo/services/ai_suggestion.h"
#include "ravo/services/catalog_service.h"

#include "catalog_test_support.h"

namespace ravo
{
namespace
{

[[nodiscard]] bool write_jpeg(const std::filesystem::path &path, const QColor &color)
{
    QImage image(12, 8, QImage::Format_RGB888);
    image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.fill(color);
    return image.save(QString::fromStdString(path.string()), "JPEG", 90);
}

} // namespace

TEST_F(CatalogServiceTest, AiSuggestionKeywordAcceptMergesTagsRejectLeavesUnchanged)
{
    ASSERT_TRUE(open_service(true));
    const auto path = root / "ai04-keyword.jpg";
    ASSERT_TRUE(write_jpeg(path, QColor(40, 90, 140)));
    auto imported = service->import_one(path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto asset_id = imported.value().asset->id;

    ASSERT_TRUE(service->set_tags(asset_id, {"existing"}));

    AiSuggestionCreateRequest denied;
    denied.asset_id = asset_id;
    denied.kind = AiSuggestionKind::kKeyword;
    denied.user_initiated = false;
    auto missing = service->create_ai_suggestion(denied);
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().context.at("reason"), "ai_suggestion_not_user_initiated");

    AiSuggestionCreateRequest request;
    request.asset_id = asset_id;
    request.kind = AiSuggestionKind::kKeyword;
    request.user_initiated = true;
    auto created = service->create_ai_suggestion(request);
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_EQ(created.value().contract_version, kAiSuggestionContractVersion);
    EXPECT_EQ(created.value().provider.model_id, kAiStubSuggestionModelId);
    EXPECT_EQ(created.value().status, AiSuggestionStatus::kPending);
    EXPECT_FALSE(created.value().suggested_keywords.empty());
    EXPECT_TRUE(created.value().catalog_mutated_on_accept);

    auto rejected = service->reject_ai_suggestion(created.value().id);
    ASSERT_TRUE(rejected) << rejected.error().message;
    EXPECT_EQ(rejected.value().status, AiSuggestionStatus::kRejected);
    auto after_reject = service->get_ai_suggestion(created.value().id);
    ASSERT_TRUE(after_reject);
    auto asset_after_reject = service->list_assets();
    ASSERT_TRUE(asset_after_reject);
    const auto *found = [&]() -> const AssetRecord *
    {
        for (const auto &asset : asset_after_reject.value())
        {
            if (asset.id == asset_id)
                return &asset;
        }
        return nullptr;
    }();
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->tags, (std::vector<std::string>{"existing"}));

    auto created2 = service->create_ai_suggestion(request);
    ASSERT_TRUE(created2) << created2.error().message;
    auto accepted = service->accept_ai_suggestion(created2.value().id);
    ASSERT_TRUE(accepted) << accepted.error().message;
    EXPECT_TRUE(accepted.value().catalog_mutated);
    EXPECT_EQ(accepted.value().suggestion.status, AiSuggestionStatus::kAccepted);

    auto listed = service->list_assets();
    ASSERT_TRUE(listed);
    const AssetRecord *accepted_asset = nullptr;
    for (const auto &asset : listed.value())
    {
        if (asset.id == asset_id)
            accepted_asset = &asset;
    }
    ASSERT_NE(accepted_asset, nullptr);
    EXPECT_NE(std::find(accepted_asset->tags.begin(), accepted_asset->tags.end(), "existing"),
              accepted_asset->tags.end());
    for (const auto &keyword : created2.value().suggested_keywords)
    {
        EXPECT_NE(std::find(accepted_asset->tags.begin(), accepted_asset->tags.end(), keyword),
                  accepted_asset->tags.end());
    }
}

TEST_F(CatalogServiceTest, AiSuggestionCaptionAcceptWritesDescriptionAndHeadline)
{
    ASSERT_TRUE(open_service(true));
    const auto path = root / "ai04-caption.jpg";
    ASSERT_TRUE(write_jpeg(path, QColor(20, 70, 120)));
    auto imported = service->import_one(path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto asset_id = imported.value().asset->id;

    AiSuggestionCreateRequest request;
    request.asset_id = asset_id;
    request.kind = AiSuggestionKind::kCaption;
    request.user_initiated = true;
    auto created = service->create_ai_suggestion(request);
    ASSERT_TRUE(created) << created.error().message;
    ASSERT_TRUE(created.value().suggested_caption);
    ASSERT_TRUE(created.value().suggested_headline);

    auto accepted = service->accept_ai_suggestion(created.value().id);
    ASSERT_TRUE(accepted) << accepted.error().message;
    EXPECT_TRUE(accepted.value().catalog_mutated);

    auto listed = service->list_assets();
    ASSERT_TRUE(listed);
    const AssetRecord *asset = nullptr;
    for (const auto &row : listed.value())
    {
        if (row.id == asset_id)
            asset = &row;
    }
    ASSERT_NE(asset, nullptr);
    ASSERT_TRUE(asset->metadata.description);
    EXPECT_EQ(*asset->metadata.description, *created.value().suggested_caption);
    ASSERT_TRUE(asset->metadata.headline);
    EXPECT_EQ(*asset->metadata.headline, *created.value().suggested_headline);
}

TEST_F(CatalogServiceTest, AiSuggestionFocusAndDuplicateNeverMutateCatalog)
{
    ASSERT_TRUE(open_service(true));
    const auto a_path = root / "ai04-focus.jpg";
    const auto b_path = root / "ai04-peer.jpg";
    ASSERT_TRUE(write_jpeg(a_path, QColor(10, 10, 10)));
    ASSERT_TRUE(write_jpeg(b_path, QColor(200, 100, 50)));
    auto a = service->import_one(a_path.string(), CancellationToken{});
    auto b = service->import_one(b_path.string(), CancellationToken{});
    ASSERT_TRUE(a) << a.error().message;
    ASSERT_TRUE(b) << b.error().message;
    const auto a_id = a.value().asset->id;
    const auto b_id = b.value().asset->id;
    auto before = service->snapshot();
    ASSERT_TRUE(before);

    AiSuggestionCreateRequest focus;
    focus.asset_id = a_id;
    focus.kind = AiSuggestionKind::kFocus;
    focus.user_initiated = true;
    auto focus_created = service->create_ai_suggestion(focus);
    ASSERT_TRUE(focus_created) << focus_created.error().message;
    EXPECT_FALSE(focus_created.value().catalog_mutated_on_accept);
    auto focus_accepted = service->accept_ai_suggestion(focus_created.value().id);
    ASSERT_TRUE(focus_accepted) << focus_accepted.error().message;
    EXPECT_FALSE(focus_accepted.value().catalog_mutated);

    AiSuggestionCreateRequest dup;
    dup.asset_id = a_id;
    dup.kind = AiSuggestionKind::kDuplicate;
    dup.user_initiated = true;
    dup.peer_asset_id = b_id;
    auto dup_created = service->create_ai_suggestion(dup);
    ASSERT_TRUE(dup_created) << dup_created.error().message;
    ASSERT_FALSE(dup_created.value().peer_asset_ids.empty());
    EXPECT_EQ(dup_created.value().peer_asset_ids.front(), b_id);
    auto dup_accepted = service->accept_ai_suggestion(dup_created.value().id);
    ASSERT_TRUE(dup_accepted) << dup_accepted.error().message;
    EXPECT_FALSE(dup_accepted.value().catalog_mutated);

    // Both assets still present; accept never deletes peers.
    auto listed = service->list_assets();
    ASSERT_TRUE(listed);
    EXPECT_GE(listed.value().size(), 2U);
    auto after = service->snapshot();
    ASSERT_TRUE(after);
    // Focus/duplicate accept must not bump catalog revision via metadata writes.
    EXPECT_EQ(before.value().revision, after.value().revision);
}

TEST_F(CatalogServiceTest, AiSuggestionCancelLeavesCatalogUntouched)
{
    ASSERT_TRUE(open_service(true));
    const auto path = root / "ai04-cancel.jpg";
    ASSERT_TRUE(write_jpeg(path, QColor(80, 40, 20)));
    auto imported = service->import_one(path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;

    AiSuggestionCreateRequest request;
    request.asset_id = imported.value().asset->id;
    request.kind = AiSuggestionKind::kKeyword;
    request.user_initiated = true;
    auto created = service->create_ai_suggestion(request);
    ASSERT_TRUE(created) << created.error().message;
    auto cancelled = service->cancel_ai_suggestion(created.value().id);
    ASSERT_TRUE(cancelled) << cancelled.error().message;
    EXPECT_EQ(cancelled.value().status, AiSuggestionStatus::kCancelled);

    auto again = service->accept_ai_suggestion(created.value().id);
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().context.at("reason"), "ai_suggestion_not_pending");
}

TEST_F(CatalogServiceTest, AiSuggestionSurvivesReloadAndListsByAsset)
{
    ASSERT_TRUE(open_service(true));
    const auto path = root / "ai04-reload.jpg";
    ASSERT_TRUE(write_jpeg(path, QColor(30, 60, 90)));
    auto imported = service->import_one(path.string(), CancellationToken{});
    ASSERT_TRUE(imported) << imported.error().message;
    const auto asset_id = imported.value().asset->id;

    AiSuggestionCreateRequest request;
    request.asset_id = asset_id;
    request.kind = AiSuggestionKind::kFocus;
    request.user_initiated = true;
    auto created = service->create_ai_suggestion(request);
    ASSERT_TRUE(created) << created.error().message;
    const auto suggestion_id = created.value().id;

    ASSERT_TRUE(service->close());
    ASSERT_TRUE(open_service(false));

    auto loaded = service->get_ai_suggestion(suggestion_id);
    ASSERT_TRUE(loaded) << loaded.error().message;
    EXPECT_EQ(loaded.value().kind, AiSuggestionKind::kFocus);
    EXPECT_EQ(loaded.value().status, AiSuggestionStatus::kPending);
    EXPECT_EQ(loaded.value().provider.model_id, kAiStubSuggestionModelId);

    auto listed = service->list_ai_suggestions(asset_id);
    ASSERT_TRUE(listed) << listed.error().message;
    ASSERT_EQ(listed.value().size(), 1U);
    EXPECT_EQ(listed.value().front().id, suggestion_id);
}

} // namespace ravo
