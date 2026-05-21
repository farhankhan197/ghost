#ifndef GHOST_CLI_COMMANDS_HPP
#define GHOST_CLI_COMMANDS_HPP

#include <string>
#include <vector>
#include <map>

namespace ghost {
namespace cli {

struct CommandInfo {
    std::string name;
    std::vector<std::string> aliases;
    std::string description;
    std::string usage;
    std::vector<std::string> examples;
    std::vector<std::string> flags;
};

class CommandRegistry {
public:
    static const std::map<std::string, CommandInfo>& getCommands();
    static std::string resolveCommand(const std::string& input);
    static std::vector<std::string> getSuggestions(const std::string& input, int max = 3);
    static void printHelp(const std::string& command);
    static void printGlobalHelp();
    static void printVersion();
};

} // namespace cli
} // namespace ghost

#endif
