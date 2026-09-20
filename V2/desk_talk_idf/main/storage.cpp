// 2026-09-19: Implement persistent application data on the migrated NVS and SPIFFS partitions.
#include "storage.h"

#include <cstdio>
#include <cstring>

// 2026-09-20: Keep every Flash operation on an internal-RAM worker stack, including calls from PSRAM-backed tasks.
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace desk_talk::storage {
namespace {

constexpr char kLogTag[] = "storage";
constexpr char kSpiffsBasePath[] = "/spiffs";
constexpr char kWifiNamespace[] = "wifi";
constexpr char kWifiSsidKey[] = "ssid";
constexpr char kWifiPasswordKey[] = "password";

// 2026-09-20: Serialize synchronous storage requests without allocating task stacks or synchronization objects from the fragmented heap.
enum class StorageOperation {
  kInitialize,
  kReadText,
  kWriteText,
  kLoadWifi,
  kSaveWifi,
  kClearWifi,
};

struct StorageRequest {
  StorageOperation operation;
  const char *path = nullptr;
  std::string *output = nullptr;
  std::string *second_output = nullptr;
  const std::string *input = nullptr;
  const std::string *second_input = nullptr;
  bool success = false;
  esp_err_t initialization_result = ESP_FAIL;
};

constexpr size_t kStorageStackBytes = 4096;
DRAM_ATTR StackType_t storage_stack[kStorageStackBytes / sizeof(StackType_t)];
DRAM_ATTR StaticTask_t storage_task_buffer;
DRAM_ATTR StaticSemaphore_t storage_mutex_buffer;
DRAM_ATTR StaticSemaphore_t storage_done_buffer;
TaskHandle_t storage_task = nullptr;
SemaphoreHandle_t storage_mutex = nullptr;
SemaphoreHandle_t storage_done = nullptr;
StorageRequest *pending_request = nullptr;

// 2026-09-20: The worker re-enters the public helpers; their task check selects the original implementation on this cache-safe stack.
void storage_loop(void *) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    StorageRequest *request = pending_request;
    if (request == nullptr) continue;
    switch (request->operation) {
      case StorageOperation::kInitialize:
        request->initialization_result = initialize();
        break;
      case StorageOperation::kReadText:
        request->success = read_text_file(request->path, *request->output);
        break;
      case StorageOperation::kWriteText:
        request->success = write_text_file(request->path, *request->input);
        break;
      case StorageOperation::kLoadWifi:
        request->success = load_wifi_credentials(
            *request->output, *request->second_output);
        break;
      case StorageOperation::kSaveWifi:
        request->success = save_wifi_credentials(
            *request->input, *request->second_input);
        break;
      case StorageOperation::kClearWifi:
        request->success = clear_wifi_credentials();
        break;
    }
    pending_request = nullptr;
    // 2026-09-20: Do not touch the caller-owned request after signaling completion; its stack frame may immediately return.
    xSemaphoreGive(storage_done);
  }
}

// 2026-09-20: Start once from app_main before any storage clients, reserving the Flash-safe stack before Wi-Fi and ESP-SR allocate memory.
esp_err_t start_storage_worker() {
  if (storage_task != nullptr) return ESP_OK;
  storage_mutex = xSemaphoreCreateMutexStatic(&storage_mutex_buffer);
  storage_done = xSemaphoreCreateBinaryStatic(&storage_done_buffer);
  if (storage_mutex == nullptr || storage_done == nullptr) {
    return ESP_ERR_NO_MEM;
  }
  storage_task = xTaskCreateStatic(storage_loop, "flash_storage",
                                  kStorageStackBytes, nullptr, 3,
                                  storage_stack, &storage_task_buffer);
  if (storage_task == nullptr) return ESP_ERR_NO_MEM;
  ESP_LOGI(kLogTag, "Flash storage worker ready (internal stack=%u)",
           static_cast<unsigned>(kStorageStackBytes));
  return ESP_OK;
}

// 2026-09-20: Hold callers until their request completes, so Flash formatting/GC cannot outlive referenced strings or PSRAM stack frames.
bool execute_storage_request(StorageRequest &request) {
  if (storage_task == nullptr || storage_mutex == nullptr ||
      storage_done == nullptr) {
    ESP_LOGE(kLogTag, "Flash storage worker is unavailable");
    return false;
  }
  if (xSemaphoreTake(storage_mutex, portMAX_DELAY) != pdTRUE) return false;
  pending_request = &request;
  if (xTaskNotifyGive(storage_task) != pdPASS) {
    pending_request = nullptr;
    xSemaphoreGive(storage_mutex);
    return false;
  }
  const bool completed = xSemaphoreTake(storage_done, portMAX_DELAY) == pdTRUE;
  xSemaphoreGive(storage_mutex);
  return completed;
}

std::string absolute_path(const char *path) {
  if (path == nullptr || path[0] == '\0') {
    return {};
  }
  if (std::strncmp(path, kSpiffsBasePath, std::strlen(kSpiffsBasePath)) == 0) {
    return path;
  }
  return std::string(kSpiffsBasePath) + (path[0] == '/' ? path : "/") + path;
}

bool read_nvs_string(nvs_handle_t handle, const char *key,
                     std::string &value) {
  size_t length = 0;
  if (nvs_get_str(handle, key, nullptr, &length) != ESP_OK || length == 0) {
    return false;
  }
  value.resize(length - 1);
  std::string buffer(length, '\0');
  if (nvs_get_str(handle, key, buffer.data(), &length) != ESP_OK) {
    value.clear();
    return false;
  }
  value.assign(buffer.c_str());
  return true;
}

}  // namespace

esp_err_t initialize() {
  // 2026-09-20: Mount/format SPIFFS and initialize NVS on the same internal-stack worker used for later reads and writes.
  if (xTaskGetCurrentTaskHandle() != storage_task) {
    const esp_err_t worker_result = start_storage_worker();
    if (worker_result != ESP_OK) return worker_result;
    StorageRequest request{StorageOperation::kInitialize};
    return execute_storage_request(request) ? request.initialization_result
                                            : ESP_ERR_INVALID_STATE;
  }
  esp_err_t result = nvs_flash_init();
  if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
      result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    // 2026-09-19: Erase only the NVS partition when its on-flash format cannot be opened; never erase the complete 16 MB flash implicitly.
    ESP_LOGW(kLogTag, "NVS requires reinitialization: %s",
             esp_err_to_name(result));
    result = nvs_flash_erase();
    if (result == ESP_OK) {
      result = nvs_flash_init();
    }
  }
  if (result != ESP_OK) {
    ESP_LOGE(kLogTag, "NVS initialization failed: %s",
             esp_err_to_name(result));
    return result;
  }

  esp_vfs_spiffs_conf_t spiffs_config = {};
  spiffs_config.base_path = kSpiffsBasePath;
  spiffs_config.partition_label = "spiffs";
  spiffs_config.max_files = 8;
#if 0
  // 2026-09-19: Retain the pre-migration behavior for reference; an Arduino FFat or blank partition cannot be mounted as SPIFFS.
  spiffs_config.format_if_mount_failed = false;
#endif
  // 2026-09-19: Format only the SPIFFS partition when its previous Arduino FFat layout cannot be mounted after migration.
  spiffs_config.format_if_mount_failed = true;
  result = esp_vfs_spiffs_register(&spiffs_config);
  if (result != ESP_OK) {
#if 0
    // 2026-09-19: Retain the former message; automatic SPIFFS recovery is now enabled above.
    ESP_LOGE(kLogTag, "SPIFFS mount failed without formatting: %s",
             esp_err_to_name(result));
#endif
    // 2026-09-19: Distinguish a failed automatic recovery from the former no-format mount failure.
    ESP_LOGE(kLogTag, "SPIFFS mount or format failed: %s",
             esp_err_to_name(result));
    return result;
  }

  size_t total = 0;
  size_t used = 0;
  result = esp_spiffs_info("spiffs", &total, &used);
  if (result == ESP_OK) {
    ESP_LOGI(kLogTag, "SPIFFS ready: used=%u total=%u",
             static_cast<unsigned>(used), static_cast<unsigned>(total));
  }
  return result;
}

bool read_text_file(const char *path, std::string &content) {
  // 2026-09-20: LLM history reads must not enter SPIFFS from the conversation task's PSRAM stack, even when the file does not exist yet.
  if (xTaskGetCurrentTaskHandle() != storage_task) {
    content.clear();
    StorageRequest request{StorageOperation::kReadText};
    request.path = path;
    request.output = &content;
    return execute_storage_request(request) && request.success;
  }
  content.clear();
  const std::string full_path = absolute_path(path);
  if (full_path.empty()) {
    return false;
  }
  FILE *file = std::fopen(full_path.c_str(), "rb");
  if (file == nullptr) {
    return false;
  }
  if (std::fseek(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    return false;
  }
  const long length = std::ftell(file);
  if (length < 0 || std::fseek(file, 0, SEEK_SET) != 0) {
    std::fclose(file);
    return false;
  }
  content.resize(static_cast<size_t>(length));
  const size_t read = length == 0
                          ? 0
                          : std::fread(content.data(), 1, content.size(), file);
  std::fclose(file);
  return read == content.size();
}

bool write_text_file(const char *path, const std::string &content) {
  // 2026-09-20: Route history commits and servo calibration writes through the Flash-safe worker without changing their synchronous API.
  if (xTaskGetCurrentTaskHandle() != storage_task) {
    StorageRequest request{StorageOperation::kWriteText};
    request.path = path;
    request.input = &content;
    return execute_storage_request(request) && request.success;
  }
  const std::string full_path = absolute_path(path);
  if (full_path.empty()) {
    return false;
  }
  FILE *file = std::fopen(full_path.c_str(), "wb");
  if (file == nullptr) {
    return false;
  }
  const size_t written = content.empty()
                             ? 0
                             : std::fwrite(content.data(), 1, content.size(), file);
  const bool closed = std::fclose(file) == 0;
  return written == content.size() && closed;
}

bool load_wifi_credentials(std::string &ssid, std::string &password) {
  // 2026-09-20: Keep NVS access under the same serialized internal-stack boundary as SPIFFS.
  if (xTaskGetCurrentTaskHandle() != storage_task) {
    ssid.clear();
    password.clear();
    StorageRequest request{StorageOperation::kLoadWifi};
    request.output = &ssid;
    request.second_output = &password;
    return execute_storage_request(request) && request.success;
  }
  ssid.clear();
  password.clear();
  nvs_handle_t handle = 0;
  if (nvs_open(kWifiNamespace, NVS_READONLY, &handle) != ESP_OK) {
    return false;
  }
  const bool has_ssid = read_nvs_string(handle, kWifiSsidKey, ssid);
  read_nvs_string(handle, kWifiPasswordKey, password);
  nvs_close(handle);
  return has_ssid && !ssid.empty();
}

bool save_wifi_credentials(const std::string &ssid,
                           const std::string &password) {
  // 2026-09-20: Provisioning writes may erase/commit NVS pages and must always execute with an internal-RAM stack.
  if (xTaskGetCurrentTaskHandle() != storage_task) {
    StorageRequest request{StorageOperation::kSaveWifi};
    request.input = &ssid;
    request.second_input = &password;
    return execute_storage_request(request) && request.success;
  }
  if (ssid.empty()) {
    return false;
  }
  nvs_handle_t handle = 0;
  if (nvs_open(kWifiNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    return false;
  }
  esp_err_t result = nvs_set_str(handle, kWifiSsidKey, ssid.c_str());
  if (result == ESP_OK) {
    result = nvs_set_str(handle, kWifiPasswordKey, password.c_str());
  }
  if (result == ESP_OK) {
    result = nvs_commit(handle);
  }
  nvs_close(handle);
  return result == ESP_OK;
}

bool clear_wifi_credentials() {
  // 2026-09-20: The PSRAM-backed command console must delegate reset_wifi's Flash erase/commit to the worker.
  if (xTaskGetCurrentTaskHandle() != storage_task) {
    StorageRequest request{StorageOperation::kClearWifi};
    return execute_storage_request(request) && request.success;
  }
  nvs_handle_t handle = 0;
  if (nvs_open(kWifiNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    return false;
  }
  esp_err_t result = nvs_erase_all(handle);
  if (result == ESP_OK) {
    result = nvs_commit(handle);
  }
  nvs_close(handle);
  return result == ESP_OK;
}

}  // namespace desk_talk::storage
