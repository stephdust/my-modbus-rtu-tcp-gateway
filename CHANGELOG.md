## 1.1.13

- Added a bilingual Web UI option for timeout behavior, shared by TCP Client and TCP Server modes.
- Default `Standard` behavior sends a Modbus gateway exception when the remote device does not respond before the configured timeout.
- Optional `Silent` behavior sends no RTU response in Client mode or no TCP response in Server mode when the remote device times out; the upstream master/client then applies its own timeout.
- The setting is persisted in ESP32 preferences and survives restart. The silent option applies only to a missing/incomplete remote response timeout; invalid RTU CRC frames and other transport/protocol errors are still handled and reported.
- Client-mode remote TCP response timeouts use gateway exception `0x0B` in Standard mode; other client-side transport/protocol failures retain `0x0A`. In Server mode, an RTU slave timeout uses TCP gateway exception `0x0B` in Standard mode.
- Updated the GitHub documentation for the timeout policy, diagnostics, cache invalidation rules and v1.1.13 defaults.

## 1.1.12

- Replaced broad Unit ID cache invalidation with write-range-aware invalidation for successful standard Modbus write operations.
- Function `05` and `0F` writes invalidate only overlapping cached Read Coils (`01`) entries.
- Functions `06`, `10`, `16`, and `17` invalidate only overlapping cached Read Holding Registers (`03`) entries.
- Non-overlapping cached ranges and read-only spaces (Discrete Inputs `02` and Input Registers `04`) are retained.
- Unknown or malformed non-read operations conservatively invalidate all cached reads for the affected Unit ID in the active communication direction.
- Failed write operations and Modbus exception responses do not invalidate cache entries.
- Cache invalidation diagnostics now include the write function, address range, affected read function and number of removed entries.

## 1.1.11

- Made the Web UI response-cache controls mode-aware: TCP Client mode shows only the Client cache switch; TCP Server mode shows only the Server cache switch.
- Switching modes updates the visible cache option immediately without a page reload.
- Saving settings updates only the active mode's cache preference and preserves the hidden mode's previously saved preference.
- Clarified in the bilingual Web UI and documentation that the cache TTL is shared and the inactive mode's setting is retained.
- Updated the GitHub documentation for persistent TCP sessions, cooperative asynchronous Client TCP response handling, independent response caches, RTU CRC rejection, timing diagnostics, and the current Waveshare reference board.

## 1.1.10

- Explicitly reject RTU frames with invalid CRC in both TCP Client and TCP Server modes.
- Client mode logs `CRC ERROR` and drops a corrupt RTU master request before forwarding it to TCP or consulting/updating the response cache.
- Server mode logs the invalid RTU response (with CRC visibly marked in the Web trace), drops the corrupt response before TCP forwarding/cache storage, and returns Modbus TCP gateway exception `0x0B` to the requesting client.
- CRC diagnostics include received and calculated CRC values, frame length, Unit ID/function where available, and a cumulative invalid-frame count since boot.
- Incomplete/timeout RTU responses are logged separately as `RTU ERROR` and are not counted as CRC failures.

## 1.1.9

- Refactored the normal TCP Client (RTU → TCP) transaction path into a cooperative, non-blocking response state machine.
- TCP MBAP headers and response PDUs are now collected incrementally from `loop()` without waiting in a blocking read loop.
- The Web UI, Wi-Fi servicing, and other ESPHome loop work continue while the remote Modbus TCP server is preparing a response.
- The configured TCP timeout is enforced separately for the MBAP header and response PDU stages, preserving the previous timeout model.
- Preserved persistent TCP reuse, response-cache behavior, Modbus TID/Unit ID/MBAP validation, and gateway-exception replies on failed transactions.
- RTU requests received while a TCP transaction is pending are buffered (up to 2 KiB) and handled sequentially after the previous response is returned.
- Manual Web UI test requests remain synchronous and are rejected with HTTP 409 while an RTU-originated Client TCP transaction is active, preventing concurrent use of the same socket.
- TCP Server mode and its four-client scheduler are unchanged.
- Initial/reconnect `WiFiClient::connect()` and RS-485 transmission completion remain synchronous; the long wait for the remote TCP response is now cooperative/non-blocking.

## 1.1.8

- Added an optional 64-entry response cache for TCP Client mode (RTU → TCP) and TCP Server mode (TCP → RTU), with independent enable switches.
- Added a shared configurable cache TTL (50–10,000 ms; default 500 ms). Both caches are disabled by default.
- Cache keys include direction, effective RTU Unit ID, read function, start address and quantity.
- Only successful, structurally valid Modbus read responses (functions 01–04) are stored; exceptions and write/read-write functions are never cached.
- Successful non-read operations invalidate cached responses for the affected Unit ID.
- TCP Server cache hits retain the requesting client's Transaction ID and TCP Unit ID while bypassing the RTU bus.
- Cache hit/miss/store/invalidation events are visible in the frame diagnostics and text export.
- Cache contents are volatile and are cleared after configuration is saved or the ESP32 restarts.

## 1.1.7

- Fixed a malformed JavaScript translation-map entry introduced in 1.1.6; the page script now parses correctly so frame refresh, language switching and Web UI actions run again.
- Kept the 1.1.6 byte-timing instrumentation and transaction behavior unchanged.

## 1.1.6

- Added first-byte latency and first-to-last receive-span diagnostics for TCP Client responses.
- Added RTU request first-to-last receive-span diagnostics in TCP Client mode.
- Added RTU response first-byte latency and first-to-last receive-span diagnostics in TCP Server mode.
- Added TCP request first-to-last receive-span diagnostics in TCP Server mode.
- Receive timestamps are collected alongside the existing buffers without changing transaction timeouts or RS-485 guard intervals.
- Clarified timing output in the Web trace and text export; measurements represent MCU-side receive observations, not a physical-layer oscilloscope measurement.

## 1.1.5

- Added per-transaction latency measurements in both TCP Client and TCP Server modes.
- Client diagnostics report TCP connect/reuse time, request write time, TCP response wait and total TCP transaction time.
- Server diagnostics report queue wait, RS-485 transmit time, RTU response wait, TCP response write and total processing time.
- Added end-to-end RTU Client transaction timing.
- Increased the rolling diagnostics history from 32 to 64 entries so timing events do not immediately displace frame records.
- Reserved reusable RTU/TCP buffers during setup to reduce heap allocations during transactions.
- Kept TCP_NODELAY enabled and preserved Modbus RTU direction-control guard times and the existing timeout behavior.
- The 1 ms cooperative polling delays are intentionally retained for this measurement release; timing data should guide any further tuning without risking Wi-Fi/task scheduling or RTU reliability.

## 1.1.4
- Add optional persistent TCP client mode (enabled by default) to avoid a new TCP handshake for each RTU request.
- Reuse an active TCP socket and automatically establish a new connection after a disconnect.
- Display the actual Wi-Fi IP address from ESPHome's WiFiComponent in TCP diagnostics.
- Add a Web UI reboot button using the existing reboot countdown page.

# Changelog

## 1.1.3

- Client TCP: no longer blocks connection attempts based only on `WiFi.status()`.
- Added Wi-Fi status and local IP details to the `TCP CONNECT` frame diagnostic.
- The actual TCP socket connection result now determines whether the remote Modbus TCP server is reachable.

## 1.1.2

- Added Client TCP connection diagnostics to the frame trace and text export.
