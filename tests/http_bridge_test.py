import json
import base64
import sys
import unittest
from pathlib import Path
from unittest.mock import patch
from urllib.error import HTTPError
from email.message import Message
from io import BytesIO

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from http_bridge import MAX_BODY, MAX_ENVELOPE, fetch


class FakeResponse:
    status = 201
    reason = "Created"
    headers = Message()

    def __init__(self, body=b'{"ok":true}'):
        self.body = body

    def __enter__(self):
        return self

    def __exit__(self, *_):
        pass

    def read(self, size):
        return self.body[:size]


class HttpBridgeTests(unittest.TestCase):
    def test_methods_headers_and_body(self):
        def run(request, timeout):
            self.assertEqual(timeout, 15)
            self.assertEqual(request.get_method(), "POST")
            self.assertEqual(request.full_url, "https://example.com/path")
            self.assertEqual(request.data, b'{"n":42}')
            self.assertEqual(request.get_header("X-test"), "a")
            self.assertEqual(request.get_header("User-agent"), "Roblox/WinInet")
            return FakeResponse()

        with patch("http_bridge.urllib.request.urlopen", side_effect=run):
            result = fetch({"Url": "https://example.com/path", "Method": "POST", "Headers": {"X-test": "a"}, "Body": '{"n":42}'})
        self.assertEqual((result["Success"], result["StatusCode"], result["Body"]), (True, 201, '{"ok":true}'))

    def test_http_error_is_response(self):
        with patch("http_bridge.urllib.request.urlopen", side_effect=HTTPError(
            "https://example.com/missing", 404, "Not Found", Message(), BytesIO(b"gone"))):
            response = fetch({"Url": "https://example.com/missing"})
        self.assertFalse(response["Success"])
        self.assertEqual(response["StatusCode"], 404)
        self.assertEqual(response["Body"], "gone")

    def test_explicit_user_agent_is_preserved(self):
        def run(request, timeout):
            self.assertEqual(request.get_header("User-agent"), "TestClient/1.0")
            return FakeResponse()

        with patch("http_bridge.urllib.request.urlopen", side_effect=run):
            fetch({"Url": "https://example.com/", "Headers": {"user-agent": "TestClient/1.0"}})

    def test_large_source_response_and_limits(self):
        source = b"--" + b"a" * (2290215 - 2)
        with patch("http_bridge.urllib.request.urlopen", return_value=FakeResponse(source)):
            self.assertEqual(fetch({"Url": "https://example.com/"})["Body"].encode(), source)
        with patch("http_bridge.urllib.request.urlopen", return_value=FakeResponse(b"x" * MAX_BODY)):
            self.assertEqual(len(fetch({"Url": "https://example.com/"})["Body"]), MAX_BODY)
        with patch("http_bridge.urllib.request.urlopen", return_value=FakeResponse(b"x" * (MAX_BODY + 1))):
            with self.assertRaisesRegex(ValueError, "response exceeds"):
                fetch({"Url": "https://example.com/"})
        with patch("http_bridge.urllib.request.urlopen") as request:
            with self.assertRaisesRegex(ValueError, "Body exceeds"):
                fetch({"Url": "https://example.com/", "Body": "x" * (MAX_BODY + 1)})
            request.assert_not_called()

    def test_json_envelope_fits_worst_case_escaping(self):
        with patch("http_bridge.urllib.request.urlopen", return_value=FakeResponse(b"\x00" * MAX_BODY)):
            result = fetch({"Url": "https://example.com/"})
        encoded = json.dumps(result, separators=(",", ":")).encode("utf-8")
        self.assertLessEqual(len(encoded), MAX_ENVELOPE)
        self.assertEqual(len(json.loads(encoded)["Body"]), MAX_BODY)

    def test_binary_request_and_response_are_lossless(self):
        binary = bytes(range(256))
        def run(request, timeout):
            self.assertEqual(request.data, binary)
            return FakeResponse(binary)
        with patch("http_bridge.urllib.request.urlopen", side_effect=run):
            result = fetch({"Url": "https://example.com/", "Method": "POST",
                            "BodyBase64": base64.b64encode(binary).decode()})
        self.assertEqual(base64.b64decode(result["BodyBase64"]), binary)

    def test_bad_request_is_rejected(self):
        for url in ("", "file:///etc/passwd", "data:text/plain,invalid"):
            with self.assertRaises(ValueError):
                fetch({"Url": url})
        with self.assertRaises(ValueError):
            fetch({"Url": "https://example.com/", "Headers": {"X-Test\n": "oops"}})


if __name__ == "__main__":
    unittest.main()
