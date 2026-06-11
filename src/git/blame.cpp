#include "blame.hpp"
#include "command.hpp"
#include <sstream>
#include <vector>

namespace ghost {
namespace git {

BlameResult Blame::getLineAuthorMap(const std::string& file_path) {
    return getLineAuthorMap("", file_path, "");
}

BlameResult Blame::getLineAuthorMap(const std::string& file_path, const std::string& commit_sha) {
    return getLineAuthorMap("", file_path, commit_sha);
}

BlameResult Blame::getLineAuthorMap(
    const std::string& repo_root,
    const std::string& file_path,
    const std::string& commit_sha
) {
    BlameResult result;

    std::vector<std::string> args;
    if (commit_sha.empty()) {
        args = {"blame", "--line-porcelain", "--", file_path};
    } else {
        args = {"blame", "--line-porcelain", commit_sha, "--", file_path};
    }
    auto process = Command::run(repo_root, args);

    std::string currentCommit;
    int currentLine = 0;
    int currentSourceLine = 0;
    std::string currentFilename = file_path;
    bool inHeader = false;

    std::string line;
    std::istringstream output(process.stdoutText);
    while (std::getline(output, line)) {
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
                        // Format: <sha> <src-line> <dst-line> [count]
                        size_t secondSpace = line.find(' ', space + 1);
                        size_t thirdSpace = line.find(' ', secondSpace + 1);
                        if (secondSpace != std::string::npos) {
                            try {
                                currentSourceLine = std::stoi(line.substr(space + 1, secondSpace - space - 1));
                                size_t destStart = secondSpace + 1;
                                size_t destLen = thirdSpace == std::string::npos
                                    ? std::string::npos
                                    : thirdSpace - destStart;
                                currentLine = std::stoi(line.substr(destStart, destLen));
                            } catch (...) {}
                        }
                        inHeader = true;
                        continue;
                    }
                }
            }
        }

        if (inHeader && line.find("filename ") == 0) {
            currentFilename = line.substr(9);
            inHeader = false;
            continue;
        }

        if (inHeader) continue;

        if (!currentCommit.empty() && currentLine > 0) {
            // Ensure vector is large enough ( blame lines are sequential )
            if ((size_t)currentLine > result.lines.size()) {
                result.lines.resize(currentLine);
                result.filenames.resize(currentLine);
                result.source_lines.resize(currentLine);
            }
            result.lines[currentLine - 1] = currentCommit;
            result.filenames[currentLine - 1] = currentFilename;
            result.source_lines[currentLine - 1] = currentSourceLine > 0 ? currentSourceLine : currentLine;
        }

        currentLine++;
    }

    return result;
}

}
}
