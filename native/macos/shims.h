#pragma once

#include <string>

namespace native_poc {

struct Bridge {
    void* address = nullptr;
    bool guarded = false;
    bool data = false;
};

// Only explicitly reviewed symbols are bridged. Unknown imports stay unresolved.
Bridge bridgeSymbol(const std::string& name);

} // namespace native_poc
