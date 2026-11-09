#pragma once
#include "shims.h"
#include <string>
namespace native_poc {
Bridge formatBridge(const std::string& name);
void setGuestLogging(bool enabled);
}
