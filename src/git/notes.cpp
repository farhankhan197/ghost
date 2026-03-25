#include "notes.hpp"
#include <cstdio>
#include <memory>
#include <cstdlib>

namespace ghost {
namespace git {

static std::string runGitCommand(const std::string& cmd) {
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) {
        return "";
    }
    
    std::string result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        result += buffer;
    }
    
    // Trim trailing newline
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    
    return result;
}

std::string Notes::show(const std::string& ref, const std::string& commit_sha) {
    // Command: git notes --ref=refs/notes/ghost show <sha>
    // Returns empty string if no note exists
    std::string cmd = "git notes --ref=" + ref + " show " + commit_sha;
    
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) {
        return "";
    }
    
    std::string result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        result += buffer;
    }
    
    // Trim trailing newline
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    
    return result;
}

bool Notes::write(const std::string& ref, const std::string& commit_sha, const std::string& content) {
    // Using git notes add with -f (force) and -m (message)
    // Escape double quotes in content for shell
    std::string escaped = content;
    size_t pos = 0;
    while ((pos = escaped.find("\"", pos)) != std::string::npos) {
        escaped.replace(pos, 1, "\\\"");
        pos += 2;
    }
    
    std::string cmd = "git notes --ref=" + ref + " add -f -m \"" + escaped + "\" " + commit_sha;
    
    int result = system(cmd.c_str());
    return result == 0;
}

bool Notes::exists(const std::string& ref, const std::string& commit_sha) {
    // Command: git notes --ref=refs/notes/ghost list <sha>
    // Returns the note blob SHA if exists, empty if not
    std::string cmd = "git notes --ref=" + ref + " list " + commit_sha;
    
    std::string result = runGitCommand(cmd);
    return !result.empty();
}   

}
}