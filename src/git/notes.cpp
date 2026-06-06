#include "notes.hpp"
#include <cstdio>
#include <memory>
#include <sstream>

namespace ghost {
namespace git {

static std::string runGitCommand(const std::string& cmd) {
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) {
        return "";
    }
    
    std::string result;
    char buffer[4096];
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
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        result += buffer;
    }
    
    if (!result.empty() && result.back() == '\n') {
        result.pop_back();
    }
    
    return result;
}

bool Notes::write(const std::string& ref, const std::string& commit_sha, const std::string& content) {
    std::string cmd = "git notes --ref=" + ref + " add -f -F - " + commit_sha;
    FILE* pipe = popen(cmd.c_str(), "w");
    if (!pipe) return false;
    fwrite(content.c_str(), 1, content.size(), pipe);
    return pclose(pipe) == 0;
}

bool Notes::exists(const std::string& ref, const std::string& commit_sha) {
    std::string cmd = "git notes --ref=" + ref + " list " + commit_sha;
    
    std::string result = runGitCommand(cmd);
    return !result.empty();
}

std::map<std::string, std::string> Notes::showBatch(const std::string& ref) {
    return showBatch(ref, {});
}

std::map<std::string, std::string> Notes::showBatch(
    const std::string& ref,
    const std::vector<std::string>& commit_shas
) {
    std::map<std::string, std::string> result;
    
    // Step 1: Get all note mappings via git notes list
    std::map<std::string, std::string> blobToCommit;  // blob_sha -> commit_sha
    {
        std::string cmd = "git notes --ref=" + ref + " list";
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
        if (!pipe) return result;
        
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
            std::string line = buffer;
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
                line.pop_back();
            }
            if (line.empty()) continue;
            
            size_t space = line.find(' ');
            if (space != std::string::npos && space > 0) {
                std::string blobSha = line.substr(0, space);
                std::string commitSha = line.substr(space + 1);
                
                // If specific SHAs requested, filter
                if (!commit_shas.empty()) {
                    bool found = false;
                    for (const auto& wanted : commit_shas) {
                        if (wanted == commitSha) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) continue;
                }
                
                blobToCommit[blobSha] = commitSha;
            }
        }
    }
    
    if (blobToCommit.empty()) return result;
    
    // Step 2: Build inline echo command to pipe blob SHAs to git cat-file --batch
    // Avoids temp file path issues on MSYS2/Windows
    std::string shasCmd;
    for (auto it = blobToCommit.begin(); it != blobToCommit.end(); ++it) {
        shasCmd += (it == blobToCommit.begin() ? "" : " & ") + std::string("echo ") + it->first;
    }
    
    std::string batchOutput;
    {
        std::string cmd = "(" + shasCmd + ") | git cat-file --batch 2>nul";
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
        if (!pipe) {
            return result;
        }
        
        char buffer[4096];
        while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
            batchOutput += buffer;
        }
    }
    
    if (batchOutput.empty()) return result;
    
    // Step 4: Parse batch output
    // Format per entry: "<sha> blob <size>\n<content>"
    // We need to parse the size header, then read exactly <size> bytes of content
    size_t pos = 0;
    while (pos < batchOutput.size()) {
        // Find end of header line
        size_t headerEnd = batchOutput.find('\n', pos);
        if (headerEnd == std::string::npos) break;
        
        std::string header = batchOutput.substr(pos, headerEnd - pos);
        pos = headerEnd + 1;  // skip past the newline
        
        // Parse header: "<sha> blob <size>"
        size_t firstSpace = header.find(' ');
        if (firstSpace == std::string::npos) continue;
        
        size_t secondSpace = header.find(' ', firstSpace + 1);
        if (secondSpace == std::string::npos) continue;
        
        std::string blobSha = header.substr(0, firstSpace);
        std::string type = header.substr(firstSpace + 1, secondSpace - firstSpace - 1);
        
        if (type != "blob" && type != "missing") continue;
        
        // If missing, content is empty and there's a newline
        if (type == "missing") {
            if (pos < batchOutput.size() && batchOutput[pos] == '\n') {
                pos++;
            }
            continue;
        }
        
        int contentSize = 0;
        try {
            contentSize = std::stoi(header.substr(secondSpace + 1));
        } catch (...) {
            continue;
        }
        
        // Look up this blob in our mapping
        auto it = blobToCommit.find(blobSha);
        if (it == blobToCommit.end()) {
            // Skip content for unknown blobs
            pos += contentSize;
            if (pos < batchOutput.size() && batchOutput[pos] == '\n') {
                pos++;
            }
            continue;
        }
        
        // Extract content (exactly contentSize bytes)
        std::string content;
        if (contentSize > 0 && pos + contentSize <= batchOutput.size()) {
            content = batchOutput.substr(pos, contentSize);
            pos += contentSize;
        }
        
        // Skip trailing newline after content
        if (pos < batchOutput.size() && batchOutput[pos] == '\n') {
            pos++;
        }
        
        result[it->second] = content;
    }
    
    return result;
}

}
}
