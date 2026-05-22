#include "rewrite_log.hpp"
#include "persist/db.hpp"
#include <sstream>
#include <iostream>
#include <algorithm>

namespace ghost {
namespace rewrite {

std::string eventTypeToString(RewriteEventType type) {
    switch (type) {
        case RewriteEventType::RebaseStart: return "rebase_start";
        case RewriteEventType::RebaseComplete: return "rebase_complete";
        case RewriteEventType::RebaseAbort: return "rebase_abort";
        case RewriteEventType::CherryPickStart: return "cherry_pick_start";
        case RewriteEventType::CherryPickComplete: return "cherry_pick_complete";
        case RewriteEventType::CherryPickAbort: return "cherry_pick_abort";
        case RewriteEventType::Merge: return "merge";
        case RewriteEventType::MergeSquash: return "merge_squash";
        case RewriteEventType::Reset: return "reset";
        case RewriteEventType::CommitAmend: return "commit_amend";
        case RewriteEventType::Stash: return "stash";
        default: return "unknown";
    }
}

RewriteEventType parseEventType(const std::string& s) {
    if (s == "rebase_start") return RewriteEventType::RebaseStart;
    if (s == "rebase_complete") return RewriteEventType::RebaseComplete;
    if (s == "rebase_abort") return RewriteEventType::RebaseAbort;
    if (s == "cherry_pick_start") return RewriteEventType::CherryPickStart;
    if (s == "cherry_pick_complete") return RewriteEventType::CherryPickComplete;
    if (s == "cherry_pick_abort") return RewriteEventType::CherryPickAbort;
    if (s == "merge") return RewriteEventType::Merge;
    if (s == "merge_squash") return RewriteEventType::MergeSquash;
    if (s == "reset") return RewriteEventType::Reset;
    if (s == "commit_amend") return RewriteEventType::CommitAmend;
    if (s == "stash") return RewriteEventType::Stash;
    return RewriteEventType::Unknown;
}

// --- RebaseStartEvent ---

std::string RebaseStartEvent::toJson() const {
    std::ostringstream oss;
    oss << "{\"original_head\":\"" << original_head << "\",";
    oss << "\"is_interactive\":" << (is_interactive ? "true" : "false") << ",";
    oss << "\"onto_head\":\"" << onto_head << "\"}";
    return oss.str();
}

std::optional<RebaseStartEvent> RebaseStartEvent::fromJson(const std::string& json) {
    RebaseStartEvent ev;
    size_t pos = json.find("\"original_head\":\"");
    if (pos == std::string::npos) return std::nullopt;
    pos += 18;
    size_t end = json.find("\"", pos);
    if (end == std::string::npos) return std::nullopt;
    ev.original_head = json.substr(pos, end - pos);

    pos = json.find("\"is_interactive\":");
    if (pos != std::string::npos) {
        pos += 18;
        ev.is_interactive = (json.substr(pos, 4) == "true");
    }

    pos = json.find("\"onto_head\":\"");
    if (pos != std::string::npos) {
        pos += 14;
        end = json.find("\"", pos);
        if (end != std::string::npos) ev.onto_head = json.substr(pos, end - pos);
    }
    return ev;
}

// --- RebaseCompleteEvent ---

std::string RebaseCompleteEvent::toJson() const {
    std::ostringstream oss;
    oss << "{\"original_head\":\"" << original_head << "\",";
    oss << "\"new_head\":\"" << new_head << "\",";
    oss << "\"is_interactive\":" << (is_interactive ? "true" : "false") << ",";
    oss << "\"original_commits\":[";
    for (size_t i = 0; i < original_commits.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "\"" << original_commits[i] << "\"";
    }
    oss << "],\"new_commits\":[";
    for (size_t i = 0; i < new_commits.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "\"" << new_commits[i] << "\"";
    }
    oss << "]}";
    return oss.str();
}

std::optional<RebaseCompleteEvent> RebaseCompleteEvent::fromJson(const std::string& json) {
    RebaseCompleteEvent ev;
    // Simple parser: extract arrays
    size_t pos = json.find("\"original_head\":\"");
    if (pos != std::string::npos) {
        pos += 18;
        size_t end = json.find("\"", pos);
        if (end != std::string::npos) ev.original_head = json.substr(pos, end - pos);
    }
    pos = json.find("\"new_head\":\"");
    if (pos != std::string::npos) {
        pos += 12;
        size_t end = json.find("\"", pos);
        if (end != std::string::npos) ev.new_head = json.substr(pos, end - pos);
    }
    pos = json.find("\"is_interactive\":");
    if (pos != std::string::npos) {
        pos += 18;
        ev.is_interactive = (json.substr(pos, 4) == "true");
    }
    // Parse original_commits array
    pos = json.find("\"original_commits\":[");
    if (pos != std::string::npos) {
        pos += 20;
        size_t end = json.find("]", pos);
        if (end != std::string::npos) {
            std::string arr = json.substr(pos, end - pos);
            size_t p = 0;
            while ((p = arr.find("\"", p)) != std::string::npos) {
                p++;
                size_t e = arr.find("\"", p);
                if (e == std::string::npos) break;
                ev.original_commits.push_back(arr.substr(p, e - p));
                p = e + 1;
            }
        }
    }
    // Parse new_commits array
    pos = json.find("\"new_commits\":[");
    if (pos != std::string::npos) {
        pos += 15;
        size_t end = json.find("]", pos);
        if (end != std::string::npos) {
            std::string arr = json.substr(pos, end - pos);
            size_t p = 0;
            while ((p = arr.find("\"", p)) != std::string::npos) {
                p++;
                size_t e = arr.find("\"", p);
                if (e == std::string::npos) break;
                ev.new_commits.push_back(arr.substr(p, e - p));
                p = e + 1;
            }
        }
    }
    return ev;
}

// --- CommitAmendEvent ---

std::string CommitAmendEvent::toJson() const {
    std::ostringstream oss;
    oss << "{\"original_commit\":\"" << original_commit << "\",";
    oss << "\"amended_commit_sha\":\"" << amended_commit_sha << "\"}";
    return oss.str();
}

std::optional<CommitAmendEvent> CommitAmendEvent::fromJson(const std::string& json) {
    CommitAmendEvent ev;
    size_t pos = json.find("\"original_commit\":\"");
    if (pos != std::string::npos) {
        pos += 20;
        size_t end = json.find("\"", pos);
        if (end != std::string::npos) ev.original_commit = json.substr(pos, end - pos);
    }
    pos = json.find("\"amended_commit_sha\":\"");
    if (pos != std::string::npos) {
        pos += 23;
        size_t end = json.find("\"", pos);
        if (end != std::string::npos) ev.amended_commit_sha = json.substr(pos, end - pos);
    }
    return ev;
}

// --- ResetEvent ---

std::string ResetEvent::toJson() const {
    std::ostringstream oss;
    oss << "{\"kind\":\"" << kind << "\",";
    oss << "\"new_head_sha\":\"" << new_head_sha << "\",";
    oss << "\"old_head_sha\":\"" << old_head_sha << "\"}";
    return oss.str();
}

std::optional<ResetEvent> ResetEvent::fromJson(const std::string& json) {
    ResetEvent ev;
    size_t pos = json.find("\"kind\":\"");
    if (pos != std::string::npos) {
        pos += 9;
        size_t end = json.find("\"", pos);
        if (end != std::string::npos) ev.kind = json.substr(pos, end - pos);
    }
    pos = json.find("\"new_head_sha\":\"");
    if (pos != std::string::npos) {
        pos += 16;
        size_t end = json.find("\"", pos);
        if (end != std::string::npos) ev.new_head_sha = json.substr(pos, end - pos);
    }
    pos = json.find("\"old_head_sha\":\"");
    if (pos != std::string::npos) {
        pos += 16;
        size_t end = json.find("\"", pos);
        if (end != std::string::npos) ev.old_head_sha = json.substr(pos, end - pos);
    }
    return ev;
}

// --- RewriteLog API ---

int RewriteLog::append(const std::string& repoRoot, RewriteEventType type, const std::string& jsonPayload) {
    auto* db = persist::getRepoDb(repoRoot);
    if (!db) return -1;
    return db->appendRewriteEvent(eventTypeToString(type), jsonPayload);
}

std::vector<RewriteEvent> RewriteLog::load(const std::string& repoRoot, int maxCount) {
    auto* db = persist::getRepoDb(repoRoot);
    if (!db) return {};
    auto rows = db->loadRewriteEvents(maxCount);
    std::vector<RewriteEvent> result;
    for (const auto& row : rows) {
        RewriteEvent ev;
        ev.type = parseEventType(row.event_type);
        ev.json_payload = row.json_data;
        ev.timestamp = row.timestamp;
        result.push_back(ev);
    }
    return result;
}

bool RewriteLog::trim(const std::string& repoRoot, int maxCount) {
    auto* db = persist::getRepoDb(repoRoot);
    if (!db) return false;
    return db->trimRewriteEvents(maxCount);
}

std::vector<std::pair<std::string, std::string>> RewriteLog::readStdinMappings() {
    std::vector<std::pair<std::string, std::string>> result;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        std::string oldSha = line.substr(0, sp);
        std::string newSha = line.substr(sp + 1);
        result.emplace_back(oldSha, newSha);
    }
    return result;
}

} // namespace rewrite
} // namespace ghost
