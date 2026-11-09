#pragma once
#include <string>
namespace native_poc {
class Loader;
std::string probeTlsAbi(Loader& loader);
std::string probeRuntimeAbi(const std::string& assets);
std::string probeServiceAbi(Loader& loader, const std::string& assets);
}
