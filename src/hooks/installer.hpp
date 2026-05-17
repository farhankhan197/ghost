#ifndef GHOST_HOOKS_INSTALLER_HPP
#define GHOST_HOOKS_INSTALLER_HPP

#include <string>

namespace ghost {
namespace hooks {

class Installer {
public:
    static int installRepo(const std::string& repoRoot);
    static int installGlobal();
    static int installBin();
    static int uninstallRepo(const std::string& repoRoot);
    static int uninstallGlobal();
};

}
}

#endif
