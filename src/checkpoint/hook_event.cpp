#include "hook_event.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>

namespace ghost {
namespace checkpoint {
namespace {

using Json = nlohmann::json;

std::string normalizeModel(std::string value) {
    if (value.empty()) return "";
    size_t slash = value.rfind('/');
    return slash == std::string::npos ? value : value.substr(slash + 1);
}

std::string normalizeTool(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool containsAny(const std::string& value, std::initializer_list<const char*> needles) {
    for (const char* needle : needles) {
        if (value.find(needle) != std::string::npos) return true;
    }
    return false;
}

std::string getString(const Json& value) {
    if (value.is_string()) return value.get<std::string>();
    return "";
}

std::string firstStringField(const Json& value, std::initializer_list<const char*> keys) {
    if (!value.is_object()) return "";
    for (const char* key : keys) {
        auto it = value.find(key);
        if (it != value.end()) {
            std::string found = getString(*it);
            if (!found.empty()) return found;
        }
    }
    return "";
}

std::string findPath(const Json& value) {
    if (value.is_object()) {
        std::string direct = firstStringField(value, {"file_path", "filePath", "path", "file"});
        if (!direct.empty()) return direct;

        auto filesIt = value.find("files");
        if (filesIt != value.end() && filesIt->is_array() && !filesIt->empty()) {
            std::string first = getString((*filesIt)[0]);
            if (!first.empty()) return first;
        }

        for (auto it = value.begin(); it != value.end(); ++it) {
            std::string nested = findPath(*it);
            if (!nested.empty()) return nested;
        }
    } else if (value.is_array()) {
        for (const auto& item : value) {
            std::string nested = findPath(item);
            if (!nested.empty()) return nested;
        }
    }
    return "";
}

std::string findModel(const Json& value) {
    if (value.is_object()) {
        std::string direct = firstStringField(value, {"model", "model_id", "modelId", "modelID"});
        if (!direct.empty()) return normalizeModel(direct);
        for (auto it = value.begin(); it != value.end(); ++it) {
            std::string nested = findModel(*it);
            if (!nested.empty()) return nested;
        }
    } else if (value.is_array()) {
        for (const auto& item : value) {
            std::string nested = findModel(item);
            if (!nested.empty()) return nested;
        }
    }
    return "";
}

std::string findTool(const Json& value) {
    if (value.is_object()) {
        std::string direct = firstStringField(value, {"tool", "tool_name", "toolName", "name"});
        if (!direct.empty()) return direct;
        for (auto it = value.begin(); it != value.end(); ++it) {
            std::string nested = findTool(*it);
            if (!nested.empty()) return nested;
        }
    } else if (value.is_array()) {
        for (const auto& item : value) {
            std::string nested = findTool(item);
            if (!nested.empty()) return nested;
        }
    }
    return "";
}

}

bool HookEvent::isEditTool() const {
    if (!file_path.empty()) return true;

    std::string normalized = normalizeTool(tool);
    if (normalized.empty()) return false;

    return containsAny(normalized, {
        "apply_patch",
        "applydiff",
        "multi_edit",
        "multiedit",
        "edit_file",
        "write_file",
        "replace_file",
        "filesystem__edit",
        "filesystem__write"
    }) || normalized == "edit" || normalized == "write" || normalized == "replace";
}

HookEvent HookEvent::parse(const std::string& jsonText) {
    HookEvent event;
    if (jsonText.empty()) return event;

    Json json;
    try {
        json = Json::parse(jsonText);
    } catch (...) {
        return event;
    }

    event.valid_json = true;
    event.cwd = firstStringField(json, {"cwd", "workingDirectory", "working_directory"});
    event.file_path = findPath(json);
    event.model = findModel(json);
    event.tool = findTool(json);
    return event;
}

}
}
