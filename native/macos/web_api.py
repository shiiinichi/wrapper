"""Apple web requests using tokens issued by the native Android library."""
import base64
import gzip
import json
import re
import ssl
import time
import urllib.error
import urllib.parse
import urllib.request


class ApiError(Exception):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def line_lyrics(ttml):
    """Keep localizations and their keys while removing word-level spans."""
    result = ttml.replace('itunes:timing="Word"', 'itunes:timing="Line"')
    result = re.sub(r"</?(?:[A-Za-z_][\w.-]*:)?span\b[^>]*>", "", result)
    return re.sub(r"(<(?:p|text)\b[^>]*>)([\s\S]*?)(</(?:p|text)>)",
                  lambda match: match[1] + re.sub(r" {2,}", " ", match[2]) + match[3], result)


class AppleWebApi:
    def __init__(self, token_provider):
        self.token_provider = token_provider
        self.developer = ""
        self.developer_time = 0
        context = ssl.create_default_context(cafile="/etc/ssl/cert.pem")
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}),
                                                  urllib.request.HTTPSHandler(context=context))

    def fetch(self, url, headers=None, body=None):
        if urllib.parse.urlsplit(url).scheme != "https":
            raise ApiError(502, "Apple request requires HTTPS")
        values = {"User-Agent": "Music/5.7 Android/10 model/Pixel6GR1YH build/1234 (dt:66)",
                  "Accept-Encoding": "gzip"}
        values.update(headers or {})
        request = urllib.request.Request(url, headers=values,
                                         data=json.dumps(body).encode() if body is not None else None)
        if body is not None:
            request.add_header("Content-Type", "application/json")
        try:
            with self.opener.open(request, timeout=25) as response:
                raw = response.read(16 * 1024 * 1024 + 1)
                if len(raw) > 16 * 1024 * 1024:
                    raise ApiError(502, "Apple response is too large")
                if response.headers.get("Content-Encoding") == "gzip":
                    raw = gzip.decompress(raw)
                if len(raw) > 32 * 1024 * 1024:
                    raise ApiError(502, "Apple response is too large")
                return raw.decode("utf-8")
        except urllib.error.HTTPError as error:
            # Do not expose signed request URLs or Apple account response bodies.
            raise ApiError(502, "Apple returned HTTP " + str(error.code)) from None
        except (urllib.error.URLError, TimeoutError, OSError, ValueError):
            raise ApiError(502, "Apple network request failed") from None

    def web_developer_token(self):
        if self.developer and time.monotonic() - self.developer_time < 86400:
            return self.developer
        html = self.fetch("https://music.apple.com")
        script = re.search(r"/assets/index~[^/\s\"<>]+\.js", html)
        if not script:
            raise ApiError(502, "Apple web token asset was not found")
        javascript = self.fetch("https://music.apple.com" + script[0])
        for candidate in re.findall(r"eyJ[A-Za-z0-9_=-]+\.[A-Za-z0-9_=-]+\.[A-Za-z0-9_=-]+", javascript):
            try:
                encoded = candidate.split(".")[1]
                claims = json.loads(base64.urlsafe_b64decode(encoded + "=" * (-len(encoded) % 4)))
                if claims.get("iss") == "AMPWebPlay":
                    self.developer = candidate
                    self.developer_time = time.monotonic()
                    return candidate
            except (ValueError, UnicodeError):
                continue
        raise ApiError(502, "Apple web developer token was not found")

    def credentials(self, require_web_token=False):
        tokens = self.token_provider()
        try:
            developer = self.web_developer_token()
        except ApiError:
            if require_web_token:
                raise
            developer = tokens.get("developer_token", "")
        if not developer or not tokens.get("music_token"):
            raise ApiError(401, "Account tokens are unavailable; sign in again")
        return tokens, developer

    def json_request(self, url, headers, body=None):
        try:
            value = json.loads(self.fetch(url, headers, body))
        except json.JSONDecodeError:
            raise ApiError(502, "Apple returned invalid JSON") from None
        if not isinstance(value, dict) or value.get("errors") or value.get("failureType"):
            raise ApiError(502, "Apple rejected the request")
        return value

    def lyrics(self, adam, language, syllable, script):
        tokens, developer = self.credentials()
        region = tokens.get("storefront", "")
        if not re.fullmatch(r"[A-Za-z]{2}", region):
            raise ApiError(401, "The account storefront is unavailable")
        query = urllib.parse.urlencode({"l[lyrics]": language, "extend": "ttmlLocalizations", "l[script]": script})
        value = self.json_request("https://amp-api.music.apple.com/v1/catalog/" + region + "/songs/" + adam + "/syllable-lyrics?" + query,
                                  {"Authorization": "Bearer " + developer, "media-user-token": tokens["music_token"],
                                   "Origin": "https://music.apple.com"})
        data = value.get("data") or []
        attributes = data[0].get("attributes", {}) if data else {}
        lyrics = attributes.get("ttmlLocalizations", "")
        if not isinstance(lyrics, str) or not lyrics:
            raise ApiError(404, "lyrics not found")
        return {"adamId": adam, "syllable": "true" if syllable else "false",
                "lyrics": lyrics if syllable else line_lyrics(lyrics)}

    def enhanced_hls(self, adam):
        tokens, developer = self.credentials()
        region = tokens.get("storefront", "")
        if not re.fullmatch(r"[A-Za-z]{2}", region):
            raise ApiError(401, "The account storefront is unavailable")
        value = self.json_request("https://amp-api.music.apple.com/v1/catalog/" + region + "/songs/" + adam + "?extend=extendedAssetUrls",
                                  {"Authorization": "Bearer " + developer, "media-user-token": tokens["music_token"],
                                   "Origin": "https://music.apple.com"})
        songs = value.get("data") or []
        attributes = songs[0].get("attributes", {}) if songs else {}
        url = (attributes.get("extendedAssetUrls") or {}).get("enhancedHls", "")
        if not isinstance(url, str) or not url.startswith("https://"):
            raise ApiError(404, "enhanced HLS not available")
        return {"adamId": adam, "m3u8": url}

    def webplayback(self, adam):
        tokens, developer = self.credentials()
        value = self.json_request("https://play.music.apple.com/WebObjects/MZPlay.woa/wa/webPlayback",
                                  {"Authorization": "Bearer " + developer,
                                   "X-Apple-Music-User-Token": tokens["music_token"]}, {"salableAdamId": adam})
        songs = value.get("songList") or []
        first = songs[0] if songs else {}
        url = first.get("hls-playlist-url", "")
        if not url:
            url = next((asset.get("URL", "") for asset in first.get("assets", [])
                        if asset.get("flavor") == "28:ctrp256"), "")
        if not isinstance(url, str) or not url.startswith("https://"):
            raise ApiError(404, "webplayback not available")
        return {"adamId": adam, "m3u8": url}

    def license(self, adam, challenge, uri, drm):
        tokens, developer = self.credentials(require_web_token=True)
        value = self.json_request("https://play.itunes.apple.com/WebObjects/MZPlay.woa/wa/acquireWebPlaybackLicense",
                                  {"Authorization": "Bearer " + developer,
                                   "X-Apple-Music-User-Token": tokens["music_token"]},
                                  {"challenge": challenge, "uri": uri, "key-system": "com.microsoft.playready" if drm == "pr" else "com.widevine.alpha",
                                   "adamId": adam, "isLibrary": False, "user-initiated": True})
        license = value.get("license")
        if not isinstance(license, str) or not license:
            raise ApiError(502, "license acquisition failed")
        return {"adamId": adam, "license": license, "renew": value.get("renew-after", 0)}
