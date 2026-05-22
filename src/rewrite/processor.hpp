#ifndef GHOST_REWRITE_PROCESSOR_HPP
#define GHOST_REWRITE_PROCESSOR_HPP

#include <string>
#include <vector>
#include <map>

namespace ghost {
namespace rewrite {

// Process rewrite events and migrate ghost notes
class Processor {
public:
    // Called after a rebase completes: copy notes from original SHAs to new SHAs
    static bool processRebase(const std::string& repoRoot,
                              const std::vector<std::string>& originalCommits,
                              const std::vector<std::string>& newCommits);

    // Called after commit --amend: move note from original to amended SHA
    static bool processAmend(const std::string& repoRoot,
                             const std::string& originalCommit,
                             const std::string& amendedCommit);

    // Called after cherry-pick: copy notes from source to new SHAs
    static bool processCherryPick(const std::string& repoRoot,
                                  const std::vector<std::string>& sourceCommits,
                                  const std::vector<std::string>& newCommits);

    // Called after merge: handle squash merge working state
    static bool processMergeSquash(const std::string& repoRoot,
                                   const std::string& sourceHead,
                                   const std::string& baseHead);

    // Called after reset --soft/--mixed: recover sessions from unwound commits
    static bool processReset(const std::string& repoRoot,
                             const std::string& oldHeadSha,
                             const std::string& newHeadSha,
                             const std::string& kind);

    // Detect stash pop in post-checkout and restore working state
    static bool detectStashPop(const std::string& repoRoot,
                               const std::string& prevHead,
                               const std::string& newHead);

    // Generic: copy a note from one SHA to another
    static bool copyNote(const std::string& repoRoot,
                         const std::string& fromRef,
                         const std::string& fromSha,
                         const std::string& toSha);

    // Generic: read note content for a SHA
    static std::string readNote(const std::string& repoRoot,
                                  const std::string& ref,
                                  const std::string& sha);

    // Generic: write note content for a SHA
    static bool writeNote(const std::string& repoRoot,
                          const std::string& ref,
                          const std::string& sha,
                          const std::string& content);
};

} // namespace rewrite
} // namespace ghost

#endif // GHOST_REWRITE_PROCESSOR_HPP
