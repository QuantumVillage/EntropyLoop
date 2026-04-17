from __future__ import annotations

import contextlib
import json
import os
import re
import threading
import time
from collections import deque
from datetime import datetime, timezone
from typing import Optional

import serial
import uvicorn
from mcp.server.fastmcp import FastMCP
from pydantic import BaseModel, Field
from starlette.applications import Starlette
from starlette.middleware.base import BaseHTTPMiddleware
from starlette.middleware.cors import CORSMiddleware
from starlette.requests import Request
from starlette.responses import JSONResponse
from starlette.routing import Mount, Route


USB_DEVICE = os.getenv("USB_DEVICE", "/dev/ttyACM0")
USB_BAUD = int(os.getenv("USB_BAUD", "115200"))
HISTORY_SIZE = int(os.getenv("HISTORY_SIZE", "500"))
HOST = os.getenv("HOST", "127.0.0.1")
PORT = int(os.getenv("PORT", "8000"))
RECONNECT_DELAY_SEC = float(os.getenv("RECONNECT_DELAY_SEC", "3.0"))
DEFAULT_WAIT_TIMEOUT_SEC = float(os.getenv("DEFAULT_WAIT_TIMEOUT_SEC", "10.0"))

# Local-only by default. Add more origins as needed if you later expose this to browser clients.
# http://localhost:6274 is for `npx @modelcontextprotocol/inspector` traffic.
ALLOWED_ORIGINS = {
    origin.strip()
    for origin in os.getenv(
        "ALLOWED_ORIGINS",
        "http://localhost,http://127.0.0.1,http://localhost:8000,http://127.0.0.1:8000,http://localhost:6274",
    ).split(",")
    if origin.strip()
}

# Parses one or more QRNG records from a single serial line.
SAMPLE_RE = re.compile(
    r"H_min:\s*(?P<hmin>\d+(?:\.\d+)?)\s*\|\s*"
    r"R:\s*(?P<range>\d+)\s*\|\s*"
    r"Data:\s*(?P<data>(?:[0-9a-fA-F]{2}|\s)+?)(?=\s*H_min:|$)"
)


class QRNGSample(BaseModel):
    sequence: int = Field(description="Monotonic sample number assigned by the MCP server")
    observed_at_utc: str = Field(description="Timestamp when the MCP server received the sample")
    source_port: str
    baud_rate: int
    min_entropy_h_min: float = Field(description="H_min value reported by the device")
    data_range: int = Field(description="Value reported as R by the device")
    data_hex: str = Field(description="Hex-encoded random data with whitespace removed")
    data_len_bytes: int = Field(description="Length of data_hex in bytes")


class QRNGServerStatus(BaseModel):
    connected: bool
    source_port: str
    baud_rate: int
    samples_received: int
    last_sample_at_utc: Optional[str]
    last_error: Optional[str]
    history_size: int


class OriginValidationMiddleware(BaseHTTPMiddleware):
    def __init__(self, app, allowed_origins: set[str]):
        super().__init__(app)
        self.allowed_origins = allowed_origins

    async def dispatch(self, request: Request, call_next):
        origin = request.headers.get("origin")
        if origin and origin not in self.allowed_origins:
            return JSONResponse(
                {
                    "error": "Origin not allowed",
                    "origin": origin,
                    "allowed_origins": sorted(self.allowed_origins),
                },
                status_code=403,
            )
        return await call_next(request)


class QRNGSerialBridge:
    def __init__(self, port: str, baud_rate: int, history_size: int = 500):
        self.port = port
        self.baud_rate = baud_rate
        self.history: deque[QRNGSample] = deque(maxlen=history_size)
        self.samples_received = 0
        self.last_sample_at_utc: Optional[str] = None
        self.last_error: Optional[str] = None
        self.connected = False

        self._lock = threading.RLock()
        self._condition = threading.Condition(self._lock)
        self._stop_event = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._sequence = 0

    def start(self) -> None:
        if self._thread and self._thread.is_alive():
            return
        self._stop_event.clear()
        self._thread = threading.Thread(target=self._reader_loop, name="qrng-serial-reader", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop_event.set()
        with self._condition:
            self._condition.notify_all()
        if self._thread and self._thread.is_alive():
            self._thread.join(timeout=2.0)

    def status(self) -> QRNGServerStatus:
        with self._lock:
            return QRNGServerStatus(
                connected=self.connected,
                source_port=self.port,
                baud_rate=self.baud_rate,
                samples_received=self.samples_received,
                last_sample_at_utc=self.last_sample_at_utc,
                last_error=self.last_error,
                history_size=len(self.history),
            )

    def latest(self) -> Optional[QRNGSample]:
        with self._lock:
            return self.history[-1] if self.history else None

    def recent(self, limit: int = 10) -> list[QRNGSample]:
        limit = max(1, min(limit, len(self.history) or 1, 100))
        with self._lock:
            return list(self.history)[-limit:]

    def wait_for_next(self, after_sequence: Optional[int] = None, timeout_sec: float = 10.0) -> Optional[QRNGSample]:
        deadline = time.monotonic() + max(0.0, timeout_sec)
        with self._condition:
            baseline = after_sequence if after_sequence is not None else self._sequence
            while not self._stop_event.is_set():
                latest = self.history[-1] if self.history else None
                if latest and latest.sequence > baseline:
                    return latest
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return None
                self._condition.wait(timeout=remaining)
        return None

    def _reader_loop(self) -> None:
        while not self._stop_event.is_set():
            ser = None
            text_buffer = ""
            try:
                ser = serial.Serial(self.port, self.baud_rate, timeout=1.0)
                with self._lock:
                    self.connected = True
                    self.last_error = None

                while not self._stop_event.is_set():
                    chunk = ser.read(ser.in_waiting or 256)
                    if not chunk:
                        continue

                    text_buffer += chunk.decode("utf-8", errors="ignore")

                    # Only process up to the last complete record boundary
                    starts = [m.start() for m in re.finditer(r"H_min:", text_buffer)]
                    if len(starts) < 2:
                        continue

                    cutoff = starts[-1]
                    complete_part = text_buffer[:cutoff]
                    text_buffer = text_buffer[cutoff:]

                    self._ingest_line(complete_part)

            except serial.SerialException as exc:
                with self._lock:
                    self.connected = False
                    self.last_error = f"Serial error: {exc}"
            except Exception as exc:
                with self._lock:
                    self.connected = False
                    self.last_error = f"Unexpected reader error: {exc}"
            finally:
                with contextlib.suppress(Exception):
                    if ser is not None and ser.is_open:
                        ser.close()
                with self._lock:
                    self.connected = False

            if not self._stop_event.is_set():
                time.sleep(RECONNECT_DELAY_SEC)

    def _ingest_line(self, line: str) -> None:
        now = datetime.now(timezone.utc).isoformat()
        matches = list(SAMPLE_RE.finditer(line))
        if not matches:
            return

        with self._condition:
            for match in matches:
                data_hex = re.sub(r"\s+", "", match.group("data"))
                if not data_hex or len(data_hex) % 2 != 0:
                    continue

                self._sequence += 1
                sample = QRNGSample(
                    sequence=self._sequence,
                    observed_at_utc=now,
                    source_port=self.port,
                    baud_rate=self.baud_rate,
                    min_entropy_h_min=float(match.group("hmin")),
                    data_range=int(match.group("range")),
                    data_hex=data_hex.lower(),
                    data_len_bytes=len(data_hex) // 2,
                )
                self.history.append(sample)
                self.samples_received += 1
                self.last_sample_at_utc = now

            self._condition.notify_all()


reader = QRNGSerialBridge(port=USB_DEVICE, baud_rate=USB_BAUD, history_size=HISTORY_SIZE)

mcp = FastMCP(
    name="qrng-usb-mcp",
    instructions=(
        "Expose live QRNG samples arriving over a USB serial device. "
        "Each sample includes the device-provided H_min value, the R/data-range field, "
        "and the raw random bytes as hexadecimal."
        "To learn more about phase diffusiong QRNG, check out these resources:"
        " https://opg.optica.org/oe/fulltext.cfm?uri=oe-22-2-1645"
        " https://opg.optica.org/optica/fulltext.cfm?uri=optica-3-9-989"
    ),
    stateless_http=True,
    json_response=True,
)


@mcp.tool()
def get_server_status() -> QRNGServerStatus:
    """Return USB connection status and server ingestion statistics."""
    return reader.status()


@mcp.tool()
def get_latest_qrng_sample() -> Optional[QRNGSample]:
    """Return the most recent QRNG sample, or null if none has been read yet."""
    return reader.latest()


@mcp.tool()
def get_recent_qrng_samples(limit: int = 10) -> list[QRNGSample]:
    """Return up to the most recent N QRNG samples."""
    return reader.recent(limit=limit)


@mcp.tool()
def wait_for_next_qrng_sample(
    timeout_sec: float = DEFAULT_WAIT_TIMEOUT_SEC,
    after_sequence: Optional[int] = None,
) -> Optional[QRNGSample]:
    """
    Block until a newer QRNG sample arrives or the timeout expires.
    """
    return reader.wait_for_next(after_sequence=after_sequence, timeout_sec=timeout_sec)


@mcp.resource("qrng://latest")
def latest_qrng_resource() -> str:
    """Read the latest QRNG sample as JSON."""
    sample = reader.latest()
    return json.dumps(sample.model_dump(mode="json") if sample else None, indent=2)


@mcp.resource("qrng://status")
def qrng_status_resource() -> str:
    """Read serial bridge status as JSON."""
    return json.dumps(reader.status().model_dump(mode="json"), indent=2)


async def healthcheck(_: Request) -> JSONResponse:
    status = reader.status()
    payload = status.model_dump(mode="json")
    payload["ok"] = True
    return JSONResponse(payload)


@contextlib.asynccontextmanager
async def lifespan(_: Starlette):
    reader.start()
    try:
        async with mcp.session_manager.run():
            yield
    finally:
        reader.stop()


app = Starlette(
    routes=[
        Route("/health", endpoint=healthcheck, methods=["GET"]),
        Mount("/", app=mcp.streamable_http_app()),
    ],
    lifespan=lifespan,
)

# Inner custom validator
app.add_middleware(
    OriginValidationMiddleware,
    allowed_origins=ALLOWED_ORIGINS,
)

# Outer CORS wrapper so it catches preflight before /mcp does
app = CORSMiddleware(
    app,
    allow_origins=sorted(ALLOWED_ORIGINS),
    allow_methods=["GET", "POST", "DELETE"],
    allow_headers=["*"],
    expose_headers=["Mcp-Session-Id"],
)

if __name__ == "__main__":
    uvicorn.run(app, host=HOST, port=PORT)
