#include "notes.hpp"
#include <cstdio>
#include <memory>

namespace ghost {
namespace git {

std::string Notes::show(const std::string& ref, const std::string& commit_sha) {
    std::string cmd = "git notes show " + ref + " " + commit_sha;
    
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) {
        return "";
    }
    
    std::string result;
    char buffer[128];
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        result += buffer;
    }
    return result;
}

bool Notes::write(const std::string& ref, const std::string& commit_sha, const std::string& content) {
    std::string cmd = "git notes append -F - " + ref + " " + commit_sha;
    return system(cmd.c_str()) == 0;
}

bool Notes::exists(const std::string& ref, const std::string& commit_sha) {
    std::string result = show(ref, commit_sha);
    return !result.empty();
}

}
}