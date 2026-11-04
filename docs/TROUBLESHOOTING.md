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
