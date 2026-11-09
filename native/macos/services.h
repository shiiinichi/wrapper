#pragma once
#include <string>
namespace native_poc {
class Loader;
std::string probeServices(Loader& loader, const std::string& data_directory);
}
