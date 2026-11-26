"""Request/reply OSC client for AbletonOSC (send :11000, receive :11001).

AbletonOSC answers a query on the same address it was asked on, so a call is:
send -> wait for the next message on that address -> return its args.
All AutoProducer extension calls (/live/ap/...) reply ("ok", ...) or ("error", msg);
`ap()` turns "error" into a LiveError so a failure always stops the step that caused it.
"""
from __future__ import annotations

import queue
import threading
from collections import defaultdict
from typing import Any

from pythonosc import dispatcher, osc_server, udp_client

from .actionlog import log


class LiveError(RuntimeError):
    pass


class LiveTimeout(LiveError):
    pass


class OSCClient:
    def __init__(self, host: str = "127.0.0.1", send_port: int = 11000, recv_port: int = 11001,
                 timeout: float = 5.0):
        self.timeout = timeout
        self._client = udp_client.SimpleUDPClient(host, send_port)
        self._queues: dict[str, queue.Queue] = defaultdict(queue.Queue)
        self._qlock = threading.Lock()
        disp = dispatcher.Dispatcher()
        disp.set_default_handler(self._on_message)
        self._server = osc_server.ThreadingOSCUDPServer(("127.0.0.1", recv_port), disp)
        self._thread = threading.Thread(target=self._server.serve_forever, daemon=True)
        self._thread.start()
        self.errors: list[str] = []

    def _q(self, address: str) -> queue.Queue:
        with self._qlock:
            return self._queues[address]

    def _on_message(self, address: str, *args: Any) -> None:
        if address == "/live/error":
            self.errors.append(" ".join(map(str, args)))
            log("osc_live_error", args=args)
        self._q(address).put(args)

    def close(self) -> None:
        self._server.shutdown()
        self._server.server_close()

    def send(self, address: str, *args: Any) -> None:
        log("osc_send", address=address, args=_short(args))
        self._client.send_message(address, list(args))

    def query(self, address: str, *args: Any, timeout: float | None = None) -> tuple:
        q = self._q(address)
        while not q.empty():  # drop stale replies
            q.get_nowait()
        self.send(address, *args)
        try:
            reply = q.get(timeout=timeout or self.timeout)
        except queue.Empty:
            log("osc_timeout", address=address)
            raise LiveTimeout(f"no reply from Live on {address} within {timeout or self.timeout}s") from None
        log("osc_recv", address=address, args=_short(reply))
        return tuple(reply)

    def ap(self, address: str, *args: Any, timeout: float | None = None) -> tuple:
        """Call an AutoProducer extension endpoint; raise on ("error", msg)."""
        reply = self.query(address, *args, timeout=timeout)
        if not reply:
            raise LiveError(f"{address}: empty reply")
        status, rest = reply[0], reply[1:]
        if status == "error":
            raise LiveError(f"{address}{_short(args)}: {rest[0] if rest else 'unknown error'}")
        if status != "ok":
            raise LiveError(f"{address}: unexpected reply {reply!r} (is the AutoProducer extension installed?)")
        return tuple(rest)

    def ap_paged(self, address: str, *args: Any, page_size: int = 40, stride: int = 1) -> list:
        """Fetch a list endpoint called as (*args, page, page_size) that replies
        (total_rows, *flat_values) with `stride` values per row. Returns a list of rows."""
        flat: list = []
        page = 0
        while True:
            total, *chunk = self.ap(address, *args, page, page_size)
            flat.extend(chunk)
            page += 1
            if len(flat) >= total * stride or not chunk:
                break
        if stride == 1:
            return flat
        return [tuple(flat[i:i + stride]) for i in range(0, len(flat), stride)]


def _short(args: Any, limit: int = 24) -> Any:
    args = list(args)
    if len(args) > limit:
        return args[:limit] + [f"... (+{len(args) - limit} values)"]
    return args
