# Feature 02: Local Web Control & Diagnostics Portal (`http://10.0.0.1`)

## 1. Problem Statement & Objective

Currently, inspecting diagnostic logs (`/boot/car_debug.log`), checking whether the Raspberry Pi 4 experienced USB voltage sag, or changing a configuration flag in `/boot/aawgd.conf` requires physically unplugging the Raspberry Pi from the Passat's center console, extracting the microSD card, mounting it on a computer, and re-inserting it in the car.

Meanwhile, whenever the user is in the car, their Android phone is **already connected** to the `futedongle` 5 GHz Wi-Fi Access Point (`10.0.0.0/24`), where the Raspberry Pi holds the static IP `10.0.0.1`.

**Objective**: Provide an ultra-lightweight, zero-dependency Web Control & Diagnostics Portal at `http://10.0.0.1` accessible from the phone's browser while sitting in the driver's seat, enabling live telemetry inspection, log downloads, one-tap service restarts, and safe atomic editing of `/boot/aawgd.conf`.

---

## 2. Network Routing & Coexistence with Mobile Data (4G/5G)

In commit `c0cb6e9`, `aa_wireless_dongle/board/common/rootfs_overlay/etc/dnsmasq.conf` was hardened with:
```ini
interface=wlan0
port=0
dhcp-range=10.0.0.2,10.0.0.20,12h
dhcp-option=3
dhcp-option=6
dhcp-authoritative
```

### How `http://10.0.0.1` Works Without a Default Gateway
Even when DHCP Option 3 (Default Gateway) and Option 6 (DNS) are omitted so Android keeps 4G/5G as the default route (`0.0.0.0/0`), Android's kernel routing table for the `wlan0` network interface always installs a **direct link-local subnet route** for `10.0.0.0/24`:
- Any packet addressed to `10.0.0.1` matches the `/24` prefix on the Wi-Fi interface (longest-prefix match beats `/0` on cellular).
- Therefore, opening `http://10.0.0.1` in Chrome/Firefox on the Android phone routes **directly to the Raspberry Pi over 5 GHz Wi-Fi**, while Spotify, Waze, and Google Maps continue using 4G/5G uninterrupted.

---

## 3. Backend Architecture

### 3.1 Server Engine Selection
To keep RAM usage under 2 MB and avoid adding heavy dynamic runtimes (Node.js/Python) to the Buildroot image, two lightweight options exist:

1. **Option A — BusyBox `httpd` + POSIX `/bin/sh` CGI (Zero New Binaries)**:
   - BusyBox is already the core userland of this image. Enabling `CONFIG_HTTPD=y` and `CONFIG_FEATURE_HTTPD_CGI=y` in BusyBox adds ~18 KB to `/bin/busybox` and uses 0 MB of resident RAM when idle (forking CGI scripts only when the user actually opens `http://10.0.0.1`).
2. **Option B — Embedded C++17 Micro-Server (`futedongle_web`)**:
   - A small single-threaded `poll()` HTTP server built alongside `aawgd` in `aa_wireless_dongle/package/aawg/src/`, bound strictly to `10.0.0.1:80` and running at `nice -n 10`.

**Recommendation**: **Option B (`futedongle_web` in C++17)** or **BusyBox `httpd`**. Both bind strictly to `10.0.0.1:80` so the service is never exposed on `eth0`.

### 3.2 REST API Contract

All endpoints return `application/json; charset=utf-8` with `Cache-Control: no-store`.

#### 1. `GET /api/v1/status` — System, Hardware & Android Auto Health
Reads kernel virtual filesystems (`/proc`, `/sys`, `/run`) without spawning subshells:
- **System**:
  - `uptime_sec`: `/proc/uptime`
  - `load_avg`: `/proc/loadavg`
  - `mem_free_kb` / `mem_total_kb`: `/proc/meminfo`
- **Raspberry Pi 4 Hardware & Power**:
  - `soc_temp_c`: `/sys/class/thermal/thermal_zone0/temp` divided by `1000.0`
  - `cpu_freq_mhz`: `/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq`
  - `undervoltage_detected`: parsed from `dmesg` or `/sys/devices/platform/soc/soc:firmware/get_throttled` (`0x0` = healthy; bit `0` = currently under-voltage; bit `16` = under-voltage has occurred since boot).
- **USB Gadget & MIB3 State**:
  - `udc_state`: `/sys/class/udc/fe980000.usb/state` (`not attached` | `powered` | `default` | `addressed` | `configured`)
  - `udc_speed`: `/sys/class/udc/fe980000.usb/current_speed` (`high-speed`)
  - `active_gadget`: checks `/sys/kernel/config/usb_gadget/accessory/UDC` vs `default/UDC`.
- **Wireless & Proxy State**:
  - `wifi_ssid`: `futedongle`
  - `dhcp_leases`: parsed from `/var/lib/misc/dnsmasq.leases` (connected phone IP, MAC, hostname)
  - `bt_alias` & `bt_connected_devices`: from BlueZ DBus
  - `tcp_proxy_active`: checks whether port `5288` has an `ESTABLISHED` socket.
- **GTE Hybrid Telemetry**:
  - Embeds the contents of `/run/gte_telemetry.json` if present (Feature 01).

#### 2. `GET /api/v1/logs?source=<messages|hostapd|dmesg|car_debug>&lines=200`
- Streams the last `N` lines of:
  - `messages`: `/var/log/messages` (includes `aawgd` and `bluetoothd` logs)
  - `hostapd`: `/var/log/hostapd`
  - `dmesg`: kernel ring buffer
  - `car_debug`: `/boot/car_debug.log`
- Also supports `?download=1` (`Content-Disposition: attachment; filename="car_debug.log"`) so the user can save the full diagnostic log directly to their phone's Downloads folder and share it.

#### 3. `GET /api/v1/config` & `POST /api/v1/config` — Safe Atomic Configuration Editor
- **`GET /api/v1/config`**:
  - Parses `/boot/aawgd.conf` and returns the structured key-value whitelist:
    - `AAWG_COUNTRY_CODE` (2-letter ISO code, e.g., `ES`)
    - `AAWG_CONNECTION_STRATEGY` (`0` | `1` | `2`)
    - `AAWG_EARLY_USB_VERSION_EXCHANGE` (`0` | `1`)
    - `AAWG_BLUETOOTH_NAME` (alphanumeric + `-_`, max 24 chars)
    - `AAWG_WIFI_SSID` (alphanumeric + `-_`, max 32 chars)
    - `AAWG_ENABLE_SSH` (`0` | `1`)
    - `AAWG_ENABLE_DEBUG_LOG` (`0` | `1`)
    - `AAWG_ENABLE_OBD_TELEMETRY` (`0` | `1`)
    - `AAWG_OBD_BLE_MAC` (MAC address regex `^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$`)
- **`POST /api/v1/config`**:
  - Validates every submitted key against strict regex patterns (preventing shell injection when `/etc/aawgd.conf` is sourced by init scripts!).
  - Performs an atomic, power-fail-safe write to `/boot/aawgd.conf`:
    1. Acquire an exclusive file lock (`/var/run/aawgd_conf.lock`) so `S51debug` and the web portal never remount `/boot` simultaneously.
    2. `mount -o remount,rw /boot`
    3. Write updated file to `/boot/aawgd.conf.tmp` and call `fsync()`.
    4. `mv /boot/aawgd.conf.tmp /boot/aawgd.conf` and `sync`.
    5. `mount -o remount,ro /boot`
    6. Release `/var/run/aawgd_conf.lock`.

#### 4. `POST /api/v1/actions` — Operational Controls
- `{"action": "restart_aawgd"}`: Runs `/etc/init.d/S93aawgd restart` to reset the Bluetooth/USB/TCP state machine without rebooting Linux.
- `{"action": "clear_bt_pairings"}`: Removes stale device directories under `/persist/bluetooth/` and restarts `bluetoothd` + `aawgd` when switching phones.
- `{"action": "capture_snapshot_now"}`: Triggers an immediate diagnostic snapshot into `/boot/car_debug.log`.
- `{"action": "reboot"}`: Clean `sync && /sbin/reboot`.

---

## 4. Frontend UI Specification (Offline Single-Page Dashboard)

Because the phone has no internet route over `wlan0`, the frontend at `/` (`/usr/share/futedongle-web/index.html`) must be **100% self-contained** (inline CSS and vanilla JS, zero external fonts, CDNs, or icons).

### UI Tabs (Mobile-First Touch Layout)
1. **Live Status Tab**:
   - Status cards with color-coded badges:
     - **Passat MIB3 USB**: `Configured (Accessory Mode)` / `Waiting`
     - **Android Auto Proxy**: `Active (TCP 5288 <-> USB)` / `Idle`
     - **Pi 4 Health**: SoC Temperature (`41.2 °C`), CPU Clock (`600 MHz`), Power Status (`OK - 0 Under-voltage events`).
   - Quick-action buttons: **Restart AAWG Daemon**, **Download Debug Log**, **Reboot Dongle**.
2. **Passat GTE Hybrid Tab** (populated when Feature 01 is active):
   - Real-time gauges for **HV Battery SoC (Net vs Gross %)**, **Instantaneous Power (kW)**, **HV Cell Temp (°C)**, **1.4 TSI Oil Temp (°C)**, and **12V Battery (V)**, plus a list of downloadable `.jsonl` trip logs from `/persist/telemetry/trips/`.
3. **Settings (`aawgd.conf`) Tab**:
   - Toggle switches for `AAWG_ENABLE_DEBUG_LOG`, `AAWG_ENABLE_SSH`, `AAWG_EARLY_USB_VERSION_EXCHANGE`, `AAWG_CONNECTION_STRATEGY`, and text inputs for `AAWG_WIFI_SSID` / `AAWG_BLUETOOTH_NAME` with **Save & Apply**.
4. **Live Logs Tab**:
   - Auto-refreshing terminal view (`aawgd` syslog, `hostapd`, `dmesg`) with one-tap **Copy to Clipboard** and **Download `.log`**.

---

## 5. Coordination with `S51debug` (`/boot` Mount Lock)

Because both `S51debug` (during the first 90 seconds of boot) and `POST /api/v1/config` remount `/boot` (`rw` -> `ro`), both must use BusyBox `flock` on `/var/run/boot_mount.lock`:

```sh
flock -x /var/run/boot_mount.lock -c '
    mount -o remount,rw /boot &&
    ... &&
    sync &&
    mount -o remount,ro /boot
'
```
This guarantees that `S51debug` never remounts `/boot` as read-only in the middle of a web config save, and vice versa.

