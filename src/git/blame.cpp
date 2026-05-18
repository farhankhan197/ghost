#include "blame.hpp"
#include <cstdio>
#include <memory>
#include <sstream>

namespace ghost {
namespace git {

std::map<int, std::string> Blame::getLineAuthorMap(const std::string& file_path) {
    std::map<int, std::string> result;

    std::string cmd = "git blame --line-porcelain -- \"" + file_path + "\" 2>nul";
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return result;

    std::string currentCommit;
    int currentLine = 0;
    bool inHeader = false;

    std::string line;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        line = buffer;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }

        if (line.empty()) continue;

        if (!inHeader) {
            size_t space = line.find(' ');
            if (space != std::string::npos) {
                std::string first = line.substr(0, space);
                if (first.size() >= 6) {
                    bool isSha = true;
                    for (char c : first) {
                        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                            isSha = false;
                            break;
                        }
                    }
                    if (isSha) {
                        currentCommit = first;
                        size_t secondSpace = line.find(' ', space + 1);
                        if (secondSpace != std::string::npos) {
                            try {
                                currentLine = std::stoi(line.substr(space + 1, secondSpace - space - 1));
                            } catch (...) {}
                        }
                        inHeader = true;
                        continue;
                    }
                }
            }
        }

        if (inHeader && line.find("filename ") == 0) {
            inHeader = false;
            continue;
        }

        if (inHeader) continue;

        if (!currentCommit.empty() && currentLine > 0) {
            result[currentLine] = currentCommit;
        }

        currentLine++;
    }

    return result;
}

}
}
