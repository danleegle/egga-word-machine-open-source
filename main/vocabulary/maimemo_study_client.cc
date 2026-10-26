#include "maimemo_study_client.h"

#include "board.h"
#include "maimemo_auth_store.h"
#include "network_interface.h"
#include "web_socket.h"
#include "wifi_manager.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <utility>
#include <vector>

namespace {

constexpr char kTag[] = "MaimemoClient";
constexpr char kMaimemoLoginPageUrl[] = "https://www.maimemo.com/home/login?continue=home/web_study";
constexpr char kMaimemoLoginUrl[] = "https://www.maimemo.com/auth/login";
constexpr char kMaimemoWebStudyUrl[] = "https://www.maimemo.com/home/web_study";
constexpr char kMaimemoPrecheckUrl[] = "https://tc-apis.maimemo.com/study/api/v1/webstudy/precheck";
constexpr char kMaimemoDailyTargetCompleteStatus[] = "daily target complete";
constexpr char kWebStudyWsHost[] = "wss://tc-apis.maimemo.com/study/ws/webstudy?token=";
constexpr int kEventSystemReady = 1;
constexpr int kEventSystemPing = 2;
constexpr int kEventInitStudy = 1001;
constexpr int kEventGetWord = 1002;
constexpr int kEventSubmitResponse = 1003;
constexpr uint32_t kSubmitRecallDurationMs = 1000;
constexpr uint32_t kSubmitStudyDurationMs = 2000;
// WebStudy can validate elapsed time on its own server clock. The normal path
// submits immediately (matching the responsive 2.62 behavior); this value is
// only used to recover from an explicit "study duration illegal" response.
constexpr uint32_t kServerMinStudyWaitMs = 2500;
constexpr int kInitStudyAttempts = 2;
constexpr uint32_t kInitStudyRetryDelayMs = 350;
constexpr int kGetWordDecodeAttempts = 3;
constexpr uint32_t kGetWordRetryDelayMs = 300;
constexpr int kSubmitNextWordFetchAttempts = 1;
constexpr uint32_t kSubmitNextWordRetryDelayMs = 700;
constexpr int kWifiConnectWaitAttempts = 30;
constexpr uint32_t kWifiConnectWaitDelayMs = 250;
constexpr uint8_t kStudyMethodEnCn = 0;
constexpr uint8_t kStudyMethodCnEn = 1;
constexpr uint32_t kAuthRefreshTimeoutMs = 20000;
constexpr size_t kAuthRefreshMaxBytes = 64 * 1024;
constexpr char kStudyMethodSettingCode[] = "study.algorithm.study_method";
constexpr char kStudyMethodCnEnSettingValue[] = "CE";
// The recursive protobuf dump can emit hundreds of synchronous UART log lines
// while the CPU frequency is changing. Keep it available for targeted debugging,
// but disable it in normal firmware to avoid starving the interrupt watchdog.
constexpr bool kEnablePayloadDiagnostics = false;

enum WireType : uint32_t {
    kVarint = 0,
    kFixed64 = 1,
    kLengthDelimited = 2,
    kFixed32 = 5,
};

struct FieldView {
    uint32_t number = 0;
    uint32_t wire_type = 0;
    uint64_t varint = 0;
    const uint8_t* bytes = nullptr;
    size_t bytes_len = 0;
};

struct HttpResult {
    int status = -1;
    std::string body;
    std::string location;
};

struct HttpContext {
    HttpResult* result = nullptr;
    std::map<std::string, std::string>* cookies = nullptr;
};

bool EqualsIgnoreCase(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    while (*a != '\0' && *b != '\0') {
        if (std::tolower(static_cast<unsigned char>(*a)) !=
            std::tolower(static_cast<unsigned char>(*b))) {
            return false;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

void StoreSetCookie(std::map<std::string, std::string>* cookies, const char* value) {
    if (cookies == nullptr || value == nullptr || value[0] == '\0') {
        return;
    }
    std::string pair(value);
    const size_t semi = pair.find(';');
    if (semi != std::string::npos) {
        pair.resize(semi);
    }
    const size_t eq = pair.find('=');
    if (eq == std::string::npos || eq == 0) {
        return;
    }
    (*cookies)[pair.substr(0, eq)] = pair.substr(eq + 1);
}

std::string CookieHeader(const std::map<std::string, std::string>& cookies) {
    std::string out;
    for (const auto& item : cookies) {
        if (!out.empty()) {
            out += "; ";
        }
        out += item.first;
        out.push_back('=');
        out += item.second;
    }
    return out;
}

esp_err_t HttpEventHandler(esp_http_client_event_t* event) {
    HttpContext* context = event != nullptr ? static_cast<HttpContext*>(event->user_data) : nullptr;
    if (context == nullptr || context->result == nullptr) {
        return ESP_OK;
    }
    switch (event->event_id) {
        case HTTP_EVENT_ON_HEADER:
            if (EqualsIgnoreCase(event->header_key, "set-cookie")) {
                StoreSetCookie(context->cookies, event->header_value);
            } else if (EqualsIgnoreCase(event->header_key, "location")) {
                context->result->location = event->header_value != nullptr ? event->header_value : "";
            }
            break;
        case HTTP_EVENT_ON_DATA:
            if (event->data != nullptr && event->data_len > 0 &&
                context->result->body.size() < kAuthRefreshMaxBytes) {
                const size_t room = kAuthRefreshMaxBytes - context->result->body.size();
                context->result->body.append(static_cast<const char*>(event->data),
                                             std::min(room, static_cast<size_t>(event->data_len)));
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

void WriteVarint(std::vector<uint8_t>* out, uint64_t value) {
    while (value >= 0x80) {
        out->push_back(static_cast<uint8_t>(value | 0x80));
        value >>= 7;
    }
    out->push_back(static_cast<uint8_t>(value));
}

void WriteKey(std::vector<uint8_t>* out, uint32_t field_number, uint32_t wire_type) {
    WriteVarint(out, (static_cast<uint64_t>(field_number) << 3) | wire_type);
}

void WriteVarintField(std::vector<uint8_t>* out, uint32_t field_number, uint64_t value) {
    WriteKey(out, field_number, kVarint);
    WriteVarint(out, value);
}

void WriteBytesField(std::vector<uint8_t>* out,
                     uint32_t field_number,
                     const uint8_t* data,
                     size_t len) {
    WriteKey(out, field_number, kLengthDelimited);
    WriteVarint(out, len);
    if (data != nullptr && len > 0) {
        out->insert(out->end(), data, data + len);
    }
}

void WriteStringField(std::vector<uint8_t>* out,
                      uint32_t field_number,
                      const std::string& value) {
    WriteBytesField(out,
                    field_number,
                    reinterpret_cast<const uint8_t*>(value.data()),
                    value.size());
}

bool StatusLooksPermissionDenied(const std::string& status) {
    return status.find("permission denied") != std::string::npos ||
           status.find("unauthorized") != std::string::npos ||
           status.find("auth expired") != std::string::npos ||
           status.find("HTTP 401") != std::string::npos ||
           status.find("HTTP 403") != std::string::npos ||
           status.find("认证") != std::string::npos;
}

#if 0
bool RefreshTokenWithCredentials(const MaimemoAuthConfig& auth,
                                 MaimemoAuthConfig* refreshed,
                                 std::string* error) {
    if (!auth.credential_saved || auth.account.empty() || auth.password.empty()) {
        if (error != nullptr) {
            *error = "墨墨认证已失效，请重新导入";
        }
        return false;
    }
    if (!WifiManager::GetInstance().IsConnected()) {
        if (error != nullptr) {
            *error = "WiFi not connected";
        }
        return false;
    }

    const std::string body = std::string("{\"account\":\"") + JsonEscape(auth.account) +
        "\",\"password\":\"" + JsonEscape(auth.password) + "\"}";
    esp_http_client_config_t config = {};
    config.url = kMaimemoLoginUrl;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = kAuthRefreshTimeoutMs;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 1024;
    config.buffer_size_tx = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        if (error != nullptr) {
            *error = "token refresh init failed";
        }
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    bool ok = false;
    std::string response;
    esp_err_t err = esp_http_client_open(client, body.size());
    if (err != ESP_OK) {
        if (error != nullptr) {
            *error = std::string("token refresh failed: ") + esp_err_to_name(err);
        }
    } else {
        const int written = esp_http_client_write(client, body.data(), body.size());
        if (written < 0 || static_cast<size_t>(written) != body.size()) {
            if (error != nullptr) {
                *error = "token refresh write failed";
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
        (void)esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        char buffer[512] = {};
        while (response.size() < kAuthRefreshMaxBytes) {
            const int read = esp_http_client_read(client, buffer, sizeof(buffer));
            if (read <= 0) {
                break;
            }
            response.append(buffer, static_cast<size_t>(read));
        }
        if (status != 200) {
            if (error != nullptr) {
                *error = "token refresh HTTP " + std::to_string(status);
            }
        } else {
            cJSON* json = cJSON_Parse(response.c_str());
            cJSON* token = json != nullptr ? cJSON_GetObjectItemCaseSensitive(json, "token") : nullptr;
            cJSON* account_masked = json != nullptr ? cJSON_GetObjectItemCaseSensitive(json, "accountMasked") : nullptr;
            cJSON* obtained_at = json != nullptr ? cJSON_GetObjectItemCaseSensitive(json, "obtainedAt") : nullptr;
            if (cJSON_IsString(token) && token->valuestring != nullptr && strlen(token->valuestring) >= 16) {
                if (refreshed != nullptr) {
                    *refreshed = auth;
                    refreshed->token = token->valuestring;
                    if (cJSON_IsString(account_masked) && account_masked->valuestring != nullptr) {
                        refreshed->account_masked = account_masked->valuestring;
                    }
                    if (cJSON_IsString(obtained_at) && obtained_at->valuestring != nullptr) {
                        refreshed->obtained_at = obtained_at->valuestring;
                    }
                    refreshed->configured = true;
                }
                ok = true;
            } else if (error != nullptr) {
                *error = "token refresh response invalid";
            }
            if (json != nullptr) {
                cJSON_Delete(json);
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

bool StartWebStudySession(const std::string& token, std::string* error) {
    if (token.empty()) {
        if (error != nullptr) {
            *error = "import auth first";
        }
        return false;
    }
    if (!WifiManager::GetInstance().IsConnected()) {
        if (error != nullptr) {
            *error = "WiFi not connected";
        }
        return false;
    }

    esp_http_client_config_t config = {};
    config.url = kWebStudyStartUrl;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = kAuthRefreshTimeoutMs;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.buffer_size = 1024;
    config.buffer_size_tx = 512;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        if (error != nullptr) {
            *error = "webstudy start init failed";
        }
        return false;
    }

    const std::string authorization = "Bearer " + token;
    esp_http_client_set_header(client, "Authorization", authorization.c_str());
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Origin", "https://www.maimemo.com");
    esp_http_client_set_header(client, "Referer", "https://www.maimemo.com/home/web_study");

    bool ok = false;
    std::string response;
    const char body[] = "{}";
    esp_err_t err = esp_http_client_open(client, strlen(body));
    if (err != ESP_OK) {
        if (error != nullptr) {
            *error = std::string("webstudy start failed: ") + esp_err_to_name(err);
        }
    } else {
        const int written = esp_http_client_write(client, body, strlen(body));
        if (written < 0 || static_cast<size_t>(written) != strlen(body)) {
            if (error != nullptr) {
                *error = "webstudy start write failed";
            }
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return false;
        }
        (void)esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        char buffer[512] = {};
        while (response.size() < kAuthRefreshMaxBytes) {
            const int read = esp_http_client_read(client, buffer, sizeof(buffer));
            if (read <= 0) {
                break;
            }
            response.append(buffer, static_cast<size_t>(read));
        }
        if (status >= 200 && status < 300) {
            ok = true;
        } else if (error != nullptr) {
            ESP_LOGW(kTag, "webstudy start rejected status=%d body=%s", status, response.c_str());
            *error = "webstudy start HTTP " + std::to_string(status);
            if (!response.empty()) {
                *error += ": " + response.substr(0, 80);
            }
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

#endif

std::string UrlEncode(const std::string& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() + 8);
    for (const unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(kHex[(ch >> 4) & 0x0f]);
            out.push_back(kHex[ch & 0x0f]);
        }
    }
    return out;
}

int HexValue(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

std::string UrlDecode(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int hi = HexValue(value[i + 1]);
            const int lo = HexValue(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(value[i] == '+' ? ' ' : value[i]);
    }
    return out;
}

std::string JsonStringValue(cJSON* object, const char* key) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        return {};
    }
    return item->valuestring;
}

bool EnsureWifiReadyForMaimemo(std::string* error) {
    if (WifiManager::GetInstance().IsConnected()) {
        return true;
    }
    Board::GetInstance().RequestNetwork();
    for (int i = 0; i < kWifiConnectWaitAttempts; ++i) {
        if (WifiManager::GetInstance().IsConnected()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(kWifiConnectWaitDelayMs));
    }
    if (error != nullptr) {
        *error = "WiFi not connected";
    }
    return false;
}

bool FetchHttp(const std::string& url,
               esp_http_client_method_t method,
               const std::vector<std::pair<std::string, std::string>>& headers,
               const std::string& body,
               std::map<std::string, std::string>* cookies,
               HttpResult* result,
               std::string* error) {
    if (result == nullptr) {
        return false;
    }
    *result = HttpResult{};
    HttpContext context;
    context.result = result;
    context.cookies = cookies;

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = method;
    config.timeout_ms = kAuthRefreshTimeoutMs;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.event_handler = HttpEventHandler;
    config.user_data = &context;
    config.disable_auto_redirect = true;
    config.buffer_size = 2048;
    config.buffer_size_tx = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        if (error != nullptr) {
            *error = "HTTP init failed";
        }
        return false;
    }

    esp_http_client_set_header(client, "User-Agent", "Mozilla/5.0");
    if (cookies != nullptr && !cookies->empty()) {
        const std::string cookie = CookieHeader(*cookies);
        esp_http_client_set_header(client, "Cookie", cookie.c_str());
    }
    for (const auto& header : headers) {
        esp_http_client_set_header(client, header.first.c_str(), header.second.c_str());
    }
    if (method == HTTP_METHOD_POST) {
        esp_http_client_set_post_field(client, body.data(), body.size());
    }

    const esp_err_t ret = esp_http_client_perform(client);
    result->status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (ret != ESP_OK) {
        if (error != nullptr) {
            *error = std::string("HTTP failed: ") + esp_err_to_name(ret);
        }
        return false;
    }
    return true;
}

bool IsRedirectStatus(int status) {
    return status >= 300 && status < 400;
}

std::string ResolveUrl(const std::string& base, const std::string& location) {
    if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) {
        return location;
    }
    const size_t scheme = base.find("://");
    if (scheme == std::string::npos) {
        return location;
    }
    const size_t origin_end = base.find('/', scheme + 3);
    const std::string origin = origin_end == std::string::npos ? base : base.substr(0, origin_end);
    if (!location.empty() && location[0] == '/') {
        return origin + location;
    }
    const size_t dir_end = base.rfind('/');
    return (dir_end == std::string::npos ? origin + "/" : base.substr(0, dir_end + 1)) + location;
}

bool IsTokenDelimiter(char ch) {
    return ch == '\0' || ch == '"' || ch == '\'' || ch == '&' || ch == '#' ||
           ch == '<' || ch == '>' || std::isspace(static_cast<unsigned char>(ch));
}

std::string FindToken(const std::string& value) {
    size_t pos = value.find("token=");
    if (pos != std::string::npos) {
        pos += strlen("token=");
        size_t end = pos;
        while (end < value.size() && !IsTokenDelimiter(value[end])) {
            ++end;
        }
        if (end > pos) {
            return UrlDecode(value.substr(pos, end - pos));
        }
    }
    pos = value.find("Bearer ");
    if (pos != std::string::npos) {
        pos += strlen("Bearer ");
        size_t end = pos;
        while (end < value.size() && !IsTokenDelimiter(value[end])) {
            ++end;
        }
        if (end > pos) {
            return value.substr(pos, end - pos);
        }
    }
    for (const char quote : {'"', '\''}) {
        std::string key;
        key.push_back(quote);
        key += "token";
        key.push_back(quote);
        pos = value.find(key);
        if (pos == std::string::npos) {
            continue;
        }
        pos = value.find(':', pos + key.size());
        if (pos == std::string::npos) {
            continue;
        }
        pos = value.find_first_of("\"'", pos + 1);
        if (pos == std::string::npos) {
            continue;
        }
        const char token_quote = value[pos++];
        const size_t end = value.find(token_quote, pos);
        if (end != std::string::npos && end > pos && end - pos >= 16) {
            return value.substr(pos, end - pos);
        }
    }
    return {};
}

std::string FindWebStudyIframe(const std::string& html) {
    const std::string marker = "https://tc-apis.maimemo.com/webstudy/app";
    const size_t pos = html.find(marker);
    if (pos == std::string::npos) {
        return {};
    }
    size_t end = pos;
    while (end < html.size() && !IsTokenDelimiter(html[end])) {
        ++end;
    }
    return html.substr(pos, end - pos);
}

std::string MaskMaimemoAccount(const std::string& account) {
    if (account.empty()) {
        return {};
    }
    const size_t at = account.find('@');
    if (at != std::string::npos) {
        const std::string name = account.substr(0, at);
        const std::string domain = account.substr(at);
        if (name.size() <= 3) {
            return name.substr(0, 1) + "***" + domain;
        }
        return name.substr(0, 2) + std::string(std::max<size_t>(3, name.size() - 3), '*') +
            name.substr(name.size() - 1) + domain;
    }
    if (account.size() <= 4) {
        return account.substr(0, 1) + "***";
    }
    return account.substr(0, 3) + "****" + account.substr(account.size() - 4);
}

std::string CurrentTimestamp() {
    const time_t now = time(nullptr);
    if (now < 1600000000) {
        return {};
    }
    char buffer[32] = {};
    struct tm time_info = {};
    gmtime_r(&now, &time_info);
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &time_info);
    return buffer;
}

bool FetchWebStudyToken(std::map<std::string, std::string>* cookies,
                        std::string* token,
                        std::string* error) {
    if (cookies == nullptr || token == nullptr) {
        return false;
    }
    std::string url = kMaimemoWebStudyUrl;
    for (int index = 0; index < 6; ++index) {
        HttpResult result;
        if (!FetchHttp(url,
                       HTTP_METHOD_GET,
                       {{"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"},
                        {"Referer", kMaimemoLoginPageUrl}},
                       "",
                       cookies,
                       &result,
                       error)) {
            return false;
        }
        *token = FindToken(result.location);
        if (!token->empty()) {
            return true;
        }
        if (!result.location.empty() && IsRedirectStatus(result.status)) {
            url = ResolveUrl(url, result.location);
            continue;
        }
        *token = FindToken(result.body);
        if (!token->empty()) {
            return true;
        }
        const std::string iframe = FindWebStudyIframe(result.body);
        if (!iframe.empty()) {
            HttpResult iframe_result;
            if (!FetchHttp(iframe,
                           HTTP_METHOD_GET,
                           {{"Accept", "text/html,*/*"},
                            {"Referer", kMaimemoWebStudyUrl}},
                           "",
                           cookies,
                           &iframe_result,
                           error)) {
                return false;
            }
            *token = FindToken(iframe_result.location);
            if (token->empty()) {
                *token = FindToken(iframe_result.body);
            }
            if (!token->empty()) {
                return true;
            }
        }
        break;
    }
    if (error != nullptr) {
        *error = "WebStudy token not found";
    }
    return false;
}

bool PrecheckWebStudyToken(const std::string& token, std::string* error) {
    HttpResult result;
    const std::string authorization = "Bearer " + token;
    if (!FetchHttp(kMaimemoPrecheckUrl,
                   HTTP_METHOD_POST,
                   {{"Authorization", authorization},
                    {"Accept", "application/json"}},
                   "",
                   nullptr,
                   &result,
                   error)) {
        return false;
    }
    if (result.status < 200 || result.status >= 300) {
        if (error != nullptr) {
            *error = "precheck HTTP " + std::to_string(result.status);
        }
        return false;
    }
    cJSON* json = cJSON_Parse(result.body.c_str());
    cJSON* data = json != nullptr ? cJSON_GetObjectItemCaseSensitive(json, "data") : nullptr;
    cJSON* privileged = data != nullptr ? cJSON_GetObjectItemCaseSensitive(data, "is_privileged") : nullptr;
    const bool is_privileged = cJSON_IsTrue(privileged) ||
        (cJSON_IsNumber(privileged) && privileged->valueint != 0);
    if (json != nullptr) {
        cJSON_Delete(json);
    }
    if (!is_privileged) {
        if (error != nullptr) {
            *error = "WebStudy access is not enabled for this account";
        }
        return false;
    }
    return true;
}

bool RefreshTokenWithCredentials(const MaimemoAuthConfig& auth,
                                 MaimemoAuthConfig* refreshed,
                                 std::string* error) {
    ESP_LOGI(kTag, "refresh auth with saved credentials");
    if (!auth.credential_saved || auth.account.empty() || auth.password.empty()) {
        if (error != nullptr) {
            *error = "auth expired; import account/password again";
        }
        return false;
    }
    if (!EnsureWifiReadyForMaimemo(error)) {
        return false;
    }

    std::map<std::string, std::string> cookies;
    HttpResult result;
    if (!FetchHttp(kMaimemoLoginPageUrl,
                   HTTP_METHOD_GET,
                   {{"Accept", "text/html,*/*"}},
                   "",
                   &cookies,
                   &result,
                   error)) {
        ESP_LOGW(kTag, "refresh login page failed: %s", error != nullptr ? error->c_str() : "");
        return false;
    }
    ESP_LOGI(kTag, "refresh login page status=%d cookies=%u",
             result.status,
             static_cast<unsigned>(cookies.size()));

    const std::string body = "email=" + UrlEncode(auth.account) +
        "&password=" + UrlEncode(auth.password);
    if (!FetchHttp(kMaimemoLoginUrl,
                   HTTP_METHOD_POST,
                   {{"Accept", "application/json, text/javascript, */*; q=0.01"},
                    {"Content-Type", "application/x-www-form-urlencoded; charset=UTF-8"},
                    {"Origin", "https://www.maimemo.com"},
                    {"Referer", kMaimemoLoginPageUrl},
                    {"X-Requested-With", "XMLHttpRequest"}},
                   body,
                   &cookies,
                   &result,
                   error)) {
        ESP_LOGW(kTag, "refresh login request failed: %s", error != nullptr ? error->c_str() : "");
        return false;
    }
    ESP_LOGI(kTag, "refresh login status=%d body_len=%u cookies=%u",
             result.status,
             static_cast<unsigned>(result.body.size()),
             static_cast<unsigned>(cookies.size()));
    if (result.status < 200 || result.status >= 300) {
        if (error != nullptr) {
            *error = "login HTTP " + std::to_string(result.status);
        }
        return false;
    }
    cJSON* login_json = cJSON_Parse(result.body.c_str());
    if (login_json != nullptr) {
        cJSON* valid = cJSON_GetObjectItemCaseSensitive(login_json, "valid");
        const bool rejected = (cJSON_IsNumber(valid) && valid->valueint == 0) ||
            (cJSON_IsBool(valid) && !cJSON_IsTrue(valid));
        const std::string login_error = JsonStringValue(login_json, "error");
        cJSON_Delete(login_json);
        if (rejected) {
            if (error != nullptr) {
                *error = login_error.empty() ? "maimemo login rejected" : login_error;
            }
            return false;
        }
    }

    std::string token;
    if (!FetchWebStudyToken(&cookies, &token, error)) {
        ESP_LOGW(kTag, "refresh webstudy token failed: %s", error != nullptr ? error->c_str() : "");
        return false;
    }
    ESP_LOGI(kTag, "refresh webstudy token len=%u", static_cast<unsigned>(token.size()));
    if (token.size() < 16 || token.size() > 3900) {
        if (error != nullptr) {
            *error = "WebStudy token invalid";
        }
        return false;
    }
    if (!PrecheckWebStudyToken(token, error)) {
        ESP_LOGW(kTag, "refresh precheck failed: %s", error != nullptr ? error->c_str() : "");
        return false;
    }
    ESP_LOGI(kTag, "refresh precheck ok");

    if (refreshed != nullptr) {
        *refreshed = auth;
        refreshed->configured = true;
        refreshed->credential_saved = true;
        refreshed->token = token;
        if (refreshed->account_masked.empty()) {
            refreshed->account_masked = MaskMaimemoAccount(auth.account);
        }
        refreshed->obtained_at = CurrentTimestamp();
    }
    return true;
}

bool ReadVarint(const uint8_t* data, size_t len, size_t* offset, uint64_t* value) {
    if (data == nullptr || offset == nullptr || value == nullptr) {
        return false;
    }

    uint64_t result = 0;
    uint32_t shift = 0;
    while (*offset < len && shift <= 63) {
        const uint8_t byte = data[(*offset)++];
        result |= static_cast<uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            *value = result;
            return true;
        }
        shift += 7;
    }
    return false;
}

bool ReadField(const uint8_t* data, size_t len, size_t* offset, FieldView* field) {
    if (field == nullptr) {
        return false;
    }

    uint64_t key = 0;
    if (!ReadVarint(data, len, offset, &key)) {
        return false;
    }

    field->number = static_cast<uint32_t>(key >> 3);
    field->wire_type = static_cast<uint32_t>(key & 0x07);
    field->varint = 0;
    field->bytes = nullptr;
    field->bytes_len = 0;

    switch (field->wire_type) {
        case kVarint:
            return ReadVarint(data, len, offset, &field->varint);
        case kLengthDelimited: {
            uint64_t bytes_len = 0;
            if (!ReadVarint(data, len, offset, &bytes_len) ||
                bytes_len > len ||
                *offset > len - static_cast<size_t>(bytes_len)) {
                return false;
            }
            field->bytes = data + *offset;
            field->bytes_len = static_cast<size_t>(bytes_len);
            *offset += field->bytes_len;
            return true;
        }
        case kFixed32:
            if (*offset > len - 4) {
                return false;
            }
            *offset += 4;
            return true;
        case kFixed64:
            if (*offset > len - 8) {
                return false;
            }
            *offset += 8;
            return true;
        default:
            return false;
    }
}

std::string FieldString(const FieldView& field) {
    if (field.bytes == nullptr || field.bytes_len == 0) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(field.bytes), field.bytes_len);
}

void AppendLine(std::string* target, const std::string& text, size_t max_len) {
    if (target == nullptr || text.empty() || target->size() >= max_len) {
        return;
    }
    if (!target->empty()) {
        target->push_back('\n');
    }
    const size_t remaining = max_len > target->size() ? max_len - target->size() : 0;
    target->append(text, 0, remaining);
}

bool IsAsciiText(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) {
        return false;
    }

    for (size_t i = 0; i < len; ++i) {
        const unsigned char ch = data[i];
        if (ch < 0x20 || ch > 0x7e) {
            return false;
        }
    }
    return true;
}

std::string HexSnippet(const uint8_t* data, size_t len, size_t max_bytes = 32) {
    if (data == nullptr || len == 0) {
        return {};
    }

    static constexpr char kHex[] = "0123456789abcdef";
    const size_t bytes_to_show = std::min(len, max_bytes);
    std::string out;
    out.reserve(bytes_to_show * 3 + 4);
    for (size_t i = 0; i < bytes_to_show; ++i) {
        if (!out.empty()) {
            out.push_back(' ');
        }
        const uint8_t byte = data[i];
        out.push_back(kHex[(byte >> 4) & 0x0f]);
        out.push_back(kHex[byte & 0x0f]);
    }
    if (bytes_to_show < len) {
        out += " ...";
    }
    return out;
}

bool LooksLikeOpaqueHexId(const std::string& value) {
    if (value.size() < 16) {
        return false;
    }

    for (unsigned char ch : value) {
        if (!std::isxdigit(ch)) {
            return false;
        }
    }
    return true;
}

bool IsLikelyWordText(const std::string& value) {
    if (value.empty() || value.size() > 64 || LooksLikeOpaqueHexId(value)) {
        return false;
    }

    bool has_visible = false;
    bool has_letter = false;
    for (unsigned char ch : value) {
        if (ch < 0x20 || ch == 0x7f) {
            return false;
        }
        if (ch >= 0x80) {
            has_visible = true;
            has_letter = true;
            continue;
        }
        if (std::isalpha(ch)) {
            has_letter = true;
        }
        if (std::isalnum(ch) || ch == ' ' || ch == '\'' || ch == '-' || ch == '.' || ch == '/') {
            has_visible = true;
        }
    }
    return has_visible && has_letter;
}

std::string ToLowerAscii(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

bool StartsWithHttpUrl(const std::string& value) {
    return value.rfind("http://", 0) == 0 || value.rfind("https://", 0) == 0;
}

std::string ExtractUrlCandidate(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) {
        return {};
    }

    const char* text = reinterpret_cast<const char*>(data);
    size_t start = len;
    for (size_t i = 0; i + 7 < len; ++i) {
        if (std::memcmp(text + i, "http://", 7) == 0 ||
            (i + 8 <= len && std::memcmp(text + i, "https://", 8) == 0)) {
            start = i;
            break;
        }
    }
    if (start == len) {
        return {};
    }

    size_t end = start;
    while (end < len) {
        const unsigned char ch = data[end];
        if (ch <= 0x20 || ch == '"' || ch == '\'' || ch == '<' || ch == '>' || ch == '\\') {
            break;
        }
        ++end;
    }
    return std::string(text + start, end - start);
}

enum class PronunciationAccent : uint8_t {
    kUnknown = 0,
    kUs,
    kUk,
};

PronunciationAccent DetectPronunciationAccent(const std::string& value) {
    const std::string lower = ToLowerAscii(value);
    if (lower == "us" || lower == "usa" || lower == "american" ||
        lower.find("american") != std::string::npos ||
        lower.find("accent=us") != std::string::npos ||
        lower.find("/us/") != std::string::npos ||
        lower.find("_us.") != std::string::npos ||
        lower.find("-us.") != std::string::npos) {
        return PronunciationAccent::kUs;
    }
    if (lower == "uk" || lower == "british" ||
        lower.find("british") != std::string::npos ||
        lower.find("accent=uk") != std::string::npos ||
        lower.find("/uk/") != std::string::npos ||
        lower.find("_uk.") != std::string::npos ||
        lower.find("-uk.") != std::string::npos) {
        return PronunciationAccent::kUk;
    }
    return PronunciationAccent::kUnknown;
}

bool LooksLikePronunciationUrl(const std::string& url, PronunciationAccent accent) {
    if (!StartsWithHttpUrl(url)) {
        return false;
    }
    if (accent != PronunciationAccent::kUnknown) {
        return true;
    }

    const std::string lower = ToLowerAscii(url);
    return lower.find(".mp3") != std::string::npos ||
           lower.find(".m4a") != std::string::npos ||
           lower.find(".aac") != std::string::npos ||
           lower.find(".wav") != std::string::npos ||
           lower.find(".ogg") != std::string::npos ||
           lower.find("audio") != std::string::npos ||
           lower.find("voice") != std::string::npos ||
           lower.find("sound") != std::string::npos ||
           lower.find("pronunciation") != std::string::npos;
}

void AssignPronunciationUrl(MaimemoWordCard* card,
                            PronunciationAccent accent,
                            const std::string& url) {
    if (card == nullptr || !LooksLikePronunciationUrl(url, accent)) {
        return;
    }

    if (accent == PronunciationAccent::kUs) {
        if (card->pronunciation_url_us.empty()) {
            card->pronunciation_url_us = url;
        }
        return;
    }
    if (accent == PronunciationAccent::kUk) {
        if (card->pronunciation_url_uk.empty()) {
            card->pronunciation_url_uk = url;
        }
        return;
    }

    if (card->pronunciation_url_us == url || card->pronunciation_url_uk == url) {
        return;
    }
    if (card->pronunciation_url_uk.empty()) {
        card->pronunciation_url_uk = url;
    } else if (card->pronunciation_url_us.empty()) {
        card->pronunciation_url_us = url;
    }
}

void CollectPronunciationCandidatesRecursive(const uint8_t* data,
                                             size_t len,
                                             MaimemoWordCard* card,
                                             int depth) {
    constexpr int kMaxDepth = 5;
    constexpr size_t kMaxScanBytes = 4096;
    if (data == nullptr || card == nullptr || len == 0 || len > kMaxScanBytes || depth > kMaxDepth) {
        return;
    }

    PronunciationAccent local_accent = PronunciationAccent::kUnknown;
    if (IsAsciiText(data, len)) {
        local_accent = DetectPronunciationAccent(std::string(reinterpret_cast<const char*>(data), len));
    }
    const std::string direct_url = ExtractUrlCandidate(data, len);
    if (!direct_url.empty()) {
        PronunciationAccent url_accent = DetectPronunciationAccent(direct_url);
        if (url_accent == PronunciationAccent::kUnknown) {
            url_accent = local_accent;
        }
        AssignPronunciationUrl(card, url_accent, direct_url);
    }

    std::vector<std::string> urls;
    size_t offset = 0;
    while (offset < len) {
        FieldView field;
        if (!ReadField(data, len, &offset, &field)) {
            return;
        }
        if (field.wire_type != kLengthDelimited || field.bytes == nullptr || field.bytes_len == 0) {
            continue;
        }

        const std::string field_url = ExtractUrlCandidate(field.bytes, field.bytes_len);
        if (!field_url.empty()) {
            urls.push_back(field_url);
            PronunciationAccent url_accent = DetectPronunciationAccent(field_url);
            if (url_accent != PronunciationAccent::kUnknown) {
                local_accent = url_accent;
            }
        }
        if (IsAsciiText(field.bytes, field.bytes_len)) {
            PronunciationAccent text_accent =
                DetectPronunciationAccent(std::string(reinterpret_cast<const char*>(field.bytes),
                                                      field.bytes_len));
            if (text_accent != PronunciationAccent::kUnknown) {
                local_accent = text_accent;
            }
        }
        CollectPronunciationCandidatesRecursive(field.bytes, field.bytes_len, card, depth + 1);
    }

    for (const std::string& url : urls) {
        PronunciationAccent accent = DetectPronunciationAccent(url);
        if (accent == PronunciationAccent::kUnknown) {
            accent = local_accent;
        }
        AssignPronunciationUrl(card, accent, url);
    }
}

bool SameWordText(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty() || a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) {
            return false;
        }
    }
    return true;
}

bool IsAvoidedCard(const MaimemoWordCard& card,
                   const std::string* avoid_voc_id,
                   const std::string* avoid_word) {
    if (avoid_voc_id != nullptr && !avoid_voc_id->empty() && card.voc_id == *avoid_voc_id) {
        return true;
    }
    if (avoid_word != nullptr && SameWordText(card.word, *avoid_word)) {
        return true;
    }
    return false;
}

bool DecodeProgressMessage(const uint8_t* data,
                           size_t len,
                           uint16_t* progress_current,
                           uint16_t* daily_target) {
    bool found_current = false;
    bool found_total = false;
    uint16_t parsed_current = 0;
    uint16_t parsed_total = 0;
    size_t offset = 0;
    while (offset < len) {
        FieldView field;
        if (!ReadField(data, len, &offset, &field)) {
            return false;
        }
        if (field.wire_type != kVarint || field.varint > UINT16_MAX) {
            continue;
        }
        if (field.number == 1) {
            parsed_current = static_cast<uint16_t>(field.varint);
            found_current = true;
        } else if (field.number == 2) {
            parsed_total = static_cast<uint16_t>(field.varint);
            found_total = true;
        }
    }
    if (!found_current && !found_total) {
        return false;
    }
    if (progress_current != nullptr && found_current) {
        *progress_current = parsed_current;
    }
    if (daily_target != nullptr && found_total) {
        *daily_target = parsed_total;
    }
    return true;
}

bool DecodeProgressPayloadRecursive(const uint8_t* data,
                                    size_t len,
                                    int depth,
                                    uint16_t* progress_current,
                                    uint16_t* daily_target) {
    constexpr int kMaxDepth = 4;
    constexpr size_t kMaxNestedBytes = 16384;
    if (data == nullptr || len == 0 || depth > kMaxDepth) {
        return false;
    }

    size_t offset = 0;
    while (offset < len) {
        FieldView field;
        if (!ReadField(data, len, &offset, &field)) {
            return false;
        }
        if (field.wire_type != kLengthDelimited || field.bytes == nullptr || field.bytes_len == 0) {
            continue;
        }

        if (field.number == 22 &&
            DecodeProgressMessage(field.bytes, field.bytes_len, progress_current, daily_target)) {
            return true;
        }
        if (field.bytes_len <= kMaxNestedBytes &&
            DecodeProgressPayloadRecursive(field.bytes,
                                           field.bytes_len,
                                           depth + 1,
                                           progress_current,
                                           daily_target)) {
            return true;
        }
    }
    return false;
}

bool DecodeProgressPayload(const std::vector<uint8_t>& payload,
                           uint16_t* progress_current,
                           uint16_t* daily_target) {
    if (payload.empty()) {
        return false;
    }
    return DecodeProgressPayloadRecursive(payload.data(),
                                          payload.size(),
                                          0,
                                          progress_current,
                                          daily_target);
}

bool DecodeStringValueRecursive(const uint8_t* data, size_t len, std::string* value, int depth) {
    if (data == nullptr || value == nullptr || len == 0 || depth > 6) {
        return false;
    }

    size_t offset = 0;
    while (offset < len) {
        FieldView field;
        if (!ReadField(data, len, &offset, &field)) {
            return false;
        }
        if (field.number == 3 && field.wire_type == kLengthDelimited) {
            *value = FieldString(field);
            return true;
        }
        if (field.wire_type == kLengthDelimited &&
            field.bytes != nullptr &&
            DecodeStringValueRecursive(field.bytes, field.bytes_len, value, depth + 1)) {
            return true;
        }
    }
    return false;
}

bool DecodeStringValue(const uint8_t* data, size_t len, std::string* value) {
    return DecodeStringValueRecursive(data, len, value, 0);
}

bool DecodeUserSettingMessage(const uint8_t* data,
                              size_t len,
                              std::string* code,
                              std::string* value) {
    if (data == nullptr || len == 0 || code == nullptr || value == nullptr) {
        return false;
    }

    size_t offset = 0;
    while (offset < len) {
        FieldView field;
        if (!ReadField(data, len, &offset, &field)) {
            return false;
        }
        if (field.number == 1 && field.wire_type == kLengthDelimited) {
            *code = FieldString(field);
        } else if (field.number == 2 && field.wire_type == kLengthDelimited) {
            std::string decoded_value;
            if (DecodeStringValue(field.bytes, field.bytes_len, &decoded_value)) {
                *value = std::move(decoded_value);
            }
        }
    }
    return !code->empty();
}

void ApplyInitStudySettings(const std::vector<uint8_t>& payload, uint8_t* study_method) {
    if (payload.empty() || study_method == nullptr) {
        return;
    }

    size_t offset = 0;
    while (offset < payload.size()) {
        FieldView field;
        if (!ReadField(payload.data(), payload.size(), &offset, &field)) {
            return;
        }
        if (field.number != 3 || field.wire_type != kLengthDelimited || field.bytes == nullptr) {
            continue;
        }

        std::string code;
        std::string value;
        if (!DecodeUserSettingMessage(field.bytes, field.bytes_len, &code, &value) ||
            code != kStudyMethodSettingCode) {
            continue;
        }

        *study_method = value == kStudyMethodCnEnSettingValue
            ? kStudyMethodCnEn
            : kStudyMethodEnCn;
        ESP_LOGI(kTag,
                 "maimemo study method=%s",
                 *study_method == kStudyMethodCnEn ? "STUDY_CN_EN" : "STUDY_EN_CN");
        return;
    }
}

bool LooksLikeProtoMessage(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) {
        return false;
    }

    size_t offset = 0;
    bool has_any = false;
    while (offset < len) {
        FieldView field;
        if (!ReadField(data, len, &offset, &field)) {
            return false;
        }
        has_any = true;
    }
    return has_any;
}

void DumpProtoFields(const uint8_t* data, size_t len, int depth, int max_depth, size_t max_fields) {
    if (data == nullptr || len == 0 || depth > max_depth) {
        return;
    }

    size_t offset = 0;
    size_t field_count = 0;
    const std::string indent(static_cast<size_t>(depth) * 2, ' ');
    while (offset < len && field_count < max_fields) {
        FieldView field;
        if (!ReadField(data, len, &offset, &field)) {
            ESP_LOGI(kTag,
                     "%s<decode error at byte %u/%u>",
                     indent.c_str(),
                     static_cast<unsigned>(offset),
                     static_cast<unsigned>(len));
            return;
        }

        ++field_count;
        if (field.wire_type == kVarint) {
            ESP_LOGI(kTag,
                     "%sfield %u varint=%llu",
                     indent.c_str(),
                     static_cast<unsigned>(field.number),
                     static_cast<unsigned long long>(field.varint));
            continue;
        }

        if (field.wire_type == kLengthDelimited) {
            if (IsAsciiText(field.bytes, field.bytes_len)) {
                ESP_LOGI(kTag,
                         "%sfield %u text=\"%s\"",
                         indent.c_str(),
                         static_cast<unsigned>(field.number),
                         FieldString(field).c_str());
                continue;
            }

            if (depth < max_depth && field.bytes_len <= 2048 &&
                LooksLikeProtoMessage(field.bytes, field.bytes_len)) {
                ESP_LOGI(kTag,
                         "%sfield %u message(len=%u)",
                         indent.c_str(),
                         static_cast<unsigned>(field.number),
                         static_cast<unsigned>(field.bytes_len));
                DumpProtoFields(field.bytes, field.bytes_len, depth + 1, max_depth, max_fields);
                continue;
            }

            ESP_LOGI(kTag,
                     "%sfield %u bytes(len=%u hex=%s)",
                     indent.c_str(),
                     static_cast<unsigned>(field.number),
                     static_cast<unsigned>(field.bytes_len),
                     HexSnippet(field.bytes, field.bytes_len).c_str());
            continue;
        }

        ESP_LOGI(kTag,
                 "%sfield %u wire=%u",
                 indent.c_str(),
                 static_cast<unsigned>(field.number),
                 static_cast<unsigned>(field.wire_type));
    }
}

void LogPayloadProto(const char* label, const std::vector<uint8_t>& payload) {
    if (!kEnablePayloadDiagnostics) {
        return;
    }
    if (label == nullptr) {
        label = "payload";
    }
    ESP_LOGI(kTag,
             "%s len=%u hex=%s",
             label,
             static_cast<unsigned>(payload.size()),
             HexSnippet(payload.data(), payload.size(), 48).c_str());
    DumpProtoFields(payload.data(), payload.size(), 0, 3, 48);
}

std::vector<uint8_t> EncodeOuterRequest(const std::string& id,
                                        int event,
                                        const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    out.reserve(payload.size() + id.size() + 16);
    WriteStringField(&out, 1, id);
    WriteStringField(&out, 2, "");
    WriteVarintField(&out, 3, static_cast<uint32_t>(event));
    WriteBytesField(&out, 4, payload.data(), payload.size());
    WriteVarintField(&out, 5, 1);
    return out;
}

std::vector<uint8_t> EncodeOuterResponse(const std::string& reply_id,
                                         int event,
                                         const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    out.reserve(payload.size() + reply_id.size() + 16);
    WriteStringField(&out, 1, "");
    WriteStringField(&out, 2, reply_id);
    WriteVarintField(&out, 3, static_cast<uint32_t>(event));
    WriteBytesField(&out, 4, payload.data(), payload.size());
    WriteVarintField(&out, 5, 1);
    return out;
}

std::vector<uint8_t> EncodeGetWordRequest(bool back) {
    std::vector<uint8_t> out;
    WriteVarintField(&out, 1, back ? 1 : 0);
    return out;
}

std::vector<uint8_t> EncodeSubmitRequest(const std::string& voc_id,
                                         MaimemoAnswer answer,
                                         uint32_t recall_duration_ms,
                                         uint32_t study_duration_ms,
                                         uint8_t study_method) {
    (void)recall_duration_ms;
    (void)study_duration_ms;
    std::vector<uint8_t> out;
    WriteStringField(&out, 1, voc_id);
    WriteVarintField(&out, 2, static_cast<uint8_t>(answer));
    if (study_method != kStudyMethodEnCn) {
        WriteVarintField(&out, 3, study_method);
    }
    // WebStudy validates these fields as part of its current server session,
    // so replaying wall-clock durations captured before a reconnect is invalid.
    // Use the stable protocol values used by the online-only implementation.
    WriteVarintField(&out, 4, kSubmitRecallDurationMs);
    WriteVarintField(&out, 5, kSubmitStudyDurationMs);
    return out;
}

bool DecodeProtocolError(const uint8_t* data,
                         size_t len,
                         std::string* code,
                         std::string* message,
                         std::string* info);

bool DecodeOuter(const char* data, size_t len, MaimemoStudyClient::WsMessage* message) {
    if (data == nullptr || message == nullptr) {
        return false;
    }

    const auto* bytes = reinterpret_cast<const uint8_t*>(data);
    size_t offset = 0;
    bool has_any = false;
    while (offset < len) {
        FieldView field;
        if (!ReadField(bytes, len, &offset, &field)) {
            return false;
        }
        has_any = true;
        switch (field.number) {
            case 1:
                if (field.wire_type == kLengthDelimited) {
                    message->id = FieldString(field);
                }
                break;
            case 2:
                if (field.wire_type == kLengthDelimited) {
                    message->reply_id = FieldString(field);
                }
                break;
            case 3:
                if (field.wire_type == kVarint) {
                    message->event = static_cast<int>(field.varint);
                }
                break;
            case 4:
                if (field.wire_type == kLengthDelimited && field.bytes != nullptr) {
                    message->data.assign(field.bytes, field.bytes + field.bytes_len);
                }
                break;
            case 5:
                if (field.wire_type == kVarint) {
                    message->success = field.varint != 0;
                    message->has_success = true;
                }
                break;
            case 6:
                if (field.wire_type == kLengthDelimited && field.bytes != nullptr &&
                    message->error_code.empty() && message->error_message.empty()) {
                    DecodeProtocolError(field.bytes,
                                        field.bytes_len,
                                        &message->error_code,
                                        &message->error_message,
                                        &message->error_info);
                }
                break;
            default:
                break;
        }
    }
    return has_any;
}

bool DecodeProtocolError(const uint8_t* data,
                         size_t len,
                         std::string* code,
                         std::string* message,
                         std::string* info) {
    if (data == nullptr || len == 0) {
        return false;
    }

    size_t offset = 0;
    bool has_any = false;
    while (offset < len) {
        FieldView field;
        if (!ReadField(data, len, &offset, &field)) {
            return false;
        }
        if (field.wire_type != kLengthDelimited) {
            continue;
        }
        has_any = true;
        if (field.number == 1 && code != nullptr) {
            *code = FieldString(field);
        } else if (field.number == 2 && message != nullptr) {
            *message = FieldString(field);
        } else if (field.number == 3 && info != nullptr) {
            *info = FieldString(field);
        }
    }
    return has_any;
}

bool DecodeWordMessage(const uint8_t* data, size_t len, MaimemoWordCard* card) {
    size_t offset = 0;
    bool found_word = false;
    while (offset < len) {
        FieldView field;
        if (!ReadField(data, len, &offset, &field)) {
            return false;
        }
        if (field.wire_type != kLengthDelimited) {
            continue;
        }
        CollectPronunciationCandidatesRecursive(field.bytes, field.bytes_len, card, 0);
        const std::string value = FieldString(field);
        switch (field.number) {
            case 1:
                card->voc_id = value;
                break;
            case 3:
                if (IsLikelyWordText(value)) {
                    card->word = value;
                    found_word = true;
                } else {
                    ESP_LOGW(kTag,
                             "ignore suspicious word field len=%u hex=%s",
                             static_cast<unsigned>(value.size()),
                             HexSnippet(reinterpret_cast<const uint8_t*>(value.data()),
                                        value.size(),
                                        24).c_str());
                }
                break;
            case 4:
                if (card->phonetic.empty()) {
                    card->phonetic = value;
                }
                break;
            case 5:
                if (card->phonetic.empty()) {
                    card->phonetic = value;
                } else if (!value.empty() && card->phonetic.find(value) == std::string::npos) {
                    card->phonetic += " / ";
                    card->phonetic += value;
                }
                break;
            default:
                break;
        }
    }
    return found_word;
}

bool DecodeInterpretationMessage(const uint8_t* data, size_t len, std::string* meaning) {
    size_t offset = 0;
    while (offset < len) {
        FieldView field;
        if (!ReadField(data, len, &offset, &field)) {
            return false;
        }
        if (field.number == 3 && field.wire_type == kLengthDelimited) {
            AppendLine(meaning, FieldString(field), 160);
        }
    }
    return meaning != nullptr && !meaning->empty();
}

bool DecodePhraseMessage(const uint8_t* data,
                         size_t len,
                         std::string* example,
                         std::string* translation) {
    size_t offset = 0;
    while (offset < len) {
        FieldView field;
        if (!ReadField(data, len, &offset, &field)) {
            return false;
        }
        if (field.wire_type != kLengthDelimited) {
            continue;
        }
        if (field.number == 3 && example != nullptr && example->empty()) {
            *example = FieldString(field);
        } else if (field.number == 4 && translation != nullptr && translation->empty()) {
            *translation = FieldString(field);
        }
    }
    return example != nullptr && !example->empty();
}

bool DecodeNoteMessage(const uint8_t* data,
                       size_t len,
                       std::string* note_type,
                       std::string* note) {
    size_t offset = 0;
    while (offset < len) {
        FieldView field;
        if (!ReadField(data, len, &offset, &field)) {
            return false;
        }
        if (field.wire_type != kLengthDelimited) {
            continue;
        }
        if (field.number == 3 && note_type != nullptr && note_type->empty()) {
            *note_type = FieldString(field);
        } else if (field.number == 4 && note != nullptr && note->empty()) {
            *note = FieldString(field);
        }
    }
    return note != nullptr && !note->empty();
}

std::string CompactNoteLine(std::string value) {
    bool previous_space = false;
    size_t out = 0;
    for (size_t i = 0; i < value.size(); ++i) {
        char ch = value[i];
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            ch = ' ';
        }
        if (ch == ' ') {
            if (previous_space) {
                continue;
            }
            previous_space = true;
        } else {
            previous_space = false;
        }
        value[out++] = ch;
    }
    value.resize(out);
    while (!value.empty() && value.front() == ' ') {
        value.erase(value.begin());
    }
    while (!value.empty() && value.back() == ' ') {
        value.pop_back();
    }
    return value;
}

int ScoreWordCard(const MaimemoWordCard& card) {
    int score = 0;
    if (!card.voc_id.empty()) {
        score += 4;
    }
    if (!card.word.empty()) {
        score += 4;
    }
    if (!card.phonetic.empty()) {
        score += 1;
    }
    if (!card.meaning.empty()) {
        score += 2;
    }
    if (!card.example.empty()) {
        score += 1;
    }
    if (!card.example_translation.empty()) {
        score += 1;
    }
    if (!card.note.empty()) {
        score += 1;
    }
    return score;
}

struct WordCardDecodeSelection {
    MaimemoWordCard preferred_card;
    int preferred_score = -1;
    MaimemoWordCard fallback_card;
    int fallback_score = -1;
};

void ConsiderWordCardCandidate(const MaimemoWordCard& card,
                               const std::string* avoid_voc_id,
                               const std::string* avoid_word,
                               WordCardDecodeSelection* selection) {
    if (selection == nullptr) {
        return;
    }

    const int score = ScoreWordCard(card);
    if (score > selection->fallback_score) {
        selection->fallback_card = card;
        selection->fallback_score = score;
    }
    if (!IsAvoidedCard(card, avoid_voc_id, avoid_word) && score > selection->preferred_score) {
        selection->preferred_card = card;
        selection->preferred_score = score;
    }
}

bool TryDecodeWordCardPayload(const uint8_t* data, size_t len, MaimemoWordCard* card) {
    if (card == nullptr || data == nullptr || len == 0) {
        return false;
    }

    MaimemoWordCard decoded;
    size_t offset = 0;
    bool found_word = false;
    uint8_t note_count = 0;
    while (offset < len) {
        FieldView field;
        if (!ReadField(data, len, &offset, &field)) {
            return false;
        }
        if (field.wire_type != kLengthDelimited || field.bytes == nullptr) {
            continue;
        }
        if (field.number == 1 && decoded.word.empty()) {
            found_word = DecodeWordMessage(field.bytes, field.bytes_len, &decoded) || found_word;
        } else if (field.number == 2) {
            std::string meaning;
            if (DecodeInterpretationMessage(field.bytes, field.bytes_len, &meaning)) {
                AppendLine(&decoded.meaning, meaning, 180);
            }
        } else if (field.number == 3 && decoded.example.empty()) {
            DecodePhraseMessage(field.bytes,
                                field.bytes_len,
                                &decoded.example,
                                &decoded.example_translation);
        } else if (field.number == 4 && note_count < 2) {
            std::string note_type;
            std::string note;
            if (DecodeNoteMessage(field.bytes, field.bytes_len, &note_type, &note)) {
                note = CompactNoteLine(note);
                if (!note.empty()) {
                    if (!note_type.empty()) {
                        note = "[" + note_type + "] " + note;
                    }
                    AppendLine(&decoded.note, note, 220);
                    ++note_count;
                }
            }
        }
    }

    if (!found_word || decoded.word.empty()) {
        return false;
    }
    if (decoded.voc_id.empty()) {
        return false;
    }
    if (decoded.meaning.empty()) {
        decoded.meaning = "meaning unavailable";
    }
    *card = std::move(decoded);
    return true;
}

bool DecodeGetWordResponsePayload(const std::vector<uint8_t>& payload, MaimemoWordCard* card) {
    if (card == nullptr || payload.empty()) {
        return false;
    }
    return TryDecodeWordCardPayload(payload.data(), payload.size(), card);
}

bool DecodeSubmitNextPayload(const std::vector<uint8_t>& payload, MaimemoWordCard* card) {
    if (card == nullptr || payload.empty()) {
        return false;
    }

    size_t offset = 0;
    while (offset < payload.size()) {
        FieldView field;
        if (!ReadField(payload.data(), payload.size(), &offset, &field)) {
            return false;
        }
        if (field.number != 1 || field.wire_type != kLengthDelimited || field.bytes == nullptr) {
            continue;
        }
        if (TryDecodeWordCardPayload(field.bytes, field.bytes_len, card)) {
            return true;
        }
    }
    return false;
}

bool DecodeWordPayloadRecursive(const uint8_t* data,
                                size_t len,
                                int depth,
                                const std::string* avoid_voc_id,
                                const std::string* avoid_word,
                                WordCardDecodeSelection* selection) {
    if (data == nullptr || len == 0 || selection == nullptr) {
        return false;
    }

    bool found = false;
    MaimemoWordCard direct_card;
    if (TryDecodeWordCardPayload(data, len, &direct_card)) {
        ConsiderWordCardCandidate(direct_card, avoid_voc_id, avoid_word, selection);
        found = true;
    }

    if (depth >= 4) {
        return found;
    }

    size_t offset = 0;
    while (offset < len) {
        FieldView field;
        if (!ReadField(data, len, &offset, &field)) {
            return found;
        }
        if (field.wire_type != kLengthDelimited || field.bytes == nullptr || field.bytes_len == 0) {
            continue;
        }
        if (!LooksLikeProtoMessage(field.bytes, field.bytes_len)) {
            continue;
        }
        found = DecodeWordPayloadRecursive(field.bytes,
                                           field.bytes_len,
                                           depth + 1,
                                           avoid_voc_id,
                                           avoid_word,
                                           selection) || found;
    }
    return found;
}

bool DecodeWordPayload(const std::vector<uint8_t>& payload,
                       MaimemoWordCard* card,
                       const std::string* avoid_voc_id = nullptr,
                       const std::string* avoid_word = nullptr,
                       bool require_preferred = false) {
    if (card == nullptr || payload.empty()) {
        return false;
    }

    WordCardDecodeSelection selection;
    if (!DecodeWordPayloadRecursive(payload.data(),
                                    payload.size(),
                                    0,
                                    avoid_voc_id,
                                    avoid_word,
                                    &selection)) {
        return false;
    }
    if (selection.preferred_score >= 0) {
        *card = std::move(selection.preferred_card);
        return true;
    }
    if (require_preferred) {
        return false;
    }
    if (selection.fallback_score < 0) {
        return false;
    }
    *card = std::move(selection.fallback_card);
    return true;
}

std::string MakeRequestId(uint32_t seq) {
    char id[32] = {};
    snprintf(id, sizeof(id), "egga-%lu", static_cast<unsigned long>(seq));
    return id;
}

bool IsMustGetWordFirstError(const MaimemoStudyClient::WsMessage& response) {
    return response.error_info.find("must get word first") != std::string::npos ||
        (response.error_code == "webstudy_invalid_param" &&
         response.error_message.find("无效") != std::string::npos);
}

bool IsStudyDurationIllegalError(const MaimemoStudyClient::WsMessage& response) {
    return response.error_info.find("study duration illegal") != std::string::npos ||
        response.error_message.find("study duration illegal") != std::string::npos;
}

}  // namespace

MaimemoStudyClient::MaimemoStudyClient() = default;

MaimemoStudyClient::~MaimemoStudyClient() = default;

bool MaimemoStudyClient::GetWord(MaimemoWordCard* card, bool back) {
    return GetWordAvoiding(card, back, "", "");
}

bool MaimemoStudyClient::GetWordAvoiding(MaimemoWordCard* card,
                                         bool back,
                                         const std::string& avoid_voc_id,
                                         const std::string& avoid_word) {
    if (card == nullptr) {
        return false;
    }

    const bool require_non_avoided = !avoid_voc_id.empty() || !avoid_word.empty();
    bool saw_avoided_card = false;
    for (int attempt = 0; attempt < kGetWordDecodeAttempts; ++attempt) {
        WsMessage response;
        bool retry_after_transport_timeout = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!EnsureReadyLocked(lock)) {
                return false;
            }
            if (!SendRequestWaitLocked(lock, kEventGetWord, EncodeGetWordRequest(back), &response)) {
                // Get-word is read-only and safe to retry. A timed-out socket is
                // invalidated by SendRequestWaitLocked(), so the next attempt
                // establishes a fresh WebStudy session instead of repeatedly
                // sending into a half-open connection.
                retry_after_transport_timeout = websocket_ == nullptr &&
                    attempt + 1 < kGetWordDecodeAttempts;
                if (!retry_after_transport_timeout) {
                    return false;
                }
            }
            if (retry_after_transport_timeout) {
                ESP_LOGW(kTag,
                         "get-word transport failed; reconnecting, retry %d/%d",
                         attempt + 2,
                         kGetWordDecodeAttempts);
            } else {
                if (!word_payload_logged_) {
                    word_payload_logged_ = true;
                    LogPayloadProto("get-word", response.data);
                }
                uint16_t progress_current = progress_current_;
                uint16_t daily_target = daily_target_;
                if (DecodeProgressPayload(response.data, &progress_current, &daily_target)) {
                    progress_current_ = progress_current;
                    daily_target_ = daily_target;
                }
                if (response.data.empty()) {
                    ESP_LOGI(kTag, "get-word returned empty payload; daily target may be complete");
                    SetLastStatusLocked(kMaimemoDailyTargetCompleteStatus);
                    return false;
                }
            }
        }
        if (retry_after_transport_timeout) {
            vTaskDelay(pdMS_TO_TICKS(kGetWordRetryDelayMs));
            continue;
        }

        MaimemoWordCard decoded_card;
        if (DecodeGetWordResponsePayload(response.data, &decoded_card)) {
            if (!IsAvoidedCard(decoded_card,
                               avoid_voc_id.empty() ? nullptr : &avoid_voc_id,
                               avoid_word.empty() ? nullptr : &avoid_word)) {
                *card = std::move(decoded_card);
                std::lock_guard<std::mutex> lock(mutex_);
                last_server_word_received_at_ = std::chrono::steady_clock::now();
                SetLastStatusLocked("loaded");
                return true;
            }
            saw_avoided_card = true;
        } else if (!require_non_avoided &&
                   DecodeWordPayload(response.data, &decoded_card)) {
            *card = std::move(decoded_card);
            std::lock_guard<std::mutex> lock(mutex_);
            last_server_word_received_at_ = std::chrono::steady_clock::now();
            SetLastStatusLocked("loaded");
            return true;
        }

        ESP_LOGW(kTag,
                 "get-word decode/avoid failed, retry %d/%d, payload_len=%u",
                 attempt + 1,
                 kGetWordDecodeAttempts,
                 static_cast<unsigned>(response.data.size()));
        if (attempt + 1 < kGetWordDecodeAttempts) {
            vTaskDelay(pdMS_TO_TICKS(kGetWordRetryDelayMs));
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    SetLastStatusLocked(saw_avoided_card ? "same card repeated" : "decode failed");
    return false;
}

bool MaimemoStudyClient::SubmitResponse(const std::string& voc_id,
                                        const std::string& word,
                                        MaimemoAnswer answer,
                                        uint32_t recall_duration_ms,
                                        uint32_t study_duration_ms,
                                        MaimemoWordCard* next_card,
                                        bool* submitted) {
    if (submitted != nullptr) {
        *submitted = false;
    }
    if (voc_id.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        SetLastStatusLocked("missing voc id");
        return false;
    }

    WsMessage response;
    std::string submit_voc_id = voc_id;
    uint32_t submit_recall_duration_ms = recall_duration_ms;
    uint32_t submit_study_duration_ms = study_duration_ms;
    bool resynced_before_submit = false;
    for (int submit_attempt = 0; submit_attempt < 3; ++submit_attempt) {
        bool must_get_word_first = false;
        bool study_duration_illegal = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!EnsureReadyLocked(lock)) {
                return false;
            }
            const auto payload = EncodeSubmitRequest(submit_voc_id,
                                                     answer,
                                                     submit_recall_duration_ms,
                                                     submit_study_duration_ms,
                                                     study_method_);
            if (!SendRequestWaitLocked(lock, kEventSubmitResponse, payload, &response)) {
                must_get_word_first = IsMustGetWordFirstError(response);
                study_duration_illegal = IsStudyDurationIllegalError(response);
            } else {
                if (submitted != nullptr) {
                    *submitted = true;
                }
                uint16_t progress_current = progress_current_;
                uint16_t daily_target = daily_target_;
                if (DecodeProgressPayload(response.data, &progress_current, &daily_target)) {
                    progress_current_ = progress_current;
                    daily_target_ = daily_target;
                }
                break;
            }
        }

        if (study_duration_illegal && submit_attempt < 2) {
            uint32_t wait_ms = kServerMinStudyWaitMs;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (last_server_word_received_at_.time_since_epoch().count() != 0) {
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - last_server_word_received_at_).count();
                    wait_ms = elapsed < static_cast<int64_t>(kServerMinStudyWaitMs)
                        ? static_cast<uint32_t>(kServerMinStudyWaitMs - elapsed)
                        : 100;
                }
            }
            ESP_LOGW(kTag,
                     "submit rejected for study duration; retrying after %u ms",
                     static_cast<unsigned>(wait_ms));
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
            continue;
        }

        if (!must_get_word_first || next_card == nullptr || resynced_before_submit) {
            return false;
        }

        MaimemoWordCard current_card;
        if (!GetWord(&current_card, false)) {
            return false;
        }
        resynced_before_submit = true;
        if (!IsAvoidedCard(current_card, &voc_id, &word)) {
            *next_card = std::move(current_card);
            std::lock_guard<std::mutex> lock(mutex_);
            SetLastStatusLocked("resynced");
            return true;
        }
        if (current_card.voc_id != voc_id && SameWordText(current_card.word, word)) {
            submit_voc_id = current_card.voc_id;
            submit_recall_duration_ms = 0;
            submit_study_duration_ms = 0;
            ESP_LOGI(kTag, "mapped snapshot voc id to WebStudy id for word=%s", word.c_str());
        }
    }

    if (next_card == nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        SetLastStatusLocked("");
        return true;
    }
    if ((DecodeSubmitNextPayload(response.data, next_card) ||
         DecodeGetWordResponsePayload(response.data, next_card)) &&
        !next_card->word.empty()) {
        if (IsAvoidedCard(*next_card, &voc_id, &word)) {
            ESP_LOGW(kTag,
                     "submit response echoed current card voc_id=%s word=%s, refetch next word",
                     voc_id.c_str(),
                     word.c_str());
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            SetLastStatusLocked("");
            ESP_LOGI(kTag, "WebStudy submit delivered next card in the submit response");
            return true;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!submit_payload_logged_) {
            submit_payload_logged_ = true;
            LogPayloadProto("submit-response", response.data);
        }
    }
    ESP_LOGI(kTag, "WebStudy submit response had no usable next card; requesting it separately");
    for (int attempt = 0; attempt < kSubmitNextWordFetchAttempts; ++attempt) {
        if (!GetWordAvoiding(next_card, false, voc_id, word)) {
            if (LastStatus() == kMaimemoDailyTargetCompleteStatus) {
                return false;
            }
            if (attempt + 1 < kSubmitNextWordFetchAttempts) {
                vTaskDelay(pdMS_TO_TICKS(kSubmitNextWordRetryDelayMs));
                continue;
            }
            return false;
        }
        if (!IsAvoidedCard(*next_card, &voc_id, &word)) {
            if (submitted != nullptr && resynced_before_submit) {
                *submitted = true;
            }
            return true;
        }
        ESP_LOGW(kTag,
                 "get-word echoed current card voc_id=%s word=%s after submit, retry %d/%d",
                 voc_id.c_str(),
                 word.c_str(),
                 attempt + 1,
                 kSubmitNextWordFetchAttempts);
        if (attempt + 1 < kSubmitNextWordFetchAttempts) {
            vTaskDelay(pdMS_TO_TICKS(kSubmitNextWordRetryDelayMs));
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        SetLastStatusLocked("same card repeated");
    }
    return false;
}

std::string MaimemoStudyClient::LastStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_status_;
}

uint16_t MaimemoStudyClient::ProgressCurrent() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return progress_current_;
}

uint16_t MaimemoStudyClient::DailyTarget() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return daily_target_;
}

void MaimemoStudyClient::Reset() {
    std::unique_ptr<WebSocket> websocket_to_close;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        websocket_to_close = std::move(websocket_);
        pending_id_.clear();
        pending_done_ = false;
        system_ready_ = false;
        initialized_ = false;
        progress_current_ = 0;
        daily_target_ = 0;
        study_method_ = kStudyMethodEnCn;
        init_payload_logged_ = false;
        word_payload_logged_ = false;
        submit_payload_logged_ = false;
        token_.clear();
        last_server_word_received_at_ = {};
        SetLastStatusLocked("not connected");
        cv_.notify_all();
    }
    if (websocket_to_close != nullptr) {
        websocket_to_close->Close();
    }
}

bool MaimemoStudyClient::EnsureReadyLocked(std::unique_lock<std::mutex>& lock) {
    MaimemoAuthConfig auth = MaimemoAuthStore::GetInstance().Load();
    if (!auth.configured) {
        auto websocket_to_close = std::move(websocket_);
        pending_id_.clear();
        pending_done_ = false;
        system_ready_ = false;
        initialized_ = false;
        progress_current_ = 0;
        daily_target_ = 0;
        study_method_ = kStudyMethodEnCn;
        init_payload_logged_ = false;
        word_payload_logged_ = false;
        submit_payload_logged_ = false;
        token_.clear();
        last_server_word_received_at_ = {};
        SetLastStatusLocked("import auth first");
        lock.unlock();
        if (websocket_to_close != nullptr) {
            websocket_to_close->Close();
        }
        lock.lock();
        SetLastStatusLocked("import auth first");
        return false;
    }

    if (auth.token.empty()) {
        auto websocket_to_close = std::move(websocket_);
        pending_id_.clear();
        pending_done_ = false;
        system_ready_ = false;
        initialized_ = false;
        progress_current_ = 0;
        daily_target_ = 0;
        study_method_ = kStudyMethodEnCn;
        init_payload_logged_ = false;
        word_payload_logged_ = false;
        submit_payload_logged_ = false;
        token_.clear();
        SetLastStatusLocked("refreshing auth");
        lock.unlock();
        if (websocket_to_close != nullptr) {
            websocket_to_close->Close();
        }
        MaimemoAuthConfig refreshed_auth;
        std::string refresh_error;
        const bool refreshed = RefreshTokenWithCredentials(auth, &refreshed_auth, &refresh_error);
        if (refreshed) {
            (void)MaimemoAuthStore::GetInstance().Save(refreshed_auth);
        }
        lock.lock();
        if (!refreshed) {
            SetLastStatusLocked(refresh_error.empty() ? "auth refresh failed" : refresh_error);
            return false;
        }
        auth = refreshed_auth;
        SetLastStatusLocked("auth refreshed");
    }

    if (token_ != auth.token) {
        auto websocket_to_close = std::move(websocket_);
        initialized_ = false;
        system_ready_ = false;
        progress_current_ = 0;
        daily_target_ = 0;
        study_method_ = kStudyMethodEnCn;
        init_payload_logged_ = false;
        word_payload_logged_ = false;
        submit_payload_logged_ = false;
        token_ = auth.token;
        lock.unlock();
        if (websocket_to_close != nullptr) {
            websocket_to_close->Close();
        }
        lock.lock();
    }

    if (!ConnectLocked(lock)) {
        return false;
    }
    return InitStudyLocked(lock);
}

bool MaimemoStudyClient::ConnectLocked(std::unique_lock<std::mutex>& lock) {
    if (websocket_ != nullptr && websocket_->IsConnected()) {
        if (system_ready_) {
            return true;
        }
        SetLastStatusLocked("waiting system ready");
        const bool ready = cv_.wait_for(
            lock,
            std::chrono::seconds(8),
            [this]() {
                return system_ready_ ||
                    websocket_ == nullptr ||
                    !websocket_->IsConnected();
            });
        if (ready && system_ready_) {
            return true;
        }
        SetLastStatusLocked("system ready timeout");
        return false;
    }

    SetLastStatusLocked("正在连接 WiFi");
    lock.unlock();
    Board::GetInstance().RequestNetwork();
    bool wifi_connected = WifiManager::GetInstance().IsConnected();
    for (int i = 0; !wifi_connected && i < kWifiConnectWaitAttempts; ++i) {
        vTaskDelay(pdMS_TO_TICKS(kWifiConnectWaitDelayMs));
        wifi_connected = WifiManager::GetInstance().IsConnected();
    }
    lock.lock();
    if (!wifi_connected) {
        SetLastStatusLocked("wifi not connected");
        return false;
    }

    NetworkInterface* network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        SetLastStatusLocked("network unavailable");
        return false;
    }

    websocket_ = network->CreateWebSocket();
    if (websocket_ == nullptr) {
        SetLastStatusLocked("websocket unavailable");
        return false;
    }

    pending_id_.clear();
    pending_done_ = false;
    system_ready_ = false;
    initialized_ = false;

    websocket_->SetReceiveBufferSize(8192);
    websocket_->SetHeader("Origin", "https://www.maimemo.com");
    websocket_->SetHeader("User-Agent", "EggaVocabulary/1.0");
    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        HandleWsData(data, len, binary);
    });
    websocket_->OnDisconnected([this]() {
        std::lock_guard<std::mutex> lock(mutex_);
        initialized_ = false;
        system_ready_ = false;
        int close_code = 0;
        std::string close_reason;
        if (websocket_ != nullptr) {
            close_code = websocket_->GetCloseCode();
            close_reason = websocket_->GetCloseReason();
        }
        if (close_code == 3400 || close_code == 3401 ||
            close_reason.find("permission denied") != std::string::npos ||
            close_reason.find("unauthorized") != std::string::npos) {
            SetLastStatusLocked("auth expired");
        } else if (close_code != 0 || !close_reason.empty()) {
            char status[96] = {};
            snprintf(status,
                     sizeof(status),
                     "ws closed %d %s",
                     close_code,
                     close_reason.c_str());
            SetLastStatusLocked(status);
        } else {
            SetLastStatusLocked("disconnected");
        }
        if (!pending_id_.empty()) {
            pending_response_ = WsMessage{};
            pending_response_.reply_id = pending_id_;
            pending_response_.event = 0;
            pending_response_.has_success = true;
            pending_response_.success = false;
            pending_response_.error_message = last_status_;
        }
        pending_done_ = true;
        cv_.notify_all();
    });
    websocket_->OnError([this](int error) {
        std::lock_guard<std::mutex> lock(mutex_);
        char status[64] = {};
        snprintf(status, sizeof(status), "濠⒀佸妼閳攱娼婚悙鏉戝闂佹寧鐟ㄩ?%d", error);
        SetLastStatusLocked(status);
        cv_.notify_all();
    });

    SetLastStatusLocked("connecting");
    const std::string uri = std::string(kWebStudyWsHost) + UrlEncode(token_);
    if (!websocket_->Connect(uri.c_str())) {
        auto websocket_to_close = std::move(websocket_);
        lock.unlock();
        if (websocket_to_close != nullptr) {
            websocket_to_close->Close();
        }
        lock.lock();
        SetLastStatusLocked("ws connect failed");
        return false;
    }

    SetLastStatusLocked("waiting system ready");
    const bool ready = cv_.wait_for(
        lock,
        std::chrono::seconds(8),
        [this]() {
            return system_ready_ ||
                websocket_ == nullptr ||
                !websocket_->IsConnected();
        });
    if (!ready || !system_ready_) {
        auto websocket_to_close = std::move(websocket_);
        lock.unlock();
        if (websocket_to_close != nullptr) {
            websocket_to_close->Close();
        }
        lock.lock();
        SetLastStatusLocked(ready ? "ws disconnected before ready" : "system ready timeout");
        return false;
    }

    SetLastStatusLocked("connected");
    return true;
}

bool MaimemoStudyClient::InitStudyLocked(std::unique_lock<std::mutex>& lock) {
    if (initialized_) {
        return true;
    }

    bool refresh_attempted = false;
    for (int attempt = 0; attempt < kInitStudyAttempts; ++attempt) {
        if (!ConnectLocked(lock)) {
            return false;
        }

        WsMessage response;
        const std::vector<uint8_t> empty_payload;
        if (SendRequestWaitLocked(lock, kEventInitStudy, empty_payload, &response)) {
            if (!init_payload_logged_) {
                init_payload_logged_ = true;
                LogPayloadProto("init-study", response.data);
            }
            ApplyInitStudySettings(response.data, &study_method_);
            initialized_ = response.success && !response.data.empty();
            if (initialized_) {
                SetLastStatusLocked("initialized");
                return true;
            }
            ESP_LOGW(kTag,
                     "init-study returned empty/unsuccessful response, retry %d/%d, has_success=%d success=%d data_len=%u",
                     attempt + 1,
                     kInitStudyAttempts,
                     response.has_success ? 1 : 0,
                     response.success ? 1 : 0,
                     static_cast<unsigned>(response.data.size()));
        } else {
            ESP_LOGW(kTag,
                     "init-study request failed, retry %d/%d, status=%s",
                     attempt + 1,
                     kInitStudyAttempts,
                     last_status_.c_str());
        }

        initialized_ = false;
        if (StatusLooksPermissionDenied(last_status_)) {
            if (!refresh_attempted) {
                refresh_attempted = true;
                auto websocket_to_close = std::move(websocket_);
                pending_id_.clear();
                pending_done_ = false;
                system_ready_ = false;
                const MaimemoAuthConfig current_auth = MaimemoAuthStore::GetInstance().Load();
                MaimemoAuthConfig refreshed_auth;
                std::string refresh_error;
                SetLastStatusLocked("refreshing auth");
                lock.unlock();
                if (websocket_to_close != nullptr) {
                    websocket_to_close->Close();
                }
                const bool refreshed = RefreshTokenWithCredentials(current_auth,
                                                                   &refreshed_auth,
                                                                   &refresh_error);
                if (refreshed) {
                    (void)MaimemoAuthStore::GetInstance().Save(refreshed_auth);
                }
                lock.lock();
                if (refreshed) {
                    token_ = refreshed_auth.token;
                    initialized_ = false;
                    system_ready_ = false;
                    progress_current_ = 0;
                    daily_target_ = 0;
                    study_method_ = kStudyMethodEnCn;
                    init_payload_logged_ = false;
                    word_payload_logged_ = false;
                    submit_payload_logged_ = false;
                    SetLastStatusLocked("auth refreshed");
                    attempt = -1;
                    continue;
                }
                SetLastStatusLocked(refresh_error.empty() ? "auth refresh failed" : refresh_error);
            }
            break;
        }
#if 0
        if (!refresh_attempted && StatusLooksPermissionDenied(last_status_)) {
            refresh_attempted = true;
            auto websocket_to_close = std::move(websocket_);
            pending_id_.clear();
            pending_done_ = false;
            system_ready_ = false;
            const MaimemoAuthConfig current_auth = MaimemoAuthStore::GetInstance().Load();
            MaimemoAuthConfig refreshed_auth;
            std::string refresh_error;
            SetLastStatusLocked("正在刷新墨墨认证...");
            lock.unlock();
            if (websocket_to_close != nullptr) {
                websocket_to_close->Close();
            }
            const bool refreshed = RefreshTokenWithCredentials(current_auth,
                                                               &refreshed_auth,
                                                               &refresh_error);
            if (refreshed) {
                (void)MaimemoAuthStore::GetInstance().Save(refreshed_auth);
            }
            lock.lock();
            if (refreshed) {
                token_ = refreshed_auth.token;
                initialized_ = false;
                system_ready_ = false;
                progress_current_ = 0;
                daily_target_ = 0;
                study_method_ = kStudyMethodEnCn;
                init_payload_logged_ = false;
                word_payload_logged_ = false;
                submit_payload_logged_ = false;
                SetLastStatusLocked("墨墨认证已刷新");
                attempt = -1;
                continue;
            }
            SetLastStatusLocked(refresh_error.empty() ? "墨墨认证已失效，请重新导入" : refresh_error);
        }
#endif

        if (attempt + 1 >= kInitStudyAttempts) {
            break;
        }

        auto websocket_to_close = std::move(websocket_);
        pending_id_.clear();
        pending_done_ = false;
        system_ready_ = false;
        lock.unlock();
        if (websocket_to_close != nullptr) {
            websocket_to_close->Close();
        }
        vTaskDelay(pdMS_TO_TICKS(kInitStudyRetryDelayMs));
        lock.lock();
    }

    if (StatusLooksPermissionDenied(last_status_)) {
        SetLastStatusLocked("auth expired");
    } else if (last_status_.empty() || last_status_ == "connected") {
        SetLastStatusLocked("init failed");
    }
    return false;
}

bool MaimemoStudyClient::SendRequestWaitLocked(std::unique_lock<std::mutex>& lock,
                                               int event,
                                               const std::vector<uint8_t>& payload,
                                               WsMessage* response) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        SetLastStatusLocked("connection unavailable");
        return false;
    }

    const auto request_started_at = std::chrono::steady_clock::now();
    const std::string id = MakeRequestId(++request_seq_);
    pending_id_ = id;
    pending_done_ = false;
    pending_response_ = WsMessage{};
    const std::vector<uint8_t> frame = EncodeOuterRequest(id, event, payload);
    if (!websocket_->Send(frame.data(), frame.size(), true)) {
        pending_id_.clear();
        SetLastStatusLocked("send failed");
        return false;
    }

    const bool completed = cv_.wait_for(
        lock,
        std::chrono::seconds(8),
        [this, &id]() { return pending_done_ && pending_response_.reply_id == id; });
    if (!completed) {
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - request_started_at).count();
        ESP_LOGW(kTag,
                 "WebStudy request timeout event=%d elapsed=%lld ms",
                 event,
                 static_cast<long long>(elapsed_ms));
        // IsConnected() can stay true for a half-open TCP/WebSocket session.
        // Keeping that object made every later answer reuse the same dead
        // connection and time out forever. Detach it under the mutex, close it
        // without the mutex (the disconnect callback also takes mutex_), then
        // force EnsureReadyLocked() to establish a clean session.
        auto websocket_to_close = std::move(websocket_);
        initialized_ = false;
        system_ready_ = false;
        pending_id_.clear();
        pending_done_ = false;
        lock.unlock();
        if (websocket_to_close != nullptr) {
            websocket_to_close->Close();
        }
        lock.lock();
        pending_id_.clear();
        pending_done_ = false;
        SetLastStatusLocked("request timeout; connection reset");
        return false;
    }

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - request_started_at).count();
    ESP_LOGI(kTag,
             "WebStudy response event=%d elapsed=%lld ms",
             event,
             static_cast<long long>(elapsed_ms));
    if (response != nullptr) {
        *response = std::move(pending_response_);
    }
    pending_id_.clear();
    pending_done_ = false;
    if (response != nullptr && (!response->has_success || !response->success)) {
        ESP_LOGW(kTag,
                 "request rejected event=%d reply=%s has_success=%d success=%d error=%s message=%s info=%s data_len=%u hex=%s",
                 response->event,
                 response->reply_id.c_str(),
                 response->has_success ? 1 : 0,
                 response->success ? 1 : 0,
                 response->error_code.c_str(),
                 response->error_message.c_str(),
                 response->error_info.c_str(),
                 static_cast<unsigned>(response->data.size()),
                 HexSnippet(response->data.data(), response->data.size(), 48).c_str());
        if (!response->error_message.empty()) {
            SetLastStatusLocked(response->error_message);
        } else if (!response->error_code.empty()) {
            SetLastStatusLocked(response->error_code);
        } else {
            SetLastStatusLocked("request rejected");
        }
        return false;
    }
    return true;
}

void MaimemoStudyClient::HandleWsData(const char* data, size_t len, bool binary) {
    if (!binary) {
        ESP_LOGW(kTag, "ignore non-binary websocket message, len=%u", static_cast<unsigned>(len));
        return;
    }

    WsMessage message;
    if (!DecodeOuter(data, len, &message)) {
        ESP_LOGW(kTag, "ignore undecodable websocket message, len=%u", static_cast<unsigned>(len));
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    ESP_LOGI(kTag,
             "ws message event=%d id=%s reply=%s has_success=%d success=%d data_len=%u",
             message.event,
             message.id.c_str(),
             message.reply_id.c_str(),
             message.has_success ? 1 : 0,
             message.success ? 1 : 0,
             static_cast<unsigned>(message.data.size()));
    if (message.event == kEventSystemReady) {
        system_ready_ = true;
        cv_.notify_all();
        return;
    }

    if (message.event == kEventSystemPing) {
        const std::string reply_id = !message.id.empty() ? message.id : message.reply_id;
        if (!reply_id.empty() && websocket_ != nullptr && websocket_->IsConnected()) {
            const std::vector<uint8_t> pong = EncodeOuterResponse(reply_id,
                                                                  kEventSystemPing,
                                                                  std::vector<uint8_t>{});
            (void)websocket_->Send(pong.data(), pong.size(), true);
        }
        return;
    }

    if (!pending_id_.empty() &&
        (message.reply_id == pending_id_ || message.id == pending_id_)) {
        if (message.reply_id.empty()) {
            message.reply_id = pending_id_;
        }
        pending_response_ = std::move(message);
        pending_done_ = true;
        cv_.notify_all();
    }
}

void MaimemoStudyClient::SetLastStatusLocked(const std::string& status) {
    last_status_ = status;
}
