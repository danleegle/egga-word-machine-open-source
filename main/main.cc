#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <esp_ota_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "system_info.h"

#define TAG "main"

namespace {

constexpr uint32_t kOtaHealthConfirmationDelayMs = 30 * 1000;
constexpr uint32_t kOtaHealthConfirmationRetryMs = 5 * 1000;
constexpr uint32_t kOtaHealthConfirmationWindowMs = 2 * 60 * 1000;

void ConfirmOtaAfterHealthWindow(void* arg) {
    auto* app = static_cast<Application*>(arg);
    vTaskDelay(pdMS_TO_TICKS(kOtaHealthConfirmationDelayMs));
    uint32_t elapsed_ms = kOtaHealthConfirmationDelayMs;
    while (elapsed_ms <= kOtaHealthConfirmationWindowMs) {
        if (app != nullptr && app->CanEnterSleepMode()) {
            const esp_err_t ret = esp_ota_mark_app_valid_cancel_rollback();
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "OTA image confirmed after %lu ms health window",
                         static_cast<unsigned long>(elapsed_ms));
                vTaskDelete(nullptr);
                return;
            }
            ESP_LOGW(TAG, "OTA image confirmation failed: %s", esp_err_to_name(ret));
        }
        vTaskDelay(pdMS_TO_TICKS(kOtaHealthConfirmationRetryMs));
        elapsed_ms += kOtaHealthConfirmationRetryMs;
    }
    ESP_LOGE(TAG, "OTA image left unconfirmed after health window");
    vTaskDelete(nullptr);
}

void ScheduleOtaHealthConfirmation(Application* app) {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    if (running == nullptr || esp_ota_get_state_partition(running, &ota_state) != ESP_OK ||
        ota_state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    if (xTaskCreate(ConfirmOtaAfterHealthWindow,
                    "ota_health",
                    3072,
                    app,
                    2,
                    nullptr) != pdPASS) {
        ESP_LOGE(TAG, "Unable to create OTA health confirmation task");
    }
}

}  // namespace

static void LogNvsStats() {
    nvs_stats_t stats = {};
    esp_err_t ret = nvs_get_stats(nullptr, &stats);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS stats read failed: %s", esp_err_to_name(ret));
        return;
    }
    uint32_t used = stats.used_entries;
    uint32_t total = stats.total_entries;
    uint32_t free_entries = stats.free_entries;
    uint32_t percent = (total > 0) ? (used * 100U / total) : 0;
    ESP_LOGI(TAG, "NVS stats: used=%u free=%u total=%u (%u%%) namespaces=%u",
             used, free_entries, total, percent, stats.namespace_count);
}

extern "C" void app_main(void)
{
    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    LogNvsStats();

    // Initialize and run the application
    auto& app = Application::GetInstance();
    app.Initialize();
    ScheduleOtaHealthConfirmation(&app);
    app.Run();  // This function runs the main event loop and never returns
}
