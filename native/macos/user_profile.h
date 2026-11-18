#pragma once
#include <string>

namespace native_poc {
class Loader;
struct Services;
// Reads only the active account's profile. Refresh is an explicit SDK GET;
// callers must enforce the worker deadline as URL-bag bootstrap can block.
std::string currentUserProfile(Loader&, const Services&, bool refresh);
std::string probeUserProfileAbi(Loader&, const std::string& directory);
} // namespace native_poc
