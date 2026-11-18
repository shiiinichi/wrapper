#pragma once
#include <cstdint>

namespace native_poc {
enum class WorkerStatus : uint8_t { success = 0, rejected = 1, noAccount = 2 };
constexpr uint32_t profileReadOperation = 7, profileRefreshOperation = 8;
constexpr int profileSDKTimeoutSeconds = 8, profileWorkerTimeoutSeconds = 12;
struct NoSavedAccount {};
}
