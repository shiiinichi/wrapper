#!/usr/bin/env python3
"""Upstream credential and bounded decompression regressions."""
from email.message import Message
import gzip
import io
import unittest
from unittest.mock import patch
import urllib.request
from urllib.response import addinfourl
import zlib
from web_api import ApiError, AppleWebApi, bounded_gzip

def response(body, url, code=200, headers=None):
    message = Message()
    for key, value in (headers or {}).items():
        message[key] = value
    result = addinfourl(io.BytesIO(body), message, url, code)
    result.msg = "Found" if code == 302 else "OK"
    return result


class UpstreamBoundaryTests(unittest.TestCase):
    def redirected_api(self, location):
        observed = []

        class FixtureTransport(urllib.request.BaseHandler):
            handler_order = 100

            def https_open(self, request):
                observed.append(request.full_url)
                if len(observed) == 1:
                    return response(b"", request.full_url, 302, {"Location": location})
                return response(b"fixture", request.full_url)

            def http_open(self, request):
                observed.append(request.full_url)
                return response(b"fixture", request.full_url)

        api = AppleWebApi(lambda: {})
        api.opener.add_handler(FixtureTransport())
        return api, observed

    def test_redirects_cannot_downgrade_or_forward_credentials_to_another_origin(self):
        for target in ("http://music.apple.com/next", "https://attacker.invalid/next",
                       "https://music.apple.com:444/next", "https://user@music.apple.com/next"):
            api, observed = self.redirected_api(target)
            with self.subTest(target=target), self.assertRaises(ApiError) as error:
                api.fetch("https://music.apple.com/fixture", {"Authorization": "Bearer fixture", "media-user-token": "fixture"})
            self.assertEqual(error.exception.code, 502)
            self.assertEqual(observed, ["https://music.apple.com/fixture"])

    def test_same_origin_https_redirects_still_work(self):
        api, observed = self.redirected_api("https://music.apple.com/next")
        self.assertEqual(api.fetch("https://music.apple.com/fixture"), "fixture")
        self.assertEqual(observed, ["https://music.apple.com/fixture", "https://music.apple.com/next"])

    def test_gzip_decoding_bounds_allocation_before_expanding_a_bomb(self):
        compressed = gzip.compress(b"a" * 100000)
        factory = zlib.decompressobj
        capacities = []

        class Decoder:
            def __init__(self, *args):
                self.decoder = factory(*args)

            def decompress(self, value, max_length=0):
                capacities.append(max_length)
                return self.decoder.decompress(value, max_length)

            def __getattr__(self, name):
                return getattr(self.decoder, name)

        with patch("web_api.zlib.decompressobj", Decoder), self.assertRaises(ApiError) as error:
            bounded_gzip(compressed, limit=64)
        self.assertEqual(error.exception.code, 502)
        self.assertTrue(capacities and all(0 < size <= 65 for size in capacities))

    def test_gzip_members_share_a_single_output_budget_and_truncation_is_rejected(self):
        members = gzip.compress(b"alpha") + gzip.compress(b"beta")
        self.assertEqual(bounded_gzip(members, 9), b"alphabeta")
        with self.assertRaises(ApiError):
            bounded_gzip(members, 8)
        with self.assertRaises(ApiError):
            bounded_gzip(gzip.compress(b"alpha")[:-3])


if __name__ == "__main__":
    unittest.main(verbosity=2)
