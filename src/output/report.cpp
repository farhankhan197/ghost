#include "report.hpp"
#include <sstream>
#include <iostream>
#include <cstdlib>

namespace ghost {
namespace output {

static bool useColor() {
    const char* term = std::getenv("TERM");
    const char* nocolor = std::getenv("NO_COLOR");
    if (nocolor != nullptr) return false;
    return term != nullptr;
}

static std::string violet(const std::string& s) {
    return useColor() ? "\033[38;5;141m" + s + "\033[0m" : s;
}

static std::string blue(const std::string& s) {
    return useColor() ? "\033[38;5;75m" + s + "\033[0m" : s;
}

static std::string white(const std::string& s) {
    return useColor() ? "\033[38;5;231m" + s + "\033[0m" : s;
}

static std::string dim(const std::string& s) {
    return useColor() ? "\033[2m\033[38;5;248m" + s + "\033[0m" : s;
}

static std::string red(const std::string& s) {
    return useColor() ? "\033[31m" + s + "\033[0m" : s;
}

static std::string green(const std::string& s) {
    return useColor() ? "\033[32m" + s + "\033[0m" : s;
}

static std::string yellow(const std::string& s) {
    return useColor() ? "\033[33m" + s + "\033[0m" : s;
}

static std::string bold(const std::string& s) {
    return useColor() ? "\033[1m" + s + "\033[0m" : s;
}

static std::string label(const std::string& s) {
    return useColor() ? "\033[2m\033[38;5;75m" + s + "\033[0m" : s;
}

static std::string separator() {
    std::string s;
    for (int i = 0; i < 50; i++) {
        s += useColor() ? "\033[2m\033[38;5;141m" : "";
        s += "\033[2m";
        s += (i % 2 == 0) ? "\342\224\200" : " ";
        s += "\033[0m";
    }
    return s;
}

static std::string aiBar(int ai, int total) {
    if (total == 0) return dim("[-]");
    int pct = (ai * 100) / total;
    int bars = (pct + 5) / 10;
    std::string s;
    for (int i = 0; i < 10; i++) {
        if (i < bars) {
            s += violet("#");
        } else {
            s += dim("#");
        }
    }
    s += " " + white(std::to_string(pct) + "%");
    return s;
}

std::string Report::formatCLI(const audit::AuditSummary& summary, const audit::PolicyResult& policy, bool showDetail) {
    std::ostringstream out;

    out << bold(violet("ghost")) << dim(" audit report") << "\n";
    out << separator() << "\n\n";

    if (showDetail) {
        for (const auto& commit : summary.commits) {
            out << label("commit") << " " << violet(commit.commit_sha.substr(0, 8)) << "\n";
            out << label("  author") << "     " << white(commit.author) << "\n";
            out << label("  verified") << "   " << (commit.has_verified_note ? green("yes") : red("no")) << "\n";
            out << label("  ai") << "         " << aiBar(commit.ai_lines, commit.total_lines) << "\n";

            for (const auto& file : commit.files) {
                if (file.total_lines == 0) continue;
                out << label("    ") << blue(file.file_path) << " "
                    << aiBar(file.ai_lines, file.total_lines) << "\n";
            }
            out << "\n";
        }
    }

    out << separator() << "\n\n";
    out << bold(violet("summary")) << "\n";
    out << label("  total") << "      " << white(std::to_string(summary.ai_lines) + "/" + std::to_string(summary.total_lines)) << "\n";
    out << label("  overall") << "    " << aiBar(summary.ai_lines, summary.total_lines) << "\n\n";

    out << bold(violet("result")) << "     "
        << (policy.passed ? green("PASS") : (policy.blocked ? red("BLOCKED") : yellow("WARN")))
        << "\n" << dim(policy.message) << "\n";

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

}
}
