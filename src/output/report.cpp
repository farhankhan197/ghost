#include "report.hpp"
#include "style.hpp"
#include <sstream>
#include <iostream>
#include <cstdlib>
#include <iomanip>

namespace ghost {
namespace output {

static size_t visibleLength(const std::string& s) {
    size_t len = 0;
    bool inEscape = false;
    for (size_t i = 0; i < s.length(); ++i) {
        if (s[i] == '\033') {
            inEscape = true;
        } else if (inEscape) {
            if (s[i] == 'm') inEscape = false;
        } else {
            len++;
        }
    }
    return len;
}

static std::string truncateVisible(const std::string& s, size_t maxWidth) {
    if (visibleLength(s) <= maxWidth) return s;
    
    size_t vlen = 0;
    std::string result;
    bool inEscape = false;
    
    for (size_t i = 0; i < s.length(); ++i) {
        if (s[i] == '\033') {
            inEscape = true;
            result += s[i];
        } else if (inEscape) {
            result += s[i];
            if (s[i] == 'm') inEscape = false;
        } else {
            if (vlen < maxWidth - 2) {
                result += s[i];
                vlen++;
            } else if (vlen == maxWidth - 2) {
                result += "..";
                vlen += 2;
                // Don't add any more non-escape characters
            }
        }
    }
    // Ensure ANSI codes are reset if we truncated mid-sequence (though loop handles it)
    return result;
}

static std::string padRight(const std::string& s, size_t width) {
    size_t vlen = visibleLength(s);
    if (vlen >= width) return s;
    return s + std::string(width - vlen, ' ');
}

std::string Report::formatCLI(const audit::AuditSummary& summary, const audit::PolicyResult& policy, bool showDetail) {
    std::ostringstream out;
    auto mascot = Style::mascot();

    out << Style::header("AUDIT REPORT");
    out << Style::horizontalRule() << "\n\n";

    // Header with mascot - adjusted positioning
    out << "  " << padRight(Style::bold(Style::violet("COMMITS & ATTRIBUTION")), 50) << mascot[0] << "\n";
    // out << "  " << padRight(Style::dim("Timeline of AI interactions"), 50) << mascot[1] << "\n";
    // out << "  " << padRight(Style::dim("Scanning holographic trace..."), 50) << mascot[2] << "\n";
    out << "  " << padRight("", 50) << mascot[3] << "\n\n";

    // Table Header - Borderless but aligned
    out << "  " << padRight(Style::dim("SHA"), 10)
        << padRight(Style::dim("ENTITY"), 22)
        << padRight(Style::dim("AUTHOR"), 22)
        << Style::dim("ATTRIBUTION") << "\n";
    out << "  " << Style::dim(std::string(72, ' ')) << "\n"; // Clean space instead of dash

    if (showDetail) {
        for (const auto& commit : summary.commits) {
            std::string verifiedIcon = commit.has_verified_note ? Style::success("•") : Style::error("!");
            std::string sha = Style::violet(commit.commit_sha.substr(0, 8));
            
            std::string entityRaw = commit.primary_agent;
            if (!commit.primary_model.empty()) entityRaw += Style::dim("/") + commit.primary_model;
            
            std::string authorRaw = commit.author;
            if (authorRaw.find('<') != std::string::npos) authorRaw = authorRaw.substr(0, authorRaw.find('<'));

            std::string entity = Style::glow(entityRaw);
            std::string author = Style::muted(authorRaw);


            out << "  " << padRight(sha, 10)
                << padRight(entity, 22)
                << padRight(author, 22)
                << Style::progressBar(commit.ai_lines, commit.total_lines, 20) << " " << verifiedIcon << "\n";


            if (commit.files.size() > 0) {
                for (const auto& file : commit.files) {
                    if (file.ai_lines == 0) continue;
                    out << "  " << std::setw(10) << "" << Style::dim("▫ ") << Style::blue(file.file_path) 
                        << " " << Style::dim(std::to_string(file.ai_lines) + "/" + std::to_string(file.total_lines)) << "\n";
                }
            }
            out << "\n";
        }
    }

    out << Style::horizontalRule() << "\n\n";
    out << Style::subHeader("FINAL ATTRIBUTION");
    out << "  " << padRight(Style::label("STATUS"), 15) << (policy.passed ? Style::success("PASSED") : (policy.blocked ? Style::error("BLOCKED") : Style::warning("WARNING"))) << "\n";
    out << "  " << padRight(Style::label("DENSITY"), 15) << Style::progressBar(summary.ai_lines, summary.total_lines, 40) << "\n";
    out << "  " << padRight(Style::label("TELEMETRY"), 15) << Style::glow(std::to_string(summary.ai_lines) + " AI lines / " + std::to_string(summary.total_lines) + " total") << "\n";
    
    out << "\n" << Style::dim("  " + policy.message) << "\n\n";

    return out.str();
}


std::string Report::formatJSON(const audit::AuditSummary& summary, const audit::PolicyResult& policy) {
    std::ostringstream out;

    out << "{\n";
    out << "  \"passed\": " << (policy.passed ? "true" : "false") << ",\n";
    out << "  \"blocked\": " << (policy.blocked ? "true" : "false") << ",\n";
    out << "  \"message\": \"" << policy.message << "\",\n";
    out << "  \"total_lines\": " << summary.total_lines << ",\n";
    out << "  \"ai_lines\": " << summary.ai_lines << ",\n";
    out << "  \"commits\": [\n";

    for (size_t i = 0; i < summary.commits.size(); ++i) {
        const auto& c = summary.commits[i];
        out << "    {\n";
        out << "      \"sha\": \"" << c.commit_sha << "\",\n";
        out << "      \"author\": \"" << c.author << "\",\n";
        out << "      \"total_lines\": " << c.total_lines << ",\n";
        out << "      \"ai_lines\": " << c.ai_lines << ",\n";
        out << "      \"has_ghost_note\": " << (c.has_ghost_note ? "true" : "false") << ",\n";
        out << "      \"has_verified_note\": " << (c.has_verified_note ? "true" : "false") << ",\n";
        out << "      \"files\": [\n";
        for (size_t j = 0; j < c.files.size(); ++j) {
            const auto& f = c.files[j];
            out << "        {\"path\": \"" << f.file_path
                << "\", \"total_lines\": " << f.total_lines
                << ", \"ai_lines\": " << f.ai_lines << "}";
            if (j + 1 < c.files.size()) out << ",";
            out << "\n";
        }
        out << "      ]\n";
        out << "    }";
        if (i + 1 < summary.commits.size()) out << ",";
        out << "\n";
    }

    out << "  ]\n";
    out << "}\n";

    return out.str();
}

static void renderFileRow(std::ostringstream& out, const audit::FileBlameSummary& file) {
    std::string filePath = Style::blue(file.file_path);

    std::string entityRaw = file.primary_entity;
    std::string entity = Style::glow(entityRaw);

    std::string author = Style::muted(file.primary_author);

    out << "  " << padRight(filePath, 30)
        << padRight(entity, 22)
        << padRight(author, 22)
        << Style::progressBar(file.ai_lines, file.total_lines, 20) << " " << Style::success("•") << "\n";

    if (file.entities.size() > 1) {
        for (size_t i = 1; i < file.entities.size(); ++i) {
            const auto& e = file.entities[i];
            std::string subEntity = Style::dim("▫ ") + Style::glow(e.agent + "/" + e.model);
            std::string subLines = Style::dim(std::to_string(e.lines) + " lines");
            out << "  " << std::setw(30) << "" << padRight(subEntity, 22) << subLines << "\n";
        }
    }
    out << "\n";
}

std::string Report::formatCodebaseCLI(const audit::CodebaseSummary& summary, const audit::PolicyResult& policy) {
    std::ostringstream out;
    auto mascot = Style::mascot();

    std::string shortSha = summary.target_sha.substr(0, 8);

    out << Style::header("AUDIT REPORT");
    out << Style::horizontalRule() << "\n\n";

    out << "  " << padRight(Style::bold(Style::violet("CODEBASE ATTRIBUTION (" + shortSha + ")")), 50) << mascot[0] << "\n";
    out << "  " << padRight("", 50) << mascot[1] << "\n";
    out << "  " << padRight("", 50) << mascot[2] << "\n";
    out << "  " << padRight("", 50) << mascot[3] << "\n\n";

    // Segment 1: CHANGES AT <sha>
    out << "  " << Style::bold(Style::violet("CHANGES AT " + shortSha)) << "\n\n";

    out << "  " << padRight(Style::dim("FILE"), 30)
        << padRight(Style::dim("ENTITY"), 22)
        << padRight(Style::dim("AUTHOR"), 22)
        << Style::dim("ATTRIBUTION") << "\n";
    out << "  " << Style::dim(std::string(72, ' ')) << "\n";

    bool hasInCommit = false;
    for (const auto& file : summary.files) {
        if (file.in_commit) {
            renderFileRow(out, file);
            hasInCommit = true;
        }
    }
    if (!hasInCommit) {
        out << "  " << Style::dim("  (no AI changes in this commit)") << "\n\n";
    }

    // Separator
    out << "  " << Style::dim(std::string(72, ' ')) << "\n\n";

    // Segment 2: CODEBASE ATTRIBUTION
    out << "  " << Style::bold(Style::violet("CODEBASE ATTRIBUTION")) << "\n\n";

    out << "  " << padRight(Style::dim("FILE"), 30)
        << padRight(Style::dim("ENTITY"), 22)
        << padRight(Style::dim("AUTHOR"), 22)
        << Style::dim("ATTRIBUTION") << "\n";
    out << "  " << Style::dim(std::string(72, ' ')) << "\n";

    bool hasPastAi = false;
    for (const auto& file : summary.files) {
        if (!file.in_commit) {
            renderFileRow(out, file);
            hasPastAi = true;
        }
    }
    if (!hasPastAi) {
        out << "  " << Style::dim("  (no past AI attribution)") << "\n\n";
    }

    out << Style::horizontalRule() << "\n\n";
    out << Style::subHeader("FINAL ATTRIBUTION");
    out << "  " << padRight(Style::label("STATUS"), 15) << (policy.passed ? Style::success("PASSED") : (policy.blocked ? Style::error("BLOCKED") : Style::warning("WARNING"))) << "\n";
    out << "  " << padRight(Style::label("DENSITY"), 15) << Style::progressBar(summary.ai_lines, summary.total_lines, 40) << "\n";
    out << "  " << padRight(Style::label("TELEMETRY"), 15) << Style::glow(std::to_string(summary.ai_lines) + " AI lines / " + std::to_string(summary.total_lines) + " total") << "\n";

    out << "\n" << Style::dim("  " + policy.message) << "\n\n";

    return out.str();
}

std::string Report::formatCodebaseJSON(const audit::CodebaseSummary& summary, const audit::PolicyResult& policy) {
    std::ostringstream out;

    out << "{\n";
    out << "  \"target_sha\": \"" << summary.target_sha << "\",\n";
    out << "  \"passed\": " << (policy.passed ? "true" : "false") << ",\n";
    out << "  \"blocked\": " << (policy.blocked ? "true" : "false") << ",\n";
    out << "  \"message\": \"" << policy.message << "\",\n";
    out << "  \"total_lines\": " << summary.total_lines << ",\n";
    out << "  \"ai_lines\": " << summary.ai_lines << ",\n";
    out << "  \"commit_ai_lines\": " << summary.commit_ai_lines << ",\n";
    out << "  \"commit_total_lines\": " << summary.commit_total_lines << ",\n";
    out << "  \"files\": [\n";

    for (size_t i = 0; i < summary.files.size(); ++i) {
        const auto& f = summary.files[i];
        out << "    {\n";
        out << "      \"path\": \"" << f.file_path << "\",\n";
        out << "      \"total_lines\": " << f.total_lines << ",\n";
        out << "      \"ai_lines\": " << f.ai_lines << ",\n";
        out << "      \"in_commit\": " << (f.in_commit ? "true" : "false") << ",\n";
        out << "      \"primary_entity\": \"" << f.primary_entity << "\",\n";
        out << "      \"primary_author\": \"" << f.primary_author << "\",\n";
        out << "      \"entities\": [\n";
        for (size_t j = 0; j < f.entities.size(); ++j) {
            const auto& e = f.entities[j];
            out << "        {\"agent\": \"" << e.agent
                << "\", \"model\": \"" << e.model
                << "\", \"lines\": " << e.lines << "}";
            if (j + 1 < f.entities.size()) out << ",";
            out << "\n";
        }
        out << "      ]\n";
        out << "    }";
        if (i + 1 < summary.files.size()) out << ",";
        out << "\n";
    }

    out << "  ]\n";
    out << "}\n";

    return out.str();
}

}
}
