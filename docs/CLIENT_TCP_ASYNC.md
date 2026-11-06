# Cooperative TCP Client transaction handling

Component release: **1.1.9**

In TCP Client mode, a complete RTU request is validated and forwarded to the configured Modbus TCP server. The request is written once; the response is then received incrementally by polling the socket from the component `loop()` function.

## States

1. **Idle** – collect and validate an RTU request. A valid, cacheable request may be answered immediately from the Client-mode cache.
2. **Wait MBAP** – receive the seven-byte Modbus TCP response header incrementally. Validate Transaction ID, protocol ID, length and Unit ID.
3. **Wait PDU** – receive the number of PDU bytes declared by the MBAP length field.
4. **Complete** – validate the function code, update the optional response cache, and transmit the RTU response with a newly calculated RTU CRC.
5. **Error** – close the affected TCP connection, log the reason, and return the existing Modbus gateway-path exception to the RTU master.

The component does not wait in a blocking TCP receive loop. The WebServer and other ESPHome loop work continue while the remote server is processing the request. Persistent TCP mode is still supported: a live socket is reused, and a socket found disconnected is reconnected before the next transaction.

## Important timing limits

- The initial TCP connection and reconnect still use Arduino `WiFiClient::connect()`, which is synchronous.
- Sending the RTU reply still waits for UART transmission to complete before releasing RS-485 driver enable. This is required to avoid truncating the physical RTU frame.
- The manual Web UI transaction test remains synchronous. It is rejected with HTTP 409 while an RTU-originated transaction is active, because both use the same TCP socket.
- A single RTU transaction is active at a time. Additional bytes are buffered up to 2 KiB and processed sequentially.
- The TCP timeout is enforced independently for the MBAP header and PDU phases, preserving the previous timeout behavior.

This change improves ESP32 responsiveness during the remote TCP server response wait; it does not reduce the response time of the remote Modbus server or the physical transmission time of an RTU response.
