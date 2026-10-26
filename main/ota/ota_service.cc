#include "ota_service.h"

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_crt_bundle.h>
#include <esp_err.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <sdkconfig.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "board.h"
#include "system_info.h"
#include "wifi_manager.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr char kTag[] = "OtaService";
constexpr char kDefaultBaseUrl[] = CONFIG_EGGA_OTA_BASE_URL;
constexpr int kHttpTimeoutMs = 30000;
constexpr int kMaxManifestBytes = 16 * 1024;
constexpr int kNetworkRetryCount = 4;
constexpr int kNetworkRetryDelayMs = 1500;
constexpr uint32_t kOtaTaskStackBytes = 12 * 1024;
constexpr uint32_t kOtaRebootDisplayDelayMs = 2500;

std::string JsonEscape(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 8);
    for (const unsigned char ch : value) {
        switch (ch) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (ch < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", ch);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    return out;
}

std::string JsonString(const std::string& value) {
    return "\"" + JsonEscape(value) + "\"";
}

std::string CurrentVersion() {
    const esp_app_desc_t* app = esp_app_get_description();
    return app != nullptr ? std::string(app->version) : std::string();
}

std::string TrimBaseUrl(const std::string& raw) {
    std::string value = raw.empty() ? std::string(kDefaultBaseUrl) : raw;
    while (!value.empty() && value.back() == '/') {
        value.pop_back();
    }
    return value.empty() ? std::string(kDefaultBaseUrl) : value;
}

bool IsUnreserved(char ch) {
    return (ch >= 'a' && ch <= 'z') ||
        (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') ||
        ch == '-' || ch == '_' || ch == '.' || ch == '~';
}

std::string UrlEncode(const std::string& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char ch : value) {
        if (IsUnreserved(static_cast<char>(ch))) {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(kHex[ch >> 4]);
            out.push_back(kHex[ch & 0x0f]);
        }
    }
    return out;
}

std::string BuildCheckUrl(const std::string& base_url) {
    const std::string mac = SystemInfo::GetMacAddress();
    return TrimBaseUrl(base_url) +
        "/api/ota/firmware?current=" + UrlEncode(CurrentVersion()) +
        "&target=" + UrlEncode(CONFIG_IDF_TARGET) +
        (mac.empty() ? std::string() : "&mac=" + UrlEncode(mac));
}

bool EnsureWifiReady(std::string* error) {
    WifiManager& wifi = WifiManager::GetInstance();
    if (wifi.IsConnected()) {
        return true;
    }

    Board::GetInstance().RequestNetwork();
    for (int i = 0; i < 60; ++i) {
        if (wifi.IsConnected()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    if (error != nullptr) {
        *error = "WiFi not connected";
    }
    return false;
}

const char* GetJsonString(cJSON* object, const char* key) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        return nullptr;
    }
    return item->valuestring;
}

int GetJsonInt(cJSON* object, const char* key, int default_value) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item)) {
        return default_value;
    }
    return item->valueint;
}

bool FetchTextOnce(const std::string& url, std::string* body, std::string* error) {
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = kHttpTimeoutMs;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        if (error != nullptr) {
            *error = "failed to initialize HTTP client";
        }
        return false;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    const std::string mac = SystemInfo::GetMacAddress();
    if (!mac.empty()) {
        esp_http_client_set_header(client, "X-Egga-Device-Mac", mac.c_str());
    }
    esp_err_t ret = esp_http_client_open(client, 0);
    if (ret != ESP_OK) {
        if (error != nullptr) {
            *error = std::string("HTTP open failed: ") + esp_err_to_name(ret);
        }
        esp_http_client_cleanup(client);
        return false;
    }

    const int64_t content_length = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        if (error != nullptr) {
            *error = "HTTP status " + std::to_string(status);
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }
    if (content_length > kMaxManifestBytes) {
        if (error != nullptr) {
            *error = "manifest is too large";
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    std::vector<char> buffer(1024);
    body->clear();
    while (true) {
        const int read = esp_http_client_read(client, buffer.data(), buffer.size());
        if (read < 0) {
            if (error != nullptr) {
                *error = "HTTP read failed";
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
        if (read == 0) {
            break;
        }
        body->append(buffer.data(), static_cast<size_t>(read));
        if (body->size() > kMaxManifestBytes) {
            if (error != nullptr) {
                *error = "manifest is too large";
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return true;
}

bool FetchText(const std::string& url, std::string* body, std::string* error) {
    std::string last_error;
    for (int attempt = 0; attempt < kNetworkRetryCount; ++attempt) {
        std::string attempt_error;
        if (FetchTextOnce(url, body, &attempt_error)) {
            if (attempt > 0) {
                ESP_LOGI(kTag, "HTTP request recovered after %d retries", attempt);
            }
            return true;
        }

        last_error = attempt_error.empty() ? "HTTP request failed" : attempt_error;
        ESP_LOGW(kTag, "HTTP request attempt %d/%d failed: %s",
                 attempt + 1, kNetworkRetryCount, last_error.c_str());
        if (attempt + 1 < kNetworkRetryCount) {
            if (!WifiManager::GetInstance().IsConnected()) {
                Board::GetInstance().RequestNetwork();
            }
            vTaskDelay(pdMS_TO_TICKS(kNetworkRetryDelayMs));
        }
    }

    if (error != nullptr) {
        *error = last_error;
    }
    return false;
}

struct OtaTaskArgs {
    OtaService* service;
    std::string url;
    std::string version;
    int expected_size = 0;
};

void OtaTaskEntry(void* arg) {
    std::unique_ptr<OtaTaskArgs> args(static_cast<OtaTaskArgs*>(arg));
    args->service->RunTask(std::move(args->url),
                           std::move(args->version),
                           args->expected_size);
    vTaskDelete(nullptr);
}

}  // namespace

OtaService::OtaService() {
    mutex_ = xSemaphoreCreateMutex();
}

OtaService& OtaService::GetInstance() {
    static OtaService service;
    return service;
}

OtaService::Snapshot OtaService::GetSnapshot() {
    Snapshot snapshot;
    if (mutex_ != nullptr) {
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    }
    snapshot.running = running_;
    snapshot.state = state_;
    snapshot.current_version = CurrentVersion();
    snapshot.latest_version = latest_version_;
    snapshot.message = message_;
    snapshot.url = url_;
    snapshot.progress = progress_;
    if (mutex_ != nullptr) {
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    }
    return snapshot;
}

std::string OtaService::StatusJson() {
    const Snapshot snapshot = GetSnapshot();
    std::string json = "{";
    json += "\"running\":" + std::string(snapshot.running ? "true" : "false") + ",";
    json += "\"state\":" + JsonString(snapshot.state) + ",";
    json += "\"current_version\":" + JsonString(snapshot.current_version) + ",";
    json += "\"latest_version\":" + JsonString(snapshot.latest_version) + ",";
    json += "\"progress\":" + std::to_string(std::max(0, snapshot.progress)) + ",";
    json += "\"message\":" + JsonString(snapshot.message) + ",";
    json += "\"url\":" + JsonString(snapshot.url);
    json += "}";
    return json;
}

bool OtaService::FetchUpdateInfo(const std::string& base_url, UpdateInfo* info, std::string* error) {
    if (info == nullptr) {
        return false;
    }
    const esp_partition_t* next_partition = esp_ota_get_next_update_partition(nullptr);
    if (next_partition == nullptr) {
        if (error != nullptr) {
            *error = "no OTA partition is available";
        }
        return false;
    }

    std::string body;
    const std::string url = BuildCheckUrl(base_url);
    ESP_LOGI(kTag, "checking OTA: %s", url.c_str());
    if (!FetchText(url, &body, error)) {
        return false;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        if (error != nullptr) {
            *error = "invalid OTA manifest JSON";
        }
        return false;
    }

    cJSON* update = cJSON_GetObjectItemCaseSensitive(root, "updateAvailable");
    info->available = cJSON_IsTrue(update);
    info->version = GetJsonString(root, "version") != nullptr ? GetJsonString(root, "version") : "";
    info->url = GetJsonString(root, "url") != nullptr ? GetJsonString(root, "url") : "";
    info->sha256 = GetJsonString(root, "sha256") != nullptr ? GetJsonString(root, "sha256") : "";
    info->size = GetJsonInt(root, "size", 0);
    info->message = info->available ? "update available" : "already up to date";
    const char* reason = GetJsonString(root, "reason");
    if (reason != nullptr) {
        info->message = reason;
    }
    cJSON_Delete(root);

    if (info->available && info->url.empty()) {
        if (error != nullptr) {
            *error = "OTA response does not include a firmware URL";
        }
        return false;
    }
    return true;
}

bool OtaService::Check(const std::string& base_url, std::string* error) {
    SetChecking();
    if (!EnsureWifiReady(error)) {
        SetError(error != nullptr ? *error : "WiFi not connected");
        return false;
    }
    WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::PERFORMANCE);
    UpdateInfo info;
    if (!FetchUpdateInfo(base_url, &info, error)) {
        WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::LOW_POWER);
        SetError(error != nullptr ? *error : "OTA check failed");
        return false;
    }
    WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::LOW_POWER);
    if (info.available) {
        SetAvailable(info);
    } else {
        SetNotAvailable(info.message);
    }
    return true;
}

bool OtaService::Start(const std::string& base_url, std::string* error) {
    if (mutex_ != nullptr) {
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    }
    const bool was_running = running_;
    if (mutex_ != nullptr) {
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    }
    if (was_running) {
        if (error != nullptr) {
            *error = "OTA is already running";
        }
        return false;
    }

    auto restore_wifi_power_save = []() {
        WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::LOW_POWER);
    };

    SetChecking();
    if (!EnsureWifiReady(error)) {
        SetError(error != nullptr ? *error : "WiFi not connected");
        return false;
    }
    WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::PERFORMANCE);
    UpdateInfo info;
    if (!FetchUpdateInfo(base_url, &info, error)) {
        restore_wifi_power_save();
        SetError(error != nullptr ? *error : "OTA check failed");
        return false;
    }
    if (!info.available) {
        restore_wifi_power_save();
        SetNotAvailable(info.message);
        return true;
    }
    SetStarting(info);

    auto* args = new (std::nothrow) OtaTaskArgs{
        .service = this,
        .url = info.url,
        .version = info.version,
        .expected_size = info.size
    };
    if (args == nullptr) {
        if (error != nullptr) {
            *error = "failed to allocate OTA task";
        }
        restore_wifi_power_save();
        SetError(error != nullptr ? *error : "failed to allocate OTA task");
        return false;
    }

    const BaseType_t created = xTaskCreate(
        OtaTaskEntry,
        "egga_ota",
        kOtaTaskStackBytes,
        args,
        5,
        nullptr);
    if (created != pdPASS) {
        delete args;
        if (error != nullptr) {
            *error = "failed to create OTA task";
        }
        restore_wifi_power_save();
        SetError(error != nullptr ? *error : "failed to create OTA task");
        return false;
    }
    return true;
}

void OtaService::RunTask(std::string url, std::string version, int expected_size) {
    if (mutex_ != nullptr) {
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    }
    running_ = true;
    state_ = "downloading";
    latest_version_ = version;
    url_ = url;
    progress_ = 0;
    message_ = "starting OTA download";
    if (mutex_ != nullptr) {
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    }

    auto restore_wifi_power_save = []() {
        WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::LOW_POWER);
    };

    std::string network_error;
    if (!EnsureWifiReady(&network_error)) {
        restore_wifi_power_save();
        SetError(network_error);
        return;
    }

    WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::PERFORMANCE);

    esp_http_client_config_t http_config = {};
    http_config.url = url.c_str();
    http_config.timeout_ms = kHttpTimeoutMs;
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
    http_config.buffer_size = 8192;
    http_config.keep_alive_enable = true;

    esp_https_ota_config_t ota_config = {};
    ota_config.http_config = &http_config;

    esp_https_ota_handle_t handle = nullptr;
    esp_err_t ret = ESP_FAIL;
    for (int attempt = 0; attempt < kNetworkRetryCount; ++attempt) {
        SetDownloading(0, attempt == 0 ? "opening firmware download" : "retrying firmware download");
        handle = nullptr;
        ret = esp_https_ota_begin(&ota_config, &handle);
        if (ret == ESP_OK) {
            break;
        }
        ESP_LOGW(kTag, "OTA begin attempt %d/%d failed: %s",
                 attempt + 1, kNetworkRetryCount, esp_err_to_name(ret));
        if (handle != nullptr) {
            esp_https_ota_abort(handle);
            handle = nullptr;
        }
        if (attempt + 1 < kNetworkRetryCount) {
            if (!WifiManager::GetInstance().IsConnected()) {
                Board::GetInstance().RequestNetwork();
            }
            vTaskDelay(pdMS_TO_TICKS(kNetworkRetryDelayMs));
        }
    }
    if (ret != ESP_OK) {
        restore_wifi_power_save();
        SetError(std::string("OTA begin failed: ") + esp_err_to_name(ret));
        return;
    }

    while ((ret = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        const int read = esp_https_ota_get_image_len_read(handle);
        const int total = esp_https_ota_get_image_size(handle);
        const int progress_total = total > 0 ? total : expected_size;
        const int percent = progress_total > 0
            ? std::min(99, std::max(0, read * 100 / progress_total))
            : 0;
        SetDownloading(percent, "downloading firmware");
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (ret != ESP_OK) {
        esp_https_ota_abort(handle);
        restore_wifi_power_save();
        SetError(std::string("OTA perform failed: ") + esp_err_to_name(ret));
        return;
    }

    if (!esp_https_ota_is_complete_data_received(handle)) {
        esp_https_ota_abort(handle);
        restore_wifi_power_save();
        SetError("OTA image download was incomplete");
        return;
    }

    ret = esp_https_ota_finish(handle);
    if (ret != ESP_OK) {
        restore_wifi_power_save();
        SetError(std::string("OTA finish failed: ") + esp_err_to_name(ret));
        return;
    }

    SetRebooting(version);
    // Leave enough time for the UI poller and one physical e-paper partial
    // refresh to show the completed state before rebooting.
    vTaskDelay(pdMS_TO_TICKS(kOtaRebootDisplayDelayMs));
    esp_restart();
}

void OtaService::SetIdle() {
    if (mutex_ != nullptr) {
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    }
    running_ = false;
    state_ = "idle";
    message_.clear();
    progress_ = -1;
    if (mutex_ != nullptr) {
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    }
}

void OtaService::SetChecking() {
    if (mutex_ != nullptr) {
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    }
    running_ = false;
    state_ = "checking";
    message_ = "checking OTA manifest";
    progress_ = -1;
    if (mutex_ != nullptr) {
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    }
}

void OtaService::SetStarting(const UpdateInfo& info) {
    if (mutex_ != nullptr) {
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    }
    running_ = true;
    state_ = "starting";
    latest_version_ = info.version;
    url_ = info.url;
    progress_ = 0;
    message_ = "starting OTA task";
    if (mutex_ != nullptr) {
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    }
}

void OtaService::SetAvailable(const UpdateInfo& info) {
    if (mutex_ != nullptr) {
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    }
    running_ = false;
    state_ = "available";
    latest_version_ = info.version;
    url_ = info.url;
    progress_ = 0;
    message_ = "update available";
    if (mutex_ != nullptr) {
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    }
}

void OtaService::SetNotAvailable(const std::string& message) {
    if (mutex_ != nullptr) {
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    }
    running_ = false;
    state_ = "not_available";
    progress_ = -1;
    message_ = message.empty() ? "already up to date" : message;
    if (mutex_ != nullptr) {
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    }
}

void OtaService::SetDownloading(int progress, const std::string& message) {
    if (mutex_ != nullptr) {
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    }
    running_ = true;
    state_ = "downloading";
    progress_ = progress;
    message_ = message;
    if (mutex_ != nullptr) {
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    }
}

void OtaService::SetError(const std::string& message) {
    ESP_LOGE(kTag, "%s", message.c_str());
    if (mutex_ != nullptr) {
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    }
    running_ = false;
    state_ = "error";
    message_ = message;
    if (progress_ < 0) {
        progress_ = 0;
    }
    if (mutex_ != nullptr) {
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    }
}

void OtaService::SetRebooting(const std::string& version) {
    if (mutex_ != nullptr) {
        xSemaphoreTake(static_cast<SemaphoreHandle_t>(mutex_), portMAX_DELAY);
    }
    running_ = false;
    state_ = "rebooting";
    latest_version_ = version;
    progress_ = 100;
    message_ = "OTA complete, rebooting";
    if (mutex_ != nullptr) {
        xSemaphoreGive(static_cast<SemaphoreHandle_t>(mutex_));
    }
}
