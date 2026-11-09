#pragma once
#include "shims.h"
#include <string>
namespace native_poc {
Bridge runtimeBridge(const std::string& name);
void* guardedImport(const std::string& name);
void finishImportGuards();
void* guestThreadPointer();
}
