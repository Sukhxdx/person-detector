"""Python port of person_detector estimator (mirrors src/core/estimator.c)."""

from __future__ import annotations

import math
from dataclasses import dataclass
from typing import List, Literal

PATH_LOSS_N = 2.7
DEVICES_PER_PERSON = 1.25
RSSI_MATCH_TOLERANCE = 4
DEFAULT_RSSI_CUTOFF = -85
DEFAULT_TX_POWER = -59
MAX_DISTANCE_M = 30.0


@dataclass
class DeviceObservation:
    mac: str
    protocol: Literal["ble", "wifi"]
    rssi: int
    tx_power: int = DEFAULT_TX_POWER
    timestamp: float = 0.0


@dataclass
class EstimateResult:
    estimate: float
    lower_bound: float
    upper_bound: float
    raw_device_count: int
    deduplicated_count: int
    ble_count: int
    wifi_count: int
    randomized_mac_count: int


def is_locally_administered(mac: str) -> bool:
    first = mac.split(":")[0]
    return (int(first, 16) & 0x02) != 0


def distance_m(rssi: int, tx_power: int = DEFAULT_TX_POWER, n: float = PATH_LOSS_N) -> float:
    exponent = (tx_power - rssi) / (10.0 * n)
    return 10.0 ** exponent


def poisson_bounds(lam: float) -> tuple[float, float]:
    if lam <= 0:
        return 0.0, 0.0
    lower = max(0.0, lam - 1.96 * math.sqrt(lam))
    upper = lam + 1.96 * math.sqrt(lam)
    return lower, upper


def compute_estimate(
    devices: List[DeviceObservation],
    rssi_cutoff: int = DEFAULT_RSSI_CUTOFF,
    dedup_window_ms: int = 500,
) -> EstimateResult:
    active: List[DeviceObservation] = []
    ble_count = wifi_count = randomized = 0

    for d in devices:
        if d.rssi < rssi_cutoff:
            continue
        if distance_m(d.rssi, d.tx_power) > MAX_DISTANCE_M:
            continue
        active.append(d)
        if d.protocol == "ble":
            ble_count += 1
        else:
            wifi_count += 1
        if is_locally_administered(d.mac):
            randomized += 1

    merged = [False] * len(active)
    clusters = 0
    window_sec = dedup_window_ms / 1000.0

    for i, a in enumerate(active):
        if merged[i]:
            continue
        clusters += 1
        merged[i] = True
        for j in range(i + 1, len(active)):
            if merged[j]:
                continue
            b = active[j]
            if a.protocol == b.protocol:
                continue
            if abs(a.timestamp - b.timestamp) > window_sec:
                continue
            if abs(a.rssi - b.rssi) > RSSI_MATCH_TOLERANCE:
                continue
            merged[j] = True

    lam = clusters / DEVICES_PER_PERSON
    lower, upper = poisson_bounds(lam)

    return EstimateResult(
        estimate=round(lam, 3),
        lower_bound=round(lower, 3),
        upper_bound=round(upper, 3),
        raw_device_count=len(active),
        deduplicated_count=clusters,
        ble_count=ble_count,
        wifi_count=wifi_count,
        randomized_mac_count=randomized,
    )
