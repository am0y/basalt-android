"""Bounded WebSocket sessions owned by the HTTP bridge process."""
import base64
import queue
import threading
import time
import uuid
from urllib.parse import urlsplit

from websockets.exceptions import ConnectionClosed
from websockets.sync.client import connect

SESSIONS = {}
LOCK = threading.Lock()


class Session:
    def __init__(self, socket):
        self.socket = socket
        self.events = queue.Queue(maxsize=128)
        self.closed = False
        self.last_poll = time.monotonic()
        threading.Thread(target=self.receive, daemon=True).start()

    def receive(self):
        try:
            while True:
                try:
                    message = self.socket.recv(timeout=1)
                except TimeoutError:
                    if time.monotonic() - self.last_poll > 120:
                        break
                    continue
                if isinstance(message, str):
                    event = {"message": message}
                else:
                    event = {"base64": base64.b64encode(message).decode()}
                try:
                    self.events.put(event, timeout=1)
                except queue.Full:
                    break
        except (ConnectionClosed, OSError):
            pass
        finally:
            self.closed = True
            self.socket.close()


def operate(options):
    action = options.get("Action")
    if action == "connect":
        url = options.get("Url")
        if not isinstance(url, str) or urlsplit(url).scheme not in ("ws", "wss"):
            raise ValueError("expected a ws:// or wss:// URL")
        with LOCK:
            for token, session in list(SESSIONS.items()):
                if session.closed and time.monotonic() - session.last_poll > 120:
                    del SESSIONS[token]
            if len(SESSIONS) >= 16:
                raise ValueError("too many WebSocket sessions")
            socket = connect(url, open_timeout=10, close_timeout=2, max_size=2 * 1024 * 1024)
            token = uuid.uuid4().hex
            SESSIONS[token] = Session(socket)
            return {"id": token}
    with LOCK:
        session = SESSIONS.get(options.get("Id"))
    if session is None:
        raise ValueError("unknown WebSocket session")
    session.last_poll = time.monotonic()
    if action == "send":
        message = options.get("Message")
        if not isinstance(message, str) or len(message.encode()) > 2 * 1024 * 1024:
            raise ValueError("invalid WebSocket message")
        session.socket.send(message)
        return {"sent": True}
    if action == "poll":
        events = []
        for _ in range(32):
            try:
                events.append(session.events.get_nowait())
            except queue.Empty:
                break
        return {"events": events, "closed": session.closed and session.events.empty()}
    if action == "close":
        session.socket.close()
        return {"closed": True}
    raise ValueError("unknown WebSocket action")
