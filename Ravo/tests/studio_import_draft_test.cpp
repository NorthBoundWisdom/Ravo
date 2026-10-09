#include <gtest/gtest.h>

#include "ravo/desktop/studio_import_draft.h"

using ravo::ImportDraft;

TEST(ImportDraft, DefaultsAreStable)
{
    const ImportDraft draft;
    EXPECT_EQ(draft.mode, QStringLiteral("copy"));
    EXPECT_EQ(draft.organization, QStringLiteral("single"));
    EXPECT_EQ(draft.preview_policy, QStringLiteral("standard"));
    EXPECT_TRUE(draft.source_root.isEmpty());
    EXPECT_TRUE(draft.destination.isEmpty());
    EXPECT_FALSE(draft.destination_valid);
    EXPECT_TRUE(draft.destination_error.isEmpty());
    EXPECT_FALSE(draft.rename_enabled);
    EXPECT_EQ(draft.rename_parts, (std::array<int, 3>{2, 1, 3}));
    EXPECT_EQ(draft.rename_separator, 0);
}

TEST(ImportDraft, ValueCopyPreservesFields)
{
    ImportDraft draft;
    draft.source_root = QStringLiteral("/tmp/src");
    draft.destination = QStringLiteral("/tmp/dst");
    draft.second_copy_destination = QStringLiteral("/tmp/bak");
    draft.second_copy_enabled = true;
    draft.organization = QStringLiteral("date");
    draft.mode = QStringLiteral("move");
    draft.rename_enabled = true;
    draft.rename_parts = {1, 3, 0};
    draft.rename_separator = 1;
    draft.destination_valid = true;
    draft.destination_error = QStringLiteral("ok");

    const ImportDraft copy = draft;
    EXPECT_EQ(copy.source_root, draft.source_root);
    EXPECT_EQ(copy.destination, draft.destination);
    EXPECT_EQ(copy.second_copy_destination, draft.second_copy_destination);
    EXPECT_EQ(copy.second_copy_enabled, draft.second_copy_enabled);
    EXPECT_EQ(copy.organization, draft.organization);
    EXPECT_EQ(copy.mode, draft.mode);
    EXPECT_EQ(copy.rename_enabled, draft.rename_enabled);
    EXPECT_EQ(copy.rename_parts, draft.rename_parts);
    EXPECT_EQ(copy.rename_separator, draft.rename_separator);
    EXPECT_EQ(copy.destination_valid, draft.destination_valid);
    EXPECT_EQ(copy.destination_error, draft.destination_error);
}
