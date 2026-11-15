#pragma once
#include <string>
#include <optional>
#include <stdexcept>

namespace native_poc {
struct DeviceAsset {
    std::string url, flavor;
};
struct DeviceManifestSession {
    std::optional<bool> offlineChannel;
    bool requestedPlaybackLease = false;

    void requirePlaybackLease(bool (*request)(void*, const bool*), void* lease) {
        if (requestedPlaybackLease) return;
        const bool interrupt = false;
        if (!request(lease, &interrupt))
            throw std::runtime_error("native playback lease request failed");
        requestedPlaybackLease = true;
    }
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
