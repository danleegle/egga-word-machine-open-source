#include "maimemo_open_api_client.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>

#include <algorithm>
#include <cstring>

namespace {

constexpr char kTag[] = "MaimemoOpenApi";
constexpr char kTodayItemsUrl[] =
    "https://open.maimemo.com/open/api/v1/memo/study/get_today_items";
constexpr char kLegacyTodayItemsUrl[] =
    "https://open.maimemo.com/open/api/v1/study/get_today_items";
// The snapshot is only an offline fallback. Keeping hundreds of unfinished
// words increases startup time and can exceed what a fragmented SPIFFS volume
// can write reliably, while adding no value to the current online session.
constexpr size_t kMaxOfflineTodayItems = 128;
constexpr char kTodayItemsBody[] = "{\"is_finished\":false,\"limit\":128}";
constexpr size_t kMaxResponseBytes = 256 * 1024;
constexpr int kTimeoutMs = 15000;

struct HttpContext {
    std::string body;
    bool overflow = false;
};

esp_err_t HttpEventHandler(esp_http_client_event_t* event) {
    auto* context = event != nullptr ? static_cast<HttpContext*>(event->user_data) : nullptr;
    if (context == nullptr || event->event_id != HTTP_EVENT_ON_DATA ||
        event->data == nullptr || event->data_len <= 0) {
        return ESP_OK;
    }
    if (context->body.size() + static_cast<size_t>(event->data_len) > kMaxResponseBytes) {
        context->overflow = true;
        return ESP_FAIL;
    }
    context->body.append(static_cast<const char*>(event->data),
                         static_cast<size_t>(event->data_len));
    return ESP_OK;
}

const char* JsonStringValue(cJSON* object, const char* key) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : "";
}

bool JsonBoolValue(cJSON* object, const char* key) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsTrue(item);
}

int JsonIntValue(cJSON* object, const char* key) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(item) ? item->valueint : 0;
}

}  // namespace

bool MaimemoOpenApiClient::FetchTodayItems(const std::string& token,
                                           std::vector<MaimemoTodayItem>* items,
                                           std::string* error) const {
    if (items == nullptr || token.size() < 16) {
        if (error != nullptr) {
            *error = "未配置墨墨开放 API Token";
        }
        return false;
    }

    const std::string authorization = "Bearer " + token;
    HttpContext context;
    esp_err_t ret = ESP_FAIL;
    int status = 0;
    auto perform = [&](const char* url) {
        context = HttpContext{};
        esp_http_client_config_t config = {};
        config.url = url;
        config.method = HTTP_METHOD_POST;
        config.timeout_ms = kTimeoutMs;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.event_handler = HttpEventHandler;
        config.user_data = &context;
        config.buffer_size = 2048;
        config.buffer_size_tx = 1024;

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == nullptr) {
            ret = ESP_ERR_NO_MEM;
            status = 0;
            return;
        }
        esp_http_client_set_header(client, "Authorization", authorization.c_str());
        esp_http_client_set_header(client, "Accept", "application/json");
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, kTodayItemsBody, strlen(kTodayItemsBody));
        ret = esp_http_client_perform(client);
        status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);
    };

    perform(kTodayItemsUrl);
    if (ret == ESP_OK && status == 404) {
        ESP_LOGW(kTag, "Official today-items route unavailable; trying legacy route");
        perform(kLegacyTodayItemsUrl);
    }

    if (ret != ESP_OK || context.overflow) {
        if (error != nullptr) {
            *error = context.overflow
                ? "今日任务数据过大"
                : std::string("开放 API 请求失败: ") + esp_err_to_name(ret);
        }
        return false;
    }
    if (status != 200) {
        if (error != nullptr) {
            *error = "开放 API HTTP " + std::to_string(status);
        }
        return false;
    }

    cJSON* root = cJSON_ParseWithLength(context.body.data(), context.body.size());
    cJSON* response_data = root != nullptr
        ? cJSON_GetObjectItemCaseSensitive(root, "data")
        : nullptr;
    cJSON* payload = cJSON_IsObject(response_data) ? response_data : root;
    cJSON* array = payload != nullptr
        ? cJSON_GetObjectItemCaseSensitive(payload, "today_items")
        : nullptr;
    if (!cJSON_IsArray(array)) {
        if (error != nullptr) {
            *error = "今日任务响应格式异常";
        }
        cJSON_Delete(root);
        return false;
    }

    std::vector<MaimemoTodayItem> parsed;
    parsed.reserve(std::min(static_cast<int>(kMaxOfflineTodayItems),
                            cJSON_GetArraySize(array)));
    cJSON* entry = nullptr;
    cJSON_ArrayForEach(entry, array) {
        if (parsed.size() >= kMaxOfflineTodayItems) {
            break;
        }
        MaimemoTodayItem item;
        item.card.voc_id = JsonStringValue(entry, "voc_id");
        item.card.word = JsonStringValue(entry, "voc_spelling");
        item.order = static_cast<uint16_t>(std::max(0, JsonIntValue(entry, "order")));
        item.is_new = JsonBoolValue(entry, "is_new");
        item.is_finished = JsonBoolValue(entry, "is_finished");
        if (!item.card.voc_id.empty() && !item.card.word.empty()) {
            parsed.push_back(std::move(item));
        }
    }
    cJSON_Delete(root);
    std::stable_sort(parsed.begin(), parsed.end(), [](const auto& left, const auto& right) {
        return left.order < right.order;
    });
    *items = std::move(parsed);
    ESP_LOGI(kTag, "Fetched %u today items", static_cast<unsigned>(items->size()));
    return true;
}
