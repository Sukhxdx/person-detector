#!/usr/bin/env python3
"""
Mock traffic injector for person_detector integration testing.

Injects synthetic BLE and Wi-Fi device observations into a named pipe or
stdout replay file consumed during development. Does not require raw sockets.
"""

from __future__ import annotations

import argparse
import json
import random
import sys
import time
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Iterable, List


@dataclass
class SyntheticDevice:
    mac: str
    protocol: str
    rssi: int
    tx_power: int = -59
    locally_administered: bool = False


def random_mac(randomized: bool = False) -> str:
    first = random.randint(0, 255)
    if randomized:
        first = (first | 0x02) & 0xFE
    else:
        first = first & 0xFC
    tail = [random.randint(0, 255) for _ in range(5)]
    octets = [first] + tail
    return ":".join(f"{b:02x}" for b in octets)


def build_population(count: int, randomized_ratio: float) -> List[SyntheticDevice]:
    devices: List[SyntheticDevice] = []
    for i in range(count):
        randomized = random.random() < randomized_ratio
        protocol = "ble" if i % 2 == 0 else "wifi"
        devices.append(
            SyntheticDevice(
                mac=random_mac(randomized),
                protocol=protocol,
                rssi=random.randint(-78, -55),
                locally_administered=randomized,
            )
        )
    return devices


def emit_records(devices: Iterable[SyntheticDevice], output: Path | None) -> None:
    payload = {
        "timestamp": int(time.time()),
        "devices": [asdict(d) for d in devices],
    }
    line = json.dumps(payload)
    if output is None:
        print(line)
    else:
        with output.open("a", encoding="utf-8") as fh:
            fh.write(line + "\n")


def parse_args(argv: List[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Simulate BLE/Wi-Fi device traffic")
    parser.add_argument("--count", type=int, default=8, help="Synthetic device count")
    parser.add_argument(
        "--randomized-ratio",
        type=float,
        default=0.65,
        help="Fraction of locally administered MAC addresses",
    )
    parser.add_argument(
        "--interval",
        type=float,
        default=2.0,
        help="Seconds between emission bursts",
    )
    parser.add_argument(
        "--duration",
        type=float,
        default=30.0,
        help="Total simulation duration in seconds",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Optional JSONL output file path",
    )
    parser.add_argument(
        "--seed",
        type=int,
        default=42,
        help="PRNG seed for reproducible simulations",
    )
    return parser.parse_args(argv)


def main(argv: List[str]) -> int:
    args = parse_args(argv)
    random.seed(args.seed)

    deadline = time.time() + args.duration
    burst = 0
    while time.time() < deadline:
        devices = build_population(args.count, args.randomized_ratio)
        emit_records(devices, args.output)
        burst += 1
        time.sleep(args.interval)

    print(
        f"Simulation complete: {burst} bursts, {args.count} devices/burst",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
