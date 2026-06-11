#ifndef GHOST_CHECKPOINT_HOOK_EVENT_HPP
#define GHOST_CHECKPOINT_HOOK_EVENT_HPP

#include <string>

namespace ghost {
namespace checkpoint {

struct HookEvent {
    bool valid_json = false;
    std::string cwd;
    std::string file_path;
    std::string model;
    std::string tool;

    bool isEditTool() const;

    static HookEvent parse(const std::string& jsonText);
};

}
}

#endif
