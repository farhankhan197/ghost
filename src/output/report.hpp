#ifndef GHOST_OUTPUT_REPORT_HPP
#define GHOST_OUTPUT_REPORT_HPP

#include <string>
#include "../audit/aggregator.hpp"
#include "../audit/policy.hpp"
#include "../audit/auditor.hpp"

namespace ghost {
namespace output {

class Report {
public:
    static std::string formatCLI(const audit::AuditSummary& summary, const audit::PolicyResult& policy, bool showDetail = true);
    static std::string formatJSON(const audit::AuditSummary& summary, const audit::PolicyResult& policy);
    static std::string formatCodebaseCLI(const audit::CodebaseSummary& summary, const audit::PolicyResult& policy);
    static std::string formatCodebaseJSON(const audit::CodebaseSummary& summary, const audit::PolicyResult& policy);
    static void streamCodebaseCLI(const audit::CodebaseSummary& summary, const audit::PolicyResult& policy);
};

}
}

#endif
