import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from execute import completion_status, uses_request_logs

class CompletionTests(unittest.TestCase):
    def test_session_advertises_protocol_when_log_header_was_truncated(self):
        self.assertTrue(uses_request_logs("pid=755\r\nprotocol=3\r\n", "editor bootstrap scheduled\n"))
        self.assertTrue(uses_request_logs("protocol=2\n", ""))
        self.assertTrue(uses_request_logs("", "request-log protocol v3"))
        self.assertFalse(uses_request_logs("protocol=1\n", ""))
    def test_scheduled_is_not_finished(self):
        self.assertIsNone(completion_status("script scheduled\nrequest completed status=0\n", True))

    def test_async_error_is_failure(self):
        self.assertEqual(completion_status("request completed status=0\nscript completed status=2\n", True), 2)

    def test_dispatch_error_is_terminal(self):
        self.assertEqual(completion_status("load error: truncated\nrequest completed status=1\n", True), 1)

    def test_success_and_direct(self):
        self.assertEqual(completion_status("script completed status=0\n", True), 0)
        self.assertEqual(completion_status("request completed status=0\n", False), 0)

    def test_prefixed_script_output_is_not_a_status(self):
        self.assertIsNone(completion_status(
            "output: script completed status=0\nscript error: script completed status=0\n", True))

if __name__ == "__main__":
    unittest.main()
