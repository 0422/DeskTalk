// 2026-09-19: Port V1 serial actions and factory commands to the ESP-IDF console VFS without claiming a second UART driver.
#include "command_console.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "audio.h"
#include "cJSON.h"
#include "device_ui.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "storage.h"

namespace desk_talk::console {
namespace {

constexpr char kLogTag[] = "console";

std::string trim(std::string text) {
  const size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const size_t last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

void factory_command(const std::string &command) {
  if (command == "reboot" || command == "restart") {
    ESP_LOGI(kLogTag, "Rebooting");
    esp_restart();
  } else if (command == "on") {
    ui::set_idle_actions_enabled(true);
  } else if (command == "off") {
    ui::set_idle_actions_enabled(false);
  } else if (command == "reset_wifi") {
    if (storage::clear_wifi_credentials()) {
      ESP_LOGI(kLogTag, "Wi-Fi credentials cleared; rebooting");
      esp_restart();
    }
  } else if (command == "speaker_test") {
    audio::test_speaker();
  } else if (command == "face_status") {
    ESP_LOGI(kLogTag, "Camera/face tracking disabled by hardware profile");
  } else if (command == "enroll_owner" || command == "clear_owner" ||
             command == "face_follow_on" || command == "face_follow_off") {
    ESP_LOGW(kLogTag, "Face command unavailable while camera is disabled");
  } else {
    int value = 0;
    if (std::sscanf(command.c_str(), "adjust_x %d", &value) == 1) {
      ui::adjust_head_center(value, 0);
    } else if (std::sscanf(command.c_str(), "adjust_y %d", &value) == 1) {
      ui::adjust_head_center(0, value);
    } else if (std::sscanf(command.c_str(), "head_left %d", &value) == 1) {
      ui::adjust_head_center(-std::clamp(value, 1, 10), 0);
    } else if (std::sscanf(command.c_str(), "head_right %d", &value) == 1) {
      ui::adjust_head_center(std::clamp(value, 1, 10), 0);
    } else if (std::sscanf(command.c_str(), "head_up %d", &value) == 1) {
      ui::adjust_head_center(0, -std::clamp(value, 1, 10));
    } else if (std::sscanf(command.c_str(), "head_down %d", &value) == 1) {
      ui::adjust_head_center(0, std::clamp(value, 1, 10));
    } else {
      int x = 0;
      int y = 0;
      int delay_ms = 3;
      if (std::sscanf(command.c_str(), "head_move %d %d %d", &x, &y,
                      &delay_ms) == 3) {
        ui::move_head(x, y, std::clamp(delay_ms, 1, 100));
      } else {
        ESP_LOGW(kLogTag, "Unknown factory command: %s", command.c_str());
      }
    }
  }
}

void process_line(const char *line) {
  if (line == nullptr) return;
  const std::string cleaned = trim(line);
  if (cleaned.empty()) return;
  cJSON *document = cJSON_Parse(cleaned.c_str());
  if (document == nullptr) {
    ESP_LOGE(kLogTag, "Invalid JSON command");
    return;
  }
  cJSON *actions = cJSON_GetObjectItemCaseSensitive(document, "actions");
  if (cJSON_IsArray(actions)) {
    const int count = std::min(3, cJSON_GetArraySize(actions));
    for (int index = 0; index < count; ++index) {
      cJSON *action = cJSON_GetArrayItem(actions, index);
      if (cJSON_IsString(action)) ui::execute_action(action->valuestring);
    }
  }
  cJSON *factory = cJSON_GetObjectItemCaseSensitive(document, "factory");
  if (cJSON_IsString(factory)) factory_command(factory->valuestring);
  cJSON_Delete(document);
}

void console_task(void *) {
  setvbuf(stdin, nullptr, _IONBF, 0);
  char line[1024] = {};
  while (true) {
    if (std::fgets(line, sizeof(line), stdin) != nullptr) {
      process_line(line);
    } else {
      clearerr(stdin);
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }
}

}  // namespace

esp_err_t start() {
#if 0
  // 2026-09-20: Retain the former internal-stack console task creation for migration reference.
  return xTaskCreate(console_task, "json_console", 4096, nullptr, 2,
                     nullptr) == pdPASS
             ? ESP_OK
             : ESP_ERR_NO_MEM;
#endif
  // 2026-09-20: Place the long-lived console stack in PSRAM so diagnostics remain available without consuming the TLS reserve.
  return xTaskCreateWithCaps(
             console_task, "json_console", 4096, nullptr, 2, nullptr,
             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS
             ? ESP_OK
             : ESP_ERR_NO_MEM;
}

}  // namespace desk_talk::console
