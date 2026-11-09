#pragma once

#include <string>
#include <cstddef>

namespace native_poc {

struct Bridge {
    void* address = nullptr;
    bool guarded = false;
    bool data = false;
};

// Only explicitly reviewed symbols are bridged. Unknown imports stay unresolved.
Bridge bridgeSymbol(const std::string& name);
size_t guestDestructorCount();

} // namespace native_poc
