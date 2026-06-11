#include "report.hpp"
#include "layout.hpp"
#include "style.hpp"
#include <algorithm>
#include <sstream>
#include <iostream>
#include <cstdlib>
#include <iomanip>

namespace ghost {
namespace output {

static constexpr size_t kFileCol = 34;
static constexpr size_t kEntityCol = 32;
static constexpr size_t kAuthorCol = 20;
static constexpr size_t kCommitEntityCol = 22;
static constexpr size_t kCommitAuthorCol = 22;
static constexpr size_t kColGap = 2;

static std::string padRight(const std::string& s, size_t width) {
    return Layout::padRight(s, width);
}

static std::string fitCell(const std::string& s, size_t width) {
    return Layout::fitCell(s, width, kColGap);
}

static int percentOf(int part, int total) {
    if (total <= 0) return 0;
    return std::min((part * 100) / total, 100);
}

static std::string statusWord(const audit::PolicyResult& policy) {
    if (policy.passed) return Style::success("PASSED");
    if (policy.blocked) return Style::error("BLOCKED");
    return Style::warning("WARNING");
}

static std::string displayAuthor(std::string author) {
    if (author.empty() || author == "unknown") return "";
    size_t email = author.find('<');
    if (email != std::string::npos) author = author.substr(0, email);
    while (!author.empty() && author.back() == ' ') author.pop_back();
    return author == "unknown" ? "" : author;
}

static std::string sourceForFile(const audit::FileBlameSummary& file) {
    std::string source = file.primary_entity.empty() ? "unknown" : file.primary_entity;
    std::string author = displayAuthor(file.primary_author);
    if (!author.empty()) source += " · " + author;
    return source;
}

static std::string thresholdText(const audit::PolicyResult& policy) {
    if (policy.threshold < 0) return "";
    std::string value = "threshold " + std::to_string(policy.threshold) + "%";
    if (!policy.action.empty()) value += " " + policy.action;
    return value;
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

    out << Style::header("Audit Report");
    out << Style::horizontalRule() << "\n\n";

    out << "  " << Style::bold(Style::violet("Commits & Attribution")) << "\n\n";

    // Table Header - Borderless but aligned
    out << "  " << padRight(Style::dim("SHA"), 10)
        << fitCell(Style::dim("Entity"), kCommitEntityCol)
        << fitCell(Style::dim("Author"), kCommitAuthorCol)
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
                << fitCell(entity, kCommitEntityCol)
                << fitCell(author, kCommitAuthorCol)
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

static void renderWideFileRow(
    std::ostringstream& out,
    const audit::FileBlameSummary& file,
    size_t fileCol,
    size_t linesCol,
    size_t shareCol,
    size_t sourceCol
) {
    std::string lines = std::to_string(file.ai_lines) + "/" + std::to_string(file.total_lines);
    std::string share = std::to_string(percentOf(file.ai_lines, file.total_lines)) + "%";
    out << "  " << Layout::fitCell(Style::blue(file.file_path), fileCol)
        << Layout::fitCell(Style::glow(lines), linesCol)
        << Layout::fitCell(Style::violet(share), shareCol)
        << Layout::ellipsizeMiddle(Style::glow(sourceForFile(file)), sourceCol) << "\n";

    if (file.entities.size() > 1) {
        for (size_t i = 1; i < file.entities.size(); ++i) {
            const auto& e = file.entities[i];
            std::string source = "also " + e.agent + "/" + e.model + " · " + std::to_string(e.lines) + " lines";
            out << "  " << std::string(fileCol + linesCol + shareCol + 6, ' ')
                << Style::dim(Layout::ellipsizeMiddle(source, sourceCol)) << "\n";
        }
    }
}

static void renderStackedFileRow(std::ostringstream& out, const audit::FileBlameSummary& file, size_t width) {
    size_t bodyWidth = width > 6 ? width - 6 : width;
    out << "  " << Style::blue(Layout::ellipsizeMiddle(file.file_path, bodyWidth)) << "\n";
    std::string detail = std::to_string(file.ai_lines) + "/" + std::to_string(file.total_lines) +
        " AI lines · " + std::to_string(percentOf(file.ai_lines, file.total_lines)) + "% · " + sourceForFile(file);
    out << "    " << Style::dim(Layout::ellipsizeMiddle(detail, bodyWidth)) << "\n";
    if (file.entities.size() > 1) {
        for (size_t i = 1; i < file.entities.size(); ++i) {
            const auto& e = file.entities[i];
            std::string source = "also " + e.agent + "/" + e.model + " · " + std::to_string(e.lines) + " lines";
            out << "    " << Style::dim(Layout::ellipsizeMiddle(source, bodyWidth)) << "\n";
        }
    }
}

std::string Report::formatCodebaseCLI(const audit::CodebaseSummary& summary, const audit::PolicyResult& policy) {
    std::ostringstream out;

    std::string shortSha = summary.target_sha.substr(0, 8);
    int aiPct = percentOf(summary.ai_lines, summary.total_lines);
    std::string threshold = thresholdText(policy);
    size_t width = Layout::contentWidth();
    bool stacked = width < 92;

    out << Style::header("audit");
    out << "  " << statusWord(policy)
        << Style::dim("  ") << Style::violet(std::to_string(aiPct) + "% AI")
        << Style::dim("  ") << Style::glow(std::to_string(summary.ai_lines) + " / " + std::to_string(summary.total_lines) + " lines");
    if (!threshold.empty()) {
        out << Style::dim("  ") << Style::muted(threshold);
    }
    out << Style::dim("  HEAD ") << Style::violet(shortSha) << "\n\n";

    out << Style::subHeader("Codebase Policy");
    out << Layout::keyValue("scope", Style::muted("current HEAD codebase"));
    out << Layout::keyValue("density", Style::progressBar(summary.ai_lines, summary.total_lines, stacked ? 18 : 32));
    out << Layout::keyValue("result", statusWord(policy));
    out << "\n";

    out << "  " << Style::bold(Style::violet("AI-Touched Files")) << "\n\n";

    bool hasAiFiles = false;
    if (!stacked) {
        size_t fileCol = width >= 110 ? 46 : 38;
        size_t linesCol = 10;
        size_t shareCol = 7;
        size_t sourceCol = width > fileCol + linesCol + shareCol + 10
            ? width - fileCol - linesCol - shareCol - 8
            : 24;
        out << "  " << Layout::fitCell(Style::dim("File"), fileCol)
            << Layout::fitCell(Style::dim("AI Lines"), linesCol)
            << Layout::fitCell(Style::dim("Share"), shareCol)
            << Style::dim("Source") << "\n";
        for (const auto& file : summary.files) {
            if (file.ai_lines <= 0) continue;
            renderWideFileRow(out, file, fileCol, linesCol, shareCol, sourceCol);
            hasAiFiles = true;
        }
    } else {
        for (const auto& file : summary.files) {
            if (file.ai_lines <= 0) continue;
            renderStackedFileRow(out, file, width);
            hasAiFiles = true;
        }
    }
    if (!hasAiFiles) {
        out << "  " << Style::dim("No AI-attributed lines found in the current codebase.") << "\n";
    }

    out << "\n" << Style::horizontalRule() << "\n\n";
    if (!policy.message.empty()) {
        out << Style::dim("  " + policy.message) << "\n\n";
    }

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

void Report::streamCodebaseCLI(const audit::CodebaseSummary& summary, const audit::PolicyResult& policy) {
    std::cout << formatCodebaseCLI(summary, policy);
}

}
}
