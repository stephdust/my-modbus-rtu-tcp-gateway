<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright (C) 2026 Modbus RTU-TCP Gateway contributors. -->

# System architecture

The gateway supports two complementary operating modes. The role names below describe the role played by the **ESP32 gateway** on each network interface. Each link is bidirectional: requests travel toward the destination device, and responses return along the same path.

![System architecture showing TCP Client / RTU Slave mode and TCP Server / RTU Master mode](assets/system-architecture.png)

## Mode 1 — TCP Client / RTU Slave

In this mode, the ESP32 behaves as:

- a **Modbus RTU slave** on its RS-485 interface;
- a **Modbus TCP client** on the IP network.

An external RTU master (for example, a PLC) sends a request to the gateway's RTU address. The gateway, acting as a **TCP client**, forwards that request to the configured remote Modbus TCP server. The RTU address from the incoming request is preserved as the TCP Unit ID. The remote TCP response travels back to the gateway, is converted into an RTU response, and is returned to the original RTU master.

The TCP destination host, port and timeout are configured in the web interface. In this mode, the RTU address override controls are hidden because the incoming RTU address must be preserved.

## Mode 2 — TCP Server / RTU Master

In this mode, the ESP32 behaves as:

- a **Modbus TCP server** on the IP network (port 502 by default; configurable);
- a **Modbus RTU master** on its RS-485 interface.

One to four **Modbus TCP clients** can connect to the gateway at the same time; each client acts as a Modbus TCP master. Each complete TCP request is converted into an RTU request and sent by the gateway (the RTU master) to the downstream RTU slave device. The RTU response returns to the gateway, is converted back to Modbus TCP, and is sent to the TCP client that originated the request.

The RTU address can be selected in the web interface:

- **Automatic:** use the incoming TCP Unit ID as the RTU slave address.
- **Manual:** use the configured RTU ID (1–247), independently of the incoming TCP Unit ID. The TCP response retains the original Unit ID and Transaction ID.

## Remote timeout handling

A shared Web UI option controls what the gateway does when the remote endpoint does not provide a complete response before the configured timeout (1–30 seconds; default 3 seconds):

- **Standard (default):** return a Modbus gateway exception `0x0B` to the upstream RTU master in TCP Client mode or to the requesting TCP client in TCP Server mode.
- **Silent:** send no upstream response for that remote timeout. The upstream master/client is responsible for timing out.

The selection is persistent across reboots and applies to both modes. It does not hide CRC failures, connection failures or other protocol errors. The frame diagnostics record silent timeouts as `TIMEOUT SILENT`. See [Protocol behaviour](PROTOCOL.md#remote-timeout-policy-v113).

## Why RTU transactions are sequential

The RS-485 link is shared and half-duplex. Multiple TCP sessions can remain connected, but they must not drive the same RTU bus simultaneously. The gateway therefore processes one RTU request/response transaction at a time. In TCP Server mode, ready requests are serviced across the connected client slots using a rotating starting slot, and each response is sent to its originating TCP connection.

## Latency diagnostics

The Section 4 frame log adds a `TIMING` entry for each completed transaction. Values are measured with the ESP32 microsecond timer and are diagnostic measurements, not fixed performance guarantees. TCP Client mode reports connection setup (or reuse), request write, TCP response wait and the RTU response transmission/end-to-end processing time. TCP Server mode reports per-client queue wait, RTU transmission, RTU response wait, TCP response write and total gateway processing time. These measurements help identify whether the dominant delay is network connection setup, the remote Modbus server, the downstream RTU device or queueing behind another TCP client's transaction.

The firmware retains the existing cooperative 1 ms polling waits and RS-485 direction-control guard intervals in this measurement release. These are intentionally not shortened before measurements from the target installation establish that doing so is safe for its Wi-Fi scheduling and RTU devices.

## Summary

| Gateway mode | ESP32 TCP role | ESP32 RTU role | Request direction | Response direction |
|---|---|---|---|---|
| TCP Client | Client | Slave | RTU master → gateway → remote TCP server | Remote TCP server → gateway → RTU master |
| TCP Server | Server | Master | TCP client (Modbus master) → gateway → RTU slave | RTU slave → gateway → originating TCP client |


## Mode-specific response cache

The gateway has two independent optional caches: RTU-to-TCP in Client mode and TCP-to-RTU in Server mode. The Web UI shows only the cache switch matching the currently selected TCP mode. The inactive mode's saved preference is retained when settings are applied. A shared TTL applies to both caches. Only valid Modbus read responses (functions 01–04) are cached; successful writes invalidate overlapping cached ranges in the corresponding writable data space, with a conservative Unit-ID-wide fallback for unknown or malformed writes. Invalid RTU CRC frames are never forwarded or cached. See [Cache behavior](CACHE.md).

## Client TCP cooperative receive

Normal RTU-originated Client TCP transactions receive the remote response incrementally through the ESPHome loop. This keeps the Web UI and other loop tasks responsive during remote-server processing. The initial TCP connect/reconnect and physical UART transmit completion remain synchronous; this implementation does not reduce the remote server's own response latency. See [Client TCP asynchronous handling](CLIENT_TCP_ASYNC.md).
