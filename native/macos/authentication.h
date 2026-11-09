#pragma once
#include <string>
namespace native_poc {
class Loader;
struct AuthenticationResult { std::string evidence; bool authenticated; };
AuthenticationResult authenticate(Loader& loader, const std::string& directory, bool preflight, const std::string& prompt_script = "");
std::string probeAuthNetwork(Loader& loader, const std::string& directory);
std::string probeAuthenticationAbi(Loader& loader, const std::string& directory);
std::string authenticationStatus(Loader& loader, const std::string& directory);
}
