<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Modbus RTU-TCP Gateway contributors. -->

# Troubleshooting

## The ESP32 does not join Wi-Fi

- Check `wifi_ssid` and `wifi_password` in the local `secrets.yaml`.
- Confirm that the access point provides 2.4 GHz Wi-Fi coverage.
- Check ESPHome serial logs for association or DHCP errors.
- Review the fallback AP configuration in the YAML if you need temporary access; change its default password before operational deployment.

## The web page does not open

- Confirm the ESP32's IP address from router DHCP leases or ESPHome logs.
- Open `http://<ESP32-IP>/` (HTTP port 80), not the Modbus TCP port.
- Connect the browser to the same LAN/VLAN as the ESP32.
- Check that no network firewall is blocking port 80.

## Modbus TCP client cannot connect

- In TCP Server mode, confirm that the configured port (502 by default) is reachable from the master.
- Confirm that fewer than four client slots are already occupied.
- Verify that the device is actually in TCP Server mode and that the TCP mode was saved.
- In TCP Client mode, verify the remote server IP/port and that the remote Modbus TCP server accepts connections.

## No RTU response

- Verify baud rate, data bits, parity and stop bits against the slave device.
- Check RS-485 A+/B− polarity and the other device's terminal naming.
- Confirm the default GPIO mapping for the Waveshare board: TX17, RX18, EN21.
- Verify the RS-485 direction-enable pin and wiring for non-reference boards.
- Check whether 120 Ω termination is needed at the physical bus end; avoid enabling termination on every node.
- Make sure each RTU slave has a unique address and that the selected Unit ID rule is correct.

## RTU CRC errors

- Check serial format and baud rate.
- Check wiring, bus topology, noise and termination.
- Verify that the request includes the RTU CRC when using the manual-frame tool.
- Use the frame log and exported text file to compare the bytes received and transmitted.

## GPIO changes cause a restart

This is intentional. The UART and RS-485 direction-control pin are initialized during boot; saving a changed GPIO value stores the configuration and restarts the ESP32.

## More than four TCP masters

The configured server limit is four clients. Additional connections are rejected while all slots are in use. Disconnect an unused master or adapt and retest the firmware if a higher connection limit is required.

## Useful logs to collect

- ESPHome serial log around boot and the first request.
- Web UI frame export (`modbus-frame-capture.txt`).
- Mode, TCP port, RTU serial format, Unit ID mode and GPIO values.
- Board model/revision and a description of the RS-485 topology.

### Missing `RTU TX` entries in TCP Server mode

In TCP Server mode, the frame log records the complete RTU request (`RTU TX`)
just before the ESP32 transmits it on the RS-485 bus. The expected transaction
sequence is `TCP RX` → `RTU TX` → `RTU RX` → `TCP TX`. If `RTU TX` is present
but `RTU RX` is missing, check the RS-485 wiring, slave address, serial format,
DE/RE direction control, termination, and the slave response timeout.

## TCP client connection diagnostics

In **TCP Client (RTU to server)** mode, send a valid request from the RTU master and open **Frame diagnostics**. The rolling trace now records the TCP connection sequence as well as Modbus frames:

- `TCP CONNECT` — target host and port being contacted.
- `TCP INFO` — connection established and elapsed connection time.
- `TCP ERROR` — Wi-Fi unavailable, connection refused/failed, socket write failure, response timeout, invalid MBAP header, or invalid response function.
- `TCP TX` / `TCP RX` — Modbus TCP request and response data.

The `TCP CONNECT` entry also reports the Arduino Wi-Fi status value and the local IP address for troubleshooting. The gateway does not abort solely because `WiFi.status()` reports a disconnected state; it attempts the socket connection and uses the result of `connect()` as the authoritative outcome. These events are also included in the `.txt` frame export. A typical connection failure appears after `RTU RX` as `TCP CONNECT`, followed by `TCP ERROR`. Check the displayed target address and port first; then check Wi-Fi connectivity, routing/firewall rules, and whether the remote server accepts connections from the gateway's IP address.

## Persistent TCP Client sessions

In TCP Client mode, the Web UI enables a persistent TCP connection by default. This avoids the TCP handshake on every RTU request. The diagnostics log shows `TCP CONNECT` when a socket is opened, `TCP REUSE` when an existing session is reused, and `TCP KEEP` after a successful response. If the server closes the session, the gateway opens a new connection for the next RTU request. Disable the persistent option in Section 1 if the remote server requires one connection per request.

The `TCP CONNECT` diagnostics display the station IP reported by ESPHome's Wi-Fi component. If it displays `unavailable`, check the ESPHome Wi-Fi logs and DHCP lease.


## Cache always reports `CACHE MISS`

- Confirm the cache switch for the currently selected TCP mode is enabled. The Web UI intentionally shows only the switch for the selected mode; the other mode's value is hidden but preserved.
- Check the configured TTL. An entry expires after the configured interval from the time the remote response was received; cache hits do not extend that interval.
- Confirm that the repeated requests have the same direction, effective Unit ID, function code, starting address and quantity. A different range is a different cache key.
- Read functions 01–04 are cacheable. Writes and read/write operations are not cached and successful non-read operations invalidate entries for the affected Unit ID.

## `CRC ERROR` appears in the frame log

The gateway intentionally drops RTU frames with an invalid CRC. In TCP Client mode, the bad RTU request is not sent to the remote TCP server. In TCP Server mode, the bad RTU response is not forwarded as response data and the TCP client receives gateway exception `0x0B`. Check baud rate, parity, stop bits, A+/B− polarity, shielding, termination and RS-485 direction control. Do not disable CRC validation to work around an electrical or serial-format problem.
