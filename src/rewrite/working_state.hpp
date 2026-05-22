#ifndef GHOST_REWRITE_WORKING_STATE_HPP
#define GHOST_REWRITE_WORKING_STATE_HPP

#include <string>
#include <vector>

namespace ghost {
namespace rewrite {

// Save/restore working checkpoint and session state across git operations
class WorkingState {
public:
    // Save all unprocessed checkpoints and uncommitted sessions to DB
    static bool save(const std::string& repoRoot, const std::string& key = "default");

    // Restore saved checkpoints and sessions from DB to active state
    static bool restore(const std::string& repoRoot, const std::string& key = "default");

    // Clear saved state from DB
    static bool clear(const std::string& repoRoot, const std::string& key = "default");

    // Check if saved state exists
    static bool exists(const std::string& repoRoot, const std::string& key = "default");

    // Save current session data as JSON before a destructive operation
    static bool saveSessionsJson(const std::string& repoRoot, const std::string& key);

    // Restore sessions from JSON after operation
    static bool restoreSessionsJson(const std::string& repoRoot, const std::string& key);
};

} // namespace rewrite
} // namespace ghost

#endif // GHOST_REWRITE_WORKING_STATE_HPP
