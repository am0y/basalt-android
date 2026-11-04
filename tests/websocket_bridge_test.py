import sys
import threading
import time
import unittest
from pathlib import Path
from websockets.sync.server import serve
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from websocket_bridge import operate


class WebSocketTests(unittest.TestCase):
    def test_real_echo_and_close(self):
        def echo(socket):
            for message in socket:
                socket.send(message)
        with serve(echo, "127.0.0.1", 0) as server:
            threading.Thread(target=server.serve_forever, daemon=True).start()
            port = server.socket.getsockname()[1]
            token = operate({"Action": "connect", "Url": f"ws://127.0.0.1:{port}"})["id"]
            operate({"Action": "send", "Id": token, "Message": "echo-test"})
            deadline = time.monotonic() + 3
            events = []
            while not events and time.monotonic() < deadline:
                events = operate({"Action": "poll", "Id": token})["events"]
                time.sleep(0.02)
            self.assertEqual(events, [{"message": "echo-test"}])
            operate({"Action": "close", "Id": token})
            while time.monotonic() < deadline:
                if operate({"Action": "poll", "Id": token})["closed"]:
                    break
                time.sleep(0.02)
            else:
                self.fail("close was not observed")


if __name__ == "__main__":
    unittest.main()
