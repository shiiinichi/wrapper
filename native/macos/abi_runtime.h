#pragma once
#include "shims.h"
#include <string>
namespace native_poc {
class Loader;
void bindRuntimeLoader(Loader* loader);
Bridge runtimeBridge(const std::string& name);
void* guardedImport(const std::string& name);
void finishImportGuards();
void* guestThreadPointer();
}
