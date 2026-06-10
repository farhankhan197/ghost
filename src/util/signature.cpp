#include "signature.hpp"
#include "process.hpp"

#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace ghost {
namespace util {

std::string hashFile(const std::string& path) {
    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec)) return "";
    return Process::capture("git hash-object \"" + path + "\" 2>&1");
}

std::string hashText(const std::string& repoRoot, const std::string& content) {
    std::error_code ec;
    std::filesystem::path tmpDir = std::filesystem::path(repoRoot) / ".git" / "ghost";
    std::filesystem::create_directories(tmpDir, ec);
    std::filesystem::path tmpPath = tmpDir / ("hash-" + std::to_string(std::time(nullptr)) + ".txt");
    {
        std::ofstream out(tmpPath);
        if (!out.is_open()) return "";
        out << content;
    }
    std::string digest = hashFile(tmpPath.string());
    std::filesystem::remove(tmpPath, ec);
    return digest;
}

std::map<std::string, std::string> parseSimpleSignature(const std::string& content) {
    std::map<std::string, std::string> result;
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ')) value.pop_back();
        result[key] = value;
    }
    return result;
}

long long parseSignatureTs(const std::map<std::string, std::string>& sig) {
    auto it = sig.find("ts");
    if (it == sig.end()) return 0;
    try {
        return std::stoll(it->second);
    } catch (...) {
        return 0;
    }
}

}
}
