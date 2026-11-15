#pragma once
#include <cstdint>

namespace native_poc {
enum class WorkerStatus : uint8_t { success = 0, rejected = 1, noAccount = 2 };
}
