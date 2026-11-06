<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Modbus RTU-TCP Gateway contributors. -->

# Modbus protocol behaviour

## Operating modes

### TCP Client — RTU to TCP

1. The gateway receives an RTU request on the UART/RS-485 interface.
2. It checks the RTU CRC and extracts the RTU slave address and PDU.
3. It uses the persistent TCP connection to the configured Modbus TCP host and port (opening or reconnecting the socket when needed).
4. The RTU slave address is copied to the Modbus TCP Unit ID without applying the manual RTU ID override.
5. The matching Modbus TCP response is checked and returned to the RTU master with a newly calculated RTU CRC.

The Client mode can keep one outbound TCP socket open between RTU transactions (enabled by default in the Web UI). When persistent mode is disabled, a connection is opened for each transaction. A disconnected persistent session is detected and re-established before a subsequent request.

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

- The rolling in-memory log retains the latest 64 events.
- Direction labels include RTU RX, RTU TX, TCP RX, TCP TX and MANUAL RTU.
- Times are `millis()` values (milliseconds since boot), not calendar timestamps.
- The browser refreshes the log periodically and performs an RTU CRC check for displayed RTU frames.
- The text export downloads the currently retained entries (up to 64); it is not a permanent or complete packet capture.

## RTU CRC validation

The gateway validates the Modbus RTU CRC before forwarding any received RTU frame or storing a response in the cache. Invalid RTU master requests in TCP Client mode are discarded and recorded as `CRC ERROR`. Invalid RTU slave responses in TCP Server mode are discarded, recorded as `CRC ERROR`, and reported to the TCP requester with gateway exception `0x0B` (Target Device Failed to Respond). Incomplete responses/timeouts are logged separately as `RTU ERROR`. Modbus TCP frames use the MBAP header and do not carry an RTU CRC.


## TCP Client asynchronous response handling

For normal RTU-originated requests in TCP Client mode, the gateway sends the TCP request and receives the MBAP header and response PDU incrementally from the ESPHome loop. It does not block the loop while waiting for the remote server response. The initial connection/reconnection and the physical RS-485 response transmission remain synchronous. See [Client TCP asynchronous handling](CLIENT_TCP_ASYNC.md).
