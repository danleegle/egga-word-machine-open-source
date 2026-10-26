#include "client_mgmt_service.h"

#include "application.h"
#include "assets_store.h"
#include "board.h"
#include "maimemo_auth_store.h"
#include "ota_service.h"
#include "ssid_manager.h"
#include "wifi_manager.h"

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_spiffs.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <sdkconfig.h>
#if CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED
#include <driver/usb_serial_jtag.h>
#endif
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr char kTag[] = "ClientMgmt";
constexpr char kFramePrefix[] = "ZTX1 ";
constexpr char kWifiConfigPath[] = "/config/wifi.json";
constexpr size_t kMaxLineBytes = 8192;
constexpr size_t kMaxWifiConfigBytes = 2048;
constexpr size_t kMaxMaimemoTokenBytes = 3900;
constexpr size_t kMaxMaimemoAccountBytes = 128;
constexpr size_t kMaxMaimemoPasswordBytes = 128;
constexpr size_t kMaxImportedDeckBytes = 6 * 1024 * 1024;
constexpr size_t kMinReservedAssetBytes = 64 * 1024;
constexpr int kMaxScanResults = 24;
constexpr uint32_t kClientMgmtTaskStackBytes = 12 * 1024;

struct DeckUploadState {
    FILE* file = nullptr;
    std::string deck_id;
    std::string temp_path;
    std::string target_path;
    std::string expected_sha256;
    size_t expected_size = 0;
    size_t written = 0;
    bool sha_initialized = false;
    mbedtls_sha256_context sha = {};
};

DeckUploadState deck_upload;

#if CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED
bool EnsureUsbSerialJtagDriver() {
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        config.tx_buffer_size = 4096;
        config.rx_buffer_size = kMaxLineBytes;
        esp_err_t ret = usb_serial_jtag_driver_install(&config);
        if (ret != ESP_OK) {
            ESP_LOGW(kTag, "failed to install USB serial/JTAG driver: %s", esp_err_to_name(ret));
        }
    }
    return usb_serial_jtag_is_driver_installed();
}
#endif

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
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
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

std::string CStr(const char* value) {
    return value != nullptr ? std::string(value) : std::string();
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

bool IsSafeDeckId(const char* value) {
    if (value == nullptr) {
        return false;
    }
    const size_t len = strlen(value);
    if (len == 0 || len > 48 || value[0] == '.' || value[len - 1] == '.') {
        return false;
    }
    for (const unsigned char ch : std::string(value)) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
              ch == '-' || ch == '_' || ch == '.')) {
            return false;
        }
    }
    return true;
}

bool IsSha256Hex(const char* value) {
    if (value == nullptr || strlen(value) != 64) {
        return false;
    }
    for (const unsigned char ch : std::string(value)) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
            return false;
        }
    }
    return true;
}

std::string ImportedDeckRelativePath(const char* deck_id) {
    uint64_t hash = 1469598103934665603ULL;
    for (const unsigned char ch : std::string(deck_id != nullptr ? deck_id : "")) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    char token[13] = {};
    snprintf(token, sizeof(token), "%012llx",
             static_cast<unsigned long long>(hash & 0x0000ffffffffffffULL));
    // Keep the physical SPIFFS object name below CONFIG_SPIFFS_OBJ_NAME_LEN.
    // The full logical deck id remains in the TSV metadata and is used by UI,
    // progress, selection, and replacement validation.
    return std::string("/decks/u-") + token + ".tsv";
}

std::string Sha256ToHex(const unsigned char digest[32]) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(64, '0');
    for (size_t i = 0; i < 32; ++i) {
        out[i * 2] = kHex[digest[i] >> 4];
        out[i * 2 + 1] = kHex[digest[i] & 0x0f];
    }
    return out;
}

void ResetDeckUpload(bool remove_temp) {
    if (deck_upload.file != nullptr) {
        fclose(deck_upload.file);
        deck_upload.file = nullptr;
    }
    if (deck_upload.sha_initialized) {
        mbedtls_sha256_free(&deck_upload.sha);
        deck_upload.sha_initialized = false;
    }
    if (remove_temp && !deck_upload.temp_path.empty()) {
        remove(deck_upload.temp_path.c_str());
    }
    deck_upload = {};
}

bool HasAssetSpace(size_t expected_size) {
    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info("assets", &total, &used) != ESP_OK || total <= used) {
        return true;
    }
    return expected_size + kMinReservedAssetBytes <= total - used;
}

bool BeginDeckUpload(cJSON* json, std::string* error) {
    const char* deck_id = GetJsonString(json, "deck_id");
    const char* sha256 = GetJsonString(json, "sha256");
    const int size = GetJsonInt(json, "size", 0);
    if (!IsSafeDeckId(deck_id)) {
        *error = "deck_id must contain only lowercase letters, numbers, dot, dash, or underscore";
        return false;
    }
    if (size <= 0 || static_cast<size_t>(size) > kMaxImportedDeckBytes) {
        *error = "deck size is invalid or exceeds 6 MB";
        return false;
    }
    if (!IsSha256Hex(sha256)) {
        *error = "sha256 must be 64 lowercase hexadecimal characters";
        return false;
    }
    if (!AssetsStore::GetInstance().IsMounted()) {
        *error = "assets partition is not mounted";
        return false;
    }

    ResetDeckUpload(true);
    if (!HasAssetSpace(static_cast<size_t>(size))) {
        *error = "not enough storage for imported deck";
        return false;
    }

    deck_upload.deck_id = deck_id;
    deck_upload.expected_sha256 = sha256;
    deck_upload.expected_size = static_cast<size_t>(size);
    deck_upload.target_path = AssetsStore::GetInstance().PathFor(
        ImportedDeckRelativePath(deck_id));
    deck_upload.temp_path = deck_upload.target_path + ".part";
    remove(deck_upload.temp_path.c_str());
    deck_upload.file = fopen(deck_upload.temp_path.c_str(), "wb");
    if (deck_upload.file == nullptr) {
        *error = std::string("failed to open deck file: ") + std::strerror(errno);
        ResetDeckUpload(true);
        return false;
    }
    mbedtls_sha256_init(&deck_upload.sha);
    mbedtls_sha256_starts(&deck_upload.sha, 0);
    deck_upload.sha_initialized = true;
    return true;
}

bool AppendDeckUpload(cJSON* json, std::string* error) {
    if (deck_upload.file == nullptr) {
        *error = "no deck upload is active";
        return false;
    }
    const int offset = GetJsonInt(json, "offset", -1);
    const char* encoded = GetJsonString(json, "data");
    if (offset < 0 || static_cast<size_t>(offset) != deck_upload.written || encoded == nullptr) {
        *error = "deck chunk offset or data is invalid";
        return false;
    }

    const size_t encoded_len = strlen(encoded);
    std::vector<unsigned char> decoded(encoded_len * 3 / 4 + 4);
    size_t decoded_len = 0;
    if (mbedtls_base64_decode(decoded.data(), decoded.size(), &decoded_len,
                              reinterpret_cast<const unsigned char*>(encoded), encoded_len) != 0 ||
        decoded_len == 0 || deck_upload.written + decoded_len > deck_upload.expected_size) {
        *error = "deck chunk is not valid base64 or exceeds expected size";
        return false;
    }
    if (fwrite(decoded.data(), 1, decoded_len, deck_upload.file) != decoded_len) {
        *error = "failed to write deck chunk";
        return false;
    }
    mbedtls_sha256_update(&deck_upload.sha, decoded.data(), decoded_len);
    deck_upload.written += decoded_len;
    return true;
}

bool ValidateUploadedDeck(const std::string& path,
                          const std::string& expected_deck_id,
                          std::string* error) {
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        *error = "failed to reopen uploaded deck";
        return false;
    }

    const std::string expected_metadata = "# deck_id=" + expected_deck_id;
    bool found_deck_id = false;
    bool found_row = false;
    char line[2048] = {};
    while (fgets(line, sizeof(line), file) != nullptr) {
        const size_t len = strlen(line);
        if (len == sizeof(line) - 1 && line[len - 1] != '\n' && !feof(file)) {
            fclose(file);
            *error = "deck contains an overlong row";
            return false;
        }
        size_t trimmed_len = len;
        while (trimmed_len > 0 && (line[trimmed_len - 1] == '\n' || line[trimmed_len - 1] == '\r')) {
            line[--trimmed_len] = '\0';
        }
        if (line[0] == '\0') {
            continue;
        }
        if (line[0] == '#') {
            if (expected_metadata == line) {
                found_deck_id = true;
            }
            continue;
        }
        size_t tabs = 0;
        for (const char* ch = line; *ch != '\0'; ++ch) {
            if (*ch == '\t') {
                ++tabs;
            }
        }
        if (tabs != 4 || line[0] == '\t') {
            fclose(file);
            *error = "deck rows must use the five-column Egga TSV format";
            return false;
        }
        const char* first_tab = strchr(line, '\t');
        const char* second_tab = first_tab != nullptr ? strchr(first_tab + 1, '\t') : nullptr;
        const char* third_tab = second_tab != nullptr ? strchr(second_tab + 1, '\t') : nullptr;
        if (second_tab == nullptr || third_tab == nullptr || third_tab == second_tab + 1) {
            fclose(file);
            *error = "deck row is missing a word or meaning";
            return false;
        }
        found_row = true;
    }
    const bool read_ok = !ferror(file);
    fclose(file);
    if (!read_ok || !found_deck_id || !found_row) {
        *error = !read_ok ? "failed to validate uploaded deck" :
            (!found_deck_id ? "deck metadata id does not match upload id" : "deck has no usable rows");
        return false;
    }
    return true;
}

bool FinishDeckUpload(std::string* result_json, std::string* error) {
    if (deck_upload.file == nullptr || deck_upload.written != deck_upload.expected_size) {
        *error = "uploaded deck size does not match expected size";
        return false;
    }

    unsigned char digest[32] = {};
    mbedtls_sha256_finish(&deck_upload.sha, digest);
    mbedtls_sha256_free(&deck_upload.sha);
    deck_upload.sha_initialized = false;
    if (fflush(deck_upload.file) != 0 || fclose(deck_upload.file) != 0) {
        deck_upload.file = nullptr;
        *error = "failed to flush uploaded deck";
        return false;
    }
    deck_upload.file = nullptr;

    const std::string actual_sha256 = Sha256ToHex(digest);
    if (actual_sha256 != deck_upload.expected_sha256) {
        *error = "uploaded deck sha256 mismatch";
        return false;
    }
    if (!ValidateUploadedDeck(deck_upload.temp_path, deck_upload.deck_id, error)) {
        return false;
    }

    const std::string backup_path = deck_upload.target_path + ".bak";
    remove(backup_path.c_str());
    (void)rename(deck_upload.target_path.c_str(), backup_path.c_str());
    if (rename(deck_upload.temp_path.c_str(), deck_upload.target_path.c_str()) != 0) {
        (void)rename(backup_path.c_str(), deck_upload.target_path.c_str());
        *error = std::string("failed to activate uploaded deck: ") + std::strerror(errno);
        return false;
    }
    remove(backup_path.c_str());

    *result_json = "{\"deck_id\":" + JsonString(deck_upload.deck_id) +
        ",\"size\":" + std::to_string(deck_upload.written) +
        ",\"sha256\":" + JsonString(actual_sha256) +
        ",\"reboot_required\":true}";
    ResetDeckUpload(false);
    return true;
}

std::string DeckJson() {
    return Application::GetInstance().GetVocabularyApp().DeckStatusJson();
}

std::string WifiStatusJson() {
    auto& wifi = WifiManager::GetInstance();
    std::string json = "{";
    json += "\"initialized\":" + std::string(wifi.IsInitialized() ? "true" : "false") + ",";
    json += "\"connected\":" + std::string(wifi.IsConnected() ? "true" : "false") + ",";
    json += "\"config_mode\":" + std::string(wifi.IsConfigMode() ? "true" : "false") + ",";
    json += "\"ssid\":" + JsonString(wifi.GetSsid()) + ",";
    json += "\"ip\":" + JsonString(wifi.GetIpAddress()) + ",";
    json += "\"rssi\":" + std::to_string(wifi.GetRssi()) + ",";
    json += "\"channel\":" + std::to_string(wifi.GetChannel()) + ",";
    json += "\"mac\":" + JsonString(wifi.GetMacAddress());
    json += "}";
    return json;
}

std::string BatteryJson() {
    int level = 0;
    bool charging = false;
    bool discharging = false;
    const bool available = Board::GetInstance().GetBatteryLevel(level, charging, discharging);
    std::string json = "{";
    json += "\"available\":" + std::string(available ? "true" : "false") + ",";
    json += "\"level\":" + std::to_string(std::max(0, std::min(100, level))) + ",";
    json += "\"charging\":" + std::string(charging ? "true" : "false") + ",";
    json += "\"discharging\":" + std::string(discharging ? "true" : "false");
    json += "}";
    return json;
}

std::string MaimemoStatusJson() {
    const MaimemoAuthConfig config = MaimemoAuthStore::GetInstance().Load();
    const std::string account_masked = !config.account_masked.empty()
        ? config.account_masked
        : MaskMaimemoAccount(config.account);
    std::string json = "{";
    json += "\"configured\":" + std::string(config.configured ? "true" : "false") + ",";
    json += "\"account_masked\":" + JsonString(account_masked) + ",";
    json += "\"credential_saved\":" + std::string(config.credential_saved ? "true" : "false") + ",";
    json += "\"open_api_configured\":" +
        std::string(config.open_api_configured ? "true" : "false") + ",";
    json += "\"offline\":" +
        Application::GetInstance().GetVocabularyApp().MaimemoOfflineStatusJson();
    json += "}";
    return json;
}

const char* VocabularyScreenText(VocabularyScreen screen) {
    switch (screen) {
        case VocabularyScreen::kWelcome:
            return "welcome";
        case VocabularyScreen::kCardFront:
            return "card_front";
        case VocabularyScreen::kCardBack:
            return "card_back";
        case VocabularyScreen::kDailySummary:
            return "daily_summary";
        case VocabularyScreen::kSettings:
            return "settings";
        default:
            return "unknown";
    }
}

std::string VocabularySnapshotJson() {
    const VocabularySnapshot snapshot = Application::GetInstance().GetVocabularyApp().GetSnapshot();
    std::string json = "{";
    json += "\"screen\":" + JsonString(VocabularyScreenText(snapshot.screen)) + ",";
    json += "\"has_card\":" + std::string(snapshot.has_card ? "true" : "false") + ",";
    json += "\"showing_back\":" + std::string(snapshot.showing_back ? "true" : "false") + ",";
    json += "\"card_position\":" + std::to_string(snapshot.card_position) + ",";
    json += "\"card_count\":" + std::to_string(snapshot.card_count) + ",";
    json += "\"reviewed_today\":" + std::to_string(snapshot.reviewed_today) + ",";
    json += "\"correct_today\":" + std::to_string(snapshot.correct_today) + ",";
    json += "\"due_count\":" + std::to_string(snapshot.due_count) + ",";
    json += "\"progress\":" + JsonString(snapshot.progress) + ",";
    json += "\"header\":" + JsonString(snapshot.header) + ",";
    json += "\"clock\":" + JsonString(snapshot.clock) + ",";
    json += "\"word\":" + JsonString(snapshot.word) + ",";
    json += "\"phonetic\":" + JsonString(snapshot.phonetic) + ",";
    json += "\"meaning\":" + JsonString(snapshot.meaning) + ",";
    json += "\"example\":" + JsonString(snapshot.example) + ",";
    json += "\"example_translation\":" + JsonString(snapshot.example_translation) + ",";
    json += "\"hint\":" + JsonString(snapshot.hint) + ",";
    json += "\"footer\":" + JsonString(snapshot.footer) + ",";
    json += "\"status_line\":" + JsonString(snapshot.status_line) + ",";
    json += "\"action_status\":" + JsonString(snapshot.action_status) + ",";
    json += "\"review_mark\":" + JsonString(snapshot.review_mark);
    json += "}";
    return json;
}

bool ParseVocabularyButton(cJSON* json, VocabularyButton* button) {
    if (button == nullptr) {
        return false;
    }

    cJSON* raw = cJSON_GetObjectItemCaseSensitive(json, "button");
    if (cJSON_IsNumber(raw)) {
        switch (raw->valueint) {
            case static_cast<int>(VocabularyButton::kConfirmClick):
            case static_cast<int>(VocabularyButton::kConfirmLongPress):
            case static_cast<int>(VocabularyButton::kUpClick):
            case static_cast<int>(VocabularyButton::kDownClick):
            case static_cast<int>(VocabularyButton::kUpLongPress):
            case static_cast<int>(VocabularyButton::kDownLongPress):
            case static_cast<int>(VocabularyButton::kSettingsCombo):
            case static_cast<int>(VocabularyButton::kConfirmDoubleClick):
                *button = static_cast<VocabularyButton>(raw->valueint);
                return true;
            default:
                return false;
        }
    }

    const char* value = GetJsonString(json, "button");
    if (value == nullptr) {
        return false;
    }
    if (strcmp(value, "confirm") == 0 || strcmp(value, "confirm.click") == 0) {
        *button = VocabularyButton::kConfirmClick;
    } else if (strcmp(value, "confirm.double") == 0 || strcmp(value, "confirm.dblclick") == 0) {
        *button = VocabularyButton::kConfirmDoubleClick;
    } else if (strcmp(value, "confirm.long") == 0) {
        *button = VocabularyButton::kConfirmLongPress;
    } else if (strcmp(value, "up") == 0 || strcmp(value, "up.click") == 0) {
        *button = VocabularyButton::kUpClick;
    } else if (strcmp(value, "down") == 0 || strcmp(value, "down.click") == 0) {
        *button = VocabularyButton::kDownClick;
    } else if (strcmp(value, "up.long") == 0) {
        *button = VocabularyButton::kUpLongPress;
    } else if (strcmp(value, "down.long") == 0) {
        *button = VocabularyButton::kDownLongPress;
    } else if (strcmp(value, "settings.combo") == 0) {
        *button = VocabularyButton::kSettingsCombo;
    } else {
        return false;
    }
    return true;
}

void SendRaw(const std::string& payload) {
    const std::string frame = std::string(kFramePrefix) + payload + "\n";
#if CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED
    if (EnsureUsbSerialJtagDriver()) {
        const char* data = frame.data();
        size_t remaining = frame.size();
        while (remaining > 0) {
            int written = usb_serial_jtag_write_bytes(data, remaining, pdMS_TO_TICKS(250));
            if (written <= 0) {
                break;
            }
            data += written;
            remaining -= static_cast<size_t>(written);
        }
        usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(250));
        return;
    }
#endif
    fputs(frame.c_str(), stdout);
    fflush(stdout);
}

void SendResponse(int id, bool ok, const std::string& type, const std::string& data_json) {
    std::string response = "{";
    response += "\"id\":" + std::to_string(id) + ",";
    response += "\"ok\":" + std::string(ok ? "true" : "false") + ",";
    response += "\"type\":" + JsonString(type) + ",";
    response += "\"data\":" + (data_json.empty() ? "{}" : data_json);
    response += "}";
    SendRaw(response);
}

void SendError(int id, const std::string& code, const std::string& message) {
    std::string error = "{";
    error += "\"code\":" + JsonString(code) + ",";
    error += "\"message\":" + JsonString(message);
    error += "}";
    SendResponse(id, false, "error", error);
}

void EnsureWifiStarted() {
    auto& wifi = WifiManager::GetInstance();
    if (!wifi.IsInitialized()) {
        WifiManagerConfig config;
        config.ssid_prefix = "Egga";
        config.station_scan_min_interval_seconds = 60;
        config.station_scan_max_interval_seconds = 900;
        (void)wifi.Initialize(config);
    }
}

std::string StatusJson() {
    const esp_app_desc_t* app = esp_app_get_description();
    std::string json = "{";
    json += "\"protocol\":\"ZTX1\",";
    json += "\"display_name\":" + JsonString(Board::GetInstance().GetDisplayName()) + ",";
    json += "\"board_type\":" + JsonString(Board::GetInstance().GetBoardType()) + ",";
    json += "\"uuid\":" + JsonString(Board::GetInstance().GetUuid()) + ",";
    json += "\"firmware\":{";
    json += "\"name\":" + JsonString(app != nullptr ? app->project_name : "") + ",";
    json += "\"version\":" + JsonString(app != nullptr ? app->version : "") + ",";
    json += "\"idf_version\":" + JsonString(app != nullptr ? app->idf_ver : "");
    json += "},";
    json += "\"assets\":{";
    json += "\"mounted\":" + std::string(AssetsStore::GetInstance().IsMounted() ? "true" : "false") + ",";
    json += "\"base_path\":" + JsonString(AssetsStore::GetInstance().BasePath());
    json += "},";
    json += "\"deck\":" + DeckJson() + ",";
    json += "\"wifi\":" + WifiStatusJson() + ",";
    json += "\"battery\":" + BatteryJson() + ",";
    json += "\"runtime\":{";
    json += "\"uptime_ms\":" + std::to_string(esp_timer_get_time() / 1000) + ",";
    json += "\"reset_reason\":" + std::to_string(static_cast<int>(esp_reset_reason())) + ",";
    json += "\"free_internal\":" +
        std::to_string(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) + ",";
    json += "\"min_internal\":" +
        std::to_string(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)) + ",";
    json += "\"idle_sleep_delay_ms\":" +
        std::to_string(Application::GetInstance().IdleSleepDelayMs());
    json += "},";
    json += "\"third_party\":{\"maimemo\":" + MaimemoStatusJson() + "}";
    json += "}";
    return json;
}

std::string ScanWifiJson() {
    EnsureWifiStarted();

    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
    } else if (mode == WIFI_MODE_AP) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }

    wifi_scan_config_t scan_config = {};
    esp_err_t ret = esp_wifi_scan_start(&scan_config, true);
    if (ret != ESP_OK) {
        return "{\"aps\":[],\"error\":" + JsonString(esp_err_to_name(ret)) + "}";
    }

    uint16_t ap_count = 0;
    ret = esp_wifi_scan_get_ap_num(&ap_count);
    if (ret != ESP_OK) {
        return "{\"aps\":[],\"error\":" + JsonString(esp_err_to_name(ret)) + "}";
    }

    const uint16_t total_count = ap_count;
    uint16_t read_count = std::min<uint16_t>(total_count, kMaxScanResults);
    std::vector<wifi_ap_record_t> records(read_count);
    if (read_count > 0) {
        ret = esp_wifi_scan_get_ap_records(&read_count, records.data());
        if (ret != ESP_OK) {
            return "{\"aps\":[],\"error\":" + JsonString(esp_err_to_name(ret)) + "}";
        }
        records.resize(read_count);
    }

    std::string json = "{\"aps\":[";
    for (size_t i = 0; i < records.size(); ++i) {
        if (i > 0) {
            json += ",";
        }
        const wifi_ap_record_t& ap = records[i];
        json += "{";
        json += "\"ssid\":" + JsonString(reinterpret_cast<const char*>(ap.ssid)) + ",";
        json += "\"rssi\":" + std::to_string(ap.rssi) + ",";
        json += "\"channel\":" + std::to_string(ap.primary) + ",";
        json += "\"authmode\":" + std::to_string(static_cast<int>(ap.authmode));
        json += "}";
    }
    json += "],\"total\":" + std::to_string(total_count) + "}";
    return json;
}

std::string SavedWifiJson() {
    const auto& items = SsidManager::GetInstance().GetSsidList();
    std::string json = "{\"items\":[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            json += ",";
        }
        json += "{";
        json += "\"index\":" + std::to_string(i) + ",";
        json += "\"ssid\":" + JsonString(items[i].ssid) + ",";
        json += "\"has_password\":" + std::string(items[i].password.empty() ? "false" : "true");
        json += "}";
    }
    json += "],\"total\":" + std::to_string(items.size()) + "}";
    return json;
}

bool AddWifiFromJson(cJSON* json) {
    const char* ssid = GetJsonString(json, "ssid");
    const char* password = GetJsonString(json, "password");
    if (ssid == nullptr || ssid[0] == '\0' || strlen(ssid) >= 33) {
        return false;
    }
    if (password != nullptr && strlen(password) >= 65) {
        return false;
    }

    SsidManager::GetInstance().AddSsid(ssid, password != nullptr ? password : "");
    EnsureWifiStarted();
    WifiManager::GetInstance().StartStation();
    WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::LOW_POWER);
    return true;
}

bool RemoveWifiFromJson(cJSON* json) {
    const char* ssid = GetJsonString(json, "ssid");
    auto& manager = SsidManager::GetInstance();
    if (ssid == nullptr || ssid[0] == '\0') {
        manager.Clear();
        return true;
    }

    const auto& items = manager.GetSsidList();
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].ssid == ssid) {
            manager.RemoveSsid(static_cast<int>(i));
            return true;
        }
    }
    return false;
}

bool SaveMaimemoAuthFromJson(cJSON* json) {
    const char* token = GetJsonString(json, "token");
    const char* open_api_token = GetJsonString(json, "open_api_token");
    const char* account = GetJsonString(json, "account");
    const char* password = GetJsonString(json, "password");
    const bool has_token = token != nullptr && token[0] != '\0';
    const bool has_open_api_token = open_api_token != nullptr && open_api_token[0] != '\0';
    const bool has_credentials = account != nullptr && account[0] != '\0' &&
        password != nullptr && password[0] != '\0';
    if (!has_token && !has_credentials && !has_open_api_token) {
        return false;
    }
    if (has_token && (strlen(token) < 16 || strlen(token) > kMaxMaimemoTokenBytes)) {
        return false;
    }
    if (has_open_api_token &&
        (strlen(open_api_token) < 16 || strlen(open_api_token) > kMaxMaimemoTokenBytes)) {
        return false;
    }
    if (account != nullptr && strlen(account) >= kMaxMaimemoAccountBytes) {
        return false;
    }
    if (password != nullptr && strlen(password) >= kMaxMaimemoPasswordBytes) {
        return false;
    }

    MaimemoAuthConfig config = MaimemoAuthStore::GetInstance().Load();
    if (has_token) {
        config.token = token;
    }
    if (has_open_api_token) {
        config.open_api_token = open_api_token;
    }
    const std::string account_masked = CStr(GetJsonString(json, "account_masked"));
    if (!account_masked.empty()) {
        config.account_masked = account_masked;
    } else if (config.account_masked.empty() && has_credentials) {
        config.account_masked = MaskMaimemoAccount(account);
    }
    const std::string obtained_at = CStr(GetJsonString(json, "obtained_at"));
    if (!obtained_at.empty()) {
        config.obtained_at = obtained_at;
    }
    if (has_credentials) {
        config.token.clear();
        config.credential_saved = true;
        config.account = account;
        config.password = password;
    }
    config.configured = !config.token.empty() ||
        (config.credential_saved && !config.account.empty() && !config.password.empty());
    config.open_api_configured = !config.open_api_token.empty();
    return MaimemoAuthStore::GetInstance().Save(config);
}

void HandleCommand(cJSON* json) {
    const int id = GetJsonInt(json, "id", 0);
    const char* command = GetJsonString(json, "cmd");
    if (command == nullptr) {
        SendError(id, "bad_request", "missing cmd");
        return;
    }

    if (strcmp(command, "hello") == 0) {
        SendResponse(id, true, "hello",
                     "{\"protocol\":\"ZTX1\",\"commands\":[\"hello\",\"status.get\","
                     "\"wifi.scan\",\"wifi.set\",\"wifi.forget\",\"wifi.status\",\"wifi.saved\","
                     "\"deck.status\",\"deck.upload.begin\",\"deck.upload.chunk\","
                     "\"deck.upload.finish\",\"deck.upload.abort\",\"progress.clear\","
                     "\"maimemo.auth.set\","
                     "\"maimemo.auth.clear\",\"maimemo.status\",\"maimemo.offline.clear\",\"vocab.snapshot\","
                     "\"vocab.deck.select\",\"vocab.button\",\"vocab.diag\",\"ota.status\",\"ota.check\",\"ota.start\","
                     "\"device.reboot\"]}");
    } else if (strcmp(command, "status.get") == 0) {
        SendResponse(id, true, "status", StatusJson());
    } else if (strcmp(command, "wifi.status") == 0) {
        SendResponse(id, true, "wifi.status", WifiStatusJson());
    } else if (strcmp(command, "wifi.saved") == 0) {
        SendResponse(id, true, "wifi.saved", SavedWifiJson());
    } else if (strcmp(command, "wifi.scan") == 0) {
        SendResponse(id, true, "wifi.scan", ScanWifiJson());
    } else if (strcmp(command, "wifi.set") == 0) {
        if (!AddWifiFromJson(json)) {
            SendError(id, "bad_wifi", "ssid or password is invalid");
            return;
        }
        SendResponse(id, true, "wifi.set", WifiStatusJson());
    } else if (strcmp(command, "wifi.forget") == 0) {
        if (!RemoveWifiFromJson(json)) {
            SendError(id, "not_found", "ssid was not saved");
            return;
        }
        SendResponse(id, true, "wifi.forget", WifiStatusJson());
    } else if (strcmp(command, "deck.status") == 0) {
        SendResponse(id, true, "deck.status", DeckJson());
    } else if (strcmp(command, "deck.upload.begin") == 0) {
        std::string error;
        if (!BeginDeckUpload(json, &error)) {
            SendError(id, "deck_upload_begin_failed", error);
            return;
        }
        SendResponse(id, true, "deck.upload", "{\"offset\":0}");
    } else if (strcmp(command, "deck.upload.chunk") == 0) {
        std::string error;
        if (!AppendDeckUpload(json, &error)) {
            SendError(id, "deck_upload_chunk_failed", error);
            return;
        }
        SendResponse(id, true, "deck.upload",
                     "{\"offset\":" + std::to_string(deck_upload.written) + "}");
    } else if (strcmp(command, "deck.upload.finish") == 0) {
        std::string result;
        std::string error;
        if (!FinishDeckUpload(&result, &error)) {
            ResetDeckUpload(true);
            SendError(id, "deck_upload_finish_failed", error);
            return;
        }
        SendResponse(id, true, "deck.upload", result);
    } else if (strcmp(command, "deck.upload.abort") == 0) {
        ResetDeckUpload(true);
        SendResponse(id, true, "deck.upload", "{\"aborted\":true}");
    } else if (strcmp(command, "progress.clear") == 0) {
        const bool cleared = Application::GetInstance().GetVocabularyApp().ClearProgress();
        if (!cleared) {
            SendError(id, "clear_failed", "failed to clear vocabulary progress");
            return;
        }
        SendResponse(id, true, "progress.clear", "{\"cleared\":true}");
    } else if (strcmp(command, "maimemo.auth.set") == 0) {
        if (!SaveMaimemoAuthFromJson(json)) {
            SendError(id, "bad_maimemo_auth", "token or credentials are invalid");
            return;
        }
        Application::GetInstance().GetVocabularyApp().NotifyMaimemoAuthChanged();
        SendResponse(id, true, "maimemo.auth", MaimemoStatusJson());
    } else if (strcmp(command, "maimemo.auth.clear") == 0) {
        MaimemoAuthStore::GetInstance().Clear();
        Application::GetInstance().GetVocabularyApp().NotifyMaimemoAuthChanged();
        SendResponse(id, true, "maimemo.auth", MaimemoStatusJson());
    } else if (strcmp(command, "maimemo.status") == 0) {
        SendResponse(id, true, "maimemo.status", MaimemoStatusJson());
    } else if (strcmp(command, "maimemo.offline.clear") == 0) {
        if (!Application::GetInstance().GetVocabularyApp().ClearMaimemoOfflineState()) {
            SendError(id, "maimemo_offline_clear_failed", "failed to clear Maimemo offline state");
            return;
        }
        SendResponse(id, true, "maimemo.offline.clear", "{\"cleared\":true}");
    } else if (strcmp(command, "vocab.snapshot") == 0) {
        SendResponse(id, true, "vocab.snapshot", VocabularySnapshotJson());
    } else if (strcmp(command, "vocab.deck.select") == 0) {
        const char* deck_id = GetJsonString(json, "deck_id");
        if (deck_id == nullptr || deck_id[0] == '\0') {
            SendError(id, "bad_deck", "deck_id is required");
            return;
        }
        if (!Application::GetInstance().GetVocabularyApp().SelectDeck(deck_id)) {
            SendError(id, "deck_not_found", "deck is not installed");
            return;
        }
        SendResponse(id, true, "vocab.snapshot", VocabularySnapshotJson());
    } else if (strcmp(command, "vocab.diag") == 0) {
        SendResponse(id, true, "vocab.diag",
                     Application::GetInstance().GetVocabularyApp().DiagnosticsJson());
    } else if (strcmp(command, "vocab.button") == 0) {
        VocabularyButton button = VocabularyButton::kConfirmClick;
        if (!ParseVocabularyButton(json, &button)) {
            SendError(id, "bad_button", "button is invalid");
            return;
        }
        Application::GetInstance().Schedule([button]() {
            Application::GetInstance().GetVocabularyApp().HandleButton(button);
        });
        SendResponse(id, true, "vocab.snapshot", VocabularySnapshotJson());
    } else if (strcmp(command, "ota.status") == 0) {
        SendResponse(id, true, "ota.status", OtaService::GetInstance().StatusJson());
    } else if (strcmp(command, "ota.check") == 0) {
        std::string error;
        const char* base_url = GetJsonString(json, "base_url");
        if (!OtaService::GetInstance().Check(base_url != nullptr ? base_url : "", &error)) {
            SendError(id, "ota_check_failed", error);
            return;
        }
        SendResponse(id, true, "ota.status", OtaService::GetInstance().StatusJson());
    } else if (strcmp(command, "ota.start") == 0) {
        std::string error;
        const char* base_url = GetJsonString(json, "base_url");
        if (!OtaService::GetInstance().Start(base_url != nullptr ? base_url : "", &error)) {
            SendError(id, "ota_start_failed", error);
            return;
        }
        SendResponse(id, true, "ota.status", OtaService::GetInstance().StatusJson());
    } else if (strcmp(command, "device.reboot") == 0) {
        SendResponse(id, true, "device.reboot", "{\"rebooting\":true}");
        vTaskDelay(pdMS_TO_TICKS(250));
        esp_restart();
    } else {
        SendError(id, "unknown_cmd", command);
    }
}

void ProcessLine(char* line) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }

    if (strncmp(line, kFramePrefix, strlen(kFramePrefix)) != 0) {
        return;
    }

    cJSON* json = cJSON_Parse(line + strlen(kFramePrefix));
    if (json == nullptr) {
        SendError(0, "bad_json", "could not parse management frame");
        return;
    }
    HandleCommand(json);
    cJSON_Delete(json);
}

void FeedInputByte(char ch, char* line, size_t& len) {
    if (ch == '\n') {
        line[len] = '\0';
        ProcessLine(line);
        len = 0;
        return;
    }
    if (ch == '\r') {
        return;
    }
    if (len + 1 >= kMaxLineBytes) {
        len = 0;
        return;
    }
    line[len++] = ch;
}

void ClientMgmtTask(void*) {
    ESP_LOGI(kTag, "management task started");
    setvbuf(stdout, nullptr, _IONBF, 0);

    static char line[kMaxLineBytes] = {};
    size_t line_len = 0;

#if CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED
    bool usb_ready = EnsureUsbSerialJtagDriver();
    if (usb_ready) {
        ESP_LOGI(kTag, "listening on USB serial/JTAG management channel");
    }
    static char usb_buffer[256] = {};
#endif

    while (true) {
#if CONFIG_SOC_USB_SERIAL_JTAG_SUPPORTED
        if (usb_ready) {
            int read = usb_serial_jtag_read_bytes(usb_buffer, sizeof(usb_buffer), pdMS_TO_TICKS(50));
            if (read > 0) {
                for (int i = 0; i < read; ++i) {
                    FeedInputByte(usb_buffer[i], line, line_len);
                }
                continue;
            }
        } else {
            usb_ready = EnsureUsbSerialJtagDriver();
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void ImportWifiObject(cJSON* object) {
    if (object == nullptr) {
        return;
    }
    if (!AddWifiFromJson(object)) {
        ESP_LOGW(kTag, "skip invalid wifi object from assets");
    }
}

}  // namespace

ClientMgmtService& ClientMgmtService::GetInstance() {
    static ClientMgmtService service;
    return service;
}

void ClientMgmtService::Start() {
    if (started_) {
        return;
    }
    const BaseType_t created = xTaskCreate(
        ClientMgmtTask,
        "client_mgmt",
        kClientMgmtTaskStackBytes,
        nullptr,
        5,
        nullptr);
    if (created != pdPASS) {
        ESP_LOGE(kTag, "failed to create management task");
        return;
    }
    started_ = true;
}

void ClientMgmtService::ImportWifiConfigFromAssetsOnce() {
    if (imported_wifi_config_) {
        return;
    }
    imported_wifi_config_ = true;

    std::string raw;
    if (!AssetsStore::GetInstance().ReadFile(kWifiConfigPath, &raw, kMaxWifiConfigBytes)) {
        return;
    }

    cJSON* json = cJSON_Parse(raw.c_str());
    if (json == nullptr) {
        ESP_LOGW(kTag, "invalid assets wifi config");
        return;
    }

    cJSON* networks = cJSON_GetObjectItemCaseSensitive(json, "networks");
    if (cJSON_IsArray(networks)) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, networks) {
            ImportWifiObject(item);
        }
    } else {
        ImportWifiObject(json);
    }

    cJSON_Delete(json);
}
