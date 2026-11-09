#pragma once
#include "shims.h"
#include <string>
namespace native_poc {
Bridge sqliteBridge(const std::string& name, bool data);
size_t nativeSqliteOpenCount();
}
