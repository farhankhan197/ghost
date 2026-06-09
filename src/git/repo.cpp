#include "repo.hpp"
#include "engine.hpp"

namespace ghost {
namespace git {

std::string Repo::getRoot() {
    return Engine::discoverRoot(".");
}

std::string Repo::getHead() {
    return Engine::headSha(".");
}

bool Repo::isRepo() {
    return !getRoot().empty();
}

std::string Repo::getUserEmail() {
    return Engine::configString(".", "user.email");
}

}
}
