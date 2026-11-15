#include "runtime.h"
#include "../music_client.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <regex>

namespace wrapper {
namespace {
std::string urlEncode(const std::string &value) {
    const char *hex = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : value) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~')
            result += char(c);
        else {
            result += '%';
            result += hex[c >> 4];
            result += hex[c & 15];
        }
    }
    return result;
}
Bytes decode64(const std::string &input, bool url = false) {
    static const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    Bytes result;
    uint32_t value = 0;
    int bits = 0;
    size_t padding = 0, digits = 0;
    for (char c : input) {
        if (c == '=') {
            ++padding;
            continue;
        }
        if (padding)
            return {};
        if (url && c == '-')
            c = '+';
        if (url && c == '_')
            c = '/';
        auto index = alphabet.find(c);
        if (index == std::string::npos)
            return {};
        ++digits;
        value = (value << 6) | uint32_t(index);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result.push_back(static_cast<uint8_t>(value >> bits));
        }
    }
    if (!digits || digits % 4 == 1 || padding > 2 || (padding && (digits + padding) % 4) ||
        (!url && input.size() % 4))
        return {};
    return result;
}
bool hasErrors(const Json &root) {
    if (!root.is_object())
        return true;
    if (root.contains("errors") && !root["errors"].empty())
        return true;
    if (root.contains("failureType") && !root["failureType"].is_null())
        return true;
    for (auto name : {"status", "errorCode"})
        if (root.contains(name) && root[name].is_number() && root[name] != 0)
            return true;
    return false;
}
const Json &firstItem(const Json &root, const char *name) {
    if (!root.contains(name) || !root[name].is_array() || root[name].empty() || !root[name][0].is_object())
        fail(NMW_INVALID);
    return root[name][0];
}
std::string playableURL(const Json &object, const char *key) {
    auto s = field(object, key);
    return urlHost(s).empty() ? std::string{} : s;
}
void replaceAll(std::string &text, const std::string &a, const std::string &b) {
    size_t pos = 0;
    while ((pos = text.find(a, pos)) != std::string::npos) {
        text.replace(pos, a.size(), b);
        pos += b.size();
    }
}
// Operate on tags while retaining translations and enclosing TTML attributes.
// A linear scan also bounds stack use for long lyrics.
std::string lineLyrics(std::string source) {
    replaceAll(source, "itunes:timing=\"Word\"", "itunes:timing=\"Line\"");
    std::string text;
    text.reserve(source.size());
    size_t at = 0;
    bool collapse = false;
    while (at < source.size()) {
        if (source[at] != '<') {
            char c = source[at++];
            if (!collapse || c != ' ' || text.empty() || text.back() != ' ')
                text += c;
            continue;
        }
        auto end = source.find('>', at + 1);
        if (end == std::string::npos) {
            text.append(source, at);
            break;
        }
        bool closing = at + 1 < end && source[at + 1] == '/';
        auto start = at + (closing ? 2 : 1);
        auto stop = source.find_first_of(" \r\n\t/>", start);
        auto name = source.substr(start, stop - start);
        auto colon = name.rfind(':');
        auto local = colon == std::string::npos ? name : name.substr(colon + 1);
        if (local != "span")
            text.append(source, at, end - at + 1);
        if (local == "p" || local == "text")
            collapse = !closing;
        at = end + 1;
    }
    return text;
}
Json artwork(const Json &img, const std::string &crop, const std::string &ext, bool wide = false) {
    if (!img.is_object() || !img.contains("width") || !img.contains("height") || !img["width"].is_number() ||
        !img["height"].is_number())
        return nullptr;
    const double w = img["width"].get<double>(), h = img["height"].get<double>();
    if (!std::isfinite(w) || !std::isfinite(h) || std::trunc(w) != w || std::trunc(h) != h || w <= 0 || h <= 0 ||
        w > INT32_MAX || h > INT32_MAX)
        return nullptr;
    auto raw = field(img, "url");
    if (raw.size() > 8192 || raw.rfind("https://", 0) != 0)
        return nullptr;
    auto query = raw.find_first_of("?#");
    auto path = raw.substr(0, query);
    auto last = path.rfind('/');
    if (last == std::string::npos || urlHost(path.substr(0, last) + "/image").empty())
        return nullptr;
    static const std::regex filename(R"(^\{w\}x\{h\}(?:\{c\}|[A-Za-z0-9]*)\.(?:\{f\}|jpg|jpeg|png|webp|avif)$)",
                                     std::regex::icase);
    if (!std::regex_match(path.substr(last + 1), filename))
        return nullptr;
    std::string pattern = path.substr(0, last + 1) + "{w}x{h}" + crop + "." + ext +
                          (query == std::string::npos ? "" : raw.substr(query));
    int width = wide ? 1600 : int(w), height = wide ? 900 : int(h);
    auto rendered = pattern;
    replaceAll(rendered, "{w}", std::to_string(width));
    replaceAll(rendered, "{h}", std::to_string(height));
    return {{"url", rendered}, {"urlTemplate", pattern}, {"width", width}, {"height", height}};
}
Json artist(const Json &root, const std::string &id, const std::string &region) {
    if (!root.contains("data") || !root["data"].is_array())
        fail(NMW_INVALID);
    if (root["data"].empty())
        return nullptr;
    const auto &first = firstItem(root, "data");
    if (field(first, "type") != "artists" || field(first, "id") != id || !first.contains("attributes"))
        fail(NMW_INVALID);
    const auto &a = first["attributes"];
    auto name = field(a, "name");
    if (name.empty())
        fail(NMW_INVALID);
    Json genres = Json::array();
    if (a.contains("genreNames") && a["genreNames"].is_array() &&
        std::all_of(a["genreNames"].begin(), a["genreNames"].end(), [](const auto &v) { return v.is_string(); }))
        genres = a["genreNames"];
    Json j = {{"adamId", id}, {"storefront", region}, {"name", name}, {"genreNames", genres}};
    for (auto key : {"url", "artwork", "artistBio", "origin", "bornOrFormed", "editorialNotes"})
        j[key] = a.contains(key) ? a[key] : Json(nullptr);
    auto editorial = a.value("editorialArtwork", Json(nullptr));
    j["artistLogo"] = artwork(
        editorial.is_object() ? editorial.value("musicContentColorLogoTrimmed", Json(nullptr)) : Json(nullptr),
        "bb", "png");
    Json landscape = artwork(editorial.is_object() ? editorial.value("centeredFullscreenBackground", Json(nullptr))
                                                   : Json(nullptr),
                             "sr", "jpg", true);
    if (landscape.is_null() && a.contains("hero") && a["hero"].is_array() && !a["hero"].empty()) {
        const auto &hero = a["hero"][0];
        if (hero.is_object() && hero.contains("content") && hero["content"].is_array() &&
            !hero["content"].empty() && hero["content"][0].is_object()) {
            auto picture = hero["content"][0].value("artwork", Json(nullptr));
            if (picture.is_object() && picture.contains("recommendedCropCodes") &&
                picture["recommendedCropCodes"].is_array() &&
                std::find(picture["recommendedCropCodes"].begin(), picture["recommendedCropCodes"].end(), "vf") !=
                    picture["recommendedCropCodes"].end())
                landscape = artwork(picture, "vf", "jpg", true);
        }
    }
    j["wideArtwork"] = landscape;
    return j;
}
} // namespace
bool validChallenge(const std::string &challenge, const std::string &uri, const std::string &drm) {
    return (drm == "wv" || drm == "pr") && challenge.size() <= 48 * 1024 && !decode64(challenge).empty() &&
           !uri.empty() && uri.size() <= 8192 &&
           std::none_of(uri.begin(), uri.end(), [](unsigned char c) { return c < 32 || c == 127; });
}
Bytes Service::fetch(const HTTPRequest &value, const NMWOperation &op) {
    HTTPRequest request = value;
    request.headers["User-Agent"] = native_poc::music_client::userAgent();
    if (!request.body.empty())
        request.headers["Content-Type"] = "application/json";
    if (request.body.size() > maxMetadata)
        fail(NMW_OVERSIZED);
    if (!hook)
        return https(request, op);
    return external(NMW_HTTP,
                    bytes(Json{{"url", request.url}, {"headers", request.headers}, {"body", request.body}}), op);
}
std::string Service::webToken(const NMWOperation &op) {
    check(op);
    std::unique_lock<std::timed_mutex> lock(developerLock, std::defer_lock);
    while (!lock.try_lock_for(std::chrono::milliseconds(25)))
        check(op);
    check(op);
    if (!developer.empty() && Clock::now() - developerAt < std::chrono::hours(24))
        return developer;
    auto document = fetch({"https://music.apple.com", {}, {}}, op);
    std::string html(document.begin(), document.end());
    auto start = html.find("/assets/index~");
    if (start == std::string::npos)
        fail(NMW_MESSAGE, "Apple web token asset was not found.");
    auto end = html.find(".js", start), invalid = html.find_first_of(" \t\r\n\"<>", start);
    if (end == std::string::npos || end - start > 1024 || invalid < end || html.find('/', start + 8) < end)
        fail(NMW_MESSAGE, "Apple web token asset was not found.");
    auto script = fetch({"https://music.apple.com" + html.substr(start, end - start + 3), {}, {}}, op);
    std::string js(script.begin(), script.end());
    size_t offset = 0;
    auto part = [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-' || c == '='; };
    while ((offset = js.find("eyJ", offset)) != std::string::npos) {
        auto head = offset;
        while (head < js.size() && part(js[head]))
            ++head;
        auto payload = head + 1, tail = payload;
        if (head < js.size() && js[head] == '.')
            while (tail < js.size() && part(js[tail]))
                ++tail;
        if (tail < js.size() && js[tail] == '.' && tail > payload && tail - payload < 128 * 1024) {
            auto finish = tail + 1;
            while (finish < js.size() && part(js[finish]))
                ++finish;
            auto claims = decode64(js.substr(payload, tail - payload), true);
            auto j = Json::parse(claims.begin(), claims.end(), nullptr, false);
            if (finish > tail + 1 && finish - offset < 128 * 1024 && !j.is_discarded() &&
                field(j, "iss") == "AMPWebPlay") {
                developer = js.substr(offset, finish - offset);
                developerAt = Clock::now();
                return developer;
            }
        }
        offset = head > offset ? head : offset + 3;
    }
    fail(NMW_MESSAGE, "Apple web developer token was not found.");
}
Json Service::credentials(const NMWOperation &op, bool webRequired) {
    if (!tokens.is_object() || Clock::now() - tokenAt >= std::chrono::minutes(30)) {
        Bytes req;
        appendWord(req, 5);
        auto raw = exchange(req, op);
        if (raw.size() > 512 * 1024)
            fail(NMW_OVERSIZED);
        tokens = json(raw);
        tokenAt = Clock::now();
    }
    std::string bearer;
    try {
        bearer = webToken(op);
    } catch (const Failure &e) {
        if (e.code == NMW_CANCELLED || webRequired)
            throw;
        bearer = field(tokens, "developer_token");
    }
    auto user = field(tokens, "music_token");
    for (const auto *s : {&user, &bearer})
        if (s->empty() || s->size() > 128 * 1024 || s->find_first_of("\r\n") != std::string::npos)
            fail(NMW_NO_ACCOUNT);
    return {{"bearer", bearer}, {"user", user}, {"storefront", field(tokens, "storefront")}};
}
Json Service::webRoute(const std::string &route, const std::string &id, const Json &q, const Bytes &body,
                       const NMWOperation &op) {
    const std::string origin = "https://music.apple.com", amp = "https://amp-api.music.apple.com/v1/";
    if (route == "/artist") {
        auto region = field(q, "storefront", "us");
        std::transform(region.begin(), region.end(), region.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        const auto url = amp + "catalog/" + region + "/artists/" + id +
                         "?extend=artistBio,origin,bornOrFormed,editorialArtwork,hero";
        auto bearer = webToken(op);
        try {
            auto root =
                json(fetch({url, {{"Authorization", "Bearer " + bearer}, {"Origin", origin}}, {}}, op));
            if (hasErrors(root))
                fail(NMW_INVALID);
            return artist(root, id, region);
        } catch (const Failure &e) {
            if (e.code == NMW_HTTP_BASE + 404)
                return nullptr;
            throw;
        }
    }
    Json license;
    if (route == "/license") {
        if (body.size() > maxMetadata)
            fail(NMW_OVERSIZED);
        license = json(body);
        if (license.contains("drm-type") && !license["drm-type"].is_string())
            fail(NMW_MESSAGE, "drm-type must be wv or pr.");
        auto challenge = field(license, "challenge"), uri = field(license, "uri"),
             drm = field(license, "drm-type", "wv");
        if (!validID(field(license, "adamId")) || !validChallenge(challenge, uri, drm))
            fail(NMW_MESSAGE, "Enter a valid Base64 license challenge and key URI.");
    }
    auto credentials = Service::credentials(op, route == "/license");
    auto user = field(credentials, "user"), region = field(credentials, "storefront");
    HTTPRequest request{{}, {{"Authorization", "Bearer " + field(credentials, "bearer")}, {"Origin", origin}}, {}};
    if (route == "/lyrics") {
        if (region.size() != 2 ||
            !std::all_of(region.begin(), region.end(), [](unsigned char c) { return std::isalpha(c) && c < 128; }))
            fail(NMW_NO_ACCOUNT);
        auto language = field(q, "language", "en"), script = field(q, "script", "en-Latn");
        request.url = amp + "catalog/" + region + "/songs/" + id +
                      "/syllable-lyrics?l%5Blyrics%5D=" + urlEncode(language) +
                      "&extend=ttmlLocalizations&l%5Bscript%5D=" + urlEncode(script);
        request.headers["media-user-token"] = user;
    } else if (route == "/assets") {
        if (user.find(';') != std::string::npos)
            fail(NMW_INVALID);
        request.url = amp + "play/assets?id=" + id +
                      "&kind=song&includeLicenseUrls=true&hlsEncryption=CBC&hlsProfile=enhancedHls";
        request.headers["media-user-token"] = user;
        request.headers["Cookie"] = "media-user-token=" + user;
    } else {
        request.headers["X-Apple-Music-User-Token"] = user;
        if (route == "/license") {
            request.url = "https://play.itunes.apple.com/WebObjects/MZPlay.woa/wa/acquireWebPlaybackLicense";
            request.body = Json{
                {"challenge", license["challenge"]},
                {"uri", license["uri"]},
                {"key-system",
                 field(license, "drm-type") == "pr" ? "com.microsoft.playready" : "com.widevine.alpha"},
                {"adamId", license["adamId"]},
                {"isLibrary", false},
                {"user-initiated",
                 true}}.dump();
        } else {
            request.url = "https://play.music.apple.com/WebObjects/MZPlay.woa/wa/webPlayback";
            request.body = Json{{"salableAdamId", id}}.dump();
        }
    }
    Json root;
    try {
        root = json(fetch(request, op));
    } catch (const Failure &error) {
        if (error.code == NMW_HTTP_BASE + 404 && (route == "/lyrics" || route == "/webplayback"))
            fail(NMW_NOT_FOUND, "Content was not found.");
        throw;
    }
    if (hasErrors(root))
        fail(NMW_MESSAGE, "Apple rejected the request.");
    if (route == "/lyrics") {
        if (root.contains("data") && root["data"].is_array() && root["data"].empty())
            fail(NMW_NOT_FOUND, "Lyrics were not found.");
        const auto &item = firstItem(root, "data");
        if (!item.contains("attributes"))
            fail(NMW_INVALID);
        const auto &a = item["attributes"];
        auto lyrics = field(a, "ttmlLocalizations");
        auto syllable = field(q, "syllable", "1");
        std::transform(syllable.begin(), syllable.end(), syllable.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        bool words = syllable != "0" && syllable != "false" && syllable != "no" && syllable != "off";
        if (!words)
            lyrics = lyrics.empty() ? field(a, "text") : lineLyrics(std::move(lyrics));
        if (lyrics.empty())
            fail(NMW_NOT_FOUND, "Lyrics were not found.");
        return {{"adamId", id}, {"syllable", words ? "true" : "false"}, {"lyrics", lyrics}};
    }
    if (route == "/assets") {
        if (root.contains("results") && root["results"].is_object() && root["results"].contains("assets") &&
            root["results"]["assets"].is_array())
            for (const auto &a : root["results"]["assets"]) {
                auto url = playableURL(a, "url");
                if (!url.empty())
                    return {{"adamId", id}, {"m3u8", url}, {"streamType", "hls"}};
            }
        fail(NMW_MESSAGE, "Apple returned no HLS playback asset.");
    }
    if (route == "/license") {
        auto value = field(root, "license");
        if (value.empty())
            fail(NMW_MESSAGE, "Apple returned no playback license.");
        return {{"adamId", license["adamId"]},
                {"license", value},
                {"renew", root.contains("renew-after") ? root["renew-after"] : Json(0)}};
    }
    if (root.contains("songList") && root["songList"].is_array() && root["songList"].empty())
        fail(NMW_NOT_FOUND, "Web playback is unavailable for this track.");
    const auto &song = firstItem(root, "songList");
    if (song.contains("songId")) {
        auto found = song["songId"].is_string() ? song["songId"].get<std::string>() : song["songId"].dump();
        if (found != id)
            fail(NMW_MESSAGE, "Apple returned playback assets for a different track.");
    }
    auto hls = playableURL(song, "hls-playlist-url");
    std::string aac;
    Json assets = Json::array();
    if (song.contains("assets") && song["assets"].is_array())
        for (const auto &a : song["assets"]) {
            auto f = field(a, "flavor"), url = playableURL(a, "URL");
            if (url.empty() || f.empty())
                continue;
            assets.push_back({{"flavor", f}, {"URL", url}});
            if (f == "28:ctrp256" && aac.empty())
                aac = url;
        }
    if (hls.empty() && aac.empty())
        fail(NMW_NOT_FOUND, "Web playback is unavailable for this track.");
    Json result = {{"adamId", id},
                   {"m3u8", hls.empty() ? aac : hls},
                   {"streamType", hls.empty() ? "aac-cenc" : "hls"},
                   {"assets", assets}};
    if (hls.empty())
        result["flavor"] = "28:ctrp256";
    return result;
}
} // namespace wrapper
extern "C" int nmw_valid_challenge(const char *challenge, const char *uri, const char *drm) {
    if (!challenge || !uri || !drm)
        return 0;
    try {
        return wrapper::validChallenge(challenge, uri, drm);
    } catch (...) {
        return 0;
    }
}
