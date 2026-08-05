# Linux Deployment Guide

Verified on Ubuntu 22.04 LTS and 24.04 LTS. Also works on Debian 12 and Raspberry Pi OS
(Bookworm).

---

## 1. Install Dependencies

```bash
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    pkg-config \
    libbluetooth-dev \
    libnl-3-dev \
    libnl-genl-3-dev \
    valgrind \
    clang-format \
    wireless-tools \
    iw \
    rfkill
```

| Package | Why it is needed |
|---------|------------------|
| `libbluetooth-dev` | BlueZ headers and library for HCI sockets |
| `libnl-3-dev`, `libnl-genl-3-dev` | Netlink libraries for wireless configuration |
| `iw`, `rfkill` | Creating monitor interfaces, unblocking radios |
| `valgrind` | `make check-memory` |
| `clang-format` | `make format` |

---

## 2. Build

```bash
cd person_detector

make build     # Release
make debug     # Debug with AddressSanitizer + UBSan
make test      # Unit tests
```

`make test` should end with `All tests passed`.

---

## 3. Verify Hardware

### Bluetooth

```bash
sudo rfkill unblock bluetooth
sudo hciconfig hci0 up
hciconfig -a
```

Expect `UP RUNNING` on `hci0`. If no device is listed, the adapter is missing or the driver did
not load — check `dmesg | grep -i bluetooth`.

### Wi-Fi monitor mode support

```bash
iw list | grep -A 10 "Supported interface modes"
```

`* monitor` must appear. If it does not, the adapter or driver cannot sniff and only the BLE path
will work. Adapters based on Atheros AR9271, Ralink RT3070, or MediaTek MT7612U are known-good.

---

## 4. Enable Monitor Mode

```bash
sudo ip link set wlan0 down
sudo iw dev wlan0 interface add wlan0mon type monitor
sudo ip link set wlan0mon up
```

Confirm:

```bash
iw dev wlan0mon info    # type should read "monitor"
```

To revert:

```bash
sudo ip link set wlan0mon down
sudo iw dev wlan0mon del
sudo ip link set wlan0 up
```

> Creating a monitor interface usually drops the machine's Wi-Fi connectivity. Use a second
> adapter, or connect over Ethernet, if you need the network while capturing.

### Optional: channel hopping

A monitor interface listens on one channel at a time. Probe requests are sent across many
channels, so cycling improves capture rate:

```bash
while true; do
  for ch in 1 6 11; do
    sudo iw dev wlan0mon set channel $ch
    sleep 0.5
  done
done
```

Run this in a second terminal alongside the detector.

---

## 5. Run

### Foreground

```bash
sudo ./build-debug/person_detector \
    --ble-dev hci0 \
    --wifi-iface wlan0mon \
    --rssi-cutoff -85 \
    --window 500 \
    --interval 5 \
    --ttl 60 \
    --json-out /tmp/metrics.jsonl \
    --verbose
```

### Without hardware

```bash
make replay
```

### Single protocol

```bash
sudo ./build/person_detector --no-wifi --ble-dev hci0     # BLE only
sudo ./build/person_detector --no-ble --wifi-iface wlan0mon  # Wi-Fi only
```

---

## 6. Run Without Root

Raw sockets normally require root. Granting the binary two capabilities is narrower:

```bash
sudo setcap cap_net_raw,cap_net_admin+eip ./build/person_detector
./build/person_detector --ble-dev hci0 --wifi-iface wlan0mon
```

`cap_net_raw` allows raw socket creation; `cap_net_admin` allows the HCI scan configuration.

> Capabilities are cleared on rebuild, so re-run `setcap` after each `make build`.

---

## 7. systemd Service

```bash
sudo make install
sudo mkdir -p /var/log/person_detector
sudo cp scripts/person-detector.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now person-detector.service
```

Operate:

```bash
systemctl status person-detector
journalctl -u person-detector -f
sudo systemctl restart person-detector
sudo systemctl stop person-detector
```

The unit sets `NoNewPrivileges`, `ProtectSystem=strict`, `ProtectHome`, and `PrivateTmp`, with
`/var/log/person_detector` as the only writable path. Edit `ExecStart` in the unit file to change
flags, then `daemon-reload` and restart.

### Monitor mode at boot

The service does not create the monitor interface. Either create it in a `systemd` unit ordered
before this one, or add a udev rule. Minimal approach:

```bash
sudo tee /etc/systemd/system/wlan0mon.service >/dev/null <<'EOF'
[Unit]
Description=Create wlan0mon monitor interface
Before=person-detector.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/sbin/ip link set wlan0 down
ExecStart=/sbin/iw dev wlan0 interface add wlan0mon type monitor
ExecStart=/sbin/ip link set wlan0mon up
ExecStop=/sbin/ip link set wlan0mon down
ExecStop=/sbin/iw dev wlan0mon del

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl enable --now wlan0mon.service
```

---

## 8. Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| `Failed to open HCI socket: Operation not permitted` | Not root, no capabilities | Use `sudo` or `setcap` (Section 6) |
| `Unknown BLE device 'hci0'` | Adapter absent or down | `sudo rfkill unblock bluetooth && sudo hciconfig hci0 up` |
| `hci_le_set_scan_enable failed: Input/output error` | A scan is already running | `sudo hciconfig hci0 reset`, stop `bluetoothd` if it is scanning |
| `Unknown Wi-Fi interface 'wlan0mon'` | Monitor interface not created | Section 4 |
| `Failed to bind to interface: Network is down` | Interface exists but is down | `sudo ip link set wlan0mon up` |
| Wi-Fi count stays 0 | Not in monitor mode, or wrong channel | `iw dev wlan0mon info`; add channel hopping |
| BLE count stays 0 | `bluetoothd` holding the adapter | `sudo systemctl stop bluetooth`, retry |
| Counts far too high | Static infrastructure devices | Raise `--rssi-cutoff` to `-75`; baseline an empty room |
| Counts far too low | Cutoff too aggressive, devices in pockets | Lower `--rssi-cutoff` to `-90` |
| Estimate oscillates | TTL too short relative to advertising cadence | Raise `--ttl` to `90` or `120` |
| `libbluetooth not found` at CMake configure | Missing dev package | `sudo apt install libbluetooth-dev` |
| ASan reports leaks on exit | Genuine regression | Run `make check-memory` and report |

### Confirming frames are arriving

If counts stay at zero, check the interface independently of the application:

```bash
sudo tcpdump -i wlan0mon -c 10 'type mgt subtype probe-req'   # Wi-Fi
sudo hcitool lescan --duplicates                              # BLE
```

If these produce nothing, the problem is the adapter or driver, not `person_detector`.

---

## 9. Raspberry Pi Notes

The onboard Broadcom Wi-Fi on Pi 3/4/5 does not reliably support monitor mode. Use an external
USB adapter for the Wi-Fi path. The onboard Bluetooth works for BLE scanning without changes.

```bash
sudo apt install -y libbluetooth-dev libnl-3-dev libnl-genl-3-dev cmake build-essential
make build
```

Build times are roughly 30–60 seconds on a Pi 4.

---

## 10. Post-Deployment Checklist

- [ ] `make test` passes
- [ ] `make check-memory` reports zero leaks
- [ ] `hciconfig hci0` shows `UP RUNNING`
- [ ] `iw dev wlan0mon info` shows `type monitor`
- [ ] Detector prints non-zero `ble=` and `wifi=` counts
- [ ] `/var/log/person_detector` exists and is writable
- [ ] `systemctl status person-detector` shows `active (running)`
- [ ] JSONL output is growing and parses as valid JSON
- [ ] Baseline recorded in an empty space for offset correction
