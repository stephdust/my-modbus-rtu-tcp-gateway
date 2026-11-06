// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Modbus RTU-TCP Gateway contributors.
#pragma once
#include <Arduino.h>
#include <WiFi.h>
// ESP-IDF and Arduino WebServer both define HTTP_ANY; this component does not use it.
#ifdef HTTP_ANY
#undef HTTP_ANY
#endif
#include <WebServer.h>
#include <Preferences.h>
#include "logo_assets.h"
#include "esphome/core/component.h"
#include "esphome/core/gpio.h"
#include "esphome/components/wifi/wifi_component.h"
#include <vector>
#include <string.h>

namespace esphome {
namespace modbus_gateway {

class ModbusGateway : public Component {
 public:
  void set_tcp_host(const std::string &v) { cfg_host_ = v; }
  void set_tcp_mode(const std::string &v) { tcp_mode_ = v; }
  void set_tcp_port(uint16_t v) { cfg_port_ = v; }
  void set_baud(uint32_t v) { cfg_baud_ = v; }
  void set_data_bits(uint8_t v) { cfg_data_bits_ = v; }
  void set_parity(const std::string &v) { cfg_parity_ = v; }
  void set_stop_bits(uint8_t v) { cfg_stop_bits_ = v; }
  void set_tx_gpio(int v) { tx_gpio_ = v; }
  void set_rx_gpio(int v) { rx_gpio_ = v; }
  void set_de_re_gpio(int v) { de_re_gpio_ = v; }
  void set_tcp_timeout(uint32_t v) { tcp_timeout_ms_ = v; }
  void set_version(const std::string &v) { version_ = v; }

  void setup() override {
    load_settings_();
    rx_.reserve(260);
    rx_timestamps_us_.reserve(260);
    client_txn_.request_pdu.reserve(253);
    client_txn_.response_pdu.reserve(253);
    cached_response_tmp_.reserve(253);
    for (auto &slot : tcp_clients_) slot.pdu.reserve(253);
    setup_gpio_();
    begin_uart_();
    web_.on("/", HTTP_GET, [this]() { web_.send(200, "text/html; charset=utf-8", this->page_()); });
    // Lightweight readiness endpoint used by the reboot page before redirecting.
    web_.on("/health", HTTP_GET, [this]() {
      web_.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
      web_.send(200, "text/plain; charset=utf-8", "OK");
    });
    // Serve embedded images independently so the main HTML remains small and reliable.
    web_.on("/img/logo.webp", HTTP_GET, [this]() { this->serve_image_(LOGO_IMG, "image/webp"); });
    web_.on("/img/modbus-tcp.jpg", HTTP_GET, [this]() { this->serve_image_(TCP_IMG, "image/jpeg"); });
    web_.on("/img/modbus-rtu.jpg", HTTP_GET, [this]() { this->serve_image_(RTU_IMG, "image/jpeg"); });
    web_.on("/img/esp32.jpg", HTTP_GET, [this]() { this->serve_image_(ESP32_IMG, "image/jpeg"); });
    web_.on("/img/frame-diagnostics.jpg", HTTP_GET, [this]() { this->serve_image_(FRAME_IMG, "image/jpeg"); });
    web_.on("/save", HTTP_POST, [this]() { this->save_config_(); });
    web_.on("/reboot", HTTP_POST, [this]() { this->reboot_gateway_(); });
    web_.on("/frames", HTTP_GET, [this]() { web_.send(200, "application/json", this->frames_json_()); });
    web_.on("/frames.txt", HTTP_GET, [this]() {
      web_.sendHeader("Content-Disposition", "attachment; filename=\"modbus-frame-capture.txt\"");
      web_.send(200, "text/plain; charset=utf-8", this->frames_text_());
    });
    web_.on("/clear", HTTP_POST, [this]() { log_head_ = 0; log_count_ = 0; web_.send(200, "application/json", "{\"cleared\":true}"); });
    web_.on("/listen", HTTP_GET, [this]() { web_.send(200, "application/json", String("{\"listening\":") + (rtu_listening_ ? "true}" : "false}")); });
    web_.on("/listen", HTTP_POST, [this]() { rtu_listening_ = !rtu_listening_; rx_.clear(); rx_timestamps_us_.clear(); while (Serial1.available()) Serial1.read(); ESP_LOGI("modbus_gateway", "RTU listener %s", rtu_listening_ ? "enabled" : "paused"); web_.send(200, "application/json", String("{\"listening\":") + (rtu_listening_ ? "true}" : "false}")); });
    web_.on("/manual", HTTP_POST, [this]() { this->manual_request_(); });
    web_.onNotFound([this]() { web_.send(404, "text/plain", "Not found"); });
    web_.begin();
    start_tcp_server_();
    ESP_LOGI("modbus_gateway", "Web UI started on port 80");
  }

  void loop() override {
    web_.handleClient();
    if (tcp_mode_ == "SERVER") {
      handle_tcp_server_();
    } else if (tcp_mode_ == "CLIENT") {
      // Advance pending TCP transactions without waiting for the remote server.
      // This keeps the ESPHome loop responsive while a Modbus TCP reply is in flight.
      process_client_transaction_();
      if (rtu_listening_) {
        bool rx_overflow = false;
        while (Serial1.available()) {
          uint8_t b = uint8_t(Serial1.read());
          // Bound the pending RTU input buffer if a master keeps transmitting
          // while an earlier request is still waiting for its TCP response.
          if (rx_.size() < 2048) {
            rx_.push_back(b);
            rx_timestamps_us_.push_back(micros());
          } else {
            rx_overflow = true;
          }
          last_rx_ms_ = millis();
        }
        if (rx_overflow) {
          rx_.clear();
          rx_timestamps_us_.clear();
          add_event_("RTU ERROR", "Pending RTU input buffer overflow; buffered bytes discarded.");
        }
        parse_rtu_();
      } else {
        // Keep the UART buffer drained while the RTU listener is paused.
        while (Serial1.available()) Serial1.read();
        rx_.clear();
        rx_timestamps_us_.clear();
      }
    } else {
      // Keep the UART buffer drained if the component is in an unknown mode.
      while (Serial1.available()) Serial1.read();
      rx_.clear();
      rx_timestamps_us_.clear();
    }
  }

  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

 protected:
  std::string cfg_host_{"192.168.1.100"};
  std::string version_{"unknown"};  // Loaded from components/modbus_gateway/manifest.json
  std::string tcp_mode_{"CLIENT"};
  uint16_t cfg_port_{502};
  bool rtu_listening_{true};
  String manual_result_{"Aucune requête manuelle"};
  uint32_t cfg_baud_{9600};
  uint8_t cfg_data_bits_{8};
  std::string cfg_parity_{"NONE"};
  uint8_t cfg_stop_bits_{1};

  WebServer web_{80};
  int tx_gpio_{17};
  int rx_gpio_{18};
  int de_re_gpio_{21};
  uint32_t tcp_timeout_ms_{3000};
  bool silent_on_timeout_{false};  // false = standard Modbus gateway exception
  bool tcp_persistent_{true};
  bool cache_client_enabled_{false};
  bool cache_server_enabled_{false};
  uint32_t cache_ttl_ms_{500};
  bool manual_rtu_id_{false};
  uint8_t configured_rtu_id_{1};
  ::Preferences prefs_;
  WiFiClient client_;
  // The ESP32 Arduino NetworkServer supports a configurable client limit.
  static constexpr size_t MAX_TCP_CLIENTS = 4;
  WiFiServer tcp_server_{502, MAX_TCP_CLIENTS};

  struct ByteTiming {
    bool has_first{false};
    uint32_t first_us{0};
    uint32_t last_us{0};

    void mark(uint32_t now_us) {
      if (!has_first) {
        has_first = true;
        first_us = now_us;
      }
      last_us = now_us;
    }

    uint32_t first_delay_us(uint32_t start_us) const {
      return has_first ? first_us - start_us : 0;
    }

    uint32_t span_us() const {
      return has_first ? last_us - first_us : 0;
    }
  };

  enum class ClientTxnState : uint8_t { IDLE, WAIT_MBAP, WAIT_PDU };

  // One RTU-originated request at a time is forwarded asynchronously to TCP.
  // The TCP socket is polled from loop(), so waiting for the remote server does
  // not block the Web UI, Wi-Fi tasks, or ESPHome component loop.
  struct ClientTransaction {
    ClientTxnState state{ClientTxnState::IDLE};
    uint8_t unit_id{0};
    uint16_t transaction_id{0};
    uint16_t expected_pdu_length{0};
    uint8_t mbap[7]{};
    size_t mbap_received{0};
    size_t response_received{0};
    std::vector<uint8_t> request_pdu;
    std::vector<uint8_t> response_pdu;
    uint32_t end_to_end_started_us{0};
    uint32_t connect_us{0};
    uint32_t tcp_write_us{0};
    uint32_t response_started_us{0};
    uint32_t response_stage_started_ms{0};
    ByteTiming response_timing;
    bool reused_connection{false};
    bool cacheable_read{false};
  };
  ClientTransaction client_txn_;
  std::vector<uint8_t> cached_response_tmp_;

  struct TcpClientSlot {
    WiFiClient client;
    uint8_t mbap[7] = {};
    size_t mbap_pos{0};
    std::vector<uint8_t> pdu;
    size_t pdu_pos{0};
    uint16_t expected_pdu{0};
    uint32_t last_activity{0};
    uint32_t request_ready_us{0};
    uint32_t request_first_byte_us{0};
    uint32_t request_last_byte_us{0};
    bool request_ready{false};
  };
  TcpClientSlot tcp_clients_[MAX_TCP_CLIENTS];
  uint8_t tcp_client_cursor_{0};

  std::vector<uint8_t> rx_;
  std::vector<uint32_t> rx_timestamps_us_;
  uint16_t transaction_id_{1};
  uint32_t last_rx_ms_{0};
  uint32_t crc_error_count_{0};  // Invalid RTU frames discarded since boot.

  static constexpr size_t FRAME_LOG_CAPACITY = 64;
  static constexpr size_t RESPONSE_CACHE_CAPACITY = 64;
  static constexpr size_t RESPONSE_CACHE_MAX_PDU = 253;
  static constexpr uint8_t CACHE_DIRECTION_CLIENT = 0;
  static constexpr uint8_t CACHE_DIRECTION_SERVER = 1;

  // Only successful Modbus read responses (function codes 01..04) are cached.
  // Fixed-size entries avoid heap fragmentation during repeated transactions.
  struct ResponseCacheEntry {
    bool valid{false};
    uint8_t direction{0};
    uint8_t unit_id{0};
    uint8_t function_code{0};
    uint16_t start_address{0};
    uint16_t quantity{0};
    uint16_t response_length{0};
    uint32_t stored_at_ms{0};
    uint8_t response_pdu[RESPONSE_CACHE_MAX_PDU]{};
  };
  ResponseCacheEntry response_cache_[RESPONSE_CACHE_CAPACITY];

  struct FrameLog { String direction; String hex; uint32_t when; };
  FrameLog logs_[FRAME_LOG_CAPACITY];
  size_t log_head_{0};
  size_t log_count_{0};

  void add_log_(const char *direction, const uint8_t *data, size_t len) {
    String h;
    char b[4];
    for (size_t i=0;i<len;i++) {
      snprintf(b, sizeof(b), "%02X", data[i]);
      if (i) h += " ";
      h += b;
    }
    logs_[log_head_] = {String(direction), h, millis()};
    log_head_ = (log_head_ + 1) % FRAME_LOG_CAPACITY;
    if (log_count_ < FRAME_LOG_CAPACITY) log_count_++;
  }

  // Store readable diagnostics in the same rolling log shown by the Web UI
  // and included in the frame-capture text export.
  void add_event_(const char *category, const String &message) {
    logs_[log_head_] = {String(category), message, millis()};
    log_head_ = (log_head_ + 1) % FRAME_LOG_CAPACITY;
    if (log_count_ < FRAME_LOG_CAPACITY) log_count_++;
  }

  static bool is_cacheable_read_request_(const std::vector<uint8_t> &pdu,
                                         uint16_t &start_address, uint16_t &quantity) {
    if (pdu.size() != 5 || pdu[0] < 1 || pdu[0] > 4) return false;
    start_address = (uint16_t(pdu[1]) << 8) | pdu[2];
    quantity = (uint16_t(pdu[3]) << 8) | pdu[4];
    if (quantity == 0) return false;
    if ((pdu[0] == 1 || pdu[0] == 2) && quantity > 2000) return false;
    if ((pdu[0] == 3 || pdu[0] == 4) && quantity > 125) return false;
    return true;
  }

  static bool is_valid_cache_response_(const std::vector<uint8_t> &request_pdu,
                                       const std::vector<uint8_t> &response_pdu) {
    uint16_t start_address = 0;
    uint16_t quantity = 0;
    if (!is_cacheable_read_request_(request_pdu, start_address, quantity)) return false;
    if (response_pdu.size() < 3 || response_pdu.size() > RESPONSE_CACHE_MAX_PDU) return false;
    if (response_pdu[0] != request_pdu[0]) return false;  // excludes Modbus exceptions
    const size_t expected_bytes = (request_pdu[0] == 1 || request_pdu[0] == 2)
        ? size_t((quantity + 7) / 8) : size_t(quantity) * 2;
    return response_pdu[1] == expected_bytes && response_pdu.size() == expected_bytes + 2;
  }

  bool cache_lookup_(uint8_t direction, uint8_t unit_id,
                     const std::vector<uint8_t> &request_pdu,
                     std::vector<uint8_t> &response_pdu, uint32_t &age_ms) {
    uint16_t start_address = 0;
    uint16_t quantity = 0;
    if (!is_cacheable_read_request_(request_pdu, start_address, quantity)) return false;
    const uint32_t now = millis();
    for (auto &entry : response_cache_) {
      if (!entry.valid) continue;
      if (uint32_t(now - entry.stored_at_ms) >= cache_ttl_ms_) {
        entry.valid = false;
        continue;
      }
      if (entry.direction == direction && entry.unit_id == unit_id &&
          entry.function_code == request_pdu[0] && entry.start_address == start_address &&
          entry.quantity == quantity) {
        response_pdu.assign(entry.response_pdu, entry.response_pdu + entry.response_length);
        age_ms = uint32_t(now - entry.stored_at_ms);
        return true;
      }
    }
    return false;
  }

  void cache_store_(uint8_t direction, uint8_t unit_id,
                    const std::vector<uint8_t> &request_pdu,
                    const std::vector<uint8_t> &response_pdu) {
    if (!is_valid_cache_response_(request_pdu, response_pdu)) return;
    uint16_t start_address = 0;
    uint16_t quantity = 0;
    if (!is_cacheable_read_request_(request_pdu, start_address, quantity)) return;

    const uint32_t now = millis();
    ResponseCacheEntry *target = nullptr;
    ResponseCacheEntry *oldest = nullptr;
    uint32_t oldest_age = 0;
    for (auto &entry : response_cache_) {
      if (entry.valid && uint32_t(now - entry.stored_at_ms) >= cache_ttl_ms_) entry.valid = false;
      if (!entry.valid && target == nullptr) target = &entry;
      if (entry.valid && entry.direction == direction && entry.unit_id == unit_id &&
          entry.function_code == request_pdu[0] && entry.start_address == start_address &&
          entry.quantity == quantity) {
        target = &entry;
        break;
      }
      if (entry.valid) {
        const uint32_t age = uint32_t(now - entry.stored_at_ms);
        if (oldest == nullptr || age > oldest_age) { oldest = &entry; oldest_age = age; }
      }
    }
    if (target == nullptr) target = oldest;
    if (target == nullptr) return;

    target->valid = false;
    target->direction = direction;
    target->unit_id = unit_id;
    target->function_code = request_pdu[0];
    target->start_address = start_address;
    target->quantity = quantity;
    target->response_length = uint16_t(response_pdu.size());
    memcpy(target->response_pdu, response_pdu.data(), response_pdu.size());
    target->stored_at_ms = now;
    target->valid = true;
    add_event_("CACHE STORE", String(direction == CACHE_DIRECTION_CLIENT ? "Client mode" : "Server mode") +
               " | Unit ID=" + String(unit_id) + "; Function=" + String(request_pdu[0]) +
               "; Address=" + String(start_address) + "; Quantity=" + String(quantity) + ".");
  }

  // Resolve the address range affected by a successful Modbus write.
  // Return false for functions whose write range is unknown; callers then
  // fall back to invalidating every cached read for this Unit ID and direction.
  static bool cache_write_impact_(const std::vector<uint8_t> &pdu,
                                  uint8_t &affected_read_function,
                                  uint16_t &write_start, uint16_t &write_quantity) {
    if (pdu.empty()) return false;

    const uint8_t function = pdu[0];
    affected_read_function = 0;
    write_start = 0;
    write_quantity = 0;

    if (function == 0x05 || function == 0x06) {
      if (pdu.size() != 5) return false;
      write_start = (uint16_t(pdu[1]) << 8) | pdu[2];
      write_quantity = 1;
      affected_read_function = function == 0x05 ? 0x01 : 0x03;
    } else if (function == 0x0F || function == 0x10) {
      if (pdu.size() < 6) return false;
      write_start = (uint16_t(pdu[1]) << 8) | pdu[2];
      write_quantity = (uint16_t(pdu[3]) << 8) | pdu[4];
      const uint8_t byte_count = pdu[5];
      if (write_quantity == 0) return false;
      if (function == 0x0F) {
        if (write_quantity > 1968 || byte_count != (write_quantity + 7) / 8 ||
            pdu.size() != size_t(6 + byte_count)) return false;
        affected_read_function = 0x01;
      } else {
        if (write_quantity > 123 || byte_count != write_quantity * 2 ||
            pdu.size() != size_t(6 + byte_count)) return false;
        affected_read_function = 0x03;
      }
    } else if (function == 0x16) {
      if (pdu.size() != 7) return false;
      write_start = (uint16_t(pdu[1]) << 8) | pdu[2];
      write_quantity = 1;
      affected_read_function = 0x03;
    } else if (function == 0x17) {
      // Read/Write Multiple Registers: the write range is at offsets 5 and 7.
      if (pdu.size() < 10) return false;
      write_start = (uint16_t(pdu[5]) << 8) | pdu[6];
      write_quantity = (uint16_t(pdu[7]) << 8) | pdu[8];
      const uint8_t byte_count = pdu[9];
      if (write_quantity == 0 || write_quantity > 121 ||
          byte_count != write_quantity * 2 || pdu.size() != size_t(10 + byte_count)) return false;
      affected_read_function = 0x03;
    } else {
      return false;
    }

    // A range may end at address 65535, but must not wrap around 16-bit space.
    return write_quantity != 0 &&
           (uint32_t(write_start) + uint32_t(write_quantity)) <= 0x10000UL;
  }

  void cache_invalidate_after_write_(uint8_t direction, uint8_t unit_id,
                                     const std::vector<uint8_t> &request_pdu) {
    uint8_t affected_read_function = 0;
    uint16_t write_start = 0;
    uint16_t write_quantity = 0;
    const bool targeted = cache_write_impact_(request_pdu, affected_read_function,
                                               write_start, write_quantity);
    const uint8_t write_function = request_pdu.empty() ? 0 : request_pdu[0];
    const uint32_t write_end = uint32_t(write_start) + uint32_t(write_quantity);
    size_t invalidated = 0;

    for (auto &entry : response_cache_) {
      if (!entry.valid || entry.direction != direction || entry.unit_id != unit_id) continue;

      if (targeted) {
        // A coil write affects cached Read Coils (FC 01), while register writes
        // affect cached Read Holding Registers (FC 03). Discrete inputs and
        // input registers are read-only address spaces and remain untouched.
        if (entry.function_code != affected_read_function) continue;
        const uint32_t cached_start = entry.start_address;
        const uint32_t cached_end = cached_start + uint32_t(entry.quantity);
        if (cached_start >= write_end || uint32_t(write_start) >= cached_end) continue;
      }

      entry.valid = false;
      invalidated++;
    }

    if (targeted) {
      const char *space = affected_read_function == 0x01 ? "Coils (read FC 01)" :
                                                            "Holding registers (read FC 03)";
      add_event_("CACHE INVALIDATE", String(direction == CACHE_DIRECTION_CLIENT ? "Client" : "Server") +
                 " mode | Unit ID=" + String(unit_id) + "; Write FC=" + String(write_function) +
                 "; Address=" + String(write_start) + "; Quantity=" + String(write_quantity) +
                 "; Affected=" + String(space) + "; Entries=" + String(invalidated) + ".");
    } else {
      add_event_("CACHE INVALIDATE", String(direction == CACHE_DIRECTION_CLIENT ? "Client" : "Server") +
                 " mode | Unit ID=" + String(unit_id) + "; Write FC=" + String(write_function) +
                 "; Scope=all cached reads for this Unit ID (unknown or malformed write range)" +
                 "; Entries=" + String(invalidated) + ".");
    }
  }

  void cache_clear_() {
    for (auto &entry : response_cache_) entry.valid = false;
  }


  static int base64_value_(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  }

  // Decode a data URI from flash and stream the binary image in small chunks.
  // This avoids building a 200+ KB HTML String on the ESP32.
  void serve_image_(const char *data_uri, const char *content_type) {
    if (data_uri == nullptr) {
      web_.send(500, "text/plain", "Image unavailable");
      return;
    }
    const char *comma = strchr(data_uri, ',');
    if (comma == nullptr) {
      web_.send(500, "text/plain", "Invalid image data");
      return;
    }
    const char *encoded = comma + 1;
    const size_t encoded_len = strlen(encoded);
    if (encoded_len == 0 || (encoded_len % 4) != 0) {
      web_.send(500, "text/plain", "Invalid image encoding");
      return;
    }
    // Validate the complete Base64 payload before sending HTTP headers.
    for (size_t i = 0; i < encoded_len; i++) {
      if (encoded[i] == '=') {
        if (i < encoded_len - 2) {
          web_.send(500, "text/plain", "Invalid image padding");
          return;
        }
      } else if (base64_value_(encoded[i]) < 0) {
        web_.send(500, "text/plain", "Invalid image character");
        return;
      }
    }

    size_t decoded_len = (encoded_len / 4) * 3;
    if (encoded_len >= 1 && encoded[encoded_len - 1] == '=') decoded_len--;
    if (encoded_len >= 2 && encoded[encoded_len - 2] == '=') decoded_len--;

    web_.sendHeader("Cache-Control", "public, max-age=86400");
    web_.setContentLength(decoded_len);
    web_.send(200, content_type, "");

    uint8_t buffer[768];
    size_t used = 0;
    for (size_t i = 0; i < encoded_len; i += 4) {
      const int a = base64_value_(encoded[i]);
      const int b = base64_value_(encoded[i + 1]);
      const int c = encoded[i + 2] == '=' ? 0 : base64_value_(encoded[i + 2]);
      const int d = encoded[i + 3] == '=' ? 0 : base64_value_(encoded[i + 3]);
      if (a < 0 || b < 0 || c < 0 || d < 0) {
        // All image assets are generated locally; this guard prevents malformed data
        // from being interpreted as arbitrary bytes if an asset is edited incorrectly.
        return;
      }
      const uint32_t triple = (uint32_t(a) << 18) | (uint32_t(b) << 12) |
                              (uint32_t(c) << 6) | uint32_t(d);
      buffer[used++] = uint8_t((triple >> 16) & 0xFF);
      if (encoded[i + 2] != '=') buffer[used++] = uint8_t((triple >> 8) & 0xFF);
      if (encoded[i + 3] != '=') buffer[used++] = uint8_t(triple & 0xFF);
      if (used >= sizeof(buffer) - 3) {
        web_.sendContent(reinterpret_cast<const char *>(buffer), used);
        used = 0;
      }
    }
    if (used != 0) web_.sendContent(reinterpret_cast<const char *>(buffer), used);
  }

  void load_settings_() {
    prefs_.begin("mbgateway", false);
    cfg_host_ = prefs_.getString("host", cfg_host_.c_str()).c_str();
    tcp_mode_ = prefs_.getString("mode", tcp_mode_.c_str()).c_str();
    cfg_port_ = prefs_.getUShort("port", cfg_port_);
    tcp_timeout_ms_ = prefs_.getUInt("timeout", tcp_timeout_ms_);
    silent_on_timeout_ = prefs_.getBool("silentto", silent_on_timeout_);
    tcp_persistent_ = prefs_.getBool("tcpkeep", tcp_persistent_);
    cache_client_enabled_ = prefs_.getBool("cachecli", cache_client_enabled_);
    cache_server_enabled_ = prefs_.getBool("cachesrv", cache_server_enabled_);
    cache_ttl_ms_ = prefs_.getUInt("cachettl", cache_ttl_ms_);
    if (cache_ttl_ms_ < 50 || cache_ttl_ms_ > 10000) cache_ttl_ms_ = 500;
    tx_gpio_ = prefs_.getInt("tx", tx_gpio_);
    rx_gpio_ = prefs_.getInt("rx", rx_gpio_);
    de_re_gpio_ = prefs_.getInt("de", de_re_gpio_);
    manual_rtu_id_ = prefs_.getBool("idmanual", false);
    configured_rtu_id_ = prefs_.getUChar("rtuid", 1);
    cfg_baud_ = prefs_.getUInt("baud", cfg_baud_);
    cfg_data_bits_ = prefs_.getUChar("dbits", cfg_data_bits_);
    cfg_parity_ = prefs_.getString("parity", cfg_parity_.c_str()).c_str();
    cfg_stop_bits_ = prefs_.getUChar("sbits", cfg_stop_bits_);
  }
  void setup_gpio_() {
    if (de_re_gpio_ >= 0) { pinMode(de_re_gpio_, OUTPUT); digitalWrite(de_re_gpio_, LOW); }
  }
  void set_direction_tx_(bool tx) { if (de_re_gpio_ >= 0) digitalWrite(de_re_gpio_, tx ? HIGH : LOW); }

  void begin_uart_() {
    uint32_t config = SERIAL_8N1;
    uint8_t bits = cfg_data_bits_;
    bool parity_on = cfg_parity_ != "NONE";
    bool odd = cfg_parity_ == "ODD";
    if (bits == 7) {
      if (!parity_on) config = SERIAL_7N1;
      else config = odd ? SERIAL_7O1 : SERIAL_7E1;
    } else {
      if (!parity_on) config = (cfg_stop_bits_ == 2) ? SERIAL_8N2 : SERIAL_8N1;
      else config = odd ? ((cfg_stop_bits_ == 2) ? SERIAL_8O2 : SERIAL_8O1)
                       : ((cfg_stop_bits_ == 2) ? SERIAL_8E2 : SERIAL_8E1);
    }
    // Serial1 is initialized directly so the web UI can apply UART changes at runtime.
    int tx = tx_gpio_;
    int rx = rx_gpio_;
    Serial1.end();
    Serial1.begin(cfg_baud_, config, rx, tx);
    ESP_LOGI("modbus_gateway", "UART: baud=%lu data_bits=%u parity=%s stop_bits=%u",
             static_cast<unsigned long>(cfg_baud_), static_cast<unsigned>(cfg_data_bits_), cfg_parity_.c_str(), static_cast<unsigned>(cfg_stop_bits_));
  }

  static uint16_t crc16_(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t pos=0;pos<len;pos++) {
      crc ^= data[pos];
      for (uint8_t i=0;i<8;i++)
        crc = (crc & 1) ? ((crc >> 1) ^ 0xA001) : (crc >> 1);
    }
    return crc;
  }

  static bool valid_crc_(const uint8_t *frame, size_t len) {
    if (len < 4) return false;
    uint16_t c = uint16_t(frame[len-2]) | (uint16_t(frame[len-1]) << 8);
    return c == crc16_(frame, len-2);
  }

  static String crc_diagnostic_(const uint8_t *frame, size_t len) {
    if (len < 2) return String("CRC unavailable (frame too short)");
    const uint16_t received = uint16_t(frame[len - 2]) | (uint16_t(frame[len - 1]) << 8);
    const uint16_t calculated = crc16_(frame, len - 2);
    char detail[72];
    snprintf(detail, sizeof(detail), "received=0x%04X; calculated=0x%04X",
             static_cast<unsigned>(received), static_cast<unsigned>(calculated));
    return String(detail);
  }

  size_t request_length_(uint8_t fc, const std::vector<uint8_t> &buf) {
    switch (fc) {
      case 1: case 2: case 3: case 4: case 5: case 6:
        return 8;
      case 15: case 16:
        if (buf.size() < 7) return 0;
        return size_t(9 + buf[6]);
      case 23:
        if (buf.size() < 11) return 0;
        return size_t(13 + buf[10]);
      default:
        return 0;
    }
  }

  void parse_rtu_() {
    if (client_txn_.state != ClientTxnState::IDLE) return;
    while (!rx_.empty() && client_txn_.state == ClientTxnState::IDLE) {
      // The RTU slave address is forwarded dynamically as the TCP Unit ID.
      if (rx_[0] == 0 || rx_[0] > 247) {
        rx_.erase(rx_.begin());
        if (!rx_timestamps_us_.empty()) rx_timestamps_us_.erase(rx_timestamps_us_.begin());
        continue;
      }
      if (rx_.size() < 2) return;
      size_t n = request_length_(rx_[1], rx_);
      if (n == 0) {
        if (millis() - last_rx_ms_ > 30) {
          rx_.clear();
          rx_timestamps_us_.clear();
        }
        return;
      }
      if (rx_.size() < n) return;
      std::vector<uint8_t> frame(rx_.begin(), rx_.begin()+n);
      uint32_t rtu_request_span_us = 0;
      if (rx_timestamps_us_.size() >= n) {
        rtu_request_span_us = rx_timestamps_us_[n - 1] - rx_timestamps_us_[0];
        rx_timestamps_us_.erase(rx_timestamps_us_.begin(), rx_timestamps_us_.begin() + n);
      } else {
        rx_timestamps_us_.clear();
      }
      rx_.erase(rx_.begin(), rx_.begin()+n);
      add_log_("RTU RX", frame.data(), frame.size());
      add_event_("TIMING", String("CLIENT RTU RX first-to-last=") + String(rtu_request_span_us) +
                 " us; bytes=" + String(n) + ".");
      if (!valid_crc_(frame.data(), frame.size())) {
        ++crc_error_count_;
        const String crc_detail = crc_diagnostic_(frame.data(), frame.size());
        add_event_("CRC ERROR", String("Client mode | RTU request discarded; Unit ID=") +
                   String(frame[0]) + "; Function=" + String(frame[1]) +
                   "; bytes=" + String(frame.size()) + "; " + crc_detail +
                   "; total CRC errors=" + String(crc_error_count_) + ".");
        ESP_LOGW("modbus_gateway", "Discarded RTU request with invalid CRC (%s)", crc_detail.c_str());
        continue;
      }
      std::vector<uint8_t> pdu(frame.begin()+1, frame.end()-2);
      const uint8_t unit_id = frame[0];
      const uint32_t end_to_end_started_us = micros();
      uint16_t cache_start = 0;
      uint16_t cache_quantity = 0;
      const bool cacheable_read = is_cacheable_read_request_(pdu, cache_start, cache_quantity);
      if (cache_client_enabled_ && cacheable_read) {
        uint32_t cache_age_ms = 0;
        cached_response_tmp_.clear();
        if (cache_lookup_(CACHE_DIRECTION_CLIENT, unit_id, pdu, cached_response_tmp_, cache_age_ms)) {
          add_event_("CACHE HIT", String("Client mode | Unit ID=") + String(unit_id) +
                     "; Function=" + String(pdu[0]) + "; Address=" + String(cache_start) +
                     "; Quantity=" + String(cache_quantity) + "; Age=" + String(cache_age_ms) + " ms.");
          const uint32_t rtu_tx_us = send_rtu_(unit_id, cached_response_tmp_);
          add_event_("TIMING", String("CLIENT cache=hit; age=") + String(cache_age_ms) +
                     " ms; RTU response TX=" + String(rtu_tx_us) + " us; total=" +
                     String(micros() - end_to_end_started_us) + " us.");
          continue;
        }
        add_event_("CACHE MISS", String("Client mode | Unit ID=") + String(unit_id) +
                   "; Function=" + String(pdu[0]) + "; Address=" + String(cache_start) +
                   "; Quantity=" + String(cache_quantity) + ".");
      }
      // Start the TCP exchange and return immediately. process_client_transaction_()
      // will collect the MBAP header and PDU over subsequent loop iterations.
      start_client_transaction_(unit_id, pdu, end_to_end_started_us, cacheable_read);
      return;
    }
  }

  void reset_client_transaction_() {
    client_txn_.state = ClientTxnState::IDLE;
    client_txn_.unit_id = 0;
    client_txn_.transaction_id = 0;
    client_txn_.expected_pdu_length = 0;
    memset(client_txn_.mbap, 0, sizeof(client_txn_.mbap));
    client_txn_.mbap_received = 0;
    client_txn_.response_received = 0;
    client_txn_.request_pdu.clear();
    client_txn_.response_pdu.clear();
    client_txn_.end_to_end_started_us = 0;
    client_txn_.connect_us = 0;
    client_txn_.tcp_write_us = 0;
    client_txn_.response_started_us = 0;
    client_txn_.response_stage_started_ms = 0;
    client_txn_.response_timing = ByteTiming{};
    client_txn_.reused_connection = false;
    client_txn_.cacheable_read = false;
  }

  void start_client_transaction_(uint8_t unit_id, const std::vector<uint8_t> &pdu,
                                 uint32_t end_to_end_started_us, bool cacheable_read) {
    if (pdu.empty() || pdu.size() > 253) return;
    reset_client_transaction_();
    client_txn_.unit_id = unit_id;
    client_txn_.request_pdu.assign(pdu.begin(), pdu.end());
    client_txn_.end_to_end_started_us = end_to_end_started_us;
    client_txn_.cacheable_read = cacheable_read;

    uint32_t connect_started_us = micros();
    const String local_ip = local_wifi_ip_();
    const bool wifi_connected = wifi::global_wifi_component != nullptr &&
                                wifi::global_wifi_component->is_connected();
    const String target = String(cfg_host_.c_str()) + ":" + String(cfg_port_);
    client_txn_.reused_connection = tcp_persistent_ && client_ && client_.connected();
    client_.setTimeout(tcp_timeout_ms_);
    client_.setNoDelay(true);

    if (client_txn_.reused_connection) {
      add_event_("TCP REUSE", String("Persistent connection reused to ") + target +
                 "; Wi-Fi=" + (wifi_connected ? "connected" : "not connected") +
                 "; local IP=" + local_ip);
    } else {
      client_.stop();
      add_event_("TCP CONNECT", String("Wi-Fi=") + (wifi_connected ? "connected" : "not connected") +
                 "; local IP=" + local_ip + "; opening connection to " + target);
      ESP_LOGI("modbus_gateway", "TCP client: Wi-Fi=%s local IP=%s; connecting to %s:%u (persistent=%s)",
               wifi_connected ? "connected" : "not connected", local_ip.c_str(), cfg_host_.c_str(),
               unsigned(cfg_port_), tcp_persistent_ ? "yes" : "no");
      if (!client_.connect(cfg_host_.c_str(), cfg_port_)) {
        client_txn_.connect_us = micros() - connect_started_us;
        add_event_("TCP ERROR", String("Connection to ") + target + " failed after " +
                   String(client_txn_.connect_us / 1000) + " ms (connect() returned false).");
        client_.stop();
        fail_client_transaction_("TCP connection failed.");
        return;
      }
      client_txn_.connect_us = micros() - connect_started_us;
      client_.setNoDelay(true);
      add_event_("TCP INFO", String("Connected to ") + target + " in " +
                 String(client_txn_.connect_us / 1000) + " ms.");
      ESP_LOGI("modbus_gateway", "TCP client: connected to %s:%u in %u ms", cfg_host_.c_str(),
               unsigned(cfg_port_), unsigned(client_txn_.connect_us / 1000));
    }

    const uint16_t tid = transaction_id_++;
    client_txn_.transaction_id = tid;
    const uint16_t length = uint16_t(1 + pdu.size());
    uint8_t request[260];
    request[0] = uint8_t(tid >> 8);
    request[1] = uint8_t(tid);
    request[2] = 0;
    request[3] = 0;
    request[4] = uint8_t(length >> 8);
    request[5] = uint8_t(length);
    request[6] = unit_id;
    memcpy(request + 7, pdu.data(), pdu.size());
    const size_t request_length = 7 + pdu.size();
    add_log_("TCP TX", request, request_length);
    const uint32_t write_started_us = micros();
    const size_t written = client_.write(request, request_length);
    client_txn_.tcp_write_us = micros() - write_started_us;
    if (written != request_length) {
      add_event_("TCP ERROR", "Failed to write the Modbus TCP request to the socket.");
      client_.stop();
      fail_client_transaction_("TCP request write failed.");
      return;
    }

    client_txn_.response_started_us = micros();
    client_txn_.response_stage_started_ms = millis();
    client_txn_.state = ClientTxnState::WAIT_MBAP;
  }

  void finish_client_transaction_() {
    const uint32_t response_us = micros() - client_txn_.response_started_us;
    const uint32_t first_byte_us = client_txn_.response_timing.first_delay_us(client_txn_.response_started_us);
    const uint32_t response_span_us = client_txn_.response_timing.span_us();
    add_log_("TCP RX", client_txn_.response_pdu.data(), client_txn_.response_pdu.size());
    if (!tcp_persistent_) {
      client_.stop();
    } else {
      const String target = String(cfg_host_.c_str()) + ":" + String(cfg_port_);
      add_event_("TCP KEEP", String("Persistent connection kept open to ") + target + ".");
    }

    const uint8_t unit_id = client_txn_.unit_id;
    const uint32_t end_to_end_started_us = client_txn_.end_to_end_started_us;
    const std::vector<uint8_t> &request_pdu = client_txn_.request_pdu;
    const std::vector<uint8_t> &response_pdu = client_txn_.response_pdu;
    const bool cacheable_read = client_txn_.cacheable_read;

    if (response_pdu.empty() ||
        (response_pdu[0] != request_pdu[0] && response_pdu[0] != uint8_t(request_pdu[0] | 0x80))) {
      fail_client_transaction_("The Modbus TCP response function code does not match the request.");
      return;
    }

    if (response_pdu[0] == request_pdu[0]) {
      if (cacheable_read && cache_client_enabled_) {
        cache_store_(CACHE_DIRECTION_CLIENT, unit_id, request_pdu, response_pdu);
      } else if (!cacheable_read) {
        cache_invalidate_after_write_(CACHE_DIRECTION_CLIENT, unit_id, request_pdu);
      }
    }

    const uint32_t total_us = micros() - end_to_end_started_us;
    add_event_("TIMING", String("CLIENT total=") + String(total_us) + " us; connect=" +
               String(client_txn_.connect_us) + " us" +
               (client_txn_.reused_connection ? " (reused)" : " (new)") +
               "; TCP write=" + String(client_txn_.tcp_write_us) +
               " us; TCP response wait=" + String(response_us) + " us; TCP first byte=" +
               String(first_byte_us) + " us; TCP first-to-last=" + String(response_span_us) +
               " us; result=OK.");
    const uint32_t rtu_tx_us = send_rtu_(unit_id, response_pdu);
    add_event_("TIMING", String("CLIENT RTU-to-RTU total=") +
               String(micros() - end_to_end_started_us) + " us; RTU response TX=" +
               String(rtu_tx_us) + " us.");
    reset_client_transaction_();
  }

  void fail_client_transaction_(const char *reason, bool is_timeout = false) {
    const uint32_t now_us = micros();
    const bool response_wait_started = client_txn_.state != ClientTxnState::IDLE;
    const uint32_t response_us = response_wait_started ?
                                 now_us - client_txn_.response_started_us : 0;
    const uint32_t first_byte_us = response_wait_started ?
        client_txn_.response_timing.first_delay_us(client_txn_.response_started_us) : 0;
    const uint32_t response_span_us = client_txn_.response_timing.span_us();
    add_event_("TCP ERROR", reason);
    if (client_) client_.stop();
    if (client_txn_.request_pdu.empty()) {
      reset_client_transaction_();
      return;
    }
    const uint8_t unit_id = client_txn_.unit_id;
    const uint8_t function = client_txn_.request_pdu[0];
    const uint32_t end_to_end_started_us = client_txn_.end_to_end_started_us;
    add_event_("TIMING", String("CLIENT total=") +
               String(now_us - end_to_end_started_us) + " us; connect=" +
               String(client_txn_.connect_us) + " us" +
               (client_txn_.reused_connection ? " (reused)" : " (new)") +
               "; TCP write=" + String(client_txn_.tcp_write_us) +
               " us; TCP response wait=" + String(response_us) + " us; TCP first byte=" +
               String(first_byte_us) + " us; TCP first-to-last=" + String(response_span_us) +
               " us; result=ERROR.");
    if (is_timeout && silent_on_timeout_) {
      add_event_("TIMEOUT SILENT", String("Client mode | TCP response timeout; no RTU reply sent. Unit ID=") +
                 String(unit_id) + "; Function=" + String(function) + ".");
      add_event_("TIMING", String("CLIENT failed transaction total=") +
                 String(micros() - end_to_end_started_us) + " us; result=silent timeout; no RTU TX.");
    } else {
      const uint8_t exception_code = is_timeout ? 0x0B : 0x0A;
      std::vector<uint8_t> exception = {uint8_t(function | 0x80), exception_code};
      const uint32_t rtu_tx_us = send_rtu_(unit_id, exception);
      add_event_("TIMING", String("CLIENT failed transaction total=") +
                 String(micros() - end_to_end_started_us) + " us; exception RTU TX=" +
                 String(rtu_tx_us) + " us; exception=" + String(exception_code, HEX) + ".");
    }
    reset_client_transaction_();
  }

  void process_client_transaction_() {
    if (client_txn_.state == ClientTxnState::IDLE) return;
    size_t budget = 512;  // Bound per-loop work so WebServer/Wi-Fi stay responsive.
    while (budget > 0) {
      if (client_txn_.state == ClientTxnState::WAIT_MBAP) {
        while (budget > 0 && client_.available() > 0 && client_txn_.mbap_received < sizeof(client_txn_.mbap)) {
          const int value = client_.read();
          if (value < 0) break;
          client_txn_.mbap[client_txn_.mbap_received++] = uint8_t(value);
          client_txn_.response_timing.mark(micros());
          --budget;
        }
        if (client_txn_.mbap_received == sizeof(client_txn_.mbap)) {
          const uint16_t response_tid = (uint16_t(client_txn_.mbap[0]) << 8) | client_txn_.mbap[1];
          const uint16_t protocol = (uint16_t(client_txn_.mbap[2]) << 8) | client_txn_.mbap[3];
          const uint16_t length = (uint16_t(client_txn_.mbap[4]) << 8) | client_txn_.mbap[5];
          if (response_tid != client_txn_.transaction_id || protocol != 0 || length < 2 ||
              length > 254 || client_txn_.mbap[6] != client_txn_.unit_id) {
            fail_client_transaction_("Invalid Modbus TCP MBAP response header.");
            return;
          }
          client_txn_.expected_pdu_length = uint16_t(length - 1);
          client_txn_.response_pdu.resize(client_txn_.expected_pdu_length);
          client_txn_.response_received = 0;
          client_txn_.state = ClientTxnState::WAIT_PDU;
          client_txn_.response_stage_started_ms = millis();
          continue;
        }
      } else if (client_txn_.state == ClientTxnState::WAIT_PDU) {
        while (budget > 0 && client_.available() > 0 &&
               client_txn_.response_received < client_txn_.expected_pdu_length) {
          const int value = client_.read();
          if (value < 0) break;
          client_txn_.response_pdu[client_txn_.response_received++] = uint8_t(value);
          client_txn_.response_timing.mark(micros());
          --budget;
        }
        if (client_txn_.response_received == client_txn_.expected_pdu_length) {
          finish_client_transaction_();
          return;
        }
      }

      if (!client_.connected() && client_.available() == 0) {
        fail_client_transaction_("TCP server disconnected before the full Modbus response was received.");
        return;
      }
      if (millis() - client_txn_.response_stage_started_ms >= tcp_timeout_ms_) {
        fail_client_transaction_(client_txn_.state == ClientTxnState::WAIT_MBAP ?
            "Timeout waiting for the Modbus TCP response header." :
            "Timeout waiting for the Modbus TCP response PDU.", true);
        return;
      }
      // No delay/yield loop here: return to ESPHome and poll again on the next loop.
      break;
    }
  }

  bool read_exact_(uint8_t *dst, size_t len, uint32_t timeout, ByteTiming *timing = nullptr) {
    size_t pos=0;
    uint32_t start=millis();
    while (pos<len && millis()-start<timeout) {
      while (client_.available() && pos<len) {
        int c=client_.read();
        if (c>=0) {
          if (timing != nullptr) timing->mark(micros());
          dst[pos++]=uint8_t(c);
        }
      }
      if (!client_.connected() && !client_.available()) break;
      delay(1);
    }
    return pos==len;
  }

  String local_wifi_ip_() {
    // ESPHome owns the Wi-Fi interface in this firmware. Its WiFiComponent
    // keeps the authoritative DHCP/static address; Arduino WiFi.localIP()
    // can report 0.0.0.0 when used alongside ESPHome's network manager.
    if (wifi::global_wifi_component != nullptr) {
      const auto addresses = wifi::global_wifi_component->get_ip_addresses();
      for (const auto &address : addresses) {
        if (address.is_set()) {
          char buffer[network::IP_ADDRESS_BUFFER_SIZE] = {};
          return String(address.str_to(buffer));
        }
      }
    }

    const IPAddress arduino_ip = WiFi.localIP();
    if (arduino_ip != IPAddress(0, 0, 0, 0)) return arduino_ip.toString();
    const IPAddress ap_ip = WiFi.softAPIP();
    if (ap_ip != IPAddress(0, 0, 0, 0)) return ap_ip.toString();
    return String("unavailable");
  }

  bool transact_tcp_(uint8_t unit_id, const std::vector<uint8_t> &pdu, std::vector<uint8_t> &response) {
    const uint32_t transaction_started_us = micros();
    uint32_t connect_us = 0;
    uint32_t tcp_write_us = 0;
    uint32_t tcp_response_us = 0;
    uint32_t tcp_first_byte_us = 0;
    uint32_t tcp_response_span_us = 0;
    bool reused_connection = false;
    auto finish = [this, transaction_started_us, &connect_us, &tcp_write_us,
                   &tcp_response_us, &tcp_first_byte_us, &tcp_response_span_us,
                   &reused_connection](bool success) -> bool {
      const uint32_t total_us = micros() - transaction_started_us;
      String detail = String("CLIENT total=") + String(total_us) + " us; connect=" +
                      String(connect_us) + " us" + (reused_connection ? " (reused)" : " (new)") +
                      "; TCP write=" + String(tcp_write_us) + " us; TCP response wait=" +
                      String(tcp_response_us) + " us; TCP first byte=" + String(tcp_first_byte_us) +
                      " us; TCP first-to-last=" + String(tcp_response_span_us) +
                      " us; result=" + (success ? "OK" : "ERROR") + ".";
      add_event_("TIMING", detail);
      return success;
    };

    // Keep one TCP session open between RTU transactions when persistent mode
    // is enabled. This avoids a TCP handshake on every Modbus request.
    const String local_ip = local_wifi_ip_();
    const bool wifi_connected = wifi::global_wifi_component != nullptr &&
                                wifi::global_wifi_component->is_connected();
    const String target = String(cfg_host_.c_str()) + ":" + String(cfg_port_);
    const bool reuse_connection = tcp_persistent_ && client_ && client_.connected();
    reused_connection = reuse_connection;

    client_.setTimeout(tcp_timeout_ms_);
    client_.setNoDelay(true);
    if (reuse_connection) {
      add_event_("TCP REUSE", String("Persistent connection reused to ") + target +
                 "; Wi-Fi=" + (wifi_connected ? "connected" : "not connected") +
                 "; local IP=" + local_ip);
    } else {
      client_.stop();
      add_event_("TCP CONNECT", String("Wi-Fi=") + (wifi_connected ? "connected" : "not connected") +
                 "; local IP=" + local_ip + "; opening connection to " + target);
      ESP_LOGI("modbus_gateway", "TCP client: Wi-Fi=%s local IP=%s; connecting to %s:%u (persistent=%s)",
               wifi_connected ? "connected" : "not connected", local_ip.c_str(), cfg_host_.c_str(), unsigned(cfg_port_), tcp_persistent_ ? "yes" : "no");
      const uint32_t connect_started_us = micros();
      if (!client_.connect(cfg_host_.c_str(), cfg_port_)) {
        connect_us = micros() - connect_started_us;
        const uint32_t elapsed_ms = connect_us / 1000;
        add_event_("TCP ERROR", String("Connection to ") + target + " failed after " + String(elapsed_ms) + " ms (connect() returned false).");
        ESP_LOGW("modbus_gateway", "TCP client: connection to %s:%u failed after %u ms", cfg_host_.c_str(), unsigned(cfg_port_), unsigned(elapsed_ms));
        client_.stop();
        return finish(false);
      }
      connect_us = micros() - connect_started_us;
      client_.setNoDelay(true);
      add_event_("TCP INFO", String("Connected to ") + target + " in " + String(connect_us / 1000) + " ms.");
      ESP_LOGI("modbus_gateway", "TCP client: connected to %s:%u in %u ms", cfg_host_.c_str(), unsigned(cfg_port_), unsigned(connect_us / 1000));
    }

    uint16_t tid=transaction_id_++;
    uint16_t len=uint16_t(1+pdu.size());
    std::vector<uint8_t> req;
    req.reserve(7 + 1 + pdu.size());
    req = {
      uint8_t(tid>>8),uint8_t(tid),0,0,uint8_t(len>>8),uint8_t(len),unit_id
    };
    req.insert(req.end(),pdu.begin(),pdu.end());
    add_log_("TCP TX", req.data(), req.size());
    const uint32_t write_started_us = micros();
    const size_t written = client_.write(req.data(),req.size());
    tcp_write_us = micros() - write_started_us;
    if (written != req.size()) {
      add_event_("TCP ERROR", "Failed to write the Modbus TCP request to the socket.");
      ESP_LOGW("modbus_gateway", "TCP client: request write failed");
      client_.stop();
      return finish(false);
    }

    const uint32_t response_started_us = micros();
    ByteTiming tcp_response_timing;
    uint8_t mbap[7];
    if (!read_exact_(mbap,7,tcp_timeout_ms_, &tcp_response_timing)) {
      tcp_response_us = micros() - response_started_us;
      tcp_first_byte_us = tcp_response_timing.first_delay_us(response_started_us);
      tcp_response_span_us = tcp_response_timing.span_us();
      add_event_("TCP ERROR", "Timeout or disconnect while waiting for the Modbus TCP response header.");
      ESP_LOGW("modbus_gateway", "TCP client: timeout waiting for response header");
      client_.stop();
      return finish(false);
    }
    uint16_t rtid=(uint16_t(mbap[0])<<8)|mbap[1];
    uint16_t proto=(uint16_t(mbap[2])<<8)|mbap[3];
    uint16_t rlen=(uint16_t(mbap[4])<<8)|mbap[5];
    if (rtid!=tid || proto!=0 || rlen<2 || rlen>254 || mbap[6]!=unit_id) {
      tcp_response_us = micros() - response_started_us;
      tcp_first_byte_us = tcp_response_timing.first_delay_us(response_started_us);
      tcp_response_span_us = tcp_response_timing.span_us();
      add_event_("TCP ERROR", "Invalid Modbus TCP MBAP header (transaction, protocol, length or Unit ID mismatch).");
      ESP_LOGW("modbus_gateway", "TCP client: invalid MBAP response (TID %u/%u, protocol %u, length %u, Unit ID %u/%u)", unsigned(rtid), unsigned(tid), unsigned(proto), unsigned(rlen), unsigned(mbap[6]), unsigned(unit_id));
      client_.stop();
      return finish(false);
    }
    std::vector<uint8_t> rest;
    rest.resize(rlen-1);
    if (!read_exact_(rest.data(),rest.size(),tcp_timeout_ms_, &tcp_response_timing)) {
      tcp_response_us = micros() - response_started_us;
      tcp_first_byte_us = tcp_response_timing.first_delay_us(response_started_us);
      tcp_response_span_us = tcp_response_timing.span_us();
      add_event_("TCP ERROR", "Timeout or disconnect while waiting for the Modbus TCP response data.");
      ESP_LOGW("modbus_gateway", "TCP client: timeout waiting for response data");
      client_.stop();
      return finish(false);
    }
    tcp_response_us = micros() - response_started_us;
    tcp_first_byte_us = tcp_response_timing.first_delay_us(response_started_us);
    tcp_response_span_us = tcp_response_timing.span_us();
    add_log_("TCP RX", rest.data(), rest.size());
    if (!tcp_persistent_) {
      client_.stop();
    } else {
      add_event_("TCP KEEP", String("Persistent connection kept open to ") + target + ".");
    }
    if (rest.empty()) {
      add_event_("TCP ERROR", "The Modbus TCP server returned an empty response PDU.");
      ESP_LOGW("modbus_gateway", "TCP client: empty response PDU");
      return finish(false);
    }
    if (rest[0]!=pdu[0] && rest[0]!=uint8_t(pdu[0]|0x80)) {
      add_event_("TCP ERROR", "The response function code does not match the request.");
      ESP_LOGW("modbus_gateway", "TCP client: response function code mismatch");
      return finish(false);
    }
    response=rest;
    return finish(true);
  }

  uint32_t send_rtu_(uint8_t unit_id, const std::vector<uint8_t> &pdu) {
    if (pdu.empty()) return 0;
    const uint32_t tx_started_us = micros();
    std::vector<uint8_t> frame;
    frame.reserve(pdu.size() + 3);
    frame.push_back(unit_id);
    frame.insert(frame.end(),pdu.begin(),pdu.end());
    uint16_t crc=crc16_(frame.data(),frame.size());
    frame.push_back(uint8_t(crc));
    frame.push_back(uint8_t(crc>>8));
    add_log_("RTU TX", frame.data(), frame.size());
    if (de_re_gpio_ >= 0) digitalWrite(de_re_gpio_, HIGH);
    delayMicroseconds(100);
    Serial1.write(frame.data(),frame.size());
    Serial1.flush();
    delayMicroseconds(100);
    if (de_re_gpio_ >= 0) digitalWrite(de_re_gpio_, LOW);
    return micros() - tx_started_us;
  }

  void clear_tcp_request_(TcpClientSlot &slot) {
    memset(slot.mbap, 0, sizeof(slot.mbap));
    slot.mbap_pos = 0;
    slot.pdu.clear();
    slot.pdu_pos = 0;
    slot.expected_pdu = 0;
    slot.last_activity = 0;
    slot.request_ready_us = 0;
    slot.request_first_byte_us = 0;
    slot.request_last_byte_us = 0;
    slot.request_ready = false;
  }

  void reset_tcp_client_(TcpClientSlot &slot, bool close_socket = true) {
    if (close_socket && slot.client) slot.client.stop();
    clear_tcp_request_(slot);
  }

  void stop_tcp_clients_() {
    for (size_t i = 0; i < MAX_TCP_CLIENTS; i++) reset_tcp_client_(tcp_clients_[i]);
    tcp_client_cursor_ = 0;
  }

  void start_tcp_server_() {
    tcp_server_.stop();
    if (client_txn_.state != ClientTxnState::IDLE) {
      fail_client_transaction_("TCP transaction aborted because the gateway configuration was applied.");
    }
    client_.stop();
    stop_tcp_clients_();
    if (tcp_mode_ == "SERVER") {
      tcp_server_.begin(cfg_port_);
      tcp_server_.setNoDelay(true);
      ESP_LOGI("modbus_gateway", "Modbus TCP server listening on port %u (max %u clients)", cfg_port_, unsigned(MAX_TCP_CLIENTS));
    }
  }

  void cleanup_disconnected_tcp_clients_() {
    for (size_t i = 0; i < MAX_TCP_CLIENTS; i++) {
      TcpClientSlot &slot = tcp_clients_[i];
      if (slot.client && !slot.client.connected() && !slot.client.available()) {
        ESP_LOGI("modbus_gateway", "Modbus TCP client %u disconnected", unsigned(i + 1));
        reset_tcp_client_(slot, false);
      }
    }
  }

  void accept_tcp_clients_() {
    // Drain the pending-connection queue. Each accepted socket is retained in
    // its own slot, so several TCP masters can keep their sessions open.
    while (tcp_server_.hasClient()) {
      WiFiClient incoming = tcp_server_.accept();
      if (!incoming) break;
      incoming.setNoDelay(true);

      int free_slot = -1;
      for (size_t i = 0; i < MAX_TCP_CLIENTS; i++) {
        if (!tcp_clients_[i].client) {
          free_slot = int(i);
          break;
        }
      }
      if (free_slot < 0) {
        ESP_LOGW("modbus_gateway", "TCP client limit reached (%u); rejecting connection", unsigned(MAX_TCP_CLIENTS));
        incoming.stop();
        continue;
      }

      TcpClientSlot &slot = tcp_clients_[free_slot];
      clear_tcp_request_(slot);
      slot.client = incoming;
      ESP_LOGI("modbus_gateway", "Modbus TCP client %u connected", unsigned(free_slot + 1));
    }
  }

  void receive_tcp_request_(TcpClientSlot &slot, size_t client_index) {
    if (!slot.client || slot.request_ready) return;

    while (slot.client.available() > 0 && !slot.request_ready) {
      int value = slot.client.read();
      if (value < 0) break;
      slot.last_activity = millis();
      const uint32_t byte_received_us = micros();
      if (slot.mbap_pos == 0 && slot.pdu_pos == 0) slot.request_first_byte_us = byte_received_us;
      slot.request_last_byte_us = byte_received_us;

      if (slot.mbap_pos < sizeof(slot.mbap)) {
        slot.mbap[slot.mbap_pos++] = uint8_t(value);
        if (slot.mbap_pos == sizeof(slot.mbap)) {
          const uint16_t protocol = (uint16_t(slot.mbap[2]) << 8) | slot.mbap[3];
          const uint16_t length = (uint16_t(slot.mbap[4]) << 8) | slot.mbap[5];
          if (protocol != 0 || length < 2 || length > 254) {
            ESP_LOGW("modbus_gateway", "Invalid Modbus TCP header from client %u", unsigned(client_index + 1));
            reset_tcp_client_(slot);
            return;
          }
          slot.expected_pdu = uint16_t(length - 1); // MBAP length includes Unit ID.
          slot.pdu.resize(slot.expected_pdu);
          slot.pdu_pos = 0;
        }
      } else if (slot.pdu_pos < slot.expected_pdu) {
        slot.pdu[slot.pdu_pos++] = uint8_t(value);
        if (slot.pdu_pos == slot.expected_pdu) {
          slot.request_ready = true;
          slot.request_ready_us = micros();
        }
      }
    }

    if (!slot.request_ready && (slot.mbap_pos > 0 || slot.pdu_pos > 0) &&
        millis() - slot.last_activity > tcp_timeout_ms_) {
      ESP_LOGW("modbus_gateway", "Incomplete Modbus TCP request timed out on client %u", unsigned(client_index + 1));
      reset_tcp_client_(slot);
      return;
    }

    if (!slot.client.connected() && !slot.client.available() && !slot.request_ready) {
      reset_tcp_client_(slot);
    }
  }

  bool read_client_exact_(WiFiClient &c, uint8_t *dst, size_t len, uint32_t timeout) {
    size_t pos = 0;
    uint32_t start = millis();
    while (pos < len && millis() - start < timeout) {
      while (c.available() && pos < len) dst[pos++] = uint8_t(c.read());
      if (!c.connected() && !c.available()) break;
      delay(1);
    }
    return pos == len;
  }

  size_t expected_rtu_response_(uint8_t fc, const uint8_t *pdu, size_t pdu_len, uint8_t *buf, size_t cap,
                                uint32_t &first_byte_delay_us, uint32_t &first_to_last_us) {
    const uint32_t wait_started_us = micros();
    uint32_t start = millis(); size_t n = 0; size_t expected = 0;
    uint32_t first_byte_at_us = 0;
    uint32_t last_byte_at_us = 0;
    first_byte_delay_us = 0;
    first_to_last_us = 0;
    while (millis() - start < tcp_timeout_ms_ && n < cap) {
      while (Serial1.available() && n < cap) {
        const uint32_t byte_received_us = micros();
        if (n == 0) {
          first_byte_at_us = byte_received_us;
          first_byte_delay_us = first_byte_at_us - wait_started_us;
        }
        last_byte_at_us = byte_received_us;
        buf[n++] = uint8_t(Serial1.read());
        if (n >= 2) {
          if (buf[1] & 0x80) expected = 5;
          else if (fc >= 1 && fc <= 4) expected = (n >= 3) ? size_t(5 + buf[2]) : 0;
          else if (fc == 5 || fc == 6 || fc == 15 || fc == 16) expected = 8;
          else if (fc == 23) expected = (n >= 3) ? size_t(5 + buf[2]) : 0;
        }
        if (expected && n >= expected) {
          first_to_last_us = last_byte_at_us - first_byte_at_us;
          return n;
        }
      }
      delay(1);
    }
    if (n > 0) first_to_last_us = last_byte_at_us - first_byte_at_us;
    return n;
  }

  void process_tcp_request_(TcpClientSlot &slot, size_t client_index) {
    if (!slot.request_ready || !slot.client || !slot.client.connected()) {
      reset_tcp_client_(slot);
      return;
    }

    const uint32_t transaction_started_us = micros();
    const uint32_t queue_wait_us = slot.request_ready_us != 0
        ? transaction_started_us - slot.request_ready_us : 0;
    uint32_t rtu_tx_us = 0;
    uint32_t rtu_response_wait_us = 0;
    uint32_t rtu_first_byte_us = 0;
    uint32_t rtu_response_span_us = 0;
    uint32_t tcp_write_us = 0;
    const uint32_t tcp_request_span_us = (slot.request_first_byte_us != 0 && slot.request_last_byte_us != 0)
        ? slot.request_last_byte_us - slot.request_first_byte_us : 0;

    const uint16_t tid = (uint16_t(slot.mbap[0]) << 8) | slot.mbap[1];
    const uint8_t tcp_unit = slot.mbap[6];
    const uint8_t unit = manual_rtu_id_ ? configured_rtu_id_ : tcp_unit;
    const std::vector<uint8_t> &pdu = slot.pdu;

    add_log_("TCP RX", slot.mbap, sizeof(slot.mbap));
    add_log_("TCP RX PDU", pdu.data(), pdu.size());

    uint16_t cache_start = 0;
    uint16_t cache_quantity = 0;
    const bool cacheable_read = is_cacheable_read_request_(pdu, cache_start, cache_quantity);
    if (cache_server_enabled_ && cacheable_read) {
      std::vector<uint8_t> cached_pdu;
      uint32_t cache_age_ms = 0;
      if (cache_lookup_(CACHE_DIRECTION_SERVER, unit, pdu, cached_pdu, cache_age_ms)) {
        add_event_("CACHE HIT", String("Server mode | RTU Unit ID=") + String(unit) +
                   "; TCP Unit ID=" + String(tcp_unit) + "; Function=" + String(pdu[0]) +
                   "; Address=" + String(cache_start) + "; Quantity=" + String(cache_quantity) +
                   "; Age=" + String(cache_age_ms) + " ms.");
        std::vector<uint8_t> cached_out = {
          uint8_t(tid >> 8), uint8_t(tid), 0, 0,
          uint8_t((cached_pdu.size() + 1) >> 8), uint8_t(cached_pdu.size() + 1), tcp_unit
        };
        cached_out.insert(cached_out.end(), cached_pdu.begin(), cached_pdu.end());
        add_log_("TCP TX", cached_out.data(), cached_out.size());
        const uint32_t cache_write_started_us = micros();
        const size_t cache_written = slot.client.write(cached_out.data(), cached_out.size());
        const uint32_t cache_write_us = micros() - cache_write_started_us;
        if (cache_written != cached_out.size()) {
          add_event_("TIMING", String("SERVER cache=hit; TCP write error; total=") +
                     String(micros() - transaction_started_us) + " us.");
          reset_tcp_client_(slot);
          return;
        }
        add_event_("TIMING", String("SERVER cache=hit; age=") + String(cache_age_ms) +
                   " ms; queue=" + String(queue_wait_us) + " us; TCP write=" +
                   String(cache_write_us) + " us; total=" +
                   String(micros() - transaction_started_us) + " us.");
        clear_tcp_request_(slot);
        return;
      }
      add_event_("CACHE MISS", String("Server mode | RTU Unit ID=") + String(unit) +
                 "; TCP Unit ID=" + String(tcp_unit) + "; Function=" + String(pdu[0]) +
                 "; Address=" + String(cache_start) + "; Quantity=" + String(cache_quantity) + ".");
    }

    std::vector<uint8_t> rtu;
    rtu.reserve(pdu.size() + 3);
    rtu.push_back(unit);
    rtu.insert(rtu.end(), pdu.begin(), pdu.end());
    const uint16_t crc = crc16_(rtu.data(), rtu.size());
    rtu.push_back(uint8_t(crc));
    rtu.push_back(uint8_t(crc >> 8));

    const uint32_t rtu_tx_started_us = micros();
    if (de_re_gpio_ >= 0) digitalWrite(de_re_gpio_, HIGH);
    delayMicroseconds(100);
    // Record the complete RTU request before transmitting it on the RS-485 bus.
    add_log_("RTU TX", rtu.data(), rtu.size());
    Serial1.write(rtu.data(), rtu.size());
    Serial1.flush();
    delayMicroseconds(100);
    if (de_re_gpio_ >= 0) digitalWrite(de_re_gpio_, LOW);
    rtu_tx_us = micros() - rtu_tx_started_us;

    uint8_t response[260];
    const uint32_t rtu_wait_started_us = micros();
    const size_t rlen = expected_rtu_response_(pdu[0], pdu.data(), pdu.size(), response, sizeof(response),
                                               rtu_first_byte_us, rtu_response_span_us);
    rtu_response_wait_us = micros() - rtu_wait_started_us;
    if (rlen >= 5 && valid_crc_(response, rlen)) {
      add_log_("RTU RX", response, rlen);
      std::vector<uint8_t> response_pdu(response + 1, response + rlen - 2);
      if (response_pdu.size() > 0 && response_pdu[0] == pdu[0]) {
        if (cacheable_read && cache_server_enabled_) {
          cache_store_(CACHE_DIRECTION_SERVER, unit, pdu, response_pdu);
        } else if (!cacheable_read) {
          cache_invalidate_after_write_(CACHE_DIRECTION_SERVER, unit, pdu);
        }
      }
      std::vector<uint8_t> out = {
        uint8_t(tid >> 8), uint8_t(tid), 0, 0,
        uint8_t((rlen - 2) >> 8), uint8_t(rlen - 2), tcp_unit
      };
      out.insert(out.end(), response + 1, response + rlen - 2);
      add_log_("TCP TX", out.data(), out.size());
      const uint32_t tcp_write_started_us = micros();
      const size_t written = slot.client.write(out.data(), out.size());
      tcp_write_us = micros() - tcp_write_started_us;
      if (written != out.size()) {
        add_event_("TIMING", String("SERVER queue=") + String(queue_wait_us) + " us; RTU TX=" +
                   String(rtu_tx_us) + " us; RTU wait=" + String(rtu_response_wait_us) +
                   " us; RTU first byte=" + String(rtu_first_byte_us) + " us; RTU first-to-last=" +
                   String(rtu_response_span_us) + " us; TCP request first-to-last=" +
                   String(tcp_request_span_us) + " us; TCP write=" + String(tcp_write_us) +
                   " us; result=write error; total=" +
                   String(micros() - transaction_started_us) + " us.");
        ESP_LOGW("modbus_gateway", "Failed to send response to TCP client %u", unsigned(client_index + 1));
        reset_tcp_client_(slot);
        return;
      }
    } else {
      if (rlen >= 5) {
        // The RTU slave answered, but a corrupt frame must never reach the TCP client
        // or response cache. Keep the raw frame in the trace so the UI marks its CRC.
        ++crc_error_count_;
        add_log_("RTU RX", response, rlen);
        const String crc_detail = crc_diagnostic_(response, rlen);
        add_event_("CRC ERROR", String("Server mode | RTU response discarded; RTU Unit ID=") +
                   String(unit) + "; Function=" + String(pdu[0]) +
                   "; bytes=" + String(rlen) + "; " + crc_detail +
                   "; total CRC errors=" + String(crc_error_count_) +
                   ". TCP gateway exception 0x0B returned.");
        ESP_LOGW("modbus_gateway", "Discarded RTU response with invalid CRC (%s)", crc_detail.c_str());
      } else {
        add_event_("RTU ERROR", String("Server mode | No complete RTU response; Unit ID=") +
                   String(unit) + "; Function=" + String(pdu[0]) +
                   (silent_on_timeout_ ? ". Silent mode: no TCP response sent." :
                                         ". TCP gateway exception 0x0B returned."));
      }
      const bool rtu_timeout = rlen < 5;
      if (rtu_timeout && silent_on_timeout_) {
        add_event_("TIMEOUT SILENT", String("Server mode | RTU slave timeout; no TCP reply sent. Unit ID=") +
                   String(unit) + "; TCP Unit ID=" + String(tcp_unit) +
                   "; Function=" + String(pdu[0]) + ".");
        tcp_write_us = 0;
      } else {
        const uint8_t exception_pdu[] = {uint8_t(pdu[0] | 0x80), 0x0B};
        const uint8_t out[] = {
          uint8_t(tid >> 8), uint8_t(tid), 0, 0, 0, 3,
          tcp_unit, exception_pdu[0], exception_pdu[1]
        };
        add_log_("TCP TX", out, sizeof(out));
        const uint32_t tcp_write_started_us = micros();
        const size_t written = slot.client.write(out, sizeof(out));
        tcp_write_us = micros() - tcp_write_started_us;
        if (written != sizeof(out)) {
          add_event_("TIMING", String("SERVER queue=") + String(queue_wait_us) + " us; RTU TX=" +
                   String(rtu_tx_us) + " us; RTU wait=" + String(rtu_response_wait_us) +
                   " us; RTU first byte=" + String(rtu_first_byte_us) + " us; RTU first-to-last=" +
                   String(rtu_response_span_us) + " us; TCP request first-to-last=" +
                   String(tcp_request_span_us) + " us; TCP write=" + String(tcp_write_us) +
                   " us; result=exception write error; total=" +
                   String(micros() - transaction_started_us) + " us.");
        ESP_LOGW("modbus_gateway", "Failed to send exception to TCP client %u", unsigned(client_index + 1));
          reset_tcp_client_(slot);
          return;
        }
      }
    }

    add_event_("TIMING", String("SERVER queue=") + String(queue_wait_us) + " us; RTU TX=" +
               String(rtu_tx_us) + " us; RTU wait=" + String(rtu_response_wait_us) +
               " us; RTU first byte=" + String(rtu_first_byte_us) + " us; RTU first-to-last=" +
               String(rtu_response_span_us) + " us; TCP request first-to-last=" +
               String(tcp_request_span_us) + " us; TCP write=" + String(tcp_write_us) + " us; total=" +
               String(micros() - transaction_started_us) + " us.");

    // Keep the TCP session open for the client's next Modbus ADU.
    clear_tcp_request_(slot);
  }

  void handle_tcp_server_() {
    cleanup_disconnected_tcp_clients_();
    accept_tcp_clients_();

    for (size_t i = 0; i < MAX_TCP_CLIENTS; i++) {
      receive_tcp_request_(tcp_clients_[i], i);
    }

    // The RS-485 bus is half-duplex: service one complete transaction at a
    // time, rotating the starting client to avoid starving quieter masters.
    for (size_t offset = 0; offset < MAX_TCP_CLIENTS; offset++) {
      const size_t index = (tcp_client_cursor_ + offset) % MAX_TCP_CLIENTS;
      if (tcp_clients_[index].request_ready) {
        process_tcp_request_(tcp_clients_[index], index);
        tcp_client_cursor_ = uint8_t((index + 1) % MAX_TCP_CLIENTS);
        break;
      }
    }
  }

  String page_() {
    // Images are served by dedicated routes; keep this HTML response compact.
    String s;
    s.reserve(19000);
    s += R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Modbus Gateway</title>
<style>
*{box-sizing:border-box}html{background:#eef3f8}body{font:15px/1.5 Arial,Helvetica,sans-serif;background:#eef3f8;color:#17202a;margin:0;padding:24px 16px}main{max-width:920px;margin:0 auto}.brand{display:flex;flex-direction:column;align-items:center;text-align:center;gap:8px;margin:0 auto 22px;padding:18px;background:#fff;border-radius:16px;box-shadow:0 3px 14px #0f172a12}.brand-logo{display:block;width:100%;max-width:920px;height:auto;max-height:none;object-fit:contain;margin:0 auto}.brand-version{font-size:12px;font-weight:600;letter-spacing:.35px;color:#64748b;margin-top:-3px}.brand-title{font-size:25px;font-weight:700;letter-spacing:.2px;color:#12345a}.brand-sub{font-size:13px;color:#64748b;margin-top:2px}section{display:block;background:#fff;border:1px solid #e1e8f0;border-radius:14px;padding:20px;margin:16px 0;box-shadow:0 3px 12px rgba(15,23,42,.05)}h2{display:flex;align-items:center;gap:10px;font-size:19px;line-height:1.25;margin:0 0 18px;color:#12345a}.section-icon{display:block;width:54px;height:42px;flex:0 0 54px;object-fit:contain;vertical-align:middle;margin:0;border-radius:6px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:14px 16px}label{display:block;font-size:13px;font-weight:600;color:#475569;margin:0 0 6px}input,select{display:block;width:100%;min-height:40px;box-sizing:border-box;padding:9px 11px;border:1px solid #cbd5e1;border-radius:8px;background:#fff;color:#17202a;font-size:15px}input:focus,select:focus{outline:2px solid #93c5fd;border-color:#2563eb}button{background:#1769aa;color:#fff;border:0;border-radius:8px;padding:11px 18px;font-size:14px;font-weight:600;cursor:pointer}button:hover{background:#125487}.muted{color:#64748b;font-size:13px}pre{white-space:pre-wrap;word-break:break-word;background:#101827;color:#dbeafe;padding:14px;border-radius:9px;max-height:360px;overflow:auto}.crc-byte{display:inline-block;border:2px solid #fbbf24;border-radius:50%;padding:1px 4px;margin:0 1px;color:#fde68a;font-weight:bold}.crc-ok{color:#86efac;font-weight:bold}.crc-bad{color:#fca5a5;font-weight:bold}#langBtn{float:right;margin:0 0 10px 10px}.check-label{display:flex;align-items:center;gap:9px;font-weight:600}.check-label input{width:18px;min-height:18px;margin:0}.reboot-panel{display:flex;align-items:center;justify-content:space-between;gap:18px;background:#fff;border:1px solid #e1e8f0;border-radius:14px;padding:14px 20px;margin:12px 0 16px}.reboot-panel button{background:#64748b}.reboot-panel button:hover{background:#475569}.apply-panel{display:flex;align-items:center;justify-content:space-between;gap:18px;background:#fff;border:1px solid #cbdbea;border-radius:14px;padding:16px 20px;margin:16px 0;box-shadow:0 3px 12px rgba(15,23,42,.05)}.apply-panel strong{display:block;color:#12345a;font-size:15px}.apply-panel .muted{margin-top:3px}.button-link{display:inline-block;background:#1769aa;color:#fff;border:0;border-radius:8px;padding:11px 18px;font-size:14px;font-weight:600;text-decoration:none;white-space:nowrap}.button-link:hover{background:#125487}@media(max-width:560px){.reboot-panel{align-items:stretch;flex-direction:column;gap:10px}.reboot-panel button{width:100%}.apply-panel{align-items:stretch;flex-direction:column;gap:10px}.apply-panel button,.apply-panel .button-link{width:100%;text-align:center}}#cfg{margin:0}@media(max-width:560px){body{padding:12px 9px}section{padding:14px}.brand{padding:12px}.brand-logo{width:100%;max-width:100%;max-height:none}.brand-title{font-size:20px}h2{font-size:17px}.section-icon{width:46px;height:36px;flex-basis:46px}}
</style></head><body><main><button type="button" id="langBtn" onclick="toggleLanguage()" style="float:right">English</button><header class="brand">
<img class="brand-logo" src="/img/logo.webp" alt="Logo passerelle Modbus"><div class="brand-version">Version )HTML";
    s += String(version_.c_str());
    s += R"HTML(</div><div><div class="brand-title">Passerelle Modbus RTU ↔ TCP</div><div class="brand-sub" data-fr="Passerelle série ↔ réseau" data-en="Serial ↔ network gateway">Passerelle série ↔ réseau</div></div></header>
<section><h2><img class="section-icon" src="/img/modbus-tcp.jpg" alt="">1. Modbus TCP</h2><div class="grid"><div><label>Mode</label><select name="mode" form="cfg">)HTML";
    s += "<option value=\"CLIENT\"" + String(tcp_mode_ == "CLIENT" ? " selected" : "") + ">Client TCP (RTU vers serveur)</option>";
    s += "<option value=\"SERVER\"" + String(tcp_mode_ == "SERVER" ? " selected" : "") + ">Serveur TCP (client TCP vers RTU)</option>";
    s += R"HTML(</select></div>
<div id="host_field"><label>Adresse IP du serveur Modbus TCP</label><input id="host_input" name="host" form="cfg" value=")HTML";
    s += cfg_host_.c_str();
    s += R"HTML("></div><div><label>Port Modbus TCP</label><input name="port" form="cfg" type="number" min="1" max="65535" value=")HTML";
    s += String(cfg_port_);
    s += R"HTML("</div><div><label>Timeout TCP (secondes)</label><input name="timeout" form="cfg" type="number" min="1" max="30" value=")HTML";
    s += String(tcp_timeout_ms_ / 1000);
    s += R"HTML("></div><div><label>Comportement en cas de timeout distant</label><select name="timeout_behavior" form="cfg"><option value="standard")HTML";
    if (!silent_on_timeout_) s += " selected";
    s += R"HTML(>Standard : envoyer une exception Modbus</option><option value="silent")HTML";
    if (silent_on_timeout_) s += " selected";
    s += R"HTML(>Silencieux : ne pas répondre</option></select><p class="muted">S’applique aux deux modes TCP. En mode silencieux, seule l’absence de réponse distante déclenche le silence ; les erreurs de trame CRC restent signalées.</p></div><div id="persistent_tcp_field"><label class="check-label"><input type="checkbox" name="persistent" value="1" form="cfg")HTML";
    if (tcp_persistent_) s += " checked";
    s += R"HTML(> Connexion TCP persistante</label><p class="muted">Garde la connexion ouverte entre les requêtes RTU pour réduire la latence (mode Client TCP uniquement).</p></div>
<div id="cache_client_field"><label class="check-label"><input type="checkbox" name="cache_client" value="1" form="cfg")HTML";
    if (cache_client_enabled_) s += " checked";
    s += R"HTML(> Activer le cache en mode Client TCP (RTU → TCP)</label></div>
<div id="cache_server_field"><label class="check-label"><input type="checkbox" name="cache_server" value="1" form="cfg")HTML";
    if (cache_server_enabled_) s += " checked";
    s += R"HTML(> Activer le cache en mode Serveur TCP (TCP → RTU)</label></div>
<div><label>Durée de validité du cache (ms)</label><input name="cache_ttl" form="cfg" type="number" min="50" max="10000" value=")HTML";
    s += String(cache_ttl_ms_);
    s += R"HTML("></div>
<div class="muted" style="grid-column:1/-1">Le cache concerne uniquement les lectures Modbus (fonctions 01 à 04). L’option affichée correspond au mode TCP sélectionné ; le réglage de l’autre mode est conservé. Les écritures réussies invalident les réponses mémorisées pour l’Unit ID concerné. 64 entrées maximum.</div>
</div></section>
<section><h2><img class="section-icon" src="/img/modbus-rtu.jpg" alt="">2. Paramètres Modbus RTU</h2><div class="grid">
<div><label>Baudrate</label><select name="baud" form="cfg">)HTML";
    const uint32_t baud_opts[]={1200,2400,4800,9600,19200,38400,57600,115200};
    for (auto b: baud_opts) { s += "<option value=\""+String(b)+"\""; if (b==cfg_baud_) s+=" selected"; s+=">"+String(b)+"</option>"; }
    s += R"HTML(</select></div><div><label>Data bits</label><select name="dbits" form="cfg">)HTML";
    for (int b: {7,8}) { s += "<option value=\""+String(b)+"\""; if (b==cfg_data_bits_) s+=" selected"; s+=">"+String(b)+"</option>"; }
    s += R"HTML(</select></div><div><label>Parity</label><select name="parity" form="cfg">)HTML";
    for (const char *p: {"NONE","EVEN","ODD"}) { s += "<option value=\""+String(p)+"\""; if (cfg_parity_==p) s+=" selected"; s+=">"+String(p)+"</option>"; }
    s += R"HTML(</select></div><div><label>Stop bits</label><select name="sbits" form="cfg">)HTML";
    for (int b: {1,2}) { s += "<option value=\""+String(b)+"\""; if (b==cfg_stop_bits_) s+=" selected"; s+=">"+String(b)+"</option>"; }
    s += R"HTML(</select></div>
<div id="rtu_id_mode_field"><label>Identifiant Modbus RTU</label><select name="idmode" form="cfg"><option value="auto")HTML"; if (!manual_rtu_id_) s += " selected"; s += R"HTML(>Automatique (adresse reçue)</option><option value="manual")HTML"; if (manual_rtu_id_) s += " selected"; s += R"HTML(>Manuel</option></select></div>
<div id="rtu_id_manual_field"><label>ID RTU manuel (1–247)</label><input name="rtuid" form="cfg" type="number" min="1" max="247" value=")HTML"; s += String(configured_rtu_id_); s += R"HTML("></div>
</div></section>
<section><h2><img class="section-icon" src="/img/esp32.jpg" alt="">3. Configuration</h2>
<div class="grid">
<div><label>GPIO TX (-1 = automatique)</label><input name="tx" form="cfg" type="number" min="-1" max="48" value=")HTML"; s += String(tx_gpio_); s += R"HTML("></div>
<div><label>GPIO RX (-1 = automatique)</label><input name="rx" form="cfg" type="number" min="-1" max="48" value=")HTML"; s += String(rx_gpio_); s += R"HTML("></div>
<div><label>GPIO DE/RE/EN (-1 = automatique)</label><input name="de" form="cfg" type="number" min="-1" max="48" value=")HTML"; s += String(de_re_gpio_); s += R"HTML("></div>
</div>
<p class="muted">-1 = sélection automatique de la broche. Les paramètres sont enregistrés en mémoire. Toute modification des GPIO redémarre l’ESP32.</p>
</section>
<form id="cfg" method="POST" action="/save"><input type="hidden" id="form_lang" name="lang" value="fr"><div class="apply-panel"><div><strong>Enregistrement global</strong><div class="muted">Les paramètres des sections 1, 2 et 3 seront enregistrés ensemble.</div></div><button type="submit">Enregistrer / appliquer</button></div></form>
<form id="rebootForm" method="POST" action="/reboot"><input type="hidden" id="reboot_lang" name="lang" value="fr"><div class="reboot-panel"><div><strong>Redémarrer la passerelle</strong><div class="muted">Redémarre l’ESP32 sans modifier les paramètres enregistrés.</div></div><button type="submit">Redémarrer</button></div></form>
<section><h2><img class="section-icon" src="/img/frame-diagnostics.jpg" alt="">4. Debug des trames</h2>
<p class="muted">RTU RX/TX et TCP TX/RX · rafraîchissement automatique toutes les 1,5 s</p>
<p class="muted">Les tentatives de connexion TCP et les erreurs réseau sont également affichées dans cette fenêtre.</p>
<p><button id="listenBtn" type="button" onclick="toggleListen()">Arrêter l’écoute RTU</button> <span id="listenState">Écoute RTU active</span></p>
<div style="margin-top:14px"><label for="rtuFrame">Trame RTU à envoyer (hexadécimal, avec adresse et CRC, ex. 0B 03 00 00 00 01 84 A0)</label>
<input id="rtuFrame" placeholder="0B 03 00 00 00 01 ... CRC" autocomplete="off">
<button type="button" onclick="sendManual()" style="margin-top:10px">Envoyer au serveur TCP</button></div>
<p class="muted">Réponse du serveur Modbus TCP :</p><pre id="manualResponse">Aucune requête manuelle</pre>
<p style="margin-bottom:6px"><button type="button" onclick="clearFrames()">Vider l’affichage des trames</button> <a class="button-link" href="/frames.txt">Exporter la capture des trames (.txt)</a></p>
<p class="muted">L’export contient les 64 dernières entrées mémorisées, y compris les mesures de latence.</p>
<pre id="frames">Chargement des trames…</pre></section>
<script>
let currentLanguage='fr';
const translations={
'Mode':'Mode','Client TCP (RTU vers serveur)':'TCP client (RTU to server)','Serveur TCP (client TCP vers RTU)':'TCP server (TCP client to RTU)',
'Connexion TCP persistante':'Persistent TCP connection','Garde la connexion ouverte entre les requêtes RTU pour réduire la latence (mode Client TCP uniquement).':'Keep the connection open between RTU requests to reduce latency (TCP Client mode only).','Redémarrer la passerelle':'Restart gateway','Redémarrer':'Restart','Redémarre l’ESP32 sans modifier les paramètres enregistrés.':'Restart the ESP32 without changing saved settings.',
'Adresse IP du serveur Modbus TCP':'Modbus TCP server IP address','Port Modbus TCP':'Modbus TCP port','Timeout TCP (secondes)':'TCP timeout (seconds)','Comportement en cas de timeout distant':'Behavior when the remote device times out','Standard : envoyer une exception Modbus':'Standard: send a Modbus exception','Silencieux : ne pas répondre':'Silent: do not reply','S’applique aux deux modes TCP. En mode silencieux, seule l’absence de réponse distante déclenche le silence ; les erreurs de trame CRC restent signalées.':'Applies to both TCP modes. Silent mode is used only when the remote device does not respond; CRC frame errors are still reported.',
'Activer le cache en mode Client TCP (RTU → TCP)':'Enable cache in TCP Client mode (RTU → TCP)','Activer le cache en mode Serveur TCP (TCP → RTU)':'Enable cache in TCP Server mode (TCP → RTU)','Durée de validité du cache (ms)':'Cache time-to-live (ms)','Le cache concerne uniquement les lectures Modbus (fonctions 01 à 04). L’option affichée correspond au mode TCP sélectionné ; le réglage de l’autre mode est conservé. Les écritures réussies invalident les réponses mémorisées pour l’Unit ID concerné. 64 entrées maximum.':'The cache stores only Modbus read requests (functions 01 to 04). Only the option for the selected TCP mode is shown; the other mode’s setting is preserved. Successful writes invalidate cached responses for the affected Unit ID. Maximum 64 entries.',
'CACHE TROUVÉ':'CACHE HIT','CACHE ABSENT':'CACHE MISS','CACHE ENREGISTRÉ':'CACHE STORE','CACHE INVALIDÉ':'CACHE INVALIDATE','Mode Client':'Client mode','Mode Serveur':'Server mode','Fonction=':'Function=','Adresse=':'Address=','Quantité=':'Quantity=','Âge=':'Age=','Entrées=':'Entries=','Opération réussie hors lecture.':'Successful non-read operation.',
'Baudrate':'Baud rate','Data bits':'Data bits','Parity':'Parity','Stop bits':'Stop bits',
'GPIO TX (-1 = automatique)':'TX GPIO (-1 = automatic)','GPIO RX (-1 = automatique)':'RX GPIO (-1 = automatic)','GPIO DE/RE/EN (-1 = automatique)':'DE/RE/EN GPIO (-1 = automatic)',
'Identifiant Modbus RTU':'Modbus RTU device ID','Automatique (adresse reçue)':'Automatic (received address)','Manuel':'Manual','ID RTU manuel (1–247)':'Manual RTU ID (1–247)',
'Enregistrer / appliquer':'Save / apply','Enregistrement global':'Apply all settings','Les paramètres des sections 1, 2 et 3 seront enregistrés ensemble.':'Settings in sections 1, 2 and 3 will be saved together.','-1 = sélection automatique de la broche. Les paramètres sont enregistrés en mémoire. Toute modification des GPIO redémarre l’ESP32.':'-1 = automatic pin selection. Settings are stored in memory. Any GPIO change restarts the ESP32.',
'Paramètres Modbus RTU':'Modbus RTU settings','3. Configuration':'3. Configuration','4. Debug des trames':'4. Frame diagnostics',
'RTU RX/TX et TCP TX/RX · rafraîchissement automatique toutes les 1,5 s':'RTU RX/TX and TCP TX/RX · automatic refresh every 1.5 s',
'Les tentatives de connexion TCP et les erreurs réseau sont également affichées dans cette fenêtre.':'TCP connection attempts and network errors are also displayed in this window.',
'Arrêter l’écoute RTU':'Stop RTU listening','Reprendre l’écoute RTU':'Resume RTU listening','Écoute RTU active':'RTU listening','Écoute RTU suspendue':'RTU listening paused','Erreur de commande':'Command error',
'Trame RTU à envoyer (hexadécimal, avec adresse et CRC, ex. 0B 03 00 00 00 01 84 A0)':'RTU frame to send (hexadecimal, including address and CRC, e.g. 0B 03 00 00 00 01 84 A0)',
'Envoyer au serveur TCP':'Send to TCP server','Réponse du serveur Modbus TCP :':'Modbus TCP server response:','Aucune requête manuelle':'No manual request','Vider l’affichage des trames':'Clear frame log','Exporter la capture des trames (.txt)':'Export frame capture (.txt)','L’export contient les 64 dernières entrées mémorisées, y compris les mesures de latence.':'The export contains the 64 most recent stored entries, including timing diagnostics.','Chargement des trames…':'Loading frames…','Aucune trame':'No frames',
'CRC Erreur':'CRC Error','CRC ERROR':'Erreur CRC','RTU ERROR':'Erreur RTU','Client mode | RTU request discarded; Unit ID=':'Mode Client | requête RTU rejetée ; ID unité=','Server mode | RTU response discarded; RTU Unit ID=':'Mode Serveur | réponse RTU rejetée ; ID unité RTU=','Server mode | No complete RTU response; Unit ID=':'Mode Serveur | aucune réponse RTU complète ; ID unité=','; Function=':'; fonction=','; bytes=':'; octets=','; received=0x':'; CRC reçu=0x','; calculated=0x':'; CRC calculé=0x','; total CRC errors=':'; total erreurs CRC=','. TCP gateway exception 0x0B returned.':'. Exception passerelle TCP 0x0B renvoyée.','Erreur':'Error','CRC OK':'CRC OK','Envoi en cours…':'Sending…','Erreur de communication avec la passerelle':'Gateway communication error',
'Erreur : saisir une trame RTU complète en hexadécimal (adresse, fonction, données, CRC).':'Error: enter a complete RTU frame in hexadecimal (address, function, data, CRC).',
'Erreur : caractère hexadécimal invalide.':'Error: invalid hexadecimal character.',
'Erreur : CRC RTU invalide. La trame doit inclure un CRC valide.':'Error: invalid RTU CRC. The frame must include a valid CRC.',
'Erreur : adresse RTU invalide ou trame trop courte.':'Error: invalid RTU address or frame too short.',
'Échec de la transaction Modbus TCP : vérifier Wi-Fi, IP/port et réponse du serveur.':'Modbus TCP transaction failed: check Wi-Fi, IP/port and server response.',
'Connecting to ':'Connexion vers ',
'Persistent connection reused to ':'Connexion persistante réutilisée vers ','Persistent connection kept open to ':'Connexion persistante maintenue vers ','Wi-Fi=connected':'Wi-Fi=connecté','Wi-Fi=not connected':'Wi-Fi=non connecté','local IP=':'IP locale=','; local IP=':'; IP locale=','; opening connection to ':'; ouverture de connexion vers ',
'Connected to ':'Connecté à ',
'Connection to ':'Connexion vers ',
' failed after ':' a échoué après ',
' ms (connect() returned false).':' ms (connect() a renvoyé false).',
' in ':' en ',
' ms.':' ms.',
' us; connect=':' µs; connexion=',' us (reused)':' µs (réutilisée)',' us (new)':' µs (nouvelle)',
'; TCP write=':'; écriture TCP=',' us; TCP response wait=':' µs; attente réponse TCP=','; result=OK.': '; résultat=OK.','; result=ERROR.': '; résultat=ERREUR.',
'CLIENT RTU RX first-to-last=':'CLIENT RTU RX premier-au-dernier octet=',
'TCP first byte=':'premier octet TCP=',
'TCP first-to-last=':'premier-au-dernier octet TCP=',
'RTU first byte=':'premier octet RTU=',
'RTU first-to-last=':'premier-au-dernier octet RTU=',
'TCP request first-to-last=':'requête TCP premier-au-dernier octet=',
'CLIENT RTU-to-RTU total=':'CLIENT total RTU à RTU=',
'; RTU response TX=':'; émission réponse RTU=',
'CLIENT failed transaction total=':'CLIENT transaction en échec=',
'; exception RTU TX=':'; émission exception RTU=',
'SERVER queue=':'SERVEUR attente file=',' us; RTU TX=':' µs; RTU TX=',' us; RTU wait=':' µs; attente réponse RTU=',
'; result=write error; total=':'; erreur écriture TCP; total=','; result=exception write error; total=':'; erreur écriture exception; total=',' us; total=':' µs; total=',
'Failed to write the Modbus TCP request to the socket.':'Échec de l’envoi de la requête Modbus TCP sur la socket.',
'Timeout or disconnect while waiting for the Modbus TCP response header.':'Délai dépassé ou déconnexion en attendant l’en-tête de réponse Modbus TCP.',
'Invalid Modbus TCP MBAP header (transaction, protocol, length or Unit ID mismatch).':'En-tête MBAP Modbus TCP invalide (transaction, protocole, longueur ou Unit ID différent).',
'Timeout or disconnect while waiting for the Modbus TCP response data.':'Délai dépassé ou déconnexion en attendant les données de réponse Modbus TCP.',
'The Modbus TCP server returned an empty response PDU.':'Le serveur Modbus TCP a renvoyé une réponse PDU vide.',
'The response function code does not match the request.':'Le code fonction de la réponse ne correspond pas à la requête.',
'TCP connection failed.':'La connexion TCP a échoué.','TCP request write failed.':'Échec de l’envoi de la requête TCP.',
'Invalid Modbus TCP MBAP response header.':'En-tête MBAP de réponse Modbus TCP invalide.',
'TCP server disconnected before the full Modbus response was received.':'Le serveur TCP s’est déconnecté avant la réception complète de la réponse Modbus.',
'Timeout waiting for the Modbus TCP response header.':'Délai dépassé en attendant l’en-tête de réponse Modbus TCP.',
'Timeout waiting for the Modbus TCP response PDU.':'Délai dépassé en attendant la PDU de réponse Modbus TCP.',
'The Modbus TCP response function code does not match the request.':'Le code fonction de la réponse Modbus TCP ne correspond pas à la requête.',
'TCP transaction aborted because the gateway configuration was applied.':'Transaction TCP interrompue car la configuration de la passerelle a été appliquée.',
'Pending RTU input buffer overflow; buffered bytes discarded.':'Débordement du tampon RTU en attente ; les octets mémorisés ont été supprimés.',
'Une transaction RTU/TCP est en cours. Réessayez après sa réponse.':'An RTU/TCP transaction is in progress. Retry after its response.',
'Réponse TCP (PDU) :':'TCP response (PDU):','Réponse RTU complète :':'Complete RTU response:',
'Passerelle série ↔ réseau':'Serial ↔ network gateway','Passerelle Modbus RTU ↔ TCP':'Modbus RTU ↔ TCP Gateway',
'1. Modbus TCP':'1. Modbus TCP','2. Paramètres Modbus RTU':'2. Modbus RTU settings','3. Configuration':'3. Configuration','4. Debug des trames':'4. Frame diagnostics'
};
function tr(fr,en){return currentLanguage==='fr'?fr:en;}
function translateValue(value){const pairs=Object.entries(translations).map(([fr,en])=>currentLanguage==='en'?[fr,en]:[en,fr]).sort((a,b)=>b[0].length-a[0].length);let out=value;for(const [from,to] of pairs){if(from)out=out.split(from).join(to);}return out;}
function applyLanguage(){
  const walker=document.createTreeWalker(document.body,NodeFilter.SHOW_TEXT);
  let node;while((node=walker.nextNode())){const parent=node.parentElement;if(!parent||['SCRIPT','STYLE'].includes(parent.tagName))continue;const raw=node.nodeValue;const trimmed=raw.trim();if(!trimmed)continue;const translated=translateValue(trimmed);if(translated!==trimmed)node.nodeValue=raw.replace(trimmed,translated);}
  document.querySelectorAll('[placeholder]').forEach(el=>{el.placeholder=translateValue(el.placeholder);});
  document.getElementById('langBtn').textContent=currentLanguage==='fr'?'English':'Français';
  updateListenState();
}
function toggleLanguage(){currentLanguage=currentLanguage==='fr'?'en':'fr';document.getElementById('form_lang').value=currentLanguage;document.getElementById('reboot_lang').value=currentLanguage;applyLanguage();}
function toggleRtuId(){const isServer=document.querySelector('select[name=mode]').value==='SERVER';document.getElementById('rtu_id_mode_field').style.display=isServer?'block':'none';document.getElementById('rtu_id_manual_field').style.display=(isServer&&document.querySelector('select[name=idmode]').value==='manual')?'block':'none';}document.querySelector('select[name=idmode]').addEventListener('change',toggleRtuId);document.querySelector('select[name=mode]').addEventListener('change',toggleRtuId);toggleRtuId();
function crc16Modbus(bytes){let crc=0xFFFF;for(const b of bytes){crc^=b;for(let i=0;i<8;i++)crc=(crc&1)?((crc>>>1)^0xA001):(crc>>>1);}return crc&0xFFFF;}
function toggleHostField(){const isServer=document.querySelector('select[name="mode"]').value==='SERVER';document.getElementById('host_field').style.display=isServer?'none':'block';document.getElementById('host_input').required=!isServer;const persistentField=document.getElementById('persistent_tcp_field');const persistentInput=document.querySelector('input[name="persistent"]');persistentField.style.display=isServer?'none':'block';persistentInput.disabled=isServer;}
document.querySelector('select[name="mode"]').addEventListener('change',toggleHostField);toggleHostField();
function toggleCacheFields(){const isClient=document.querySelector('select[name="mode"]').value==='CLIENT';document.getElementById('cache_client_field').style.display=isClient?'block':'none';document.getElementById('cache_server_field').style.display=isClient?'none':'block';}
document.querySelector('select[name="mode"]').addEventListener('change',toggleCacheFields);toggleCacheFields();
function escapeHtml(value){return String(value).replace(/[&<>]/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;'}[c];});}
function formatFrame(x){const prefix=x.time+' | '+translateValue(x.dir)+' | ';const isRtu=x.dir.startsWith('RTU')||x.dir==='MANUAL RTU';const bytes=x.hex.trim().split(/\s+/).filter(Boolean);if(!isRtu||bytes.length<3)return prefix+escapeHtml(translateValue(x.hex));const vals=bytes.map(b=>parseInt(b,16));if(vals.some(v=>!Number.isFinite(v)))return prefix+x.hex;const calculated=crc16Modbus(vals.slice(0,-2));const crcOk=vals[vals.length-2]===(calculated&255)&&vals[vals.length-1]===((calculated>>8)&255);const payload=bytes.slice(0,-2).join(' ');const crc='<span class=\"crc-byte\">'+bytes[bytes.length-2]+'</span> <span class=\"crc-byte\">'+bytes[bytes.length-1]+'</span>';return prefix+payload+' '+crc+'  <span class=\"'+(crcOk?'crc-ok':'crc-bad')+'\">CRC '+(crcOk?'OK':tr('Erreur','Error'))+'</span>';}
async function refresh(){try{let r=await fetch('/frames');let j=await r.json();document.getElementById('frames').innerHTML=j.frames.map(formatFrame).join('\n')||tr('Aucune trame','No frames');}catch(e){}}
async function clearFrames(){try{await fetch('/clear',{method:'POST'});document.getElementById('frames').textContent=tr('Aucune trame','No frames');}catch(e){}}
async function updateListenState(){try{let r=await fetch('/listen');let j=await r.json();document.getElementById('listenBtn').textContent=j.listening?tr('Arrêter l’écoute RTU','Stop RTU listening'):tr('Reprendre l’écoute RTU','Resume RTU listening');document.getElementById('listenState').textContent=j.listening?tr('Écoute RTU active','RTU listening'):tr('Écoute RTU suspendue','RTU listening paused');}catch(e){}}
async function toggleListen(){try{await fetch('/listen',{method:'POST'});await updateListenState();}catch(e){document.getElementById('listenState').textContent=tr('Erreur de commande','Command error');}}
async function sendManual(){let frame=document.getElementById('rtuFrame').value;let out=document.getElementById('manualResponse');out.textContent=tr('Envoi en cours…','Sending…');try{let r=await fetch('/manual',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'frame='+encodeURIComponent(frame)});const responseText=await r.text();out.textContent=translateValue(responseText);refresh();}catch(e){out.textContent=tr('Erreur de communication avec la passerelle','Gateway communication error');}}
updateListenState();refresh();setInterval(refresh,1500);
</script></main></body></html>)HTML";
    return s;
  }

  String frames_json_() {
    String out;
    out.reserve(12288);
    out = "{\"frames\":[";
    for (size_t i=0;i<log_count_;i++) {
      size_t idx=(log_head_+FRAME_LOG_CAPACITY-log_count_+i)%FRAME_LOG_CAPACITY;
      if (i) out+=",";
      out+="{\"time\":\""+String(logs_[idx].when)+" ms\",\"dir\":\""+logs_[idx].direction+"\",\"hex\":\""+logs_[idx].hex+"\"}";
    }
    out+="]}";
    return out;
  }

  String frames_text_() {
    String out;
    out.reserve(12288);
    out += "Modbus RTU <-> TCP - Frame capture\r\n";
    out += "Timestamps are milliseconds since ESP32 boot.\r\n";
    out += "Exported entries: ";
    out += String(log_count_);
    out += " (maximum retained: 64)\r\n\r\n";
    if (log_count_ == 0) {
      out += "No frames recorded.\r\n";
      return out;
    }
    for (size_t i=0; i<log_count_; i++) {
      const size_t idx = (log_head_ + FRAME_LOG_CAPACITY - log_count_ + i) % FRAME_LOG_CAPACITY;
      out += String(logs_[idx].when);
      out += " ms | ";
      out += logs_[idx].direction;
      out += " | ";
      out += logs_[idx].hex;
      out += "\r\n";
    }
    return out;
  }

  static String hex_bytes_(const std::vector<uint8_t> &data) {
    String out;
    char b[4];
    for (size_t i=0;i<data.size();i++) { snprintf(b,sizeof(b),"%02X",data[i]); if(i) out += " "; out += b; }
    return out;
  }

  void manual_request_() {
    if (tcp_mode_ == "CLIENT" && client_txn_.state != ClientTxnState::IDLE) {
      manual_result_ = "Une transaction RTU/TCP est en cours. Réessayez après sa réponse.";
      web_.send(409, "text/plain; charset=utf-8", manual_result_);
      return;
    }
    String input = web_.arg("frame");
    input.replace(" ", ""); input.replace(":", ""); input.replace("-", ""); input.replace("\r", ""); input.replace("\n", "");
    if (input.length() < 8 || (input.length() % 2) != 0) {
      manual_result_ = "Erreur : saisir une trame RTU complète en hexadécimal (adresse, fonction, données, CRC).";
      web_.send(400, "text/plain; charset=utf-8", manual_result_); return;
    }
    std::vector<uint8_t> frame;
    for (size_t i=0;i<input.length();i+=2) {
      char pair[3] = {input[i], input[i+1], 0};
      char *end=nullptr; long v=strtol(pair,&end,16);
      if (end==pair || *end!=0 || v<0 || v>255) {
        manual_result_="Erreur : caractère hexadécimal invalide.";
        web_.send(400,"text/plain; charset=utf-8",manual_result_); return;
      }
      frame.push_back(uint8_t(v));
    }
    add_log_("MANUAL RTU",frame.data(),frame.size());
    if (!valid_crc_(frame.data(),frame.size())) {
      manual_result_="Erreur : CRC RTU invalide. La trame doit inclure un CRC valide.";
      web_.send(400,"text/plain; charset=utf-8",manual_result_); return;
    }
    uint8_t unit_id=frame[0];
    if (unit_id==0 || unit_id>247 || frame.size()<4) {
      manual_result_="Erreur : adresse RTU invalide ou trame trop courte.";
      web_.send(400,"text/plain; charset=utf-8",manual_result_); return;
    }
    std::vector<uint8_t> pdu(frame.begin()+1,frame.end()-2), response;
    if (transact_tcp_(unit_id,pdu,response)) {
      manual_result_="Réponse TCP (PDU) : "+hex_bytes_(response)+"\nRéponse RTU complète : ";
      std::vector<uint8_t> rtu={unit_id}; rtu.insert(rtu.end(),response.begin(),response.end());
      uint16_t crc=crc16_(rtu.data(),rtu.size()); rtu.push_back(uint8_t(crc)); rtu.push_back(uint8_t(crc>>8));
      manual_result_+=hex_bytes_(rtu);
    } else {
      manual_result_="Échec de la transaction Modbus TCP : vérifier Wi-Fi, IP/port et réponse du serveur.";
    }
    web_.send(200,"text/plain; charset=utf-8",manual_result_);
  }

  String reboot_page_(bool english) {
    String page;
    page.reserve(2600);
    page += "<!doctype html><html lang=\"";
    page += english ? "en" : "fr";
    page += "\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
    page += "<meta name=\"robots\" content=\"noindex,nofollow\"><title>";
    page += english ? "Gateway restarting" : "Redémarrage de la passerelle";
    page += "</title><style>body{margin:0;min-height:100vh;display:grid;place-items:center;background:#eef3f8;color:#17202a;font:16px/1.5 Arial,Helvetica,sans-serif;padding:20px;box-sizing:border-box}.card{width:min(520px,100%);box-sizing:border-box;background:#fff;border:1px solid #dbe5ef;border-radius:16px;padding:30px 24px;text-align:center;box-shadow:0 5px 24px #0f172a14}h1{font-size:23px;color:#12345a;margin:0 0 12px}.seconds{font-size:48px;font-weight:700;color:#1769aa;margin:14px 0}.muted{color:#64748b;font-size:14px}#status{margin-top:18px;min-height:24px;color:#1769aa;font-weight:600}</style></head><body><main class=\"card\"><h1>";
    page += english ? "Gateway is restarting" : "La passerelle redémarre";
    page += "</h1><p>";
    page += english ? "Your settings have been saved." : "Vos paramètres ont été enregistrés.";
    page += "</p><div class=\"seconds\"><span id=\"seconds\">5</span></div><p class=\"muted\">";
    page += english ? "The main page will open automatically when the gateway is ready." : "La page principale s’ouvrira automatiquement lorsque la passerelle sera prête.";
    page += "</p><p id=\"status\" aria-live=\"polite\">";
    page += english ? "Restarting in 5 seconds…" : "Redémarrage dans 5 secondes…";
    page += "</p></main><script>const english=";
    page += english ? "true" : "false";
    page += ";const text=english?{waiting:'Waiting for the gateway to come back online…',checking:'Checking gateway…',online:'Gateway online. Returning to the main page…',retry:'Gateway not ready yet. Retrying…'}:{waiting:'En attente du retour de la passerelle…',checking:'Vérification de la passerelle…',online:'Passerelle accessible. Retour à la page principale…',retry:'La passerelle n’est pas encore prête. Nouvelle tentative…'};let remaining=5;const seconds=document.getElementById('seconds');const status=document.getElementById('status');const countdown=setInterval(()=>{remaining=Math.max(0,remaining-1);seconds.textContent=String(remaining);if(remaining===0){clearInterval(countdown);status.textContent=text.waiting;checkGateway();}},1000);let checking=false;async function checkGateway(){if(checking)return;checking=true;const controller=new AbortController();const timeout=setTimeout(()=>controller.abort(),1500);try{const response=await fetch('/health?ts='+Date.now(),{cache:'no-store',signal:controller.signal});if(response.ok){status.textContent=text.online;window.location.replace('/');return;}}catch(error){}finally{clearTimeout(timeout);checking=false;}status.textContent=text.retry;setTimeout(checkGateway,1000);}</script></body></html>";
    return page;
  }

  void send_reboot_response_(bool english) {
    client_.stop();
    tcp_server_.stop();
    stop_tcp_clients_();
    web_.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
    web_.sendHeader("Connection", "close");
    web_.send(200, "text/html; charset=utf-8", reboot_page_(english));
    // Give the browser time to receive and render the reboot countdown page.
    delay(1500);
    ESP.restart();
  }

  void reboot_gateway_() {
    const bool english = web_.arg("lang") == "en";
    add_event_("SYSTEM", "Manual reboot requested from Web UI.");
    send_reboot_response_(english);
  }

  void save_config_() {
    int new_tx = web_.hasArg("tx") ? web_.arg("tx").toInt() : tx_gpio_;
    int new_rx = web_.hasArg("rx") ? web_.arg("rx").toInt() : rx_gpio_;
    int new_de = web_.hasArg("de") ? web_.arg("de").toInt() : de_re_gpio_;
    bool gpio_changed = new_tx != tx_gpio_ || new_rx != rx_gpio_ || new_de != de_re_gpio_;
    if (web_.hasArg("mode")) tcp_mode_ = web_.arg("mode").c_str();
    if (web_.hasArg("host")) cfg_host_ = web_.arg("host").substring(0,63).c_str();
    if (web_.hasArg("port")) { int v=web_.arg("port").toInt(); if(v>=1 && v<=65535) cfg_port_=uint16_t(v); }
    if (web_.hasArg("timeout")) { int v=web_.arg("timeout").toInt(); if(v>=1 && v<=30) tcp_timeout_ms_=uint32_t(v)*1000; }
    if (web_.hasArg("timeout_behavior")) silent_on_timeout_ = web_.arg("timeout_behavior") == "silent";
    if (web_.hasArg("baud")) cfg_baud_=web_.arg("baud").toInt();
    if (web_.hasArg("dbits")) cfg_data_bits_=uint8_t(web_.arg("dbits").toInt());
    if (web_.hasArg("parity")) cfg_parity_=web_.arg("parity").c_str();
    if (web_.hasArg("sbits")) cfg_stop_bits_=uint8_t(web_.arg("sbits").toInt());
    if (new_tx >= -1 && new_tx <= 48) tx_gpio_=new_tx;
    if (new_rx >= -1 && new_rx <= 48) rx_gpio_=new_rx;
    if (new_de >= -1 && new_de <= 48) de_re_gpio_=new_de;
    manual_rtu_id_ = web_.arg("idmode") == "manual";
    if (web_.hasArg("rtuid")) { int v=web_.arg("rtuid").toInt(); if(v>=1 && v<=247) configured_rtu_id_=uint8_t(v); }
    prefs_.putString("host", cfg_host_.c_str()); prefs_.putString("mode", tcp_mode_.c_str()); prefs_.putUShort("port", cfg_port_);
    prefs_.putUInt("timeout", tcp_timeout_ms_); prefs_.putBool("silentto", silent_on_timeout_); prefs_.putInt("tx", tx_gpio_);
    prefs_.putInt("rx", rx_gpio_); prefs_.putInt("de", de_re_gpio_);
    prefs_.putBool("idmanual", manual_rtu_id_); prefs_.putUChar("rtuid", configured_rtu_id_);
    prefs_.putUInt("baud", cfg_baud_); prefs_.putUChar("dbits", cfg_data_bits_);
    prefs_.putString("parity", cfg_parity_.c_str()); prefs_.putUChar("sbits", cfg_stop_bits_);
    if (web_.hasArg("mode") && tcp_mode_ == "CLIENT") {
      tcp_persistent_ = web_.hasArg("persistent") && web_.arg("persistent") == "1";
    }
    prefs_.putBool("tcpkeep", tcp_persistent_);
    // The Web UI only shows the cache switch for the currently selected mode.
    // Update that mode only; preserve the hidden mode's saved preference.
    if (tcp_mode_ == "CLIENT") {
      cache_client_enabled_ = web_.hasArg("cache_client") && web_.arg("cache_client") == "1";
    } else if (tcp_mode_ == "SERVER") {
      cache_server_enabled_ = web_.hasArg("cache_server") && web_.arg("cache_server") == "1";
    }
    if (web_.hasArg("cache_ttl")) {
      int ttl = web_.arg("cache_ttl").toInt();
      if (ttl >= 50 && ttl <= 10000) cache_ttl_ms_ = uint32_t(ttl);
    }
    prefs_.putBool("cachecli", cache_client_enabled_);
    prefs_.putBool("cachesrv", cache_server_enabled_);
    prefs_.putUInt("cachettl", cache_ttl_ms_);
    cache_clear_();
    if (gpio_changed) {
      send_reboot_response_(web_.arg("lang") == "en");
      return;
    }
    begin_uart_(); start_tcp_server_();
    web_.sendHeader("Location", "/"); web_.send(303, "text/plain", "Configuration enregistrée");
  }
};

}  // namespace modbus_gateway
}  // namespace esphome
