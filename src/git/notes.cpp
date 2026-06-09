#include "notes.hpp"
#include "engine.hpp"
#include "ref.hpp"
#include <cstdio>
#include <memory>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <ctime>

namespace ghost {
namespace git {

static std::string runGitCommand(const std::string& cmd) {
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) {
        return "";
    }
    
    std::string result;
    char buffer[4096];
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
    if (!Ref::isSafeNotesRef(ref) || !Ref::isSafeCommitish(commit_sha)) return "";
    return Engine::noteShow(".", ref, commit_sha);
}

bool Notes::write(const std::string& ref, const std::string& commit_sha, const std::string& content) {
    if (!Ref::isSafeNotesRef(ref) || !Ref::isSafeCommitish(commit_sha)) return false;
    std::string cmd = "git notes --ref=" + ref + " add -f -F - " + commit_sha;
    FILE* pipe = popen(cmd.c_str(), "w");
    if (!pipe) return false;
    fwrite(content.c_str(), 1, content.size(), pipe);
    return pclose(pipe) == 0;
}

bool Notes::exists(const std::string& ref, const std::string& commit_sha) {
    if (!Ref::isSafeNotesRef(ref) || !Ref::isSafeCommitish(commit_sha)) return false;
    return Engine::noteExists(".", ref, commit_sha);
}

std::map<std::string, std::string> Notes::showBatch(const std::string& ref) {
    return showBatch(ref, {});
}

std::map<std::string, std::string> Notes::showBatch(
    const std::string& ref,
    const std::vector<std::string>& commit_shas
) {
    std::map<std::string, std::string> result;
    if (!Ref::isSafeNotesRef(ref)) return result;
    for (const auto& sha : commit_shas) {
        if (!Ref::isSafeCommitish(sha)) return result;
    }
    
    result = Engine::noteShowBatch(".", ref, commit_shas);

    if (!commit_shas.empty()) {
        for (const auto& sha : commit_shas) {
            if (result.count(sha) > 0) continue;
            std::string content = show(ref, sha);
            if (!content.empty()) {
                result[sha] = content;
            }
        }
    }
    
    return result;
}

}
}
