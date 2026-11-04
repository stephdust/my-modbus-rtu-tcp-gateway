<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Modbus RTU-TCP Gateway contributors. -->

# Modbus protocol behaviour

## Operating modes

### TCP Client — RTU to TCP

1. The gateway receives an RTU request on the UART/RS-485 interface.
2. It checks the RTU CRC and extracts the RTU slave address and PDU.
3. It opens a TCP connection to the configured Modbus TCP host and port.
4. The RTU slave address is copied to the Modbus TCP Unit ID without applying the manual RTU ID override.
5. The matching Modbus TCP response is checked and returned to the RTU master with a newly calculated RTU CRC.

The current Client-mode implementation opens and closes a TCP socket per transaction. It does not keep one persistent outbound TCP socket open.

### TCP Server — TCP to RTU

1. The gateway accepts up to four TCP client sessions on the configured listening port (502 by default).
2. Each client has its own connection slot and request parser.
3. A complete Modbus TCP ADU is validated (Protocol ID must be zero and the MBAP length must be in the supported range).
4. The request is converted to an RTU frame with a calculated CRC and sent over the shared RS-485 bus.
5. The RTU response is collected and CRC-checked.
6. The response is sent to the same TCP socket using the original Transaction ID and original TCP Unit ID.

The TCP sessions remain open between transactions. An incomplete Modbus TCP request that exceeds the configured timeout causes that client's slot/socket to be reset. A connection above the four-client limit is rejected.

## Multi-client scheduling

The four TCP sessions may remain connected at the same time. The RS-485 line is half-duplex and shared, so the gateway cannot execute four RTU transactions concurrently. It reads request data for each client slot, then processes one complete ready request at a time. The starting slot rotates after each completed transaction to reduce starvation when several clients are active.

This is **concurrent TCP connectivity, not parallel RTU execution**. Applications should use realistic polling rates and timeouts, especially when multiple masters poll the same RTU devices.

## Unit ID rules

| Flow | RTU address used | TCP Unit ID in response |
|---|---|---|
| RTU → TCP (Client mode) | Original RTU request address | Same value as RTU request address |
| TCP → RTU (Server, Automatic) | Incoming TCP Unit ID | Original incoming TCP Unit ID |
| TCP → RTU (Server, Manual) | Configured manual RTU ID (1–247) | Original incoming TCP Unit ID |

The RTU ID controls are hidden in TCP Client mode because that direction always preserves the RTU address from the incoming request.

## Function codes currently parsed by the gateway

The request/response length handling in the current source includes these standard function codes:

| Function code | Operation |
|---:|---|
| 01 | Read Coils |
| 02 | Read Discrete Inputs |
| 03 | Read Holding Registers |
| 04 | Read Input Registers |
| 05 | Write Single Coil |
| 06 | Write Single Register |
| 15 (0x0F) | Write Multiple Coils |
| 16 (0x10) | Write Multiple Registers |
| 23 (0x17) | Read/Write Multiple Registers |

Other function codes are not covered by the current RTU request-length parser and may not be handled correctly. Validate your device's required function codes before deployment.

## Frame diagnostics

- The rolling in-memory log retains the latest 32 events.
- Direction labels include RTU RX, RTU TX, TCP RX, TCP TX and MANUAL RTU.
- Times are `millis()` values (milliseconds since boot), not calendar timestamps.
- The browser refreshes the log periodically and performs an RTU CRC check for displayed RTU frames.
- The text export downloads the currently retained entries; it is not a permanent or complete packet capture.
