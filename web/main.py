"""Render-deployable demo API for person_detector internship submission."""

from __future__ import annotations

import json
import random
import time
from pathlib import Path
from typing import List

from fastapi import FastAPI
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse, HTMLResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from estimator import DeviceObservation, compute_estimate

app = FastAPI(
    title="Person Detector Demo",
    description="BLE + Wi-Fi nearby person estimation — live demo API",
    version="1.0.0",
)

app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

STATIC_DIR = Path(__file__).parent / "static"
SAMPLE_TRAFFIC = Path(__file__).parent.parent / "validation" / "sample_traffic.jsonl"


class DeviceInput(BaseModel):
    mac: str
    protocol: str
    rssi: int
    tx_power: int = -59
    timestamp: float = 0.0


class EstimateRequest(BaseModel):
    devices: List[DeviceInput]
    rssi_cutoff: int = -85
    dedup_window_ms: int = 500


class SimulateRequest(BaseModel):
    count: int = Field(default=8, ge=1, le=50)
    seed: int = 42


def random_mac(randomized: bool = False) -> str:
    first = random.randint(0, 255)
    if randomized:
        first = (first | 0x02) & 0xFE
    else:
        first &= 0xFC
    tail = [random.randint(0, 255) for _ in range(5)]
    return ":".join(f"{b:02x}" for b in [first] + tail)


@app.get("/health")
def health():
    return {"status": "ok", "service": "person-detector-demo"}


@app.get("/", response_class=HTMLResponse)
def index():
    index_path = STATIC_DIR / "index.html"
    if index_path.exists():
        return index_path.read_text(encoding="utf-8")
    return HTMLResponse("<h1>Person Detector Demo</h1><p>Static UI missing.</p>")


@app.post("/api/estimate")
def estimate(req: EstimateRequest):
    devices = [
        DeviceObservation(
            mac=d.mac,
            protocol=d.protocol,  # type: ignore[arg-type]
            rssi=d.rssi,
            tx_power=d.tx_power,
            timestamp=d.timestamp,
        )
        for d in req.devices
    ]
    result = compute_estimate(devices, req.rssi_cutoff, req.dedup_window_ms)
    return {
        "timestamp": int(time.time()),
        **result.__dict__,
    }


@app.post("/api/simulate")
def simulate(req: SimulateRequest):
    random.seed(req.seed)
    now = time.time()
    devices: List[DeviceInput] = []
    for i in range(req.count):
        randomized = random.random() < 0.65
        devices.append(
            DeviceInput(
                mac=random_mac(randomized),
                protocol="ble" if i % 2 == 0 else "wifi",
                rssi=random.randint(-78, -55),
                timestamp=now,
            )
        )
    obs = [
        DeviceObservation(
            mac=d.mac,
            protocol=d.protocol,  # type: ignore[arg-type]
            rssi=d.rssi,
            tx_power=d.tx_power,
            timestamp=d.timestamp,
        )
        for d in devices
    ]
    result = compute_estimate(obs)
    return {
        "timestamp": int(time.time()),
        "input_devices": [d.model_dump() for d in devices],
        **result.__dict__,
    }


@app.get("/api/sample")
def sample():
    if not SAMPLE_TRAFFIC.exists():
        return {"error": "sample traffic not found"}
    lines = SAMPLE_TRAFFIC.read_text(encoding="utf-8").strip().split("\n")
    last = json.loads(lines[-1])
    devices = [
        DeviceInput(
            mac=d["mac"],
            protocol=d["protocol"],
            rssi=d["rssi"],
            tx_power=d.get("tx_power", -59),
            timestamp=float(last["timestamp"]),
        )
        for d in last.get("devices", [])
    ]
    obs = [
        DeviceObservation(
            mac=d.mac,
            protocol=d.protocol,  # type: ignore[arg-type]
            rssi=d.rssi,
            tx_power=d.tx_power,
            timestamp=d.timestamp,
        )
        for d in devices
    ]
    result = compute_estimate(obs)
    return {
        "timestamp": int(time.time()),
        "source": "validation/sample_traffic.jsonl",
        "input_devices": [d.model_dump() for d in devices],
        **result.__dict__,
    }


if STATIC_DIR.exists():
    app.mount("/static", StaticFiles(directory=str(STATIC_DIR)), name="static")
