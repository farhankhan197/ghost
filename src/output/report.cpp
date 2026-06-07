#include "report.hpp"
#include "style.hpp"
#include <sstream>
#include <iostream>
#include <cstdlib>
#include <iomanip>
#include <thread>
#include <chrono>

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

static std::string jsonEscape(const std::string& s) {
    std::ostringstream escaped;
    for (char c : s) {
        switch (c) {
            case '\\': escaped << "\\\\"; break;
            case '"': escaped << "\\\""; break;
            case '\n': escaped << "\\n"; break;
            case '\r': escaped << "\\r"; break;
            case '\t': escaped << "\\t"; break;
            default: escaped << c; break;
        }
    }
    return escaped.str();
}

std::string Report::formatCLI(const audit::AuditSummary& summary, const audit::PolicyResult& policy, bool showDetail) {
    std::ostringstream out;
    auto mascot = Style::mascot();

    out << Style::header("Audit Report");
    out << Style::horizontalRule() << "\n\n";

    // Header with mascot - adjusted positioning
    out << "  " << padRight(Style::bold(Style::violet("Commits & Attribution")), 50) << mascot[0] << "\n";
    out << "  " << padRight("", 50) << mascot[1] << "\n";
    out << "  " << padRight("", 50) << mascot[2] << "\n\n";

    // Table Header - Borderless but aligned
    out << "  " << padRight(Style::dim("SHA"), 10)
        << padRight(Style::dim("Entity"), 22)
        << padRight(Style::dim("Author"), 22)
        << Style::dim("Attribution") << "\n";
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
    out << Style::subHeader("Final Attribution");
    out << "  " << padRight(Style::label("Status"), 15) << (policy.passed ? Style::success("PASSED") : (policy.blocked ? Style::error("BLOCKED") : Style::warning("WARNING"))) << "\n";
    out << "  " << padRight(Style::label("Density"), 15) << Style::progressBar(summary.ai_lines, summary.total_lines, 40) << "\n";
    out << "  " << padRight(Style::label("Telemetry"), 15) << Style::glow(std::to_string(summary.ai_lines) + " AI lines / " + std::to_string(summary.total_lines) + " total") << "\n";
    
    out << "\n" << Style::dim("  " + policy.message) << "\n\n";

    return out.str();
}


std::string Report::formatJSON(const audit::AuditSummary& summary, const audit::PolicyResult& policy) {
    std::ostringstream out;

    out << "{\n";
    out << "  \"passed\": " << (policy.passed ? "true" : "false") << ",\n";
    out << "  \"blocked\": " << (policy.blocked ? "true" : "false") << ",\n";
    out << "  \"threshold_blocked\": " << (policy.threshold_blocked ? "true" : "false") << ",\n";
    out << "  \"message\": \"" << jsonEscape(policy.message) << "\",\n";
    out << "  \"total_lines\": " << summary.total_lines << ",\n";
    out << "  \"ai_lines\": " << summary.ai_lines << ",\n";
    out << "  \"commits\": [\n";

    for (size_t i = 0; i < summary.commits.size(); ++i) {
        const auto& c = summary.commits[i];
        out << "    {\n";
        out << "      \"sha\": \"" << c.commit_sha << "\",\n";
        out << "      \"author\": \"" << jsonEscape(c.author) << "\",\n";
        out << "      \"total_lines\": " << c.total_lines << ",\n";
        out << "      \"ai_lines\": " << c.ai_lines << ",\n";
        out << "      \"has_ghost_note\": " << (c.has_ghost_note ? "true" : "false") << ",\n";
        out << "      \"has_verified_note\": " << (c.has_verified_note ? "true" : "false") << ",\n";
        out << "      \"files\": [\n";
        for (size_t j = 0; j < c.files.size(); ++j) {
            const auto& f = c.files[j];
            out << "        {\"path\": \"" << jsonEscape(f.file_path)
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

static void renderFileRow(std::ostringstream& out, const audit::FileBlameSummary& file, const std::string& entityOverride) {
    std::string filePath = Style::blue(file.file_path);

    std::string entityRaw = entityOverride.empty() ? file.primary_entity : entityOverride;
    std::string entity = Style::glow(entityRaw);

    std::string author = Style::muted(file.primary_author);

    out << "  " << padRight(filePath, 30)
        << padRight(entity, 32)
        << padRight(author, 22)
        << Style::progressBar(file.ai_lines, file.total_lines, 20) << " " << Style::success("•") << "\n";

    if (file.entities.size() > 1) {
        for (size_t i = 1; i < file.entities.size(); ++i) {
            const auto& e = file.entities[i];
            std::string subEntity = Style::dim("▫ ") + Style::glow(e.agent + "/" + e.model);
            std::string subLines = Style::dim(std::to_string(e.lines) + " lines");
            out << "  " << std::setw(30) << "" << padRight(subEntity, 32) << subLines << "\n";
        }
    }
    out << "\n";
}

std::string Report::formatCodebaseCLI(const audit::CodebaseSummary& summary, const audit::PolicyResult& policy) {
    std::ostringstream out;
    auto mascot = Style::mascot();

    std::string shortSha = summary.target_sha.substr(0, 8);

    out << Style::header("Audit Report");
    out << Style::horizontalRule() << "\n\n";

    out << "  " << padRight(Style::bold(Style::violet("Codebase Attribution (" + shortSha + ")")), 50) << mascot[0] << "\n";
    out << "  " << padRight("", 50) << mascot[1] << "\n";
    out << "  " << padRight("", 50) << mascot[2] << "\n\n";

    // Segment 1: Changes At <sha>
    out << "  " << Style::bold(Style::violet("Changes At " + shortSha)) << "\n\n";

    out << "  " << padRight(Style::dim("File"), 30)
        << padRight(Style::dim("Entity"), 32)
        << padRight(Style::dim("Author"), 22)
        << Style::dim("Attribution") << "\n";
    out << "  " << Style::dim(std::string(106, ' ')) << "\n";

    bool hasInCommit = false;
    for (const auto& file : summary.files) {
        if (file.in_commit) {
            renderFileRow(out, file, file.commit_entity);
            hasInCommit = true;
        }
    }
    if (!hasInCommit) {
        out << "  " << Style::dim("  (no AI changes in this commit)") << "\n\n";
    }

    // Separator
    out << "  " << Style::dim(std::string(106, ' ')) << "\n\n";

    // Segment 2: Codebase Attribution
    out << "  " << Style::bold(Style::violet("Codebase Attribution")) << "\n\n";

    out << "  " << padRight(Style::dim("File"), 30)
        << padRight(Style::dim("Entity"), 32)
        << padRight(Style::dim("Author"), 22)
        << Style::dim("Attribution") << "\n";
    out << "  " << Style::dim(std::string(106, ' ')) << "\n";

    bool hasPastAi = false;
    for (const auto& file : summary.files) {
        if (!file.in_commit) {
            renderFileRow(out, file, file.primary_entity);
            hasPastAi = true;
        }
    }
    if (!hasPastAi) {
        out << "  " << Style::dim("  (no past AI attribution)") << "\n\n";
    }

    out << Style::horizontalRule() << "\n\n";
    out << Style::subHeader("Final Attribution");
    out << "  " << padRight(Style::label("Status"), 15) << (policy.passed ? Style::success("PASSED") : (policy.blocked ? Style::error("BLOCKED") : Style::warning("WARNING"))) << "\n";
    out << "  " << padRight(Style::label("Density"), 15) << Style::progressBar(summary.ai_lines, summary.total_lines, 40) << "\n";
    out << "  " << padRight(Style::label("Telemetry"), 15) << Style::glow(std::to_string(summary.ai_lines) + " AI lines / " + std::to_string(summary.total_lines) + " total") << "\n";

    out << "\n" << Style::dim("  " + policy.message) << "\n\n";

    return out.str();
}

std::string Report::formatCodebaseJSON(const audit::CodebaseSummary& summary, const audit::PolicyResult& policy) {
    std::ostringstream out;

    out << "{\n";
    out << "  \"target_sha\": \"" << summary.target_sha << "\",\n";
    out << "  \"passed\": " << (policy.passed ? "true" : "false") << ",\n";
    out << "  \"blocked\": " << (policy.blocked ? "true" : "false") << ",\n";
    out << "  \"message\": \"" << jsonEscape(policy.message) << "\",\n";
    out << "  \"total_lines\": " << summary.total_lines << ",\n";
    out << "  \"ai_lines\": " << summary.ai_lines << ",\n";
    out << "  \"commit_ai_lines\": " << summary.commit_ai_lines << ",\n";
    out << "  \"commit_total_lines\": " << summary.commit_total_lines << ",\n";
    out << "  \"files\": [\n";

    for (size_t i = 0; i < summary.files.size(); ++i) {
        const auto& f = summary.files[i];
        out << "    {\n";
        out << "      \"path\": \"" << jsonEscape(f.file_path) << "\",\n";
        out << "      \"total_lines\": " << f.total_lines << ",\n";
        out << "      \"ai_lines\": " << f.ai_lines << ",\n";
        out << "      \"in_commit\": " << (f.in_commit ? "true" : "false") << ",\n";
        out << "      \"primary_entity\": \"" << jsonEscape(f.primary_entity) << "\",\n";
        out << "      \"primary_author\": \"" << jsonEscape(f.primary_author) << "\",\n";
        out << "      \"entities\": [\n";
        for (size_t j = 0; j < f.entities.size(); ++j) {
            const auto& e = f.entities[j];
            out << "        {\"agent\": \"" << jsonEscape(e.agent)
                << "\", \"model\": \"" << jsonEscape(e.model)
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

static void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

static void streamFileRow(const audit::FileBlameSummary& file, const std::string& entityOverride) {
    std::string filePath = Style::blue(file.file_path);

    std::string entityRaw = entityOverride.empty() ? file.primary_entity : entityOverride;
    std::string entity = Style::glow(entityRaw);

    std::string author = Style::muted(file.primary_author);

    std::cout << "  " << padRight(filePath, 30)
        << padRight(entity, 32)
        << padRight(author, 22);

    // Animate progress bar
    if (file.total_lines > 0) {
        float pct = (float)file.ai_lines / file.total_lines;
        int filled = (int)(pct * 20);
        bool useUnicode = true;
        if (std::getenv("GHOST_FORCE_ASCII") != nullptr) useUnicode = false;

        std::cout << Style::dim("|");
        for (int i = 0; i < 20; i++) {
            std::cout.flush();
            sleepMs(10);
            if (i < filled) {
                std::cout << Style::violet(useUnicode ? "█" : "#");
            } else {
                if (useUnicode) {
                    std::cout << "\033[38;5;236m" << "░" << "\033[0m";
                } else {
                    std::cout << Style::dim("-");
                }
            }
        }
        std::cout << Style::dim("|") << " " << Style::glow(std::to_string((int)(pct * 100)) + "%")
                  << " " << Style::success("•") << "\n";
    } else {
        std::cout << Style::dim("[ - ]") << "\n";
    }

    if (file.entities.size() > 1) {
        for (size_t i = 1; i < file.entities.size(); ++i) {
            const auto& e = file.entities[i];
            std::string subEntity = Style::dim("▫ ") + Style::glow(e.agent + "/" + e.model);
            std::string subLines = Style::dim(std::to_string(e.lines) + " lines");
            std::cout << "  " << std::setw(30) << "" << padRight(subEntity, 32) << subLines << "\n";
        }
    }
    std::cout << "\n";
}

void Report::streamCodebaseCLI(const audit::CodebaseSummary& summary, const audit::PolicyResult& policy) {
    auto mascot = Style::mascot();
    std::string shortSha = summary.target_sha.substr(0, 8);

    std::cout << Style::header("Audit Report");
    std::cout << Style::horizontalRule() << "\n\n";
    sleepMs(80);

    std::cout << "  " << padRight(Style::bold(Style::violet("Codebase Attribution (" + shortSha + ")")), 50) << mascot[0] << "\n";
    sleepMs(60);
    std::cout << "  " << padRight("", 50) << mascot[1] << "\n";
    sleepMs(60);
    std::cout << "  " << padRight("", 50) << mascot[2] << "\n\n";
    sleepMs(80);

    // Segment 1: Changes At <sha>
    std::cout << "  " << Style::bold(Style::violet("Changes At " + shortSha)) << "\n\n";
    sleepMs(60);

    std::cout << "  " << padRight(Style::dim("File"), 30)
        << padRight(Style::dim("Entity"), 32)
        << padRight(Style::dim("Author"), 22)
        << Style::dim("Attribution") << "\n";
    std::cout << "  " << Style::dim(std::string(106, ' ')) << "\n";

    bool hasInCommit = false;
    for (const auto& file : summary.files) {
        if (file.in_commit) {
            streamFileRow(file, file.commit_entity);
            hasInCommit = true;
        }
    }
    if (!hasInCommit) {
        std::cout << "  " << Style::dim("  (no AI changes in this commit)") << "\n\n";
    }

    std::cout << "  " << Style::dim(std::string(106, ' ')) << "\n\n";
    sleepMs(80);

    // Segment 2: Codebase Attribution
    std::cout << "  " << Style::bold(Style::violet("Codebase Attribution")) << "\n\n";
    sleepMs(60);

    std::cout << "  " << padRight(Style::dim("File"), 30)
        << padRight(Style::dim("Entity"), 32)
        << padRight(Style::dim("Author"), 22)
        << Style::dim("Attribution") << "\n";
    std::cout << "  " << Style::dim(std::string(106, ' ')) << "\n";

    bool hasPastAi = false;
    for (const auto& file : summary.files) {
        if (!file.in_commit) {
            streamFileRow(file, file.primary_entity);
            hasPastAi = true;
        }
    }
    if (!hasPastAi) {
        std::cout << "  " << Style::dim("  (no past AI attribution)") << "\n\n";
    }

    std::cout << Style::horizontalRule() << "\n\n";
    sleepMs(100);

    // Footer
    std::cout << Style::subHeader("Final Attribution");
    std::cout << "  " << padRight(Style::label("Status"), 15) << (policy.passed ? Style::success("PASSED") : (policy.blocked ? Style::error("BLOCKED") : Style::warning("WARNING"))) << "\n";

    // Animate density bar
    if (summary.total_lines > 0) {
        float pct = (float)summary.ai_lines / summary.total_lines;
        int filled = (int)(pct * 40);
        bool useUnicode = true;
        if (std::getenv("GHOST_FORCE_ASCII") != nullptr) useUnicode = false;

        std::cout << "  " << padRight(Style::label("Density"), 15) << Style::dim("|");
        for (int i = 0; i < 40; i++) {
            std::cout.flush();
            sleepMs(8);
            if (i < filled) {
                std::cout << Style::violet(useUnicode ? "█" : "#");
            } else {
                if (useUnicode) {
                    std::cout << "\033[38;5;236m" << "░" << "\033[0m";
                } else {
                    std::cout << Style::dim("-");
                }
            }
        }
        std::cout << Style::dim("|") << " " << Style::glow(std::to_string((int)(pct * 100)) + "%") << "\n";
    } else {
        std::cout << "  " << padRight(Style::label("Density"), 15) << Style::dim("[ - ]") << "\n";
    }

    std::cout << "  " << padRight(Style::label("Telemetry"), 15) << Style::glow(std::to_string(summary.ai_lines) + " AI lines / " + std::to_string(summary.total_lines) + " total") << "\n";

    std::cout << "\n" << Style::dim("  " + policy.message) << "\n\n";
}

}
}
