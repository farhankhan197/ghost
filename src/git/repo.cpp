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
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen("git rev-parse HEAD", "r"), pclose);
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

}
}