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

  void setup() override {
    load_settings_();
    setup_gpio_();
    begin_uart_();
    web_.on("/", HTTP_GET, [this]() { web_.send(200, "text/html; charset=utf-8", this->page_()); });
    // Serve embedded images independently so the main HTML remains small and reliable.
    web_.on("/img/logo.webp", HTTP_GET, [this]() { this->serve_image_(LOGO_IMG, "image/webp"); });
    web_.on("/img/modbus-tcp.jpg", HTTP_GET, [this]() { this->serve_image_(TCP_IMG, "image/jpeg"); });
    web_.on("/img/modbus-rtu.jpg", HTTP_GET, [this]() { this->serve_image_(RTU_IMG, "image/jpeg"); });
    web_.on("/img/esp32.jpg", HTTP_GET, [this]() { this->serve_image_(ESP32_IMG, "image/jpeg"); });
    web_.on("/img/frame-diagnostics.jpg", HTTP_GET, [this]() { this->serve_image_(FRAME_IMG, "image/jpeg"); });
    web_.on("/save", HTTP_POST, [this]() { this->save_config_(); });
    web_.on("/frames", HTTP_GET, [this]() { web_.send(200, "application/json", this->frames_json_()); });
    web_.on("/frames.txt", HTTP_GET, [this]() {
      web_.sendHeader("Content-Disposition", "attachment; filename=\"modbus-frame-capture.txt\"");
      web_.send(200, "text/plain; charset=utf-8", this->frames_text_());
    });
    web_.on("/clear", HTTP_POST, [this]() { log_head_ = 0; log_count_ = 0; web_.send(200, "application/json", "{\"cleared\":true}"); });
    web_.on("/listen", HTTP_GET, [this]() { web_.send(200, "application/json", String("{\"listening\":") + (rtu_listening_ ? "true}" : "false}")); });
    web_.on("/listen", HTTP_POST, [this]() { rtu_listening_ = !rtu_listening_; rx_.clear(); while (Serial1.available()) Serial1.read(); ESP_LOGI("modbus_gateway", "RTU listener %s", rtu_listening_ ? "enabled" : "paused"); web_.send(200, "application/json", String("{\"listening\":") + (rtu_listening_ ? "true}" : "false}")); });
    web_.on("/manual", HTTP_POST, [this]() { this->manual_request_(); });
    web_.onNotFound([this]() { web_.send(404, "text/plain", "Not found"); });
    web_.begin();
    start_tcp_server_();
    ESP_LOGI("modbus_gateway", "Web UI started on port 80");
  }

  void loop() override {
    web_.handleClient();
    if (tcp_mode_ == "SERVER") handle_tcp_server_();
    if (rtu_listening_ && tcp_mode_ == "CLIENT") {
      while (Serial1.available()) {
        uint8_t b = uint8_t(Serial1.read());
        rx_.push_back(b);
        last_rx_ms_ = millis();
      }
      parse_rtu_();
    } else {
      // Keep the UART buffer drained while the RTU listener is paused.
      while (Serial1.available()) Serial1.read();
      rx_.clear();
    }
  }

  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

 protected:
  std::string cfg_host_{"192.168.1.100"};
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
  bool manual_rtu_id_{false};
  uint8_t configured_rtu_id_{1};
  ::Preferences prefs_;
  WiFiClient client_;
  // The ESP32 Arduino NetworkServer supports a configurable client limit.
  static constexpr size_t MAX_TCP_CLIENTS = 4;
  WiFiServer tcp_server_{502, MAX_TCP_CLIENTS};

  struct TcpClientSlot {
    WiFiClient client;
    uint8_t mbap[7] = {};
    size_t mbap_pos{0};
    std::vector<uint8_t> pdu;
    size_t pdu_pos{0};
    uint16_t expected_pdu{0};
    uint32_t last_activity{0};
    bool request_ready{false};
  };
  TcpClientSlot tcp_clients_[MAX_TCP_CLIENTS];
  uint8_t tcp_client_cursor_{0};

  std::vector<uint8_t> rx_;
  uint16_t transaction_id_{1};
  uint32_t last_rx_ms_{0};

  struct FrameLog { String direction; String hex; uint32_t when; };
  FrameLog logs_[32];
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
    log_head_ = (log_head_ + 1) % 32;
    if (log_count_ < 32) log_count_++;
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
    while (!rx_.empty()) {
      // The RTU slave address is forwarded dynamically as the TCP Unit ID.
      if (rx_[0] == 0 || rx_[0] > 247) {
        rx_.erase(rx_.begin());
        continue;
      }
      if (rx_.size() < 2) return;
      size_t n = request_length_(rx_[1], rx_);
      if (n == 0) {
        if (millis() - last_rx_ms_ > 30) rx_.clear();
        return;
      }
      if (rx_.size() < n) return;
      std::vector<uint8_t> frame(rx_.begin(), rx_.begin()+n);
      rx_.erase(rx_.begin(), rx_.begin()+n);
      add_log_("RTU RX", frame.data(), frame.size());
      if (!valid_crc_(frame.data(), frame.size())) {
        ESP_LOGW("modbus_gateway", "Invalid RTU CRC");
        continue;
      }
      std::vector<uint8_t> pdu(frame.begin()+1, frame.end()-2);
      std::vector<uint8_t> response;
      const uint8_t unit_id = frame[0];
      if (transact_tcp_(unit_id, pdu, response)) {
        send_rtu_(unit_id, response);
      } else {
        std::vector<uint8_t> exception = {uint8_t(pdu[0] | 0x80), 0x0A}; // gateway path unavailable
        send_rtu_(unit_id, exception);
      }
    }
  }

  bool read_exact_(uint8_t *dst, size_t len, uint32_t timeout) {
    size_t pos=0;
    uint32_t start=millis();
    while (pos<len && millis()-start<timeout) {
      while (client_.available() && pos<len) {
        int c=client_.read();
        if (c>=0) dst[pos++]=uint8_t(c);
      }
      if (!client_.connected() && !client_.available()) break;
      delay(1);
    }
    return pos==len;
  }

  bool transact_tcp_(uint8_t unit_id, const std::vector<uint8_t> &pdu, std::vector<uint8_t> &response) {
    if (WiFi.status()!=WL_CONNECTED) return false;
    client_.stop();
    client_.setTimeout(tcp_timeout_ms_);
    if (!client_.connect(cfg_host_.c_str(), cfg_port_)) return false;

    uint16_t tid=transaction_id_++;
    uint16_t len=uint16_t(1+pdu.size());
    std::vector<uint8_t> req={
      uint8_t(tid>>8),uint8_t(tid),0,0,uint8_t(len>>8),uint8_t(len),unit_id
    };
    req.insert(req.end(),pdu.begin(),pdu.end());
    add_log_("TCP TX", req.data(), req.size());
    if (client_.write(req.data(),req.size())!=req.size()) {
      client_.stop(); return false;
    }

    uint8_t mbap[7];
    if (!read_exact_(mbap,7,tcp_timeout_ms_)) {
      client_.stop(); return false;
    }
    uint16_t rtid=(uint16_t(mbap[0])<<8)|mbap[1];
    uint16_t proto=(uint16_t(mbap[2])<<8)|mbap[3];
    uint16_t rlen=(uint16_t(mbap[4])<<8)|mbap[5];
    if (rtid!=tid || proto!=0 || rlen<2 || rlen>254 || mbap[6]!=unit_id) {
      client_.stop(); return false;
    }
    std::vector<uint8_t> rest(rlen-1);
    if (!read_exact_(rest.data(),rest.size(),tcp_timeout_ms_)) {
      client_.stop(); return false;
    }
    client_.stop();
    add_log_("TCP RX", rest.data(), rest.size());
    if (rest.empty()) return false;
    if (rest[0]!=pdu[0] && rest[0]!=uint8_t(pdu[0]|0x80)) return false;
    response=rest;
    return true;
  }

  void send_rtu_(uint8_t unit_id, const std::vector<uint8_t> &pdu) {
    if (pdu.empty()) return;
    std::vector<uint8_t> frame;
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
  }

  void clear_tcp_request_(TcpClientSlot &slot) {
    memset(slot.mbap, 0, sizeof(slot.mbap));
    slot.mbap_pos = 0;
    slot.pdu.clear();
    slot.pdu_pos = 0;
    slot.expected_pdu = 0;
    slot.last_activity = 0;
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
        if (slot.pdu_pos == slot.expected_pdu) slot.request_ready = true;
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

  size_t expected_rtu_response_(uint8_t fc, const uint8_t *pdu, size_t pdu_len, uint8_t *buf, size_t cap) {
    uint32_t start = millis(); size_t n = 0; size_t expected = 0;
    while (millis() - start < tcp_timeout_ms_ && n < cap) {
      while (Serial1.available() && n < cap) {
        buf[n++] = uint8_t(Serial1.read());
        if (n >= 2) {
          if (buf[1] & 0x80) expected = 5;
          else if (fc >= 1 && fc <= 4) expected = (n >= 3) ? size_t(5 + buf[2]) : 0;
          else if (fc == 5 || fc == 6 || fc == 15 || fc == 16) expected = 8;
          else if (fc == 23) expected = (n >= 3) ? size_t(5 + buf[2]) : 0;
        }
        if (expected && n >= expected) return n;
      }
      delay(1);
    }
    return n;
  }

  void process_tcp_request_(TcpClientSlot &slot, size_t client_index) {
    if (!slot.request_ready || !slot.client || !slot.client.connected()) {
      reset_tcp_client_(slot);
      return;
    }

    const uint16_t tid = (uint16_t(slot.mbap[0]) << 8) | slot.mbap[1];
    const uint8_t tcp_unit = slot.mbap[6];
    const uint8_t unit = manual_rtu_id_ ? configured_rtu_id_ : tcp_unit;
    const std::vector<uint8_t> &pdu = slot.pdu;

    add_log_("TCP RX", slot.mbap, sizeof(slot.mbap));
    add_log_("TCP RX PDU", pdu.data(), pdu.size());

    std::vector<uint8_t> rtu = {unit};
    rtu.insert(rtu.end(), pdu.begin(), pdu.end());
    const uint16_t crc = crc16_(rtu.data(), rtu.size());
    rtu.push_back(uint8_t(crc));
    rtu.push_back(uint8_t(crc >> 8));

    if (de_re_gpio_ >= 0) digitalWrite(de_re_gpio_, HIGH);
    delayMicroseconds(100);
    Serial1.write(rtu.data(), rtu.size());
    Serial1.flush();
    delayMicroseconds(100);
    if (de_re_gpio_ >= 0) digitalWrite(de_re_gpio_, LOW);

    uint8_t response[260];
    const size_t rlen = expected_rtu_response_(pdu[0], pdu.data(), pdu.size(), response, sizeof(response));
    if (rlen >= 5 && valid_crc_(response, rlen)) {
      add_log_("RTU RX", response, rlen);
      std::vector<uint8_t> out = {
        uint8_t(tid >> 8), uint8_t(tid), 0, 0,
        uint8_t((rlen - 2) >> 8), uint8_t(rlen - 2), tcp_unit
      };
      out.insert(out.end(), response + 1, response + rlen - 2);
      add_log_("TCP TX", out.data(), out.size());
      if (slot.client.write(out.data(), out.size()) != out.size()) {
        ESP_LOGW("modbus_gateway", "Failed to send response to TCP client %u", unsigned(client_index + 1));
        reset_tcp_client_(slot);
        return;
      }
    } else {
      const uint8_t exception_pdu[] = {uint8_t(pdu[0] | 0x80), 0x0B};
      const uint8_t out[] = {
        uint8_t(tid >> 8), uint8_t(tid), 0, 0, 0, 3,
        tcp_unit, exception_pdu[0], exception_pdu[1]
      };
      add_log_("TCP TX", out, sizeof(out));
      if (slot.client.write(out, sizeof(out)) != sizeof(out)) {
        ESP_LOGW("modbus_gateway", "Failed to send exception to TCP client %u", unsigned(client_index + 1));
        reset_tcp_client_(slot);
        return;
      }
    }

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
    s.reserve(16000);
    s += R"HTML(<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Modbus Gateway</title>
<style>
*{box-sizing:border-box}html{background:#eef3f8}body{font:15px/1.5 Arial,Helvetica,sans-serif;background:#eef3f8;color:#17202a;margin:0;padding:24px 16px}main{max-width:920px;margin:0 auto}.brand{display:flex;flex-direction:column;align-items:center;text-align:center;gap:8px;margin:0 auto 22px;padding:18px;background:#fff;border-radius:16px;box-shadow:0 3px 14px #0f172a12}.brand-logo{display:block;width:100%;max-width:920px;height:auto;max-height:none;object-fit:contain;margin:0 auto}.brand-title{font-size:25px;font-weight:700;letter-spacing:.2px;color:#12345a}.brand-sub{font-size:13px;color:#64748b;margin-top:2px}section{display:block;background:#fff;border:1px solid #e1e8f0;border-radius:14px;padding:20px;margin:16px 0;box-shadow:0 3px 12px rgba(15,23,42,.05)}h2{display:flex;align-items:center;gap:10px;font-size:19px;line-height:1.25;margin:0 0 18px;color:#12345a}.section-icon{display:block;width:54px;height:42px;flex:0 0 54px;object-fit:contain;vertical-align:middle;margin:0;border-radius:6px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:14px 16px}label{display:block;font-size:13px;font-weight:600;color:#475569;margin:0 0 6px}input,select{display:block;width:100%;min-height:40px;box-sizing:border-box;padding:9px 11px;border:1px solid #cbd5e1;border-radius:8px;background:#fff;color:#17202a;font-size:15px}input:focus,select:focus{outline:2px solid #93c5fd;border-color:#2563eb}button{background:#1769aa;color:#fff;border:0;border-radius:8px;padding:11px 18px;font-size:14px;font-weight:600;cursor:pointer}button:hover{background:#125487}.muted{color:#64748b;font-size:13px}pre{white-space:pre-wrap;word-break:break-word;background:#101827;color:#dbeafe;padding:14px;border-radius:9px;max-height:360px;overflow:auto}.crc-byte{display:inline-block;border:2px solid #fbbf24;border-radius:50%;padding:1px 4px;margin:0 1px;color:#fde68a;font-weight:bold}.crc-ok{color:#86efac;font-weight:bold}.crc-bad{color:#fca5a5;font-weight:bold}#langBtn{float:right;margin:0 0 10px 10px}.apply-panel{display:flex;align-items:center;justify-content:space-between;gap:18px;background:#fff;border:1px solid #cbdbea;border-radius:14px;padding:16px 20px;margin:16px 0;box-shadow:0 3px 12px rgba(15,23,42,.05)}.apply-panel strong{display:block;color:#12345a;font-size:15px}.apply-panel .muted{margin-top:3px}.button-link{display:inline-block;background:#1769aa;color:#fff;border:0;border-radius:8px;padding:11px 18px;font-size:14px;font-weight:600;text-decoration:none;white-space:nowrap}.button-link:hover{background:#125487}@media(max-width:560px){.apply-panel{align-items:stretch;flex-direction:column;gap:10px}.apply-panel button,.apply-panel .button-link{width:100%;text-align:center}}#cfg{margin:0}@media(max-width:560px){body{padding:12px 9px}section{padding:14px}.brand{padding:12px}.brand-logo{width:100%;max-width:100%;max-height:none}.brand-title{font-size:20px}h2{font-size:17px}.section-icon{width:46px;height:36px;flex-basis:46px}}
</style></head><body><main><button type="button" id="langBtn" onclick="toggleLanguage()" style="float:right">English</button><header class="brand">
<img class="brand-logo" src="/img/logo.webp" alt="Logo passerelle Modbus"><div><div class="brand-title">Passerelle Modbus RTU ↔ TCP</div><div class="brand-sub" data-fr="Passerelle série ↔ réseau" data-en="Serial ↔ network gateway">Passerelle série ↔ réseau</div></div></header>
<section><h2><img class="section-icon" src="/img/modbus-tcp.jpg" alt="">1. Modbus TCP</h2><div class="grid"><div><label>Mode</label><select name="mode" form="cfg">)HTML";
    s += "<option value=\"CLIENT\"" + String(tcp_mode_ == "CLIENT" ? " selected" : "") + ">Client TCP (RTU vers serveur)</option>";
    s += "<option value=\"SERVER\"" + String(tcp_mode_ == "SERVER" ? " selected" : "") + ">Serveur TCP (client TCP vers RTU)</option>";
    s += R"HTML(</select></div>
<div id="host_field"><label>Adresse IP du serveur Modbus TCP</label><input id="host_input" name="host" form="cfg" value=")HTML";
    s += cfg_host_.c_str();
    s += R"HTML("></div><div><label>Port Modbus TCP</label><input name="port" form="cfg" type="number" min="1" max="65535" value=")HTML";
    s += String(cfg_port_);
    s += R"HTML("></div><div><label>Timeout TCP (secondes)</label><input name="timeout" form="cfg" type="number" min="1" max="30" value=")HTML";
    s += String(tcp_timeout_ms_ / 1000);
    s += R"HTML("></div></div></section>
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
<form id="cfg" method="POST" action="/save"><div class="apply-panel"><div><strong>Enregistrement global</strong><div class="muted">Les paramètres des sections 1, 2 et 3 seront enregistrés ensemble.</div></div><button type="submit">Enregistrer / appliquer</button></div></form>
<section><h2><img class="section-icon" src="/img/frame-diagnostics.jpg" alt="">4. Debug des trames</h2>
<p class="muted">RTU RX/TX et TCP TX/RX · rafraîchissement automatique toutes les 1,5 s</p>
<p><button id="listenBtn" type="button" onclick="toggleListen()">Arrêter l’écoute RTU</button> <span id="listenState">Écoute RTU active</span></p>
<div style="margin-top:14px"><label for="rtuFrame">Trame RTU à envoyer (hexadécimal, avec adresse et CRC, ex. 0B 03 00 00 00 01 84 A0)</label>
<input id="rtuFrame" placeholder="0B 03 00 00 00 01 ... CRC" autocomplete="off">
<button type="button" onclick="sendManual()" style="margin-top:10px">Envoyer au serveur TCP</button></div>
<p class="muted">Réponse du serveur Modbus TCP :</p><pre id="manualResponse">Aucune requête manuelle</pre>
<p style="margin-bottom:6px"><button type="button" onclick="clearFrames()">Vider l’affichage des trames</button> <a class="button-link" href="/frames.txt">Exporter la capture des trames (.txt)</a></p>
<p class="muted">L’export contient les 32 dernières trames mémorisées.</p>
<pre id="frames">Chargement des trames…</pre></section>
<script>
let currentLanguage='fr';
const translations={
'Mode':'Mode','Client TCP (RTU vers serveur)':'TCP client (RTU to server)','Serveur TCP (client TCP vers RTU)':'TCP server (TCP client to RTU)',
'Adresse IP du serveur Modbus TCP':'Modbus TCP server IP address','Port Modbus TCP':'Modbus TCP port','Timeout TCP (secondes)':'TCP timeout (seconds)',
'Baudrate':'Baud rate','Data bits':'Data bits','Parity':'Parity','Stop bits':'Stop bits',
'GPIO TX (-1 = automatique)':'TX GPIO (-1 = automatic)','GPIO RX (-1 = automatique)':'RX GPIO (-1 = automatic)','GPIO DE/RE/EN (-1 = automatique)':'DE/RE/EN GPIO (-1 = automatic)',
'Identifiant Modbus RTU':'Modbus RTU device ID','Automatique (adresse reçue)':'Automatic (received address)','Manuel':'Manual','ID RTU manuel (1–247)':'Manual RTU ID (1–247)',
'Enregistrer / appliquer':'Save / apply','Enregistrement global':'Apply all settings','Les paramètres des sections 1, 2 et 3 seront enregistrés ensemble.':'Settings in sections 1, 2 and 3 will be saved together.','-1 = sélection automatique de la broche. Les paramètres sont enregistrés en mémoire. Toute modification des GPIO redémarre l’ESP32.':'-1 = automatic pin selection. Settings are stored in memory. Any GPIO change restarts the ESP32.',
'Paramètres Modbus RTU':'Modbus RTU settings','3. Configuration':'3. Configuration','4. Debug des trames':'4. Frame diagnostics',
'RTU RX/TX et TCP TX/RX · rafraîchissement automatique toutes les 1,5 s':'RTU RX/TX and TCP TX/RX · automatic refresh every 1.5 s',
'Arrêter l’écoute RTU':'Stop RTU listening','Reprendre l’écoute RTU':'Resume RTU listening','Écoute RTU active':'RTU listening','Écoute RTU suspendue':'RTU listening paused','Erreur de commande':'Command error',
'Trame RTU à envoyer (hexadécimal, avec adresse et CRC, ex. 0B 03 00 00 00 01 84 A0)':'RTU frame to send (hexadecimal, including address and CRC, e.g. 0B 03 00 00 00 01 84 A0)',
'Envoyer au serveur TCP':'Send to TCP server','Réponse du serveur Modbus TCP :':'Modbus TCP server response:','Aucune requête manuelle':'No manual request','Vider l’affichage des trames':'Clear frame log','Exporter la capture des trames (.txt)':'Export frame capture (.txt)','L’export contient les 32 dernières trames mémorisées.':'The export contains the 32 most recent stored frames.','Chargement des trames…':'Loading frames…','Aucune trame':'No frames',
'CRC Erreur':'CRC Error','Erreur':'Error','CRC OK':'CRC OK','Envoi en cours…':'Sending…','Erreur de communication avec la passerelle':'Gateway communication error',
'Erreur : saisir une trame RTU complète en hexadécimal (adresse, fonction, données, CRC).':'Error: enter a complete RTU frame in hexadecimal (address, function, data, CRC).',
'Erreur : caractère hexadécimal invalide.':'Error: invalid hexadecimal character.',
'Erreur : CRC RTU invalide. La trame doit inclure un CRC valide.':'Error: invalid RTU CRC. The frame must include a valid CRC.',
'Erreur : adresse RTU invalide ou trame trop courte.':'Error: invalid RTU address or frame too short.',
'Échec de la transaction Modbus TCP : vérifier Wi-Fi, IP/port et réponse du serveur.':'Modbus TCP transaction failed: check Wi-Fi, IP/port and server response.',
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
function toggleLanguage(){currentLanguage=currentLanguage==='fr'?'en':'fr';applyLanguage();}
function toggleRtuId(){const isServer=document.querySelector('select[name=mode]').value==='SERVER';document.getElementById('rtu_id_mode_field').style.display=isServer?'block':'none';document.getElementById('rtu_id_manual_field').style.display=(isServer&&document.querySelector('select[name=idmode]').value==='manual')?'block':'none';}document.querySelector('select[name=idmode]').addEventListener('change',toggleRtuId);document.querySelector('select[name=mode]').addEventListener('change',toggleRtuId);toggleRtuId();
function crc16Modbus(bytes){let crc=0xFFFF;for(const b of bytes){crc^=b;for(let i=0;i<8;i++)crc=(crc&1)?((crc>>>1)^0xA001):(crc>>>1);}return crc&0xFFFF;}
function toggleHostField(){const isServer=document.querySelector('select[name="mode"]').value==='SERVER';document.getElementById('host_field').style.display=isServer?'none':'block';document.getElementById('host_input').required=!isServer;}
document.querySelector('select[name="mode"]').addEventListener('change',toggleHostField);toggleHostField();
function formatFrame(x){const prefix=x.time+' | '+x.dir+' | ';const isRtu=x.dir.startsWith('RTU')||x.dir==='MANUAL RTU';const bytes=x.hex.trim().split(/\s+/).filter(Boolean);if(!isRtu||bytes.length<3)return prefix+x.hex;const vals=bytes.map(b=>parseInt(b,16));if(vals.some(v=>!Number.isFinite(v)))return prefix+x.hex;const calculated=crc16Modbus(vals.slice(0,-2));const crcOk=vals[vals.length-2]===(calculated&255)&&vals[vals.length-1]===((calculated>>8)&255);const payload=bytes.slice(0,-2).join(' ');const crc='<span class=\"crc-byte\">'+bytes[bytes.length-2]+'</span> <span class=\"crc-byte\">'+bytes[bytes.length-1]+'</span>';return prefix+payload+' '+crc+'  <span class=\"'+(crcOk?'crc-ok':'crc-bad')+'\">CRC '+(crcOk?'OK':tr('Erreur','Error'))+'</span>';}
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
    String out="{\"frames\":[";
    for (size_t i=0;i<log_count_;i++) {
      size_t idx=(log_head_+32-log_count_+i)%32;
      if (i) out+=",";
      out+="{\"time\":\""+String(logs_[idx].when)+" ms\",\"dir\":\""+logs_[idx].direction+"\",\"hex\":\""+logs_[idx].hex+"\"}";
    }
    out+="]}";
    return out;
  }

  String frames_text_() {
    String out;
    out.reserve(4096);
    out += "Modbus RTU <-> TCP - Frame capture\r\n";
    out += "Timestamps are milliseconds since ESP32 boot.\r\n";
    out += "Exported frames: ";
    out += String(log_count_);
    out += " (maximum retained: 32)\r\n\r\n";
    if (log_count_ == 0) {
      out += "No frames recorded.\r\n";
      return out;
    }
    for (size_t i=0; i<log_count_; i++) {
      const size_t idx = (log_head_ + 32 - log_count_ + i) % 32;
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

  void save_config_() {
    int new_tx = web_.hasArg("tx") ? web_.arg("tx").toInt() : tx_gpio_;
    int new_rx = web_.hasArg("rx") ? web_.arg("rx").toInt() : rx_gpio_;
    int new_de = web_.hasArg("de") ? web_.arg("de").toInt() : de_re_gpio_;
    bool gpio_changed = new_tx != tx_gpio_ || new_rx != rx_gpio_ || new_de != de_re_gpio_;
    if (web_.hasArg("mode")) tcp_mode_ = web_.arg("mode").c_str();
    if (web_.hasArg("host")) cfg_host_ = web_.arg("host").substring(0,63).c_str();
    if (web_.hasArg("port")) { int v=web_.arg("port").toInt(); if(v>=1 && v<=65535) cfg_port_=uint16_t(v); }
    if (web_.hasArg("timeout")) { int v=web_.arg("timeout").toInt(); if(v>=1 && v<=30) tcp_timeout_ms_=uint32_t(v)*1000; }
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
    prefs_.putUInt("timeout", tcp_timeout_ms_); prefs_.putInt("tx", tx_gpio_);
    prefs_.putInt("rx", rx_gpio_); prefs_.putInt("de", de_re_gpio_);
    prefs_.putBool("idmanual", manual_rtu_id_); prefs_.putUChar("rtuid", configured_rtu_id_);
    prefs_.putUInt("baud", cfg_baud_); prefs_.putUChar("dbits", cfg_data_bits_);
    prefs_.putString("parity", cfg_parity_.c_str()); prefs_.putUChar("sbits", cfg_stop_bits_);
    if (gpio_changed) {
      web_.send(200, "text/html; charset=utf-8", "<meta charset='utf-8'>GPIO enregistrés. Redémarrage de l’ESP32…");
      delay(500); ESP.restart(); return;
    }
    begin_uart_(); start_tcp_server_();
    web_.sendHeader("Location", "/"); web_.send(303, "text/plain", "Configuration enregistrée");
  }
};

}  // namespace modbus_gateway
}  // namespace esphome
