#include "git/ref.hpp"
#include <gtest/gtest.h>

using ghost::git::Ref;

TEST(GitRef, AllowsCommonCommitishValues) {
    EXPECT_TRUE(Ref::isSafeCommitish("HEAD"));
    EXPECT_TRUE(Ref::isSafeCommitish("HEAD~2"));
    EXPECT_TRUE(Ref::isSafeCommitish("origin/main"));
    EXPECT_TRUE(Ref::isSafeCommitish("feature/test-branch"));
    EXPECT_TRUE(Ref::isSafeCommitish("abc123def456"));
}

TEST(GitRef, AllowsCommonRanges) {
    EXPECT_TRUE(Ref::isSafeRange("origin/main..HEAD"));
    EXPECT_TRUE(Ref::isSafeRange("HEAD~3..HEAD"));
    EXPECT_TRUE(Ref::isSafeRange("abc123..def456"));
}

TEST(GitRef, RejectsShellMetacharacters) {
    EXPECT_FALSE(Ref::isSafeCommitish("HEAD;echo owned"));
    EXPECT_FALSE(Ref::isSafeCommitish("HEAD && echo owned"));
    EXPECT_FALSE(Ref::isSafeRange("origin/main..HEAD | cat"));
    EXPECT_FALSE(Ref::isSafeConfigRef("main:ghost.yml"));
    EXPECT_FALSE(Ref::isSafeNotesRef("refs/notes/ghost;touch x"));
}

TEST(GitRef, RestrictsNotesRefs) {
    EXPECT_TRUE(Ref::isSafeNotesRef("refs/notes/ghost"));
    EXPECT_TRUE(Ref::isSafeNotesRef("refs/notes/ghost-verified"));
    EXPECT_TRUE(Ref::isSafeNotesRef("refs/notes/ghost-signatures"));
    EXPECT_TRUE(Ref::isSafeNotesRef("refs/notes/ai"));
    EXPECT_FALSE(Ref::isSafeNotesRef("refs/heads/main"));
}

