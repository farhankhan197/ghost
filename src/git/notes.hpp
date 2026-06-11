#ifndef GHOST_GIT_NOTES_HPP
#define GHOST_GIT_NOTES_HPP

#include <string>
#include <map>
#include <vector>

namespace ghost {
namespace git {

class Notes {
public:
    static std::string show(const std::string& ref, const std::string& commit_sha);
    static std::string show(const std::string& repo_root, const std::string& ref, const std::string& commit_sha);
    static bool write(const std::string& ref, const std::string& commit_sha, const std::string& content);
    static bool write(const std::string& repo_root, const std::string& ref, const std::string& commit_sha, const std::string& content);
    static bool exists(const std::string& ref, const std::string& commit_sha);
    static bool exists(const std::string& repo_root, const std::string& ref, const std::string& commit_sha);
    
    // Batch retrieve all notes for a ref in a single subprocess
    // Returns map of commit_sha -> note_content
    static std::map<std::string, std::string> showBatch(const std::string& ref);
    static std::map<std::string, std::string> showBatch(const std::string& repo_root, const std::string& ref);
    
    // Batch retrieve only specific commit SHAs
    static std::map<std::string, std::string> showBatch(const std::string& ref, const std::vector<std::string>& commit_shas);
    static std::map<std::string, std::string> showBatch(
        const std::string& repo_root,
        const std::string& ref,
        const std::vector<std::string>& commit_shas
    );
};

}
}
#endif
