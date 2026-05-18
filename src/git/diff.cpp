#include "diff.hpp"
#include <cstdio>
#include <memory>
#include <sstream>

namespace ghost {
namespace git {

std::vector<DiffFile> Diff::getChangedFiles(const std::string& range) {
    std::vector<DiffFile> result;

    std::string cmd = "git diff --numstat " + range + " -- .";
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return result;

    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        std::string line = buffer;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        if (line.empty()) continue;

        std::istringstream iss(line);
        DiffFile df;
        std::string addsStr, delsStr;
        if (iss >> addsStr >> delsStr >> df.path) {
            if (addsStr == "-") addsStr = "0";
            if (delsStr == "-") delsStr = "0";
            try {
                df.additions = std::stoi(addsStr);
                df.deletions = std::stoi(delsStr);
            } catch (...) {
                continue;
            }
            result.push_back(df);
        }
    }

    return result;
}

}
}
