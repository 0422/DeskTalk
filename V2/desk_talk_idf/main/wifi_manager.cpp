// 2026-09-19: Port Arduino WiFi, Preferences, WebServer, and WiFiUDP behavior to native ESP-IDF services.
#include "wifi_manager.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

#include "app_config.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "storage.h"

namespace desk_talk::network {
namespace {

constexpr char kLogTag[] = "wifi";
constexpr char kProvisioningSsid[] = "Desk-Emoji";
constexpr uint16_t kUdpPort = 4210;
constexpr EventBits_t kConnectedBit = BIT0;
constexpr EventBits_t kCredentialsSavedBit = BIT1;
constexpr uint32_t kConnectTimeoutMs = 20000;

// 2026-09-19: Keep the embedded provisioning page self-contained so first boot needs no external assets.
constexpr char kProvisioningPage[] = R"html(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Desk-Emoji Wi-Fi</title><style>
body{font-family:Arial,sans-serif;margin:24px;max-width:680px;color:#202124}h1{font-size:24px}button,input{font:inherit;padding:10px;margin:4px 0}input{display:block;width:100%;box-sizing:border-box}li{padding:10px 0;border-bottom:1px solid #ddd;cursor:pointer}.status{margin:12px 0;color:#176b3a}
</style></head><body><h1>Desk-Emoji Wi-Fi</h1><button onclick="scan()">Scan networks</button><p class="status" id="status"></p><ul id="networks"></ul>
<form onsubmit="save(event)"><label>SSID<input id="ssid" required></label><label>Password<input id="password" type="password"></label><button type="submit">Connect</button></form>
<script>async function scan(){status.textContent='Scanning...';let r=await fetch('/scan-wifi');let a=await r.json();networks.innerHTML='';a.forEach(n=>{let li=document.createElement('li');li.textContent=n.ssid+' ('+n.rssi+' dBm)';li.onclick=()=>ssid.value=n.ssid;networks.appendChild(li)});status.textContent=a.length?'Select a network':'No networks found'}async function save(e){e.preventDefault();status.textContent='Saving...';let b='ssid='+encodeURIComponent(ssid.value)+'&password='+encodeURIComponent(password.value);let r=await fetch('/save-wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});let j=await r.json();status.textContent=j.success?'Saved. Device is connecting.':j.message}</script></body></html>)html";

WifiManager manager;

std::string url_decode(const std::string &input) {
  std::string output;
  output.reserve(input.size());
  for (size_t index = 0; index < input.size(); ++index) {
    if (input[index] == '+') {
      output.push_back(' ');
    } else if (input[index] == '%' && index + 2 < input.size() &&
               std::isxdigit(static_cast<unsigned char>(input[index + 1])) &&
               std::isxdigit(static_cast<unsigned char>(input[index + 2]))) {
      unsigned value = 0;
      std::sscanf(input.substr(index + 1, 2).c_str(), "%x", &value);
      output.push_back(static_cast<char>(value));
      index += 2;
    } else {
      output.push_back(input[index]);
    }
  }
  return output;
}

std::string form_value(const std::string &body, const char *name) {
  const std::string prefix = std::string(name) + '=';
  size_t start = 0;
  while (start <= body.size()) {
    const size_t end = body.find('&', start);
    const std::string field = body.substr(start, end - start);
    if (field.rfind(prefix, 0) == 0) {
      return url_decode(field.substr(prefix.size()));
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return {};
}

}  // namespace

WifiManager &wifi_manager() { return manager; }

esp_err_t WifiManager::initialize() {
  events_ = xEventGroupCreate();
  if (events_ == nullptr) {
    return ESP_ERR_NO_MEM;
  }

  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_init());
  const esp_err_t event_loop_result = esp_event_loop_create_default();
  if (event_loop_result != ESP_OK &&
      event_loop_result != ESP_ERR_INVALID_STATE) {
    return event_loop_result;
  }
  esp_netif_create_default_wifi_sta();
  esp_netif_create_default_wifi_ap();

  wifi_init_config_t initialization = WIFI_INIT_CONFIG_DEFAULT();
  esp_err_t result = esp_wifi_init(&initialization);
  if (result != ESP_OK) {
    return result;
  }
  // 2026-09-19: Register process-lifetime handlers without unused instance handles.
  ESP_RETURN_ON_ERROR(esp_event_handler_register(
                          WIFI_EVENT, ESP_EVENT_ANY_ID,
                          &WifiManager::event_handler, this),
                      kLogTag, "Unable to register Wi-Fi event handler");
  ESP_RETURN_ON_ERROR(esp_event_handler_register(
                          IP_EVENT, IP_EVENT_STA_GOT_IP,
                          &WifiManager::event_handler, this),
                      kLogTag, "Unable to register IP event handler");
  ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), kLogTag,
                      "Unable to select RAM Wi-Fi config storage");
  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), kLogTag,
                      "Unable to select station mode");
  ESP_RETURN_ON_ERROR(esp_wifi_start(), kLogTag, "Unable to start Wi-Fi");

  if (!storage::load_wifi_credentials(ssid_, password_)) {
    ssid_ = config::kDefaultWifiSsid;
    password_ = config::kDefaultWifiPassword;
  }

  while (ssid_.empty() || connect_station(kConnectTimeoutMs) != ESP_OK) {
    ESP_LOGW(kLogTag,
             "Station connection unavailable; provisioning at http://192.168.4.1");
    ESP_RETURN_ON_ERROR(start_provisioning(), kLogTag,
                        "Unable to start provisioning AP");
    xEventGroupWaitBits(events_, kCredentialsSavedBit, pdTRUE, pdTRUE,
                        portMAX_DELAY);
    stop_provisioning();
  }

  reconnect_enabled_ = true;
  update_network_identity();
  udp_socket_ = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (udp_socket_ >= 0) {
    int enabled = 1;
    lwip_setsockopt(udp_socket_, SOL_SOCKET, SO_BROADCAST, &enabled,
                    sizeof(enabled));
  }
  ESP_LOGI(kLogTag, "Connected: ip=%s mac=%s", local_ip_.c_str(),
           mac_address_.c_str());
  return ESP_OK;
}

bool WifiManager::connected() const {
  return events_ != nullptr &&
         (xEventGroupGetBits(events_) & kConnectedBit) != 0;
}

esp_err_t WifiManager::connect_station(uint32_t timeout_ms) {
  wifi_config_t station = {};
  if (ssid_.empty() || ssid_.size() >= sizeof(station.sta.ssid) ||
      password_.size() >= sizeof(station.sta.password)) {
    return ESP_ERR_INVALID_ARG;
  }
  std::memcpy(station.sta.ssid, ssid_.data(), ssid_.size());
  std::memcpy(station.sta.password, password_.data(), password_.size());
  station.sta.threshold.authmode = password_.empty() ? WIFI_AUTH_OPEN
                                                     : WIFI_AUTH_WPA2_PSK;
  station.sta.pmf_cfg.capable = true;
  station.sta.pmf_cfg.required = false;

  xEventGroupClearBits(events_, kConnectedBit);
  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), kLogTag,
                      "Unable to set station mode");
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &station), kLogTag,
                      "Unable to configure station");
  ESP_LOGI(kLogTag, "Connecting to SSID: %s", ssid_.c_str());
  ESP_RETURN_ON_ERROR(esp_wifi_connect(), kLogTag,
                      "Unable to start station connection");
  const EventBits_t bits = xEventGroupWaitBits(
      events_, kConnectedBit, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
  return (bits & kConnectedBit) != 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

void WifiManager::event_handler(void *argument, esp_event_base_t event_base,
                                int32_t event_id, void *event_data) {
  auto *self = static_cast<WifiManager *>(argument);
  if (self == nullptr || self->events_ == nullptr) {
    return;
  }
  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    xEventGroupSetBits(self->events_, kConnectedBit);
    self->update_network_identity();
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    xEventGroupClearBits(self->events_, kConnectedBit);
    if (self->reconnect_enabled_) {
      esp_wifi_connect();
    }
  }
}

esp_err_t WifiManager::start_provisioning() {
  reconnect_enabled_ = false;
  wifi_config_t access_point = {};
  std::strncpy(reinterpret_cast<char *>(access_point.ap.ssid),
               kProvisioningSsid, sizeof(access_point.ap.ssid) - 1);
  access_point.ap.ssid_len = std::strlen(kProvisioningSsid);
  access_point.ap.max_connection = 4;
  access_point.ap.authmode = WIFI_AUTH_OPEN;
  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), kLogTag,
                      "Unable to select AP+STA mode");
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &access_point), kLogTag,
                      "Unable to configure provisioning AP");

  httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
  ESP_RETURN_ON_ERROR(httpd_start(&server_, &server_config), kLogTag,
                      "Unable to start provisioning server");
  const httpd_uri_t root = {
      .uri = "/", .method = HTTP_GET, .handler = root_handler,
      .user_ctx = this};
  const httpd_uri_t scan = {
      .uri = "/scan-wifi", .method = HTTP_GET, .handler = scan_handler,
      .user_ctx = this};
  const httpd_uri_t save = {
      .uri = "/save-wifi", .method = HTTP_POST, .handler = save_handler,
      .user_ctx = this};
  httpd_register_uri_handler(server_, &root);
  httpd_register_uri_handler(server_, &scan);
  httpd_register_uri_handler(server_, &save);
  return ESP_OK;
}

void WifiManager::stop_provisioning() {
  if (server_ != nullptr) {
    httpd_stop(server_);
    server_ = nullptr;
  }
  esp_wifi_set_mode(WIFI_MODE_STA);
}

esp_err_t WifiManager::root_handler(httpd_req_t *request) {
  httpd_resp_set_type(request, "text/html");
  return httpd_resp_send(request, kProvisioningPage, HTTPD_RESP_USE_STRLEN);
}

esp_err_t WifiManager::scan_handler(httpd_req_t *request) {
  wifi_scan_config_t scan_config = {};
  scan_config.show_hidden = false;
  esp_err_t result = esp_wifi_scan_start(&scan_config, true);
  if (result != ESP_OK) {
    httpd_resp_set_status(request, "503 Service Unavailable");
    return httpd_resp_sendstr(request, "[]");
  }
  uint16_t count = 0;
  esp_wifi_scan_get_ap_num(&count);
  count = std::min<uint16_t>(count, 32);
  std::vector<wifi_ap_record_t> records(count);
  if (count > 0) {
    esp_wifi_scan_get_ap_records(&count, records.data());
  }

  cJSON *array = cJSON_CreateArray();
  for (uint16_t index = 0; index < count; ++index) {
    cJSON *network = cJSON_CreateObject();
    cJSON_AddStringToObject(
        network, "ssid",
        reinterpret_cast<const char *>(records[index].ssid));
    cJSON_AddNumberToObject(network, "rssi", records[index].rssi);
    cJSON_AddItemToArray(array, network);
  }
  char *json = cJSON_PrintUnformatted(array);
  httpd_resp_set_type(request, "application/json");
  result = httpd_resp_sendstr(request, json == nullptr ? "[]" : json);
  cJSON_free(json);
  cJSON_Delete(array);
  return result;
}

esp_err_t WifiManager::save_handler(httpd_req_t *request) {
  auto *self = static_cast<WifiManager *>(request->user_ctx);
  if (self == nullptr || request->content_len <= 0 ||
      request->content_len > 512) {
    httpd_resp_set_status(request, "400 Bad Request");
    return httpd_resp_sendstr(request,
                             "{\"success\":false,\"message\":\"Invalid request\"}");
  }
  std::string body(static_cast<size_t>(request->content_len), '\0');
  size_t received = 0;
  while (received < body.size()) {
    const int chunk = httpd_req_recv(request, body.data() + received,
                                     body.size() - received);
    if (chunk <= 0) {
      return ESP_FAIL;
    }
    received += static_cast<size_t>(chunk);
  }
  const std::string ssid = form_value(body, "ssid");
  const std::string password = form_value(body, "password");
  if (ssid.empty() || !storage::save_wifi_credentials(ssid, password)) {
    httpd_resp_set_status(request, "400 Bad Request");
    return httpd_resp_sendstr(request,
                             "{\"success\":false,\"message\":\"Unable to save credentials\"}");
  }
  self->ssid_ = ssid;
  self->password_ = password;
  httpd_resp_set_type(request, "application/json");
  const esp_err_t result =
      httpd_resp_sendstr(request, "{\"success\":true}");
  xEventGroupSetBits(self->events_, kCredentialsSavedBit);
  return result;
}

void WifiManager::update_network_identity() {
  esp_netif_t *station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip_info = {};
  if (station != nullptr && esp_netif_get_ip_info(station, &ip_info) == ESP_OK) {
    char address[IP4ADDR_STRLEN_MAX] = {};
    esp_ip4addr_ntoa(&ip_info.ip, address, sizeof(address));
    local_ip_ = address;
  }
  std::array<uint8_t, 6> mac = {};
  if (esp_wifi_get_mac(WIFI_IF_STA, mac.data()) == ESP_OK) {
    char formatted[18] = {};
    std::snprintf(formatted, sizeof(formatted),
                  "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2],
                  mac[3], mac[4], mac[5]);
    mac_address_ = formatted;
  }
}

void WifiManager::maintain() {
  if (connected() || ssid_.empty()) {
    return;
  }
  const int64_t now = esp_timer_get_time();
  if (now - last_reconnect_us_ < 5000000) {
    return;
  }
  last_reconnect_us_ = now;
  esp_wifi_connect();
}

void WifiManager::send_ip_broadcast() {
  if (!connected() || udp_socket_ < 0 || local_ip_.empty()) {
    return;
  }
  sockaddr_in destination = {};
  destination.sin_family = AF_INET;
  destination.sin_port = htons(kUdpPort);
  destination.sin_addr.s_addr = htonl(INADDR_BROADCAST);
  lwip_sendto(udp_socket_, local_ip_.data(), local_ip_.size(), 0,
              reinterpret_cast<sockaddr *>(&destination),
              sizeof(destination));
}

const std::string &WifiManager::local_ip() const { return local_ip_; }

const std::string &WifiManager::mac_address() const { return mac_address_; }

}  // namespace desk_talk::network
