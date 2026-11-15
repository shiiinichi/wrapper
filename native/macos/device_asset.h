#pragma once
#include <string>

namespace native_poc {
struct DeviceAsset {
    std::string url, flavor;
};

// wrapper-lite's get_m3u8 policy. A device-returned file is a valid result;
// only an empty first result permits a second SDK lookup.
template <class Subscription, class Playback>
DeviceAsset resolveDeviceAsset(bool offlineChannel, Subscription subscription, Playback playback) {
    auto preferred = offlineChannel ? subscription() : playback();
    if (!preferred.url.empty())
        return preferred;
    return offlineChannel ? playback() : subscription();
}
} // namespace native_poc
