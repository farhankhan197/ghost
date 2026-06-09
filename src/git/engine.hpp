#ifndef GHOST_GIT_ENGINE_HPP
#define GHOST_GIT_ENGINE_HPP

#include <map>
#include <string>
#include <vector>

namespace ghost {
namespace git {

class Engine {
public:
    static std::string discoverRoot(const std::string& startPath = ".");
    static std::string headSha(const std::string& repoRoot = ".");
    static std::string configString(const std::string& repoRoot, const std::string& key);
    static std::string resolveCommit(const std::string& repoRoot, const std::string& commitish);
    static std::string commitAuthor(const std::string& repoRoot, const std::string& sha);
    static std::map<std::string, std::string> commitAuthors(const std::string& repoRoot, const std::vector<std::string>& shas);
    static std::vector<std::string> revList(const std::string& repoRoot, const std::string& range);
    static std::vector<std::string> changedFiles(const std::string& repoRoot, const std::string& commitSha);
    static std::vector<std::string> treeFiles(const std::string& repoRoot, const std::string& commitSha);
    static std::string showBlobAtRef(const std::string& repoRoot, const std::string& ref, const std::string& path);
    static std::string noteShow(const std::string& repoRoot, const std::string& notesRef, const std::string& commitSha);
    static bool noteExists(const std::string& repoRoot, const std::string& notesRef, const std::string& commitSha);
    static std::map<std::string, std::string> noteList(const std::string& repoRoot, const std::string& notesRef);
    static std::map<std::string, std::string> noteShowBatch(
        const std::string& repoRoot,
        const std::string& notesRef,
        const std::vector<std::string>& commitShas = {}
    );
};

}
}

#endif
