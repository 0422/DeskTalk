#include "net.h"

#include <Preferences.h>

namespace {

constexpr char kWifiNvsNamespace[] = "wifi";
constexpr char kWifiNvsSsid[] = "ssid";
constexpr char kWifiNvsPassword[] = "password";

// Used only when no credentials have been provisioned yet.  Credentials
// saved through the configuration page take precedence on later boots.
constexpr char kDefaultWifiSsid[] = "Longemont";
constexpr char kDefaultWifiPassword[] = "";

const char *wifi_status_name(int status) {
  switch (status) {
    case WL_CONNECTED:
      return "WL_CONNECTED";
    case WL_NO_SSID_AVAIL:
      return "WL_NO_SSID_AVAIL (SSID not found)";
    case WL_CONNECT_FAILED:
      return "WL_CONNECT_FAILED (authentication or association failed)";
    case WL_CONNECTION_LOST:
      return "WL_CONNECTION_LOST";
    case WL_DISCONNECTED:
      return "WL_DISCONNECTED";
    case WL_IDLE_STATUS:
      return "WL_IDLE_STATUS";
    default:
      return "unknown WiFi status";
  }
}

bool load_wifi_from_nvs(String &ssid, String &password) {
  Preferences preferences;
  if (!preferences.begin(kWifiNvsNamespace, true)) {
    return false;
  }
  ssid = preferences.getString(kWifiNvsSsid, "");
  password = preferences.getString(kWifiNvsPassword, "");
  preferences.end();
  ssid.trim();
  password.trim();
  return !ssid.isEmpty();
}

bool save_wifi_to_nvs(const String &ssid, const String &password) {
  Preferences preferences;
  if (!preferences.begin(kWifiNvsNamespace, false)) {
    return false;
  }
  const size_t ssid_written = preferences.putString(kWifiNvsSsid, ssid);
  const size_t password_written =
      preferences.putString(kWifiNvsPassword, password);
  preferences.end();
  // Preferences::putString() returns the string length, so an intentionally
  // empty password for an open network returns zero even when it is valid.
  return ssid_written > 0 && (password.isEmpty() || password_written > 0);
}

void clear_wifi_from_nvs() {
  Preferences preferences;
  if (preferences.begin(kWifiNvsNamespace, false)) {
    preferences.clear();
    preferences.end();
  }
}

String json_escape(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 8);

  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    switch (c) {
      case '"':
        escaped += "\\\"";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      case '\b':
        escaped += "\\b";
        break;
      case '\f':
        escaped += "\\f";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        if (static_cast<uint8_t>(c) < 0x20) {
          char encoded[7];
          snprintf(encoded, sizeof(encoded), "\\u%04x",
                   static_cast<uint8_t>(c));
          escaped += encoded;
        } else {
          escaped += c;
        }
        break;
    }
  }

  return escaped;
}

void begin_wifi_station(const String &ssid, const String &password) {
  if (password.isEmpty()) {
    WiFi.begin(ssid.c_str());
  } else {
    WiFi.begin(ssid.c_str(), password.c_str());
  }
}

bool load_wifi_from_file(String &ssid, String &password) {
  if (!FFat.exists(WIFI_CONF_FILE)) {
    return false;
  }
  File wifi_file = FFat.open(WIFI_CONF_FILE, FILE_READ);
  if (!wifi_file) {
    return false;
  }
  ssid = wifi_file.readStringUntil('\n');
  password = wifi_file.readStringUntil('\n');
  wifi_file.close();
  ssid.trim();
  password.trim();
  return !ssid.isEmpty();
}

}  // namespace

const char WifiClient::index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML>
<html>
<head>
  <title>WiFi 配置</title>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body {
      font-family: Arial, sans-serif;
      margin: 0;
      padding: 20px;
      background-color: #f5f5f5;
      color: #333;
    }
    .container {
      max-width: 800px;
      margin: 0 auto;
      padding: 20px;
      background-color: white;
      border-radius: 8px;
      box-shadow: 0 2px 10px rgba(0, 0, 0, 0.1);
    }
    h1 {
      color: #0066cc;
      text-align: center;
      margin-bottom: 30px;
    }
    h2 {
      color: #009688;
      border-bottom: 1px solid #eee;
      padding-bottom: 10px;
    }
    .info-section {
      margin-bottom: 30px;
    }
    .status {
      background-color: #e8f5e9;
      padding: 15px;
      border-radius: 5px;
      margin: 20px 0;
    }
    .footer {
      text-align: center;
      margin-top: 30px;
      font-size: 0.9em;
      color: #666;
    }
    button {
      background-color: #4CAF50;
      border: none;
      color: white;
      padding: 10px 20px;
      text-align: center;
      text-decoration: none;
      display: inline-block;
      font-size: 16px;
      margin: 10px 2px;
      cursor: pointer;
      border-radius: 4px;
    }
    #networks-list {
      list-style-type: none;
      padding: 0;
    }
    .network-item {
      padding: 12px 15px;
      border-bottom: 1px solid #ddd;
      cursor: pointer;
      transition: background-color 0.3s;
      display: flex;
      justify-content: space-between;
      align-items: center;
    }
    .network-item:hover {
      background-color: #f0f0f0;
    }
    .network-item.selected {
      background-color: #e3f2fd;
    }
    .wifi-strength {
      margin-left: 10px;
      font-size: 0.9em;
      color: #666;
    }
    .password-form {
      margin-top: 20px;
      padding: 15px;
      background-color: #f9f9f9;
      border-radius: 5px;
      display: none;
    }
    input[type="text"], input[type="password"] {
      width: 100%;
      padding: 8px;
      margin: 8px 0;
      box-sizing: border-box;
      border: 1px solid #ddd;
      border-radius: 4px;
    }
    .message {
      padding: 10px;
      margin: 10px 0;
      border-radius: 4px;
    }
    .success {
      background-color: #d4edda;
      color: #155724;
    }
    .error {
      background-color: #f8d7da;
      color: #721c24;
    }
    .spinner {
      border: 4px solid rgba(0, 0, 0, 0.1);
      width: 20px;
      height: 20px;
      border-radius: 50%;
      border-top: 4px solid #007bff;
      animation: spin 1s linear infinite;
      display: inline-block;
      margin-right: 10px;
      vertical-align: middle;
    }
    @keyframes spin {
      0% { transform: rotate(0deg); }
      100% { transform: rotate(360deg); }
    }
    .hidden {
      display: none;
    }
    .scan-btn {
      background-color: #007bff;
      margin-bottom: 20px;
    }
    .loading-text {
      margin-left: 10px;
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>WiFi 配置</h1>
    
    <div class="status">
      <p><strong>状态:</strong> Desk-Emoji 准备配置 Wi-Fi 网络</p>
    </div>
    
    <div class="info-section">
      <h2>可用 Wi-Fi 网络</h2>
      <p>请选择一个网络连接：</p>
      
      <button id="scan-btn" class="scan-btn" onclick="scanNetworks()">
        <span id="scan-spinner" class="spinner hidden"></span>
        <span id="scan-text">扫描网络</span>
      </button>
      
      <div id="message" class="message hidden"></div>
      
      <ul id="networks-list"></ul>
      
      <div id="password-form" class="password-form">
        <h3 id="selected-network">网络名称</h3>
        <form id="wifi-form">
          <input type="hidden" id="ssid-input" name="ssid">
          <label for="password-input">密码：</label>
          <input type="password" id="password-input" name="password" placeholder="请输入密码">
          <button type="submit">保存配置</button>
        </form>
      </div>
    </div>
    
    <div class="footer">
      <p>Desk-Emoji 杭州易问科技 | &copy; 2024</p>
    </div>
  </div>

  <script>
    let selectedNetwork = null;
    
    // Scan for networks
    function scanNetworks() {
      const scanBtn = document.getElementById('scan-btn');
      const scanSpinner = document.getElementById('scan-spinner');
      const scanText = document.getElementById('scan-text');
      const messageDiv = document.getElementById('message');
      const networksList = document.getElementById('networks-list');
      
      // Show loading state
      scanSpinner.classList.remove('hidden');
      scanText.innerText = '扫描中...';
      scanBtn.disabled = true;
      messageDiv.classList.add('hidden');
      networksList.innerHTML = '';
      
      // Hide password form
      document.getElementById('password-form').style.display = 'none';
      
      // Make request to scan endpoint
      fetch('/scan-wifi')
        .then(async response => {
          const data = await response.json();
          if (!response.ok) {
            throw new Error(data.error || `HTTP ${response.status}`);
          }
          if (!Array.isArray(data)) {
            throw new Error('扫描结果格式错误');
          }
          return data;
        })
        .then(data => {
          // Reset scan button
          scanSpinner.classList.add('hidden');
          scanText.innerText = '扫描网络';
          scanBtn.disabled = false;
          
          if (data.length === 0) {
            messageDiv.innerHTML = '未找到网络';
            messageDiv.className = 'message error';
            messageDiv.classList.remove('hidden');
            return;
          }
          
          // Display networks
          data.forEach(network => {
            const listItem = document.createElement('li');
            listItem.className = 'network-item';
            listItem.setAttribute('data-ssid', network.ssid);
            
            // Determine signal strength icon
            let strengthText = '';
            if (network.rssi > -50) {
              strengthText = '强';
            } else if (network.rssi > -70) {
              strengthText = '优';
            } else if (network.rssi > -80) {
              strengthText = '中';
            } else {
              strengthText = '弱';
            }
            
            const ssidLabel = document.createElement('span');
            ssidLabel.textContent = network.ssid;
            const strengthLabel = document.createElement('span');
            strengthLabel.className = 'wifi-strength';
            strengthLabel.textContent = `${strengthText} (${network.rssi} dBm)`;
            listItem.appendChild(ssidLabel);
            listItem.appendChild(strengthLabel);
            
            listItem.addEventListener('click', () => selectNetwork(network.ssid));
            networksList.appendChild(listItem);
          });
        })
        .catch(error => {
          scanSpinner.classList.add('hidden');
          scanText.innerText = '扫描网络';
          scanBtn.disabled = false;
          
          messageDiv.innerHTML = '扫描网络错误: ' + error.message;
          messageDiv.className = 'message error';
          messageDiv.classList.remove('hidden');
        });
    }
    
    // Select a network
    function selectNetwork(ssid) {
      selectedNetwork = ssid;
      
      // Update UI to show the selected network
      const networkItems = document.querySelectorAll('.network-item');
      networkItems.forEach(item => {
        if (item.getAttribute('data-ssid') === ssid) {
          item.classList.add('selected');
        } else {
          item.classList.remove('selected');
        }
      });
      
      // Show password form
      const passwordForm = document.getElementById('password-form');
      document.getElementById('selected-network').innerText = ssid;
      document.getElementById('ssid-input').value = ssid;
      passwordForm.style.display = 'block';
      
      // Focus password field
      document.getElementById('password-input').focus();
    }
    
    // Submit form handler
    document.getElementById('wifi-form').addEventListener('submit', function(e) {
      e.preventDefault();
      
      const ssid = document.getElementById('ssid-input').value;
      const password = document.getElementById('password-input').value;
      const messageDiv = document.getElementById('message');
      const networksList = document.getElementById('networks-list');
      
      if (!ssid) {
        messageDiv.innerHTML = '请选择一个网络';
        messageDiv.className = 'message error';
        messageDiv.classList.remove('hidden');
        return;
      }
      
      // Show saving message
      messageDiv.innerHTML = '保存配置中...';
      messageDiv.className = 'message';
      messageDiv.classList.remove('hidden');
      
      // Send data to server
      fetch('/save-wifi', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/x-www-form-urlencoded',
        },
        body: `ssid=${encodeURIComponent(ssid)}&password=${encodeURIComponent(password)}`
      })
      .then(response => response.json())
      .then(data => {
        if (data.success) {
          messageDiv.innerHTML = 'WiFi 配置已保存，设备正在连接...';
          messageDiv.className = 'message success';
          networksList.innerHTML = '';
        } else {
          messageDiv.innerHTML = '错误: ' + data.message;
          messageDiv.className = 'message error';
        }
      })
      .catch(error => {
        messageDiv.innerHTML = '保存配置错误: ' + error.message;
        messageDiv.className = 'message error';
      });
    });
    
    // Start by scanning networks on page load
    window.onload = function() {
      // Wait a second before scanning to ensure the page is fully loaded
      setTimeout(scanNetworks, 1000);
    };
  </script>
</body>
</html>
)rawliteral";

WifiClient::WifiClient() : server(80) {
  broadcastIP = IPAddress(255, 255, 255, 255);
}

void WifiClient::setup_wifi() {
  oled_print("\nConnect WiFi");
  // NVS lives in the fixed nvs partition and survives application/SPIFFS
  // partition changes. The file remains as a migration fallback.
  bool credentials_loaded = load_wifi_from_nvs(ssid, password);
  if (credentials_loaded) {
    log_info("WiFi credentials loaded from NVS");
  } else if (load_wifi_from_file(ssid, password)) {
    credentials_loaded = true;
    log_info("WiFi credentials loaded from file");
    if (!save_wifi_to_nvs(ssid, password)) {
      log_warn("Failed to migrate WiFi credentials to NVS");
    }
  }

  if (!credentials_loaded || ssid.isEmpty()) {
    ssid = kDefaultWifiSsid;
    password = kDefaultWifiPassword;
    log_info("WiFi default credentials selected for SSID: %s", ssid.c_str());
  }

  // A failed station attempt falls through to AP provisioning.  Once the
  // user saves credentials there, config_wifi() updates ssid/password and we
  // retry the station connection without rebooting.
  while (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.disconnect();
    delay(100);
    begin_wifi_station(ssid, password);
    log_info("Connecting to WiFi SSID: %s", ssid.c_str());

    bool connected = false;
    for (int attempt = 1; attempt <= MAX_RECONNECT_ATTEMPTS; ++attempt) {
      const int status = WiFi.status();
      log_info("WiFi attempt %d/%d: status=%d (%s)", attempt,
               MAX_RECONNECT_ATTEMPTS, status, wifi_status_name(status));
      if (status == WL_CONNECTED) {
        connected = true;
        break;
      }
      blink_led();
      oled_print(".", 0);
      delay(1000);
    }

    if (connected || WiFi.status() == WL_CONNECTED) {
      break;
    }

    const int status = WiFi.status();
    log_error("WiFi connection failed after %d seconds: status=%d (%s)",
              MAX_RECONNECT_ATTEMPTS, status, wifi_status_name(status));
    log_info("Starting WiFi configuration AP; open http://192.168.4.1 to configure");
    config_wifi();

    // config_wifi() returns only after the page has saved a network. The
    // handler stores the new values in this object, so the next loop uses it.
    if (ssid.isEmpty()) {
      log_error("WiFi configuration returned empty SSID; using defaults");
      ssid = kDefaultWifiSsid;
      password = kDefaultWifiPassword;
    }
  }

  log_info("WiFi connected");
  local_ip = get_local_ip();
  oled_println("\n\nIP: " + local_ip);
  log_info("IP address: %s", local_ip.c_str());
  local_mac = get_mac_address();
  log_info("MAC address: %s", local_mac.c_str());
}

// 2026-09-11: Reconnect in the background without blocking audio, wake-word, or actuator handling.
void WifiClient::maintain_wifi() {
  static unsigned long last_check_time = 0;
  static unsigned long last_attempt_time = 0;
  static bool connection_was_up = true;
  static uint16_t reconnect_attempts = 0;

  unsigned long now = millis();
  if (now - last_check_time < 1000) {
    return;
  }
  last_check_time = now;

  if (WiFi.status() == WL_CONNECTED) {
    if (!connection_was_up) {
      local_ip = get_local_ip();
      log_info("WiFi reconnected");
      log_info("IP address: %s", local_ip.c_str());
    }
    connection_was_up = true;
    reconnect_attempts = 0;
    return;
  }

  if (connection_was_up) {
    log_warn("WiFi connection lost (status=%d)", WiFi.status());
    connection_was_up = false;
  }

  // 2026-09-11: Limit retries so a network outage cannot starve the main loop.
  if (now - last_attempt_time < 5000 || ssid.isEmpty()) {
    return;
  }
  last_attempt_time = now;
  reconnect_attempts++;
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  begin_wifi_station(ssid, password);
  log_info("Reconnecting to WiFi (attempt %u)", reconnect_attempts);
}

void WifiClient::config_wifi() {
  log_error("Failed to connect to WiFi, starting WiFi config...");
  WiFi.disconnect();
  set_led(COLOR_RED, 20);
  setup_http_server();
  // 2026-09-11: Avoid direct buffer access when the optional OLED did not initialize.
  if (oled_is_ready()) {
    oled.clearDisplay();
    oled.setCursor(0, 0);
  }
  oled_println("Desk-Emoji v" + String(VERSION), 500);
  oled_print("\nConnect WiFi");
}

void WifiClient::setup_http_server() {
  done_config = false;  

  // Keep the configuration AP alive while the station radio scans nearby
  // networks. Relying on the previous STA state makes scanning intermittent.
  log_info("Opening AP...");
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_AP_STA);
  delay(100);

  bool ap_started = WiFi.softAP(AP_SSID);
  if (!ap_started) {
    log_warn("WiFi configuration AP failed to start; retrying once");
    WiFi.softAPdisconnect(true);
    delay(250);
    WiFi.mode(WIFI_AP_STA);
    ap_started = WiFi.softAP(AP_SSID);
  }

  if (!ap_started) {
    log_error("WiFi configuration AP could not be started");
    return;
  }

  const IPAddress ap_ip = WiFi.softAPIP();
  if (ap_ip == IPAddress(0, 0, 0, 0)) {
    log_error("WiFi configuration AP started without a valid IP address");
    WiFi.softAPdisconnect(true);
    return;
  }
  log_info("WiFi configuration AP ready: SSID=%s, IP=%s", AP_SSID,
           ap_ip.toString().c_str());

  // Route for root / web page
  server.on("/", HTTP_GET, [this]() {
    server.send(200, "text/html", index_html);
  });
  
  // Route for scanning WiFi networks
  server.on("/scan-wifi", HTTP_GET, [this]() {
    // Remove stale asynchronous scan state before starting a synchronous scan.
    WiFi.scanDelete();
    delay(50);
    int n = WiFi.scanNetworks();
    if (n < 0) {
      log_warn("WiFi scan failed with result %d; retrying once", n);
      WiFi.scanDelete();
      delay(250);
      n = WiFi.scanNetworks();
    }

    if (n < 0) {
      log_error("WiFi scan failed with result %d", n);
      String error = "{\"error\":\"ESP32 WiFi scan failed\",\"code\":";
      error += String(n);
      error += "}";
      server.send(503, "application/json", error);
      WiFi.scanDelete();
      return;
    }

    log_info("WiFi scan completed: %d network(s) found", n);
    String json = "[";
    for (int i = 0; i < n; ++i) {
      if (i > 0) json += ",";
      json += "{";
      json += "\"ssid\":\"" + json_escape(WiFi.SSID(i)) + "\",";
      json += "\"rssi\":" + String(WiFi.RSSI(i));
      json += "}";
    }
    json += "]";
    
    server.send(200, "application/json", json);
    WiFi.scanDelete();
  });
  
  // Route for saving WiFi credentials
  server.on("/save-wifi", HTTP_POST, [this]() {
    String configured_ssid = server.arg("ssid");
    String configured_password = server.arg("password");
    
    if (configured_ssid.length() == 0) {
      server.send(400, "application/json", "{\"success\":false,\"message\":\"SSID cannot be empty\"}");
      return;
    }
    
    const bool nvs_saved = save_wifi_to_nvs(configured_ssid, configured_password);
    bool file_saved = false;
    // Keep the file for compatibility with older firmware and as a visible
    // backup, but do not make provisioning depend on the SPIFFS partition.
    File configFile = FFat.open(WIFI_CONF_FILE, FILE_WRITE);
    if (configFile) {
      configFile.println(configured_ssid);
      configFile.println(configured_password);
      configFile.close();
      file_saved = true;
    }

    if (!nvs_saved && !file_saved) {
      server.send(500, "application/json", "{\"success\":false,\"message\":\"Failed to open config file for writing\"}");
      return;
    }

    if (!nvs_saved) {
      log_warn("WiFi credentials saved to file only; NVS write failed");
    }
    
    // Apply the new credentials to the current station attempt after the AP
    // server closes. Do not print the password to serial output.
    this->ssid = configured_ssid;
    this->password = configured_password;
    log_info("WiFi credentials saved for SSID: %s", configured_ssid.c_str());
    // 2026-09-11: Never expose the saved Wi-Fi password in serial logs.
    
    server.send(200, "application/json", "{\"success\":true,\"message\":\"WiFi configuration saved\"}");
    done_config = true;
  });
  
  // Start server
  server.begin();
  
  // Get the correct IP address from softAP
  IPAddress ip = WiFi.softAPIP();
  log_info("HTTP server started at %s", ip.toString().c_str());
  
  // Display IP address on OLED screen
  // 2026-09-11: Avoid direct buffer access when Wi-Fi provisioning runs without an OLED.
  if (oled_is_ready()) {
    oled.clearDisplay();
    oled.setCursor(0, 0);
  }
  oled_println("WiFi Config Mode");
  oled_println("");
  oled_print("SSID: ");
  oled_println(AP_SSID);
  oled_println("");
  oled_print("IP: ");
  oled_println(ip.toString());
  
  while (!done_config) {
    server.handleClient();
    handle_cmd();
    delay(10);
  }
  // Release server resources
  server.close();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  blink_led(COLOR_GREEN, 3, 100);
  log_info("HTTP server stopped");
}

void WifiClient::setup_udp() {
  udp.begin(UDP_PORT);
  log_info("UDP server started");
}

void WifiClient::send_broadcast_msg(String msg) {
  udp.beginPacket(broadcastIP, UDP_PORT);
  udp.print(msg);
  udp.endPacket();
  log_debug("Send Broadcast: %s", msg.c_str());
}

void WifiClient::send_ip() {
  send_broadcast_msg("Desk-Emoji_" + local_ip);
}

void reset_wifi() {
  if (FFat.exists(WIFI_CONF_FILE)) {
    FFat.remove(WIFI_CONF_FILE);
  }
  clear_wifi_from_nvs();
  log_info("WiFi credentials cleared");
  delay(500);
  ESP.restart();
}
