#pragma once
#include <string>
namespace native_poc {
class Loader;
std::string probeSecurityAbi(Loader& loader);
std::string probeFileConfinement(const std::string& directory);
}
