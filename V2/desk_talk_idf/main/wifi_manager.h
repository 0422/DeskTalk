// 2026-09-19: Define native Wi-Fi station, provisioning AP, reconnect, and UDP discovery ownership.
#pragma once

#include <cstdint>
#include <string>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "freertos/event_groups.h"

namespace desk_talk::network {

class WifiManager {
 public:
  esp_err_t initialize();
  bool connected() const;
  void maintain();
  void send_ip_broadcast();

  const std::string &local_ip() const;
  const std::string &mac_address() const;

 private:
  static void event_handler(void *argument, esp_event_base_t event_base,
                            int32_t event_id, void *event_data);
  static esp_err_t root_handler(httpd_req_t *request);
  static esp_err_t scan_handler(httpd_req_t *request);
  static esp_err_t save_handler(httpd_req_t *request);

  esp_err_t connect_station(uint32_t timeout_ms);
  esp_err_t start_provisioning();
  void stop_provisioning();
  void update_network_identity();

  EventGroupHandle_t events_ = nullptr;
  httpd_handle_t server_ = nullptr;
  std::string ssid_;
  std::string password_;
  std::string local_ip_;
  std::string mac_address_;
  int udp_socket_ = -1;
  int64_t last_reconnect_us_ = 0;
  bool reconnect_enabled_ = false;
};

WifiManager &wifi_manager();

}  // namespace desk_talk::network
