#pragma once
#include "shims.h"
#include <string>
namespace native_poc {
void guestInlineTimeSyscall(uint64_t* registers);
Bridge posixBridge(const std::string& name);
void setRuntimeDirectory(const std::string& path);
std::string runtimeDirectory();
bool writablePath(const char* path);
int openRuntimeFile(const char* path, int flags, unsigned mode,
                    int (*openFile)(const char*, int, int) = nullptr);
int& guestErrno();
int linuxError(int value);
}
