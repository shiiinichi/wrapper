#pragma once
#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <string>

namespace native_poc {
constexpr size_t maximumProfileReply = 16 * 1024;
constexpr size_t maximumProfileName = 1024, maximumProfileHandle = 256, maximumProfileURL = 4096;
constexpr int maximumProfileDimension = 16384;
inline bool validProfileText(const std::string& value, size_t limit) {
    return !value.empty() && value.size() <= limit &&
        std::none_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
// Artwork templates are metadata, never fetched by the wrapper. Accept only
// Apple's image CDN, without credentials, query tokens, fragments or odd ports.
inline bool validProfileImageURL(const std::string& value) {
    if (value.empty() || value.size() > maximumProfileURL ||
        std::any_of(value.begin(), value.end(), [](unsigned char c) { return c <= 32 || c >= 127; }) ||
        value.find_first_of("\\\"?#") != value.npos)
        return false;
    for (size_t at = 0; at < value.size(); ++at) {
        if (value[at] == '}') return false;
        if (value[at] != '{') continue;
        if (at + 2 >= value.size() || value[at + 2] != '}' ||
            (value[at + 1] != 'w' && value[at + 1] != 'h' && value[at + 1] != 'c' && value[at + 1] != 'f'))
            return false;
        at += 2;
    }
    struct URL {
        CURLU* value = curl_url();
        ~URL() { if (value) curl_url_cleanup(value); }
        std::string get(CURLUPart part) const {
            char* raw = nullptr;
            if (curl_url_get(value, part, &raw, 0) != CURLUE_OK) return {};
            std::string result = raw ? raw : "";
            curl_free(raw);
            return result;
        }
        bool has(CURLUPart part) const {
            char* raw = nullptr;
            auto code = curl_url_get(value, part, &raw, 0);
            curl_free(raw);
            return code == CURLUE_OK;
        }
    } url;
    if (!url.value || curl_url_set(url.value, CURLUPART_URL, value.c_str(), 0) != CURLUE_OK ||
        url.get(CURLUPART_SCHEME) != "https" || url.has(CURLUPART_USER) || url.has(CURLUPART_PASSWORD) ||
        url.has(CURLUPART_QUERY) || url.has(CURLUPART_FRAGMENT))
        return false;
    auto host = url.get(CURLUPART_HOST), port = url.get(CURLUPART_PORT);
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return std::tolower(c); });
    const std::string suffix = ".mzstatic.com";
    return (port.empty() || port == "443") && host.size() > suffix.size() &&
        host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0;
}
} // namespace native_poc
