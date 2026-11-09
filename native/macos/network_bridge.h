#pragma once
#include "shims.h"
#include <string>
namespace native_poc {
Bridge networkBridge(const std::string& name);
struct NetworkState { unsigned transfers; int result; long http_status; };
NetworkState networkState();
}
