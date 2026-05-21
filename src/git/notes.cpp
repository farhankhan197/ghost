#include "notes.hpp"
#include <cstdio>
#include <memory>
#include <cstdlib>
#include <fstream>
#include <filesystem>

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
    std::string cmd = "git notes --ref=" + ref + " show " + commit_sha + " 2>nul";
    
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) {
        return "";
    }
    
    std::string result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        result += buffer;
    }
    
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    
    return result;
}

bool Notes::write(const std::string& ref, const std::string& commit_sha, const std::string& content) {
    std::string tmpPath = std::filesystem::temp_directory_path().string() + "/ghost-note-tmp.txt";
    std::ofstream tmpFile(tmpPath);
    if (!tmpFile.is_open()) return false;
    tmpFile << content;
    tmpFile.close();

    std::string cmd = "git notes --ref=" + ref + " add -f -F \"" + tmpPath + "\" " + commit_sha;
    int result = system(cmd.c_str());

    std::filesystem::remove(tmpPath);
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