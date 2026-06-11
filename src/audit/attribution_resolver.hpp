#ifndef GHOST_AUDIT_ATTRIBUTION_RESOLVER_HPP
#define GHOST_AUDIT_ATTRIBUTION_RESOLVER_HPP

#include "../note/line_range.hpp"

#include <map>
#include <string>
#include <vector>

namespace ghost {
namespace audit {

struct AttributionQuery {
    std::string repo_root;
    std::string target_ref = "HEAD";
    std::string config_ref;
    std::vector<std::string> file_filter;
    std::map<std::string, note::LineRangeSet> line_filter;
};

struct AttributionEntity {
    std::string agent;
    std::string model;
    int lines = 0;
};

struct AttributionLine {
    int line_number = 0;
    int source_line_number = 0;
    std::string commit_sha;
    bool is_ai = false;
    std::string session_id;
    std::string agent;
    std::string model;
    std::string author;
};

struct AttributionFile {
    std::string path;
    int total_lines = 0;
    int ai_lines = 0;
    std::string primary_author;
    std::string primary_entity;
    std::vector<AttributionEntity> entities;
    std::vector<AttributionLine> lines;
};

struct AttributionResult {
    std::string target_sha;
    int total_lines = 0;
    int ai_lines = 0;
    std::vector<AttributionFile> files;
    std::string message;
};

class AttributionResolver {
public:
    static AttributionResult resolve(const AttributionQuery& query);
};

}
}

#endif
