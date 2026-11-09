#pragma once
#include <string>
namespace native_poc {
class Loader;
std::string nativeApi(Loader& loader, const std::string& directory, const std::string& operation,
    const std::string& adam_id, const std::string& key_uri);
}
