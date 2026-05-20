#ifndef GHOST_AUDIT_AUDITOR_HPP
#define GHOST_AUDIT_AUDITOR_HPP

#include <string>
#include <vector>
#include "blame_overlay.hpp"
#include "aggregator.hpp"
#include "policy.hpp"
#include "../config/ghost_config.hpp"

namespace ghost {
namespace audit {

struct AuditReport {
    AuditSummary summary;
    PolicyResult policy;
    bool json;
};

struct FileEntity {
    std::string agent;
    std::string model;
    int lines;
};

struct FileBlameSummary {
    std::string file_path;
    int total_lines;
    int ai_lines;
    std::string primary_author;
    std::string primary_entity;
    std::string commit_entity;
    std::vector<FileEntity> entities;
    bool in_commit;
};

struct CodebaseSummary {
    std::string target_sha;
    std::vector<FileBlameSummary> files;
    int total_lines;
    int ai_lines;
    int commit_ai_lines;
    int commit_total_lines;
};

struct CodebaseReport {
    CodebaseSummary summary;
    PolicyResult policy;
    bool json;
};

class Auditor {
public:
    static AuditReport run(
        const std::string& repoRoot,
        const std::string& range,
        int thresholdOverride,
        bool jsonOutput
    );

    static AuditReport runFromList(
        const std::string& repoRoot,
        const std::vector<std::string>& commitShas,
        int thresholdOverride,
        bool jsonOutput
    );

    static std::vector<std::string> getCommitsWithGhostNotes();
    static PolicyResult checkPending(const std::string& repoRoot, int thresholdOverride = -1);

    static CodebaseReport runCodebaseBlame(
        const std::string& repoRoot,
        const std::string& target,
        int thresholdOverride,
        bool jsonOutput
    );
};


}
}

#endif
