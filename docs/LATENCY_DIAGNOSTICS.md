# Byte-level latency diagnostics

Component release: **1.1.13**

The frame diagnostics now expose receive timing in addition to the transaction-level timings.
All values are reported in microseconds (`us`) in the Web trace and in the exported text capture.

## TCP Client mode (RTU master -> gateway -> Modbus TCP server)

The trace includes:

- `CLIENT RTU RX first-to-last`: time between the ESP32 reading the first and last byte of the incoming RTU request.
- `TCP first byte`: time from the start of waiting for the TCP response until the ESP32 reads the first response byte.
- `TCP first-to-last`: time between the ESP32 reading the first and last byte of the TCP response (MBAP header and PDU together).
- Existing `connect`, `TCP write`, `TCP response wait` and `total` values remain available.

## TCP Server mode (Modbus TCP client -> gateway -> RTU slave)

The trace includes:

- `TCP request first-to-last`: time between the ESP32 reading the first and last byte of the incoming Modbus TCP request (MBAP header and PDU).
- `RTU first byte`: time from the beginning of the RTU response wait until the ESP32 reads the first response byte from the UART.
- `RTU first-to-last`: time between the ESP32 reading the first and last byte of the RTU response.
- Existing `queue`, `RTU TX`, `RTU wait`, `TCP write` and `total` values remain available.

## Interpreting the measurements

`first byte` is the most useful value for separating device/server response delay from the time needed to read the response. A large value usually means the remote Modbus TCP server or RTU slave takes time before beginning its reply.

`first-to-last` is an ESP32-side software observation: timestamps are taken as bytes are removed from the UART or TCP receive buffer. It is useful for comparing repeated tests on the same firmware and setup, but it is **not** a physical-layer measurement of electrical transitions or exact wire timing. Buffered UART/TCP data may be read in a short burst even if the bytes arrived over a longer interval. Use a logic analyser or protocol analyser when exact on-wire timing is required.

These diagnostics do not change Modbus timeouts, RTU silent intervals, RS-485 direction-control guard times, or transaction scheduling.

## TCP Client responsiveness in 1.1.9

Normal RTU-originated Client TCP transactions now use a cooperative state machine. After the request has been written, the component polls for the MBAP header and response PDU on subsequent ESPHome loop iterations instead of blocking in a read-exact loop. This keeps the Web UI and other loop work responsive while the remote TCP server is processing the request.

The configured timeout is applied independently to the MBAP-header stage and PDU stage, as in the previous blocking implementation. The initial TCP handshake (and reconnect handshake) still uses the Arduino `WiFiClient::connect()` call and can block for the socket connection timeout. RTU response transmission still waits for the UART TX to finish so the RS-485 direction pin can be released safely. The manual Web UI request endpoint continues to use the synchronous helper and returns HTTP 409 while a normal Client TCP transaction is in progress.

RTU requests arriving while the TCP transaction is pending are buffered and handled sequentially once the response has been transmitted. The receive queue is bounded to 2 KiB to avoid unbounded memory growth if an RTU master continues issuing requests without waiting for replies.


## First-byte and receive-span measurements

Release 1.1.6 added software-side receive observations:

- TCP Client: RTU request first-to-last, TCP response first byte, and TCP response first-to-last.
- TCP Server: TCP request first-to-last, RTU response first byte, and RTU response first-to-last.

The first-byte interval is measured from the start of the response wait to the moment the ESP32 observes the first byte available in its input buffer. The first-to-last span is measured while the component reads the available bytes. These are useful comparative diagnostics, but are not physical-layer timestamps and can include UART/Wi-Fi buffering and loop scheduling.

## Response cache impact

A `CACHE HIT` bypasses the remote TCP/RTU round trip for that matching read. Compare cache-hit timings with `CACHE MISS` transactions, and choose the TTL based on the acceptable age of process data. The two cache enable settings are independent; only the setting for the selected mode is shown in the Web UI.
