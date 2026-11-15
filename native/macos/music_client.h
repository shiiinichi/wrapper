#pragma once
#include <string>
namespace native_poc::music_client {
inline constexpr const char *name = "Music";
inline constexpr const char *version = "5.0.2";
inline constexpr const char *platform = "Android";
inline constexpr const char *platformVersion = "10";
inline constexpr const char *model = "Pixel 8";
inline constexpr const char *build = "7663314";
inline constexpr const char *locale = "en-US";
inline std::string userAgent() {
    return std::string(name) + "/" + version + " " + platform + "/" + platformVersion + " model/" + model +
           " build/" + build + " (dt:66)";
}
} // namespace native_poc::music_client
