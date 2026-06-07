#include "session.hpp"
#include "working_log.hpp"
#include <cstdio>
#include <memory>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <ctime>
#include <random>

namespace fs = std::filesystem;

namespace ghost {
namespace checkpoint {

static std::string runCommand(const std::string& cmd) {
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return "";

    std::string result;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        result += buffer;
    }

    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }

    return result;
}

std::string Session::generateId() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    
    const char hex[] = "0123456789abcdef";
    std::string id = "sess_";
    for (int i = 0; i < 12; ++i) {
        id += hex[dis(gen)];
    }
    return id;
}

std::string Session::getGitAuthor(const std::string& repoRoot) {
    std::string cachePath = WorkingLog::getGhostDir(repoRoot) + "/author.cache";
    std::ifstream cacheFile(cachePath);
    if (cacheFile.is_open()) {
        std::string author;
        std::getline(cacheFile, author);
        if (!author.empty()) return author;
    }

    std::string name = runCommand("git config user.name");
    std::string email = runCommand("git config user.email");
    std::string author = "unknown";
    if (!name.empty() && !email.empty()) {
        author = name + " <" + email + ">";
    }
    
    std::ofstream out(cachePath);
    if (out.is_open()) {
        out << author;
    }
    
    return author;
}


FileChanges Session::computeChanges(const std::string& snapshotPath, const std::string& currentPath, const std::string& filePath) {
    FileChanges result;
    result.file_path = filePath;
    result.additions = 0;
    result.deletions = 0;

    std::error_code ec;
    bool snapshotExists = fs::exists(snapshotPath, ec);
    bool currentExists = fs::exists(currentPath, ec);

    if (!snapshotExists && currentExists) {
        std::ifstream current(currentPath);
        int lineCount = 0;
        std::string line;
        while (std::getline(current, line)) {
            lineCount++;
        }
        if (lineCount > 0) {
            result.additions = lineCount;
            result.added_ranges = note::LineRangeSet::parse("1-" + std::to_string(lineCount));
        }
        return result;
    }

    if (snapshotExists && !currentExists) {
        std::ifstream snapshot(snapshotPath);
        std::string line;
        while (std::getline(snapshot, line)) {
            result.deletions++;
        }
        return result;
    }

    if (!snapshotExists && !currentExists) {
        return result;
    }

    std::string cmd = "git diff --no-index --unified=0 -- \"" + snapshotPath + "\" \"" + currentPath + "\"";
    std::string output = runCommand(cmd);

    if (output.empty()) return result;

    std::istringstream stream(output);
    std::string line;
    std::vector<std::string> rangeParts;

    while (std::getline(stream, line)) {
        if (line.size() < 4 || line.substr(0, 2) != "@@") continue;

        size_t minusPos = line.find('-', 2);
        size_t plusPos = line.find('+', 2);
        if (minusPos == std::string::npos || plusPos == std::string::npos) continue;

        std::string oldPart = line.substr(minusPos + 1);
        size_t oldEnd = oldPart.find(' ');
        if (oldEnd != std::string::npos) oldPart = oldPart.substr(0, oldEnd);

        int oldCount = 1;
        size_t oldComma = oldPart.find(',');
        if (oldComma != std::string::npos) {
            try { oldCount = std::stoi(oldPart.substr(oldComma + 1)); } catch (...) {}
        }

        std::string newPart = line.substr(plusPos + 1);
        size_t newEnd = newPart.find(' ');
        if (newEnd != std::string::npos) newPart = newPart.substr(0, newEnd);

        int newStart = 0, newCount = 1;
        size_t newComma = newPart.find(',');
        if (newComma != std::string::npos) {
            try { newStart = std::stoi(newPart.substr(0, newComma)); } catch (...) {}
            try { newCount = std::stoi(newPart.substr(newComma + 1)); } catch (...) {}
        } else {
            try { newStart = std::stoi(newPart); } catch (...) {}
        }

        if (newCount > 0) {
            rangeParts.push_back(std::to_string(newStart) + "-" + std::to_string(newStart + newCount - 1));
            result.additions += newCount;
        }

        if (oldCount > 0) {
            result.deletions += oldCount;
        }
    }

    std::string rangesStr;
    for (size_t i = 0; i < rangeParts.size(); ++i) {
        if (i > 0) rangesStr += ",";
        rangesStr += rangeParts[i];
    }

    if (!rangesStr.empty()) {
        result.added_ranges = note::LineRangeSet::parse(rangesStr);
    }

    return result;
}

}
}
