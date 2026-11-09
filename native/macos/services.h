#pragma once
#include <string>
#include "guest_types.h"
namespace native_poc {
class Loader;
struct Services {
    GuestShared context, presentation;
    std::string evidence;
};
Services initializeServices(Loader& loader, const std::string& data_directory, const std::string& identity = "a1368dcb4907e25f");
std::string probeServices(Loader& loader, const std::string& data_directory);
}
