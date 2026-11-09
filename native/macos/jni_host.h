#pragma once

#include <string>

namespace native_poc {
class Loader;
void installCrashDiagnostics(Loader& loader);
std::string probeAppleJni(Loader& loader, const std::string& library);
}
