#pragma once
#include "shims.h"
#include <string>
namespace native_poc {
void guestInlineTimeSyscall(uint64_t* registers);
Bridge posixBridge(const std::string& name);
void setRuntimeDirectory(const std::string& path);
bool writablePath(const char* path);
int& guestErrno();
int linuxError(int value);
}
