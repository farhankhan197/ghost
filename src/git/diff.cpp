#include "command.hpp"
#include "diff.hpp"
#include "path.hpp"
#include "ref.hpp"
#include <sstream>
#include <vector>

namespace ghost {
namespace git {

namespace {

static std::vector<std::string> splitRangeArgs(const std::string& range) {
    std::vector<std::string> args;
    std::istringstream stream(range);
    std::string arg;
    while (stream >> arg) {
        args.push_back(arg);
    }
    return args;
}

}

std::vector<DiffFile> Diff::getChangedFiles(const std::string& range) {
    std::vector<DiffFile> result;

    std::vector<std::string> args = {"diff", "--numstat"};
    auto rangeArgs = splitRangeArgs(range);
    args.insert(args.end(), rangeArgs.begin(), rangeArgs.end());
    args.push_back("--");
    args.push_back(".");

    std::istringstream output(Command::capture("", args));
    std::string line;
    while (std::getline(output, line)) {
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

static void addRangeLines(std::map<std::string, std::vector<int>>& fileLines,
                          const std::string& file,
                          int start,
                          int count) {
    if (file.empty() || start < 1 || count <= 0) return;
    auto& lines = fileLines[file];
    for (int i = 0; i < count; ++i) {
        lines.push_back(start + i);
    }
}

static DiffRanges parseUnifiedZeroDiff(const std::string& output, const std::string& repoRoot) {
    std::map<std::string, std::vector<int>> addedLines;
    std::map<std::string, std::vector<int>> deletedLines;
    std::map<std::string, std::string> renames;

    std::istringstream stream(output);
    std::string line;
    std::string oldFile;
    std::string newFile;
    std::string renameFrom;
    std::string renameTo;

    while (std::getline(stream, line)) {
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }

        if (line.rfind("diff --git ", 0) == 0) {
            if (!renameFrom.empty() && !renameTo.empty()) {
                renames[renameTo] = renameFrom;
            }
            oldFile.clear();
            newFile.clear();
            renameFrom.clear();
            renameTo.clear();
            continue;
        }

        if (line.rfind("rename from ", 0) == 0) {
            renameFrom = Path::normalizeRepoPathOrEmpty(line.substr(12), repoRoot);
            continue;
        }
        if (line.rfind("rename to ", 0) == 0) {
            renameTo = Path::normalizeRepoPathOrEmpty(line.substr(10), repoRoot);
            continue;
        }

        if (line.rfind("--- ", 0) == 0) {
            std::string file = line.substr(4);
            oldFile = (file == "/dev/null") ? "" : Path::normalizeRepoPathOrEmpty(file, repoRoot);
            continue;
        }
        if (line.rfind("+++ ", 0) == 0) {
            std::string file = line.substr(4);
            newFile = (file == "/dev/null") ? "" : Path::normalizeRepoPathOrEmpty(file, repoRoot);
            continue;
        }

        if (line.rfind("@@ ", 0) != 0) continue;

        size_t minusPos = line.find('-');
        size_t plusPos = line.find('+');
        if (minusPos == std::string::npos || plusPos == std::string::npos) continue;

        size_t oldEnd = line.find(' ', minusPos);
        size_t newEnd = line.find(' ', plusPos);
        if (oldEnd == std::string::npos || newEnd == std::string::npos) continue;

        std::string oldPart = line.substr(minusPos + 1, oldEnd - minusPos - 1);
        std::string newPart = line.substr(plusPos + 1, newEnd - plusPos - 1);

        auto parsePart = [](const std::string& part, int& start, int& count) {
            start = 0;
            count = 1;
            size_t comma = part.find(',');
            try {
                if (comma == std::string::npos) {
                    start = std::stoi(part);
                } else {
                    start = std::stoi(part.substr(0, comma));
                    count = std::stoi(part.substr(comma + 1));
                }
            } catch (...) {
                start = 0;
                count = 0;
            }
        };

        int oldStart = 0;
        int oldCount = 0;
        int newStart = 0;
        int newCount = 0;
        parsePart(oldPart, oldStart, oldCount);
        parsePart(newPart, newStart, newCount);

        addRangeLines(deletedLines, oldFile, oldStart, oldCount);
        addRangeLines(addedLines, newFile, newStart, newCount);
    }

    if (!renameFrom.empty() && !renameTo.empty()) {
        renames[renameTo] = renameFrom;
    }

    DiffRanges result;
    result.renames = renames;
    for (const auto& [file, lines] : addedLines) {
        result.added[file] = note::LineRangeSet::fromLines(lines);
    }
    for (const auto& [file, lines] : deletedLines) {
        result.deleted[file] = note::LineRangeSet::fromLines(lines);
    }
    return result;
}

DiffRanges Diff::getChangedRanges(const std::string& repoRoot, const std::string& range) {
    std::vector<std::string> args = {"diff", "--patch", "--find-renames=20%", "--no-ext-diff", "--unified=0"};
    auto rangeArgs = splitRangeArgs(range);
    args.insert(args.end(), rangeArgs.begin(), rangeArgs.end());
    args.push_back("--");
    args.push_back(".");
    return parseUnifiedZeroDiff(Command::capture(repoRoot, args), repoRoot);
}

DiffRanges Diff::getCommitRanges(const std::string& repoRoot, const std::string& commitSha) {
    if (!Ref::isSafeCommitish(commitSha)) return DiffRanges{};
    std::vector<std::string> args = {"diff-tree", "--root", "--patch", "--find-renames=20%", "--no-ext-diff", "--unified=0", commitSha, "--", "."};
    return parseUnifiedZeroDiff(Command::capture(repoRoot, args), repoRoot);
}

}
}
