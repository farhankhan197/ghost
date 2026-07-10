#include "cli/commands.hpp"

#include <gtest/gtest.h>

using ghost::cli::CommandRegistry;

TEST(CommandRegistryTest, ResolveExactAndAlias) {
    EXPECT_EQ(CommandRegistry::resolveCommand("status"), "status");
    EXPECT_EQ(CommandRegistry::resolveCommand("st"), "status"); // unambiguous prefix
    EXPECT_EQ(CommandRegistry::resolveCommand("--version"), "version");
    EXPECT_EQ(CommandRegistry::resolveCommand("-V"), "version");
}

TEST(CommandRegistryTest, ResolveDoesNotTreatVerboseFlagAsCommand) {
    EXPECT_EQ(CommandRegistry::resolveCommand("-v"), "");
}

TEST(CommandRegistryTest, ResolveAmbiguousPrefixReturnsEmpty) {
    // Both "stats" and "status" start with "sta"; this should now be treated as ambiguous.
    EXPECT_EQ(CommandRegistry::resolveCommand("sta"), "");
}

