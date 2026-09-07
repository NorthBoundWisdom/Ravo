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
    draft.filename_pattern = QStringLiteral("{original}");
    draft.destination_valid = true;
    draft.destination_error = QStringLiteral("ok");

    const ImportDraft copy = draft;
    EXPECT_EQ(copy.source_root, draft.source_root);
    EXPECT_EQ(copy.destination, draft.destination);
    EXPECT_EQ(copy.second_copy_destination, draft.second_copy_destination);
    EXPECT_EQ(copy.second_copy_enabled, draft.second_copy_enabled);
    EXPECT_EQ(copy.organization, draft.organization);
    EXPECT_EQ(copy.mode, draft.mode);
    EXPECT_EQ(copy.filename_pattern, draft.filename_pattern);
    EXPECT_EQ(copy.destination_valid, draft.destination_valid);
    EXPECT_EQ(copy.destination_error, draft.destination_error);
}
