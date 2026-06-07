#include "repo.hpp"
#include <cstdio>
#include <memory>

namespace ghost {
namespace git {

std::string Repo::getRoot() {
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen("git rev-parse --show-toplevel", "r"), pclose);
    if (!pipe) return "";
    
    std::string result;
    char buffer[128];
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        result += buffer;
    }
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    return result;
}

std::string Repo::getHead() {
#ifdef _WIN32
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen("git rev-parse --verify HEAD 2>nul", "r"), pclose);
#else
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen("git rev-parse --verify HEAD 2>/dev/null", "r"), pclose);
#endif
    if (!pipe) return "";
    
    std::string result;
    char buffer[128];
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        result += buffer;
    }
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    return result;
}

bool Repo::isRepo() {
    return !getRoot().empty();
}

std::string Repo::getUserEmail() {
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen("git config user.email", "r"), pclose);
    if (!pipe) return "";
    std::string result;
    char buffer[128];
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        result += buffer;
    }
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    return result;
}

}
}
