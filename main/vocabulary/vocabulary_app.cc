#include "vocabulary_app.h"

#include "ai/ai_dialog_service.h"
#include "application.h"
#include "assets_store.h"
#include "display.h"
#include "audio_codec.h"
#include "board.h"
#include "maimemo_auth_store.h"
#include "online_pronunciation_service.h"
#include "ota_service.h"
#include "ssid_manager.h"
#include "system_info.h"
#include "wifi_manager.h"
#include "boards/egga/egga_nfc.h"
#include "boards/egga-s3-epaper-4.2/rtc_pcf8563.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_log.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_netif_sntp.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

extern "C" RtcPcf8563* EggaGetRtc();
extern "C" EggaNfc* EggaGetNfc();

namespace {

constexpr int64_t kActionStatusDurationMs = 1000;

constexpr char kTag[] = "VocabularyApp";
constexpr char kNfcEntryUrl[] = "https://www.egga.com/vocab";
constexpr uint8_t kSettingsNavigationItemCount = 14;
constexpr uint8_t kSettingsNavigationOrder[kSettingsNavigationItemCount] = {
    0, 1, 2, 3, 9, 4, 8, 10, 6, 5, 11, 12, 13, 7,
};
constexpr size_t kAgainDelayCards = 3;
constexpr size_t kMaxPronunciationBytes = 512 * 1024;
constexpr uint32_t kPronunciationTaskStackBytes = 12 * 1024;
// Snapshot publication now also assembles the AI teacher context. The former
// 12 KiB task stack overflowed after receiving the first Maimemo word card.
constexpr uint32_t kMaimemoTaskStackBytes = 16 * 1024;
constexpr uint32_t kMaimemoPersistenceTaskStackBytes = 8 * 1024;
// OTA checking builds TLS/HTTP state and copies status strings while it also
// publishes e-paper snapshots.  Ten KB overflows on the ESP32-S3 in the real
// settings-entry path; keep it aligned with the other network task budgets.
constexpr uint32_t kOtaUiTaskStackBytes = 20 * 1024;
constexpr uint32_t kDeckSyncTaskStackBytes = 20 * 1024;
constexpr uint32_t kAiEntitlementTaskStackBytes = 20 * 1024;
constexpr int64_t kProgressSaveCoalesceMs = 1500;
constexpr int64_t kMaimemoWifiIdleMs = 30 * 60 * 1000;
constexpr int64_t kOfflinePronunciationWifiIdleMs = 60 * 1000;
constexpr int kOnlineWifiWaitLoops = 16;
constexpr int kOnlineWifiWaitDelayMs = 250;
constexpr int kOtaMaxPollSeconds = 15 * 60;
constexpr int kOtaProgressRefreshStep = 5;
constexpr int64_t kOtaProgressRefreshMaxIntervalMs = 3000;
constexpr int kOutputVolumeMin = 10;
constexpr int kOutputVolumeMax = 100;
constexpr int kOutputVolumeStep = 10;
constexpr int kOutputVolumeDefault = 70;
constexpr uint8_t kLowBatteryAlertThreshold = 20;
constexpr char kMaimemoDailyTargetCompleteStatus[] = "daily target complete";
constexpr char kDeveloperContact[] = "小红书号：4449420200";

bool IsPermissionFailure(const std::string& status) {
    return status.find("HTTP status 401") != std::string::npos ||
        status.find("HTTP status 403") != std::string::npos ||
        status.find("permission denied") != std::string::npos ||
        status.find("not entitled") != std::string::npos ||
        status.find("forbidden") != std::string::npos;
}

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
                    char buffer[8] = {};
                    snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
                    out += buffer;
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

std::string JsonBool(bool value) {
    return value ? "true" : "false";
}

bool AsciiEqualsIgnoreCase(const char* left, const std::string& right) {
    if (left == nullptr) {
        return false;
    }
    size_t index = 0;
    while (left[index] != '\0' && index < right.size()) {
        const auto left_ch = static_cast<unsigned char>(left[index]);
        const auto right_ch = static_cast<unsigned char>(right[index]);
        if (std::tolower(left_ch) != std::tolower(right_ch)) {
            return false;
        }
        ++index;
    }
    return left[index] == '\0' && index == right.size();
}

enum class MaimemoTaskAction : uint8_t {
    kFetch = 0,
    kSubmit = 1,
    kSyncPending = 2,
};

bool IsMaimemoSubmitAction(MaimemoTaskAction action) {
    return action == MaimemoTaskAction::kSubmit ||
        action == MaimemoTaskAction::kSyncPending;
}

const char* MaimemoAnswerBadge(MaimemoAnswer answer) {
    switch (answer) {
        case MaimemoAnswer::kFamiliar:
            return "[认识]";
        case MaimemoAnswer::kVague:
            return "[模糊]";
        case MaimemoAnswer::kForget:
            return "[忘记]";
        default:
            return "";
    }
}

struct PronunciationTaskArgs {
    VocabularyApp* app = nullptr;
    uint32_t request_id = 0;
    int voice_type = 0;
    std::string deck_id;
    std::string word;
    std::string pronunciation_url;
    std::string prefetch_word;
    std::string prefetch_url;
    bool play_audio = true;
};

struct MaimemoTaskArgs {
    VocabularyApp* app = nullptr;
    uint32_t request_id = 0;
    MaimemoTaskAction action = MaimemoTaskAction::kFetch;
    MaimemoAnswer answer = MaimemoAnswer::kFamiliar;
    std::string voc_id;
    std::string word;
    uint32_t recall_duration_ms = 0;
    uint32_t study_duration_ms = 0;
    bool back = false;
    std::string operation_id;
    bool refresh_snapshot = false;
};

struct MaimemoPersistenceTaskArgs {
    VocabularyApp* app = nullptr;
    uint32_t request_id = 0;
    MaimemoOfflineState state;
    bool save_full_state = false;
    bool save_pending_answers = false;
};

struct DeckSyncTaskArgs {
    VocabularyApp* app = nullptr;
    uint32_t request_id = 0;
    bool download = false;
    RemoteDeckEntry entry;
};

struct DeckSwitchTaskArgs {
    VocabularyApp* app = nullptr;
    uint32_t request_id = 0;
    std::string deck_id;
    std::string name;
    std::string active_deck_id;
    std::vector<uint64_t> progress_word_keys;
    std::vector<ReviewState> progress_states;
    VocabularyDailyStats progress_stats;
    bool save_progress = false;
    bool delete_deck = false;
};

struct OtaTaskArgs {
    VocabularyApp* app = nullptr;
    uint32_t request_id = 0;
    bool start_update = true;
};

struct AiEntitlementTaskArgs {
    VocabularyApp* app = nullptr;
    uint32_t request_id = 0;
};

struct ProgressSaveTaskArgs {
    VocabularyApp* app = nullptr;
    uint32_t request_id = 0;
    std::string deck_id;
    std::vector<uint64_t> word_keys;
    std::vector<ReviewState> states;
    VocabularyDailyStats stats;
};

int32_t DaysFromCivil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<int32_t>(era * 146097 + static_cast<int>(doe) - 719468);
}

const char* StatusText(ReviewStatus status) {
    switch (status) {
        case ReviewStatus::kNew:
            return "new";
        case ReviewStatus::kLearning:
            return "learning";
        case ReviewStatus::kReview:
            return "review";
        case ReviewStatus::kMastered:
            return "mastered";
        default:
            return "new";
    }
}

const char* OrderModeText(VocabularyOrderMode mode) {
    switch (mode) {
        case VocabularyOrderMode::kSequential:
            return "顺序";
        case VocabularyOrderMode::kRandom:
            return "随机";
        default:
            return "顺序";
    }
}

const char* StudyModeText(VocabularyStudyMode study_mode) {
    switch (study_mode) {
        case VocabularyStudyMode::kBrowse:
            return "背词模式";
        case VocabularyStudyMode::kMaimemo:
            return "墨墨在线";
        case VocabularyStudyMode::kOffline:
        default:
            return "本地离线";
    }
}

const char* FontModeText(VocabularyFontMode font_mode) {
    switch (font_mode) {
        case VocabularyFontMode::kCompact:
            return "紧凑小字";
        case VocabularyFontMode::kClear:
        default:
            return "清晰大字";
    }
}

const char* AutoSleepTimeoutText(VocabularyAutoSleepTimeout timeout) {
    switch (timeout) {
        case VocabularyAutoSleepTimeout::k30Minutes:
            return "30 分钟";
        case VocabularyAutoSleepTimeout::k1Hour:
            return "1 小时";
        case VocabularyAutoSleepTimeout::k6Hours:
            return "6 小时";
        case VocabularyAutoSleepTimeout::kNever:
            return "永不";
        case VocabularyAutoSleepTimeout::k3Hours:
        default:
            return "3 小时";
    }
}

VocabularyAutoSleepTimeout MoveAutoSleepTimeout(VocabularyAutoSleepTimeout timeout, int delta) {
    constexpr uint8_t kOptionCount = 5;
    uint8_t index = static_cast<uint8_t>(timeout);
    if (index >= kOptionCount) {
        index = static_cast<uint8_t>(VocabularyAutoSleepTimeout::k3Hours);
    }
    index = delta < 0
        ? static_cast<uint8_t>((index + kOptionCount - 1) % kOptionCount)
        : static_cast<uint8_t>((index + 1) % kOptionCount);
    return static_cast<VocabularyAutoSleepTimeout>(index);
}

VocabularyStudyMode ToggleStudyMode(VocabularyStudyMode mode) {
    switch (mode) {
        case VocabularyStudyMode::kOffline:
            return VocabularyStudyMode::kBrowse;
        case VocabularyStudyMode::kBrowse:
            return VocabularyStudyMode::kMaimemo;
        case VocabularyStudyMode::kMaimemo:
        default:
            return VocabularyStudyMode::kOffline;
    }
}

VocabularyFontMode ToggleFontMode(VocabularyFontMode mode) {
    return mode == VocabularyFontMode::kClear
        ? VocabularyFontMode::kCompact
        : VocabularyFontMode::kClear;
}

VocabularyOrderMode ToggleOrderMode(VocabularyOrderMode mode) {
    return mode == VocabularyOrderMode::kSequential
        ? VocabularyOrderMode::kRandom
        : VocabularyOrderMode::kSequential;
}

VocabularyVoiceType ToggleVoiceType(VocabularyVoiceType voice_type) {
    return voice_type == VocabularyVoiceType::kAmerican
        ? VocabularyVoiceType::kBritish
        : VocabularyVoiceType::kAmerican;
}

int YoudaoVoiceType(VocabularyVoiceType voice_type) {
    return voice_type == VocabularyVoiceType::kBritish ? 1 : 0;
}

int64_t NowMs() {
    return esp_timer_get_time() / 1000;
}

int CurrentOutputVolume() {
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return kOutputVolumeDefault;
    }
    return std::min(kOutputVolumeMax, std::max(kOutputVolumeMin, codec->output_volume()));
}

std::vector<uint64_t> BuildProgressWordKeysForStore(const DeckStore& store) {
    std::vector<uint64_t> keys;
    keys.reserve(store.Count());
    for (size_t i = 0; i < store.Count(); ++i) {
        const WordCard* card = store.Get(i);
        uint64_t hash = 1469598103934665603ULL;
        const char* word = card != nullptr && card->word != nullptr ? card->word : "";
        for (const unsigned char ch : std::string(word)) {
            const unsigned char normalized = ch < 0x80
                ? static_cast<unsigned char>(std::tolower(ch))
                : ch;
            hash ^= normalized;
            hash *= 1099511628211ULL;
        }
        hash ^= 0xff;
        hash *= 1099511628211ULL;
        const char* phonetic = card != nullptr && card->phonetic != nullptr ? card->phonetic : "";
        for (const unsigned char ch : std::string(phonetic)) {
            hash ^= ch;
            hash *= 1099511628211ULL;
        }
        hash ^= 0xfe;
        hash *= 1099511628211ULL;
        const char* meaning = card != nullptr && card->meaning != nullptr ? card->meaning : "";
        for (const unsigned char ch : std::string(meaning)) {
            hash ^= ch;
            hash *= 1099511628211ULL;
        }
        keys.push_back(hash == 0 ? 1 : hash);
    }
    return keys;
}

const char* SettingsMainLabel(uint8_t index) {
    switch (index) {
        case 0:
            return "学习模式";
        case 1:
            return "背诵词库";
        case 2:
            return "每日目标";
        case 3:
            return "背诵顺序";
        case 4:
            return "发音口音";
        case 5:
            return "显示字体";
        case 6:
            return "我的词库";
        case 7:
            return "系统升级";
        case 8:
            return "音量";
        case 9:
            return "自动发音";
        case 10:
            return "Wi-Fi 配网";
        case 11:
            return "AI 背词教师";
        case 12:
            return "自动休眠";
        case 13:
            return "按键说明";
        default:
            return "";
    }
}

const char* SettingsGroupLabel(uint8_t index) {
    switch (index) {
        case 0:
            return "[学习]";
        case 9:
            return "[发音]";
        case 10:
            return "[联网]";
        case 5:
            return "[系统]";
        default:
            return "      ";
    }
}

uint8_t SettingsNavigationPosition(uint8_t item_index) {
    for (uint8_t i = 0; i < kSettingsNavigationItemCount; ++i) {
        if (kSettingsNavigationOrder[i] == item_index) {
            return i;
        }
    }
    return 0;
}

uint8_t MoveSettingsSelection(uint8_t item_index, int delta) {
    uint8_t position = SettingsNavigationPosition(item_index);
    if (delta < 0) {
        position = position == 0
            ? kSettingsNavigationItemCount - 1
            : static_cast<uint8_t>(position - 1);
    } else if (delta > 0) {
        position = static_cast<uint8_t>((position + 1) % kSettingsNavigationItemCount);
    }
    return kSettingsNavigationOrder[position];
}

std::string FormatBytes(size_t bytes) {
    char buffer[24] = {};
    if (bytes >= 1024 * 1024) {
        snprintf(buffer, sizeof(buffer), "%.1fMB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        snprintf(buffer, sizeof(buffer), "%uKB", static_cast<unsigned>(bytes / 1024));
    } else if (bytes > 0) {
        snprintf(buffer, sizeof(buffer), "%uB", static_cast<unsigned>(bytes));
    } else {
        snprintf(buffer, sizeof(buffer), "--");
    }
    return buffer;
}

bool WifiStationConnected() {
    return WifiManager::GetInstance().IsConnected();
}

bool HasSavedWifiCredentials() {
    return !SsidManager::GetInstance().GetSsidList().empty();
}

bool WaitForOnlineWifiReady() {
    if (WifiStationConnected()) {
        return true;
    }

    if (!HasSavedWifiCredentials()) {
        ESP_LOGI(kTag, "Skip online pronunciation WiFi: no saved SSID");
        return false;
    }

    Board::GetInstance().RequestNetwork();
    for (int i = 0; i < kOnlineWifiWaitLoops; ++i) {
        if (WifiStationConnected()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(kOnlineWifiWaitDelayMs));
    }
    return false;
}

bool OtaSnapshotActive(const OtaService::Snapshot& snapshot) {
    return snapshot.running ||
        snapshot.state == "checking" ||
        snapshot.state == "starting" ||
        snapshot.state == "available" ||
        snapshot.state == "downloading" ||
        snapshot.state == "rebooting";
}

bool OtaSnapshotBusy(const OtaService::Snapshot& snapshot) {
    return snapshot.running ||
        snapshot.state == "checking" ||
        snapshot.state == "starting" ||
        snapshot.state == "downloading" ||
        snapshot.state == "rebooting";
}

std::string OtaStatusText(const OtaService::Snapshot& snapshot, const std::string& fallback) {
    if (snapshot.state == "checking") {
        return "检查中";
    }
    if (snapshot.state == "starting") {
        return "准备升级";
    }
    if (snapshot.running || snapshot.state == "downloading") {
        const int progress = std::max(0, snapshot.progress);
        return "升级中 " + std::to_string(progress) + "%";
    }
    if (snapshot.state == "available") {
        return snapshot.latest_version.empty()
            ? std::string("发现新版本")
            : "发现 " + snapshot.latest_version;
    }
    if (snapshot.state == "not_available") {
        return "已是最新";
    }
    if (snapshot.state == "error") {
        if (!snapshot.message.empty()) {
            return snapshot.message;
        }
        if (!fallback.empty()) {
            return fallback;
        }
        return "升级失败";
    }
    if (snapshot.state == "rebooting") {
        return "重启中";
    }
    if (!fallback.empty()) {
        return fallback;
    }
    return snapshot.current_version.empty()
        ? std::string("检查升级")
        : "当前 " + snapshot.current_version;
}

#if 0
bool WaitForOnlineWifi(std::string* status) {
    auto& wifi = WifiManager::GetInstance();
    if (wifi.IsConnected()) {
        return true;
    }

    Board::GetInstance().RequestNetwork();
    for (int i = 0; i < kOnlineWifiWaitLoops; ++i) {
        if (wifi.IsConnected()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(kOnlineWifiWaitDelayMs));
    }

    if (status != nullptr) {
        *status = "WiFi 未连接";
    }
    return false;
}

#endif

std::string SanitizeAssetName(const char* text) {
    std::string out;
    bool last_separator = false;
    for (const unsigned char* cursor = reinterpret_cast<const unsigned char*>(text != nullptr ? text : "");
         *cursor != '\0'; ++cursor) {
        const unsigned char ch = *cursor;
        if (std::isalnum(ch) != 0) {
            out.push_back(static_cast<char>(std::tolower(ch)));
            last_separator = false;
        } else if (ch == '-' || ch == '_') {
            if (!out.empty() && !last_separator) {
                out.push_back(static_cast<char>(ch));
                last_separator = true;
            }
        } else if (std::isspace(ch) != 0 || ch == '\'' || ch == '.' || ch == '/') {
            if (!out.empty() && !last_separator) {
                out.push_back('_');
                last_separator = true;
            }
        }
    }

    while (!out.empty() && (out.back() == '_' || out.back() == '-')) {
        out.pop_back();
    }
    return out;
}

bool ChunkIdEquals(const uint8_t* data, const char* id) {
    return std::memcmp(data, id, 4) == 0;
}

uint16_t ReadLe16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

uint32_t ReadLe32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) |
        (static_cast<uint32_t>(data[3]) << 24);
}

void CopyText(char* out, size_t out_size, const char* text) {
    if (out == nullptr || out_size == 0) {
        return;
    }
    snprintf(out, out_size, "%s", text != nullptr ? text : "");
}

void FillBatteryStatus(VocabularySnapshot* snapshot) {
    if (snapshot == nullptr) {
        return;
    }

    int level = 0;
    bool charging = false;
    bool discharging = false;
    if (!Board::GetInstance().GetBatteryLevel(level, charging, discharging)) {
        return;
    }

    snapshot->battery_available = true;
    snapshot->battery_charging = charging;
    snapshot->battery_level = static_cast<uint8_t>(std::min(100, std::max(0, level)));
}

uint8_t WifiSignalBars(int rssi) {
    if (rssi >= -55) {
        return 4;
    }
    if (rssi >= -67) {
        return 3;
    }
    if (rssi >= -75) {
        return 2;
    }
    return 1;
}

void FillWifiStatus(VocabularySnapshot* snapshot) {
    if (snapshot == nullptr) {
        return;
    }

    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) != ESP_OK || mode == WIFI_MODE_NULL) {
        return;
    }

    snapshot->wifi_ap_mode = WifiManager::GetInstance().IsConfigMode();
    if (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA) {
        wifi_ap_record_t ap_info = {};
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            snapshot->wifi_connected = true;
            snapshot->wifi_signal_bars = WifiSignalBars(ap_info.rssi);
            return;
        }
    }
}

bool ReadValidRtcLocalTime(tm* out_local_tm) {
    if (out_local_tm == nullptr) {
        return false;
    }
    RtcPcf8563* rtc = EggaGetRtc();
    tm value = {};
    if (rtc == nullptr || !rtc->GetTime(value)) {
        return false;
    }
    // Factory diagnostics historically left devices at 2024-01-01 08:00.
    // Dates before 2025 therefore cannot be trusted as customer wall time.
    if (value.tm_year < 125 || value.tm_year > 199 ||
        value.tm_mon < 0 || value.tm_mon > 11 ||
        value.tm_mday < 1 || value.tm_mday > 31 ||
        value.tm_hour < 0 || value.tm_hour > 23 ||
        value.tm_min < 0 || value.tm_min > 59 ||
        value.tm_sec < 0 || value.tm_sec > 59) {
        return false;
    }
    *out_local_tm = value;
    return true;
}

void FillClockStatus(VocabularySnapshot* snapshot) {
    if (snapshot == nullptr) {
        return;
    }
    tm local_tm = {};
    if (!ReadValidRtcLocalTime(&local_tm)) {
        snprintf(snapshot->clock, sizeof(snapshot->clock), "--:--");
        return;
    }
    snprintf(snapshot->clock,
             sizeof(snapshot->clock),
             "%02d:%02d",
             local_tm.tm_hour,
             local_tm.tm_min);
}

}  // namespace

void VocabularyApp::Initialize(Display* display) {
    std::lock_guard<std::mutex> lock(mutex_);
    display_ = display;
    today_ = TodayDayIndex();
    last_activity_ms_ = NowMs();
    if (wifi_idle_timer_ == nullptr) {
        const esp_timer_create_args_t timer_args = {
            .callback = &VocabularyApp::WifiIdleTimerEntry,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "vocab_wifi_idle",
        };
        if (esp_timer_create(&timer_args, &wifi_idle_timer_) != ESP_OK) {
            wifi_idle_timer_ = nullptr;
            ESP_LOGW(kTag, "Unable to create WiFi idle timer");
        }
    }
    if (progress_save_timer_ == nullptr) {
        const esp_timer_create_args_t timer_args = {
            .callback = &VocabularyApp::ProgressSaveTimerEntry,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "vocab_progress_idle",
        };
        if (esp_timer_create(&timer_args, &progress_save_timer_) != ESP_OK) {
            progress_save_timer_ = nullptr;
            ESP_LOGW(kTag, "Unable to create progress save timer");
        }
    }
    if (action_status_timer_ == nullptr) {
        const esp_timer_create_args_t timer_args = {
            .callback = &VocabularyApp::ActionStatusTimerEntry,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "vocab_action",
        };
        if (esp_timer_create(&timer_args, &action_status_timer_) != ESP_OK) {
            action_status_timer_ = nullptr;
            ESP_LOGW(kTag, "Unable to create action status timer");
        }
    }
    (void)progress_store_.LoadSettings(&settings_);
    onboarding_active_ = !settings_.onboarding_completed;
    Application::GetInstance().SetIdleSleepDelayMs(
        VocabularyAutoSleepDelayMs(settings_.auto_sleep_timeout));
    deck_store_.Initialize(settings_.active_deck_id);
    settings_.active_deck_id = deck_store_.Manifest().deck_id != nullptr
        ? deck_store_.Manifest().deck_id
        : "";

    const size_t word_count = deck_store_.Count();
    if (!progress_store_.Load(settings_.active_deck_id,
                              BuildProgressWordKeysLocked(),
                              &states_, &stats_)) {
        states_.assign(word_count, ReviewState{});
        stats_ = VocabularyDailyStats{};
    }
    tm verified_rtc_time = {};
    const bool rtc_time_valid = ReadValidRtcLocalTime(&verified_rtc_time);
    if (!rtc_time_valid && stats_.day != 0) {
        // With no trustworthy wall clock, retain the persisted natural day.
        // A later SNTP correction will re-enter the normal cross-day path.
        today_ = stats_.day;
    }
    (void)progress_store_.SaveSettings(settings_);
    progress_store_.ResetDailyStatsIfNeeded(today_, &states_, &stats_);
    (void)maimemo_offline_store_.Load(&maimemo_offline_state_);
    maimemo_offline_state_.refreshed_at_ms = 0;
    if (maimemo_offline_state_.snapshot_day_index == today_ &&
        !maimemo_offline_state_.today_items.empty()) {
        (void)LoadMaimemoOfflineItemLocked(maimemo_offline_state_.current_index,
                                           "已载入墨墨离线快照");
    }
    RebuildQueueLocked();
    RestoreCurrentPositionLocked();
    if (settings_.study_mode == VocabularyStudyMode::kMaimemo) {
        StartOnlineWifiLocked("正在打开 WiFi...");
        ArmMaimemoWifiIdleTimerLocked();
    } else {
        StopStationForOfflineLocked();
    }
    if (!rtc_time_valid &&
        settings_.study_mode != VocabularyStudyMode::kMaimemo &&
        HasSavedWifiCredentials()) {
        rtc_time_repair_pending_ = true;
        StartOnlineWifiLocked("正在校准设备时间...");
    }
    WriteNfcEntryLink();
    PublishSnapshotLocked(true);
}

void VocabularyApp::HandleButton(VocabularyButton button) {
    Application::GetInstance().RecordUserActivity();
    // All display publication must remain on the application/UI task. The
    // former try-lock fallback handed a button to a second task, which could
    // publish to the e-paper display concurrently and panic on rapid presses.
    std::lock_guard<std::mutex> lock(mutex_);
    HandleButtonLocked(button);
}

void VocabularyApp::HandleButtonLocked(VocabularyButton button) {
    ESP_LOGI(kTag,
             "button=%u mode=%u settings=%d back=%d current=%u queue_pos=%u queue_size=%u",
             static_cast<unsigned>(button),
             static_cast<unsigned>(settings_.study_mode),
             settings_open_ ? 1 : 0,
             showing_back_ ? 1 : 0,
             static_cast<unsigned>(current_word_index_),
             static_cast<unsigned>(queue_pos_),
             static_cast<unsigned>(queue_.size()));

    MarkUserActivityLocked();
    RefreshCurrentDayLocked();
    if (onboarding_active_) {
        if (button == VocabularyButton::kConfirmClick) {
            onboarding_active_ = false;
            settings_.onboarding_completed = true;
            (void)progress_store_.SaveSettings(settings_);
            PublishSnapshotLocked(true);
        } else {
            PublishSnapshotLocked(false);
        }
        return;
    }

    if (low_battery_popup_open_) {
        if (button == VocabularyButton::kConfirmClick) {
            low_battery_popup_open_ = false;
            low_battery_alert_dismissed_ = true;
        }
        PublishSnapshotLocked(false);
        return;
    }

    if (ai_overlay_active_) {
        AiDialogService& ai = AiDialogService::GetInstance();
        if (button == VocabularyButton::kConfirmLongPress) {
            if (!ai.IsBusy()) {
                ai_page_delta_ = 0;
                ++ai_page_request_id_;
                UpdateAiWordContextLocked();
                ai.StartPtt();
            }
        } else if (button == VocabularyButton::kConfirmLongRelease) {
            ai.FinishPtt();
        } else if (button == VocabularyButton::kConfirmClick) {
            if (ai.IsBusy()) {
                ai.CancelTurn();
            } else {
                CloseAiOverlayLocked();
            }
        } else if (button == VocabularyButton::kUpClick ||
                   button == VocabularyButton::kUpLongPress) {
            ai_page_delta_ = -1;
            ++ai_page_request_id_;
        } else if (button == VocabularyButton::kDownClick ||
                   button == VocabularyButton::kDownLongPress) {
            ai_page_delta_ = 1;
            ++ai_page_request_id_;
        }
        PublishSnapshotLocked(false);
        return;
    }

    if (settings_open_) {
        HandleSettingsButtonLocked(button);
        PublishSnapshotLocked(false);
        return;
    }

    if (button == VocabularyButton::kConfirmDoubleClick &&
        (deck_store_.Count() != 0 || maimemo_has_card_)) {
        const AiDialogConfig ai_config = AiDialogService::LoadConfig();
        if (ai_config.enabled && ai_config.entitlement_verified) {
            OpenAiOverlayLocked();
            PublishSnapshotLocked(false);
            return;
        }
    }

    if (button == VocabularyButton::kConfirmLongRelease) {
        return;
    }

    if (settings_.study_mode == VocabularyStudyMode::kMaimemo) {
        if (button != VocabularyButton::kSettingsCombo && !WifiStationConnected() &&
            HandleMaimemoOfflineButtonLocked(button)) {
            StartOnlineWifiLocked(nullptr);
            PublishSnapshotLocked(false);
            return;
        }
        if (button != VocabularyButton::kSettingsCombo &&
            !EnsureMaimemoWifiForUserActionLocked()) {
            QueuePendingMaimemoButtonLocked(button, "正在重新连接 WiFi，请稍候再操作");
            PublishSnapshotLocked(false);
            return;
        }
        HandleMaimemoButtonLocked(button);
        PublishSnapshotLocked(false);
        return;
    }

    if (settings_.study_mode == VocabularyStudyMode::kBrowse) {
        switch (button) {
            case VocabularyButton::kConfirmClick:
            case VocabularyButton::kDownClick:
            case VocabularyButton::kDownLongPress:
                ShowActionStatusLocked("下一个");
                BrowseCardLocked(1);
                break;
            case VocabularyButton::kUpClick:
            case VocabularyButton::kUpLongPress:
                ShowActionStatusLocked("上一个");
                BrowseCardLocked(-1);
                break;
            case VocabularyButton::kConfirmDoubleClick:
                break;
            case VocabularyButton::kConfirmLongPress:
                ShowActionStatusLocked("播放发音");
                StartTemporaryPronunciationWifiLocked();
                PlayCurrentPronunciationLocked();
                break;
            case VocabularyButton::kSettingsCombo:
                EnterSettingsLocked();
                break;
            case VocabularyButton::kConfirmLongRelease:
                break;
        }
        showing_back_ = true;
        SaveCurrentPositionLocked();
        PublishSnapshotLocked(false);
        return;
    }

    switch (button) {
        case VocabularyButton::kConfirmClick:
            if (deck_store_.Count() == 0) {
                break;
            }
            if (!showing_back_) {
                ShowActionStatusLocked("查看释义");
                showing_back_ = true;
                MaybeAutoPlayCurrentPronunciationLocked();
            } else {
                ShowActionStatusLocked("认识");
                ScoreCurrentLocked(true);
            }
            break;
        case VocabularyButton::kConfirmLongPress:
            ShowActionStatusLocked("播放发音");
            StartTemporaryPronunciationWifiLocked();
            PlayCurrentPronunciationLocked();
            break;
        case VocabularyButton::kConfirmDoubleClick:
            break;
        case VocabularyButton::kUpClick:
            if (showing_back_) {
                ShowActionStatusLocked("忘记");
                MissCurrentLocked();
            } else {
                ShowActionStatusLocked("请先查看释义");
            }
            break;
        case VocabularyButton::kDownClick:
            if (showing_back_) {
                ShowActionStatusLocked("模糊");
                DeferCurrentLocked();
            } else {
                ShowActionStatusLocked("请先查看释义");
            }
            break;
        case VocabularyButton::kUpLongPress:
            ShowActionStatusLocked("上一个");
            MoveCardLocked(-1);
            break;
        case VocabularyButton::kDownLongPress:
            ShowActionStatusLocked("下一个");
            MoveCardLocked(1);
            break;
        case VocabularyButton::kSettingsCombo:
            EnterSettingsLocked();
            break;
        case VocabularyButton::kConfirmLongRelease:
            break;
    }

    ArmProgressSaveTimerLocked();
    SaveCurrentPositionLocked();
    PublishSnapshotLocked(false);
}

void VocabularyApp::RefreshSnapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    RefreshCurrentDayLocked();
    const AiDialogSnapshot ai = AiDialogService::GetInstance().GetSnapshot();
    if (ai_overlay_active_ &&
        ai.navigation_request_id != 0 &&
        ai.navigation_request_id != last_ai_navigation_request_id_) {
        last_ai_navigation_request_id_ = ai.navigation_request_id;
        const int delta = ai.navigation_action == AiNavigationAction::kPreviousCard ? -1 : 1;
        const char* action_text = delta < 0 ? "上一个" : "下一个";

        ESP_LOGI(kTag,
                 "Executing AI navigation request=%u action=%u mode=%u",
                 static_cast<unsigned>(ai.navigation_request_id),
                 static_cast<unsigned>(ai.navigation_action),
                 static_cast<unsigned>(settings_.study_mode));
        if (settings_.study_mode == VocabularyStudyMode::kMaimemo && maimemo_has_card_) {
            ShowActionStatusLocked("请先评价");
            AiDialogService::GetInstance().ResolveNavigationRequest(
                ai.navigation_request_id,
                "墨墨模式不能跳过，请先选择认识、模糊或忘记。");
        } else if (settings_.study_mode == VocabularyStudyMode::kMaimemo) {
            HandleMaimemoButtonLocked(
                delta < 0 ? VocabularyButton::kUpClick : VocabularyButton::kDownClick);
        } else if (settings_.study_mode == VocabularyStudyMode::kBrowse) {
            ShowActionStatusLocked(action_text);
            BrowseCardLocked(delta);
            SaveCurrentPositionLocked();
            UpdateAiWordContextLocked();
            AiDialogService::GetInstance().RefreshLearningContextConnection();
        } else {
            ShowActionStatusLocked(action_text);
            MoveCardLocked(delta);
            SaveCurrentPositionLocked();
            UpdateAiWordContextLocked();
            AiDialogService::GetInstance().RefreshLearningContextConnection();
        }
    }
    PublishSnapshotLocked(false);
}

void VocabularyApp::UpdateAiWordContextLocked() {
    AiLearningContext context;
    context.course = "英语单词";
    context.lesson = snapshot_.header;
    context.learning_type = "english_vocabulary";
    context.revealed = snapshot_.showing_back;
    context.current_character = snapshot_.word;
    context.current_pinyin = snapshot_.phonetic;
    context.visible_words = snapshot_.meaning;
    if (snapshot_.example[0] != '\0') {
        context.visible_words += "；例句：";
        context.visible_words += snapshot_.example;
    }
    AiDialogService::GetInstance().SetLearningContext(context);
}

void VocabularyApp::OpenAiOverlayLocked() {
    StartTemporaryPronunciationWifiLocked();
    ai_overlay_active_ = true;
    ai_page_delta_ = 0;
    ++ai_page_request_id_;
    UpdateAiWordContextLocked();
    AiDialogService::GetInstance().StartPtt();
}

void VocabularyApp::CloseAiOverlayLocked() {
    AiDialogService::GetInstance().Stop();
    ai_overlay_active_ = false;
    RestoreWifiAfterNetworkTaskLocked();
}

void VocabularyApp::StartAiEntitlementCheckLocked() {
    if (ai_entitlement_checking_) {
        return;
    }
    if (!WifiManager::GetInstance().IsConnected()) {
        ai_entitlement_status_ = "请先连接 Wi-Fi，再开启 AI";
        return;
    }

    ai_entitlement_checking_ = true;
    ai_entitlement_status_ = "正在检查 AI 开通状态";
    const uint32_t request_id =
        ai_entitlement_request_.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto* args = new (std::nothrow) AiEntitlementTaskArgs{
        .app = this,
        .request_id = request_id,
    };
    if (args == nullptr) {
        ai_entitlement_checking_ = false;
        ai_entitlement_status_ = "检查任务启动失败，请重试";
        return;
    }
    if (xTaskCreate(&VocabularyApp::AiEntitlementTaskEntry,
                    "ai_entitlement",
                    kAiEntitlementTaskStackBytes,
                    args,
                    4,
                    nullptr) != pdPASS) {
        delete args;
        ai_entitlement_checking_ = false;
        ai_entitlement_status_ = "检查任务启动失败，请重试";
    }
}

void VocabularyApp::RunAiEntitlementCheck(uint32_t request_id) {
    std::string status_message;
    const AiEntitlementStatus status =
        AiDialogService::GetInstance().CheckEntitlement(&status_message);

    std::lock_guard<std::mutex> lock(mutex_);
    if (ai_entitlement_request_.load(std::memory_order_acquire) != request_id) {
        return;
    }
    ai_entitlement_checking_ = false;
    AiDialogConfig config = AiDialogService::LoadConfig();
    if (status == AiEntitlementStatus::kAvailable ||
        status == AiEntitlementStatus::kQuotaExceeded) {
        config.enabled = true;
        config.entitlement_verified = true;
        std::string save_error;
        if (AiDialogService::SaveConfig(config, &save_error)) {
            const AiDialogConfig persisted = AiDialogService::LoadConfig();
            if (!persisted.enabled || !persisted.entitlement_verified) {
                popup_ai_enabled_ = false;
                ai_entitlement_status_ = "AI 设置保存失败，请重试";
                AiDialogService::GetInstance().ReloadConfig();
                PublishSnapshotLocked(false);
                return;
            }
            popup_ai_enabled_ = true;
            ai_entitlement_status_.clear();
            // Successful entitlement verification is the confirmation step:
            // persist “on” and return to the settings list immediately.  The
            // previous flow left this popup open and made the saved state look
            // as if it still needed another confirmation.
            settings_popup_action_ = SettingsPopupAction::kNone;
            settings_view_ = settings_popup_return_view_;
        } else {
            popup_ai_enabled_ = false;
            ai_entitlement_status_ = "AI 设置保存失败，请重试";
        }
        AiDialogService::GetInstance().ReloadConfig();
    } else if (status == AiEntitlementStatus::kNotEntitled) {
        config.enabled = false;
        config.entitlement_verified = false;
        std::string save_error;
        (void)AiDialogService::SaveConfig(config, &save_error);
        AiDialogService::GetInstance().ReloadConfig();
        popup_ai_enabled_ = false;
        ai_entitlement_status_ = std::string("未开通 AI 教师，请联系开发者获取权限。") +
            kDeveloperContact;
    } else {
        // Keep “开启” selected so confirming again retries after Wi-Fi recovers.
        popup_ai_enabled_ = true;
        ai_entitlement_status_ = status_message.empty()
            ? "检查失败，请确认 Wi-Fi 后重试"
            : status_message;
    }
    PublishSnapshotLocked(false);
}

void VocabularyApp::StartRtcTimeSyncLocked() {
    bool expected = false;
    if (!rtc_time_sync_busy_.compare_exchange_strong(expected,
                                                     true,
                                                     std::memory_order_acq_rel)) {
        return;
    }
    const BaseType_t created = xTaskCreate(&VocabularyApp::RtcTimeSyncTaskEntry,
                                           "rtc_time_sync",
                                           4096,
                                           this,
                                           3,
                                           nullptr);
    if (created != pdPASS) {
        rtc_time_sync_busy_.store(false, std::memory_order_release);
        ESP_LOGW(kTag, "Unable to start RTC time synchronization task");
    }
}

void VocabularyApp::NotifyNetworkConnected() {
    std::lock_guard<std::mutex> lock(mutex_);
    StartRtcTimeSyncLocked();
    if (maimemo_wake_wifi_start_ms_ > 0) {
        ESP_LOGI(kTag, "Wake WiFi connected in %lld ms",
                 static_cast<long long>(NowMs() - maimemo_wake_wifi_start_ms_));
        maimemo_wake_wifi_start_ms_ = 0;
    }
    if (settings_.study_mode == VocabularyStudyMode::kMaimemo && !maimemo_busy_) {
        if (!maimemo_offline_state_.pending_answers.empty()) {
            StartMaimemoPendingSyncLocked();
        } else {
            // A card restored from the offline snapshot is not guaranteed to
            // be the current WebStudy card. Establish the server-side word
            // before accepting an answer after WiFi reconnects.
            maimemo_status_ = "WiFi 已连接，正在同步墨墨当前单词...";
            StartMaimemoFetchLocked(false);
        }
    }
    PublishSnapshotLocked(false);
}

VocabularySnapshot VocabularyApp::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(published_mutex_);
    return snapshot_;
}

std::string VocabularyApp::DeckStatusJson() const {
    std::lock_guard<std::mutex> lock(published_mutex_);
    std::string json = "{";
    json += "\"deck_id\":" + JsonString(published_deck_id_) + ",";
    json += "\"name\":" + JsonString(published_deck_name_) + ",";
    json += "\"language\":" + JsonString(published_deck_language_) + ",";
    json += "\"version\":" + std::to_string(published_deck_version_) + ",";
    json += "\"word_count\":" + std::to_string(published_deck_word_count_);
    json += "}";
    return json;
}

std::string VocabularyApp::DiagnosticsJson() const {
    std::string json = "{";
    json += "\"free_internal\":" +
        std::to_string(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) + ",";
    json += "\"min_internal\":" +
        std::to_string(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)) + ",";
    json += "\"uptime_ms\":" + std::to_string(NowMs()) + ",";
    json += "\"reset_reason\":" + std::to_string(static_cast<int>(esp_reset_reason())) + ",";
    json += "\"idle_sleep_delay_ms\":" +
        std::to_string(Application::GetInstance().IdleSleepDelayMs()) + ",";
    json += "\"deck\":" + DeckStatusJson() + ",";

    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    json += "\"state_locked\":" + JsonBool(!lock.owns_lock());
    if (lock.owns_lock()) {
        json += ",\"settings_open\":" + JsonBool(settings_open_);
        json += ",\"deck_sync_busy\":" + JsonBool(deck_sync_busy_);
        json += ",\"maimemo_busy\":" + JsonBool(maimemo_busy_);
        json += ",\"maimemo_persistence_busy\":" + JsonBool(maimemo_persistence_busy_);
        json += ",\"pronunciation_busy\":" + JsonBool(pronunciation_busy_);
        json += ",\"pronunciation_play_audio\":" + JsonBool(pronunciation_play_audio_);
        json += ",\"maimemo_submit_waiting_for_pronunciation\":" +
            JsonBool(maimemo_submit_waiting_for_pronunciation_);
        json += ",\"ota_busy\":" + JsonBool(ota_busy_);
        json += ",\"local_decks\":" + std::to_string(deck_store_.Decks().size());
        json += ",\"remote_decks\":" + std::to_string(remote_decks_.size());
        json += ",\"queue_size\":" + std::to_string(queue_.size());
        json += ",\"current_word_index\":" + std::to_string(current_word_index_);
        json += ",\"current_index_dirty\":" + JsonBool(current_index_dirty_);
        json += ",\"progress_dirty\":" + JsonBool(progress_dirty_);
        json += ",\"progress_save_busy\":" + JsonBool(progress_save_busy_);
        json += ",\"maimemo_snapshot_day\":" +
            std::to_string(maimemo_offline_state_.snapshot_day_index);
        json += ",\"maimemo_snapshot_items\":" +
            std::to_string(maimemo_offline_state_.today_items.size());
        json += ",\"maimemo_pending_answers\":" +
            std::to_string(maimemo_offline_state_.pending_answers.size());
        json += ",\"maimemo_conflicts\":" +
            std::to_string(maimemo_offline_state_.conflicts.size());
        json += ",\"maimemo_review_marks\":" +
            std::to_string(maimemo_offline_state_.review_marks.size());
        json += ",\"deck_refresh_ms\":" + std::to_string(deck_store_.LastRefreshMs());
        json += ",\"deck_load_ms\":" + std::to_string(deck_store_.LastLoadMs());
        json += ",\"deck_index_entries\":" + std::to_string(deck_store_.LastRefreshIndexEntries());
        json += ",\"deck_used_index\":" + JsonBool(deck_store_.LastRefreshUsedIndex());
        json += ",\"deck_load_used_cache\":" + JsonBool(deck_store_.LastLoadUsedCache());
        json += ",\"last_loaded_deck_id\":" + JsonString(deck_store_.LastLoadedDeckId());
        json += ",\"deck_sync_status\":" + JsonString(deck_sync_status_);
        json += ",\"deck_sync_detail\":" + JsonString(deck_sync_detail_);
        json += ",\"sync_status\":" + JsonString(sync_client_.LastStatus());
    }
    json += "}";
    return json;
}

std::string VocabularyApp::MaimemoOfflineStatusJson() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string json = "{";
    json += "\"snapshot_day\":" + std::to_string(maimemo_offline_state_.snapshot_day_index) + ",";
    json += "\"today_items\":" + std::to_string(maimemo_offline_state_.today_items.size()) + ",";
    json += "\"current_index\":" + std::to_string(maimemo_offline_state_.current_index) + ",";
    json += "\"pending_answers\":" +
        std::to_string(maimemo_offline_state_.pending_answers.size()) + ",";
    json += "\"conflicts\":" + std::to_string(maimemo_offline_state_.conflicts.size()) + ",";
    json += "\"review_marks\":" +
        std::to_string(maimemo_offline_state_.review_marks.size()) + ",";
    json += "\"conflict_words\":[";
    const size_t conflict_count = std::min<size_t>(20, maimemo_offline_state_.conflicts.size());
    for (size_t index = 0; index < conflict_count; ++index) {
        if (index > 0) {
            json += ",";
        }
        json += JsonString(maimemo_offline_state_.conflicts[index].word);
    }
    json += "]}";
    return json;
}

bool VocabularyApp::ClearProgress() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!progress_store_.ClearProgress(settings_.active_deck_id)) {
        return false;
    }

    states_.assign(deck_store_.Count(), ReviewState{});
    stats_ = VocabularyDailyStats{};
    progress_store_.ResetDailyStatsIfNeeded(today_, &states_, &stats_);
    RebuildQueueLocked();
    PublishSnapshotLocked(true);
    return true;
}

bool VocabularyApp::SelectDeck(const std::string& deck_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const VocabularyStudyMode previous_mode = settings_.study_mode;
    settings_.study_mode = VocabularyStudyMode::kOffline;
    if (!SwitchDeckLocked(deck_id)) {
        settings_.study_mode = previous_mode;
        return false;
    }
    ResetMaimemoSessionLocked(true);
    (void)progress_store_.SaveSettings(settings_);
    PublishSnapshotLocked(true);
    return true;
}

bool VocabularyApp::ClearMaimemoOfflineState() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (maimemo_persistence_busy_) {
        ESP_LOGW(kTag, "Maimemo offline clear deferred while persistence is active");
        return false;
    }
    maimemo_persistence_request_.fetch_add(1, std::memory_order_acq_rel);
    maimemo_full_state_dirty_ = false;
    maimemo_pending_answers_dirty_ = false;
    maimemo_request_.fetch_add(1, std::memory_order_acq_rel);
    maimemo_client_.Reset();
    if (!maimemo_offline_store_.Clear()) {
        return false;
    }
    maimemo_offline_state_ = MaimemoOfflineState{};
    maimemo_pending_buttons_.clear();
    maimemo_has_card_ = false;
    maimemo_showing_back_ = false;
    maimemo_busy_ = false;
    maimemo_submit_waiting_for_pronunciation_ = false;
    maimemo_submit_started_ms_ = 0;
    maimemo_pending_reveal_ = false;
    maimemo_status_ = "墨墨离线记录已清理";
    if (settings_.study_mode == VocabularyStudyMode::kMaimemo && WifiStationConnected()) {
        StartMaimemoFetchLocked(false);
    }
    PublishSnapshotLocked(false);
    return true;
}

void VocabularyApp::NotifyMaimemoAuthChanged() {
    std::lock_guard<std::mutex> lock(mutex_);
    maimemo_request_.fetch_add(1, std::memory_order_acq_rel);
    maimemo_client_.Reset();
    maimemo_pending_buttons_.clear();
    maimemo_has_card_ = false;
    maimemo_showing_back_ = false;
    maimemo_busy_ = false;
    maimemo_submit_waiting_for_pronunciation_ = false;
    maimemo_submit_started_ms_ = 0;
    maimemo_pending_reveal_ = false;
    maimemo_offline_state_.refreshed_at_ms = 0;
    const MaimemoAuthConfig config = MaimemoAuthStore::GetInstance().Load();
    maimemo_status_ = config.configured
        ? (config.open_api_configured ? "墨墨认证及离线 Token 已导入" : "墨墨认证已导入")
        : "未导入墨墨认证";
    if (settings_.study_mode == VocabularyStudyMode::kMaimemo && config.configured) {
        if (maimemo_offline_state_.snapshot_day_index == today_ &&
            !maimemo_offline_state_.today_items.empty()) {
            (void)LoadMaimemoOfflineItemLocked(maimemo_offline_state_.current_index,
                                               "认证已更新，离线快照可用");
        }
        if (WifiStationConnected()) {
            StartMaimemoFetchLocked(false);
        } else {
            StartOnlineWifiLocked("认证已更新，正在连接 WiFi");
        }
    }
    PublishSnapshotLocked(false);
}

bool VocabularyApp::CanEnterLowPowerMode() const {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        return false;
    }
    if (settings_open_) {
        return false;
    }
    if (pronunciation_busy_ || deck_sync_busy_ || maimemo_busy_ ||
        maimemo_persistence_busy_ || maimemo_full_state_dirty_ ||
        maimemo_pending_answers_dirty_ || ota_busy_) {
        return false;
    }
    if (OtaSnapshotActive(OtaService::GetInstance().GetSnapshot())) {
        return false;
    }
    return true;
}

void VocabularyApp::PrepareForLowPowerMode() {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        ESP_LOGW(kTag, "low power preparation skipped while vocabulary state is busy");
        return;
    }
    SaveCurrentPositionLocked();
    DisarmMaimemoWifiIdleTimerLocked();
    offline_pronunciation_wifi_active_ = false;
    offline_pronunciation_wifi_deadline_ms_ = 0;
    maimemo_wifi_suspended_ = true;
    WifiManager::GetInstance().StopStation();
}

void VocabularyApp::HandleUserWakeFromLowPowerMode(bool publish_snapshot) {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        ESP_LOGW(kTag, "wake resume skipped while vocabulary state is busy");
        RefreshSnapshot();
        return;
    }

    RefreshCurrentDayLocked();

    const bool should_restore_wifi =
        settings_.study_mode == VocabularyStudyMode::kMaimemo &&
        maimemo_wifi_suspended_ &&
        !WifiStationConnected();
    wake_wifi_restore_pending_ = should_restore_wifi && HasSavedWifiCredentials();

    MarkUserActivityLocked();
    if (publish_snapshot) {
        PublishSnapshotLocked(false);
    }
}

void VocabularyApp::RestoreNetworkAfterUserWake() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!wake_wifi_restore_pending_) {
        return;
    }

    wake_wifi_restore_pending_ = false;
    if (settings_.study_mode != VocabularyStudyMode::kMaimemo ||
        !maimemo_wifi_suspended_ || WifiStationConnected()) {
        return;
    }

    maimemo_wake_wifi_start_ms_ = NowMs();
    ESP_LOGI(kTag, "Wake resume starting WiFi restore after display update queued");
    StartOnlineWifiLocked("正在恢复 WiFi...");
}

int32_t VocabularyApp::TodayDayIndex() const {
    tm local_tm = {};
    if (ReadValidRtcLocalTime(&local_tm)) {
        return DaysFromCivil(local_tm.tm_year + 1900,
                             static_cast<unsigned>(local_tm.tm_mon + 1),
                             static_cast<unsigned>(local_tm.tm_mday));
    }

    // Do not invent a rolling 24-hour "day" from boot uptime. Keeping the last
    // known day is safer until RTC/SNTP supplies a trustworthy civil date.
    return today_ != 0 ? today_ : DaysFromCivil(2026, 1, 1);
}

bool VocabularyApp::RefreshCurrentDayLocked() {
    const int32_t current_day = TodayDayIndex();
    if (current_day == today_) {
        return false;
    }

    ESP_LOGI(kTag,
             "local study day changed old=%ld new=%ld reviewed=%u",
             static_cast<long>(today_),
             static_cast<long>(current_day),
             static_cast<unsigned>(stats_.reviewed));
    today_ = current_day;
    progress_store_.ResetDailyStatsIfNeeded(today_, &states_, &stats_);
    RebuildQueueLocked();
    SaveCurrentPositionLocked();
    MarkProgressDirtyLocked();
    ArmProgressSaveTimerLocked();
    return true;
}

void VocabularyApp::RebuildQueueLocked(bool preserve_current) {
    const size_t previous_word_index = current_word_index_;
    const bool previous_showing_back = showing_back_;
    queue_ = scheduler_.BuildQueue(states_, today_, settings_, stats_.reviewed);
    queue_pos_ = 0;
    const size_t word_count = deck_store_.Count();
    if (word_count == 0) {
        current_word_index_ = 0;
    } else if (preserve_current && previous_word_index < word_count) {
        current_word_index_ = previous_word_index;
        auto it = std::find(queue_.begin(), queue_.end(), current_word_index_);
        if (it != queue_.end()) {
            queue_pos_ = static_cast<size_t>(std::distance(queue_.begin(), it));
        }
    } else if (!queue_.empty()) {
        current_word_index_ = std::min(queue_.front(), word_count - 1);
    } else if (current_word_index_ >= word_count) {
        current_word_index_ = 0;
    }
    showing_back_ = preserve_current ? previous_showing_back : false;
}

void VocabularyApp::PublishSnapshotLocked(bool full_refresh, bool render_now) {
    VocabularySnapshot next_snapshot = BuildSnapshotLocked();
    if (ai_overlay_active_) {
        const AiDialogSnapshot ai = AiDialogService::GetInstance().GetSnapshot();
        next_snapshot.ai_overlay_active = true;
        next_snapshot.ai_state = static_cast<uint8_t>(ai.state);
        next_snapshot.ai_page_delta = ai_page_delta_;
        next_snapshot.ai_page_request_id = ai_page_request_id_;
        CopyText(next_snapshot.ai_question, sizeof(next_snapshot.ai_question), ai.question.c_str());
        CopyText(next_snapshot.ai_answer, sizeof(next_snapshot.ai_answer), ai.answer.c_str());
        CopyText(next_snapshot.ai_message, sizeof(next_snapshot.ai_message), ai.message.c_str());
        snprintf(next_snapshot.hint, sizeof(next_snapshot.hint), "%s",
                 (ai.state == AiDialogState::kListening ||
                  ai.state == AiDialogState::kConnecting)
                     ? "正在录音 · 松开确认键发送"
                     : ((ai.state == AiDialogState::kThinking ||
                         ai.state == AiDialogState::kSpeaking)
                            ? "AI 处理中 · 短按确认取消"
                            : "长按确认继续问 · 短按确认返回"));
    }
    UpdateLowBatteryAlertLocked(&next_snapshot);
    {
        std::lock_guard<std::mutex> published_lock(published_mutex_);
        snapshot_ = next_snapshot;
        const DeckManifest& manifest = deck_store_.Manifest();
        published_deck_id_ = manifest.deck_id != nullptr ? manifest.deck_id : "";
        published_deck_name_ = manifest.name != nullptr ? manifest.name : "";
        published_deck_language_ = manifest.language != nullptr ? manifest.language : "";
        published_deck_version_ = manifest.version;
        published_deck_word_count_ = manifest.word_count;
    }
    if (display_ == nullptr) {
        return;
    }
    display_->UpdateVocabularySnapshot(next_snapshot);
    if (render_now) {
        // OTA progress changes are small and can race the asynchronous e-paper
        // sampler. Rasterize the new label/bar into the 1-bpp framebuffer
        // before requesting the physical partial refresh.
        display_->RenderNow();
    }
    if (full_refresh) {
        display_->RequestUrgentFullRefresh();
    } else {
        display_->RequestUrgentRefresh();
    }
}

void VocabularyApp::UpdateLowBatteryAlertLocked(VocabularySnapshot* snapshot) {
    if (snapshot == nullptr || !snapshot->battery_available) {
        low_battery_popup_open_ = false;
        return;
    }

    if (snapshot->battery_level >= kLowBatteryAlertThreshold) {
        low_battery_popup_open_ = false;
        low_battery_alert_dismissed_ = false;
        return;
    }

    if (!low_battery_alert_dismissed_) {
        low_battery_popup_open_ = true;
    }

    if (low_battery_popup_open_) {
        ApplyLowBatteryPopupLocked(snapshot);
    }
}

void VocabularyApp::ApplyLowBatteryPopupLocked(VocabularySnapshot* snapshot) const {
    if (snapshot == nullptr) {
        return;
    }

    snapshot->settings_popup_open = true;
    snprintf(snapshot->settings_popup_title,
             sizeof(snapshot->settings_popup_title),
             "低电量提醒");
    snprintf(snapshot->settings_popup_badge,
             sizeof(snapshot->settings_popup_badge),
             "%u%%",
             static_cast<unsigned>(snapshot->battery_level));
    snprintf(snapshot->settings_popup_body,
             sizeof(snapshot->settings_popup_body),
             "当前电量低于 20%%。\n请尽快充电，以免学习进度或联网任务中断。");
    snprintf(snapshot->settings_popup_hint,
             sizeof(snapshot->settings_popup_hint),
             "按确认键关闭");
}

VocabularySnapshot VocabularyApp::BuildSnapshotLocked() const {
    VocabularySnapshot snapshot;
    snapshot.compact_font = settings_.font_mode == VocabularyFontMode::kCompact;
    snapshot.total_count = static_cast<uint16_t>(std::min<size_t>(deck_store_.Count(), UINT16_MAX));
    snapshot.due_count = scheduler_.CountDue(states_, today_);
    snapshot.reviewed_today = stats_.reviewed;
    snapshot.correct_today = stats_.correct;
    snapshot.streak_days = static_cast<uint16_t>(
        std::min<uint32_t>(static_cast<uint32_t>(stats_.streak_days) +
                               (stats_.reviewed > 0 ? 1U : 0U),
                           UINT16_MAX));
    const size_t word_count = deck_store_.Count();
    snapshot.card_count = static_cast<uint16_t>(std::min<size_t>(word_count, UINT16_MAX));
    snapshot.card_position = word_count == 0
        ? 0
        : static_cast<uint16_t>(std::min<size_t>(current_word_index_ + 1, UINT16_MAX));

    const DeckManifest& manifest = deck_store_.Manifest();
    snprintf(snapshot.header, sizeof(snapshot.header), "%s", manifest.name);
    FillBatteryStatus(&snapshot);
    FillWifiStatus(&snapshot);
    FillClockStatus(&snapshot);
    snprintf(snapshot.progress, sizeof(snapshot.progress), "%u/%u",
             static_cast<unsigned>(stats_.reviewed),
             static_cast<unsigned>(settings_.daily_target));

    if (onboarding_active_) {
        snapshot.screen = VocabularyScreen::kWelcome;
        snprintf(snapshot.header, sizeof(snapshot.header), "欢迎使用 Egga");
        snprintf(snapshot.progress, sizeof(snapshot.progress), "新设备");
        snprintf(snapshot.word, sizeof(snapshot.word), "EGGA");
        snprintf(snapshot.meaning, sizeof(snapshot.meaning), "很高兴见到你");
        snprintf(snapshot.example,
                 sizeof(snapshot.example),
                 "中键：查看释义 / 确认认识\n"
                 "上键：忘记    下键：模糊\n"
                 "同时按上键和下键：打开设置");
        snprintf(snapshot.hint,
                 sizeof(snapshot.hint),
                 "离线词库已准备好，无需联网即可开始。");
        snprintf(snapshot.footer, sizeof(snapshot.footer), "按中键开始学习");
        return snapshot;
    }

    if (snapshot.wifi_ap_mode && !settings_open_) {
        const std::string ap_ssid = WifiManager::GetInstance().GetApSsid();
        const std::string ap_url = WifiManager::GetInstance().GetApWebUrl();
        snapshot.screen = VocabularyScreen::kDailySummary;
        snprintf(snapshot.header, sizeof(snapshot.header), "Wi-Fi 配网");
        snprintf(snapshot.progress, sizeof(snapshot.progress), "AP");
        snprintf(snapshot.word, sizeof(snapshot.word), "Wi-Fi");
        snprintf(snapshot.meaning,
                 sizeof(snapshot.meaning),
                 "手机连接热点：%s",
                 ap_ssid.empty() ? "Egga-Setup" : ap_ssid.c_str());
        snprintf(snapshot.example,
                 sizeof(snapshot.example),
                 "连接后打开：%s\n选择家庭 Wi-Fi 并输入密码。",
                 ap_url.empty() ? "http://192.168.4.1" : ap_url.c_str());
        snprintf(snapshot.hint, sizeof(snapshot.hint), "提交成功后设备会保存网络并自动连接。");
        snprintf(snapshot.footer, sizeof(snapshot.footer), "扫码配网  上+下：返回设置");
        return snapshot;
    }

    if (settings_open_) {
        return BuildSettingsSnapshotLocked();
    }
#if 0
        const bool deck_selected = settings_selection_ == 0;
        const bool order_selected = settings_selection_ == 1;
        const bool target_selected = settings_selection_ == 2;
        const bool voice_selected = settings_selection_ == 3;
        const bool mode_selected = settings_selection_ == 4;
        const std::vector<DeckChoice> deck_choices = BuildDeckChoicesLocked();
        std::string deck_text = manifest.name != nullptr ? manifest.name : "";
        if (!deck_choices.empty()) {
            const size_t index = std::min(settings_deck_index_, deck_choices.size() - 1);
            deck_text = deck_choices[index].name;
            if (!deck_choices[index].installed) {
                deck_text += " (下载)";
            }
        }
        snapshot.screen = VocabularyScreen::kSettings;
        snprintf(snapshot.word, sizeof(snapshot.word), "设置");
        snprintf(snapshot.meaning, sizeof(snapshot.meaning), "%c 词库：%s",
                 deck_selected ? '>' : ' ', deck_text.c_str());
        snprintf(snapshot.example_translation, sizeof(snapshot.example_translation), "%c 顺序：%s",
                 order_selected ? '>' : ' ', OrderModeText(settings_.order_mode));
        const size_t len = strlen(snapshot.example_translation);
        snprintf(snapshot.example_translation + len,
                 sizeof(snapshot.example_translation) - len,
                 "  %c 目标：%u",
                 target_selected ? '>' : ' ', static_cast<unsigned>(settings_.daily_target));
        snprintf(snapshot.example, sizeof(snapshot.example), "%c 发音：%s",
                 voice_selected ? '>' : ' ', VoiceTypeText(settings_.voice_type));
        const size_t example_len = strlen(snapshot.example);
        snprintf(snapshot.example + example_len,
                 sizeof(snapshot.example) - example_len,
                 "  %c 模式：%s",
                 mode_selected ? '>' : ' ', StudyModeText(settings_.study_mode));
        snapshot.hint[0] = '\0';
        snapshot.footer[0] = '\0';
        snprintf(snapshot.status_line, sizeof(snapshot.status_line), "词库 %u/%u",
                 static_cast<unsigned>(deck_choices.empty() ? 0 : settings_deck_index_ + 1),
                 static_cast<unsigned>(deck_choices.size()));
        return snapshot;
    }

#endif
    if (settings_.study_mode == VocabularyStudyMode::kMaimemo) {
        return BuildMaimemoSnapshotLocked();
    }

    if (word_count == 0) {
        snapshot.screen = VocabularyScreen::kDailySummary;
        snprintf(snapshot.word, sizeof(snapshot.word), "暂无单词");
        snprintf(snapshot.meaning, sizeof(snapshot.meaning), "当前词库为空。请在设置里切换或下载词库。");
        snapshot.example[0] = '\0';
        snapshot.hint[0] = '\0';
        snprintf(snapshot.footer, sizeof(snapshot.footer), "上+下：进入设置");
        return snapshot;
    }

    const size_t word_index = CurrentWordIndexLocked();
    const WordCard* card = deck_store_.Get(word_index);
    if (card == nullptr || word_index >= states_.size()) {
        snapshot.screen = VocabularyScreen::kDailySummary;
        snprintf(snapshot.word, sizeof(snapshot.word), "词库异常");
        snprintf(snapshot.meaning, sizeof(snapshot.meaning), "当前学习队列为空，请重新选择词库。");
        snapshot.hint[0] = '\0';
        return snapshot;
    }

    const ReviewState& state = states_[word_index];
    snapshot.has_card = true;
    const bool browse_mode = settings_.study_mode == VocabularyStudyMode::kBrowse;
    snapshot.showing_back = browse_mode || showing_back_;
    snapshot.status = state.status;
    snapshot.interval_days = state.interval_days;
    snapshot.lapses = state.lapses;
    snapshot.screen = (browse_mode || showing_back_) ? VocabularyScreen::kCardBack : VocabularyScreen::kCardFront;
    CopyText(snapshot.word, sizeof(snapshot.word), card->word);
    CopyText(snapshot.phonetic, sizeof(snapshot.phonetic), card->phonetic);
    CopyText(snapshot.meaning, sizeof(snapshot.meaning), card->meaning);
    CopyText(snapshot.example, sizeof(snapshot.example), card->example);
    CopyText(snapshot.example_translation, sizeof(snapshot.example_translation), card->example_translation);
    snprintf(snapshot.status_line, sizeof(snapshot.status_line), "%s %ud %u",
             StatusText(state.status),
             static_cast<unsigned>(state.interval_days),
             static_cast<unsigned>(state.lapses));
    CopyText(snapshot.action_status, sizeof(snapshot.action_status), action_status_.c_str());
    snapshot.hint[0] = '\0';
    if (browse_mode) {
        snprintf(snapshot.progress, sizeof(snapshot.progress), "%u/%u",
                 static_cast<unsigned>(snapshot.card_position),
                 static_cast<unsigned>(snapshot.card_count));
        snprintf(snapshot.footer, sizeof(snapshot.footer), "上：上一词  确认/下：下一词");
    } else if (showing_back_) {
        snprintf(snapshot.footer, sizeof(snapshot.footer), "上：忘记  确认：认识  下：模糊");
    } else {
        snprintf(snapshot.footer, sizeof(snapshot.footer), "长按上/下：切词  确认：释义");
    }
    return snapshot;
}

VocabularySnapshot VocabularyApp::BuildMaimemoSnapshotLocked() const {
    VocabularySnapshot snapshot;
    snapshot.compact_font = settings_.font_mode == VocabularyFontMode::kCompact;
    snprintf(snapshot.header, sizeof(snapshot.header), "墨墨在线");
    FillBatteryStatus(&snapshot);
    FillWifiStatus(&snapshot);
    FillClockStatus(&snapshot);
    const uint16_t maimemo_current_progress = maimemo_client_.ProgressCurrent();
    const uint16_t maimemo_daily_target = maimemo_client_.DailyTarget();
    uint32_t maimemo_display_progress = maimemo_current_progress;
    if (maimemo_daily_target > 0) {
        maimemo_display_progress = std::min<uint32_t>(maimemo_display_progress,
                                                      maimemo_daily_target);
    }
    if (maimemo_daily_target > 0) {
        snprintf(snapshot.progress, sizeof(snapshot.progress), "%u/%u",
                 static_cast<unsigned>(maimemo_display_progress),
                 static_cast<unsigned>(maimemo_daily_target));
    } else {
        snprintf(snapshot.progress, sizeof(snapshot.progress), "%u/--",
                 static_cast<unsigned>(maimemo_display_progress));
    }
    snprintf(snapshot.footer, sizeof(snapshot.footer), "确认：开始同步  上+下：设置");

    if (!snapshot.wifi_connected) {
        snapshot.screen = VocabularyScreen::kDailySummary;
        const bool has_saved_wifi = HasSavedWifiCredentials();
        snprintf(snapshot.word, sizeof(snapshot.word), "%s",
                 !has_saved_wifi ? "尚未配置 Wi-Fi"
                                 : (maimemo_wifi_suspended_ ? "Wi-Fi 已休眠" : "正在连接 Wi-Fi"));
        snprintf(snapshot.meaning, sizeof(snapshot.meaning), "%s",
                 !has_saved_wifi ? "请进入设置完成 Wi-Fi 配网。"
                                 : (maimemo_wifi_suspended_
                                     ? "按确认键或下键即可重新连接并继续。"
                                     : "正在恢复网络，刚才的操作会在连接后继续。"));
        snprintf(snapshot.example, sizeof(snapshot.example), "%s", maimemo_status_.c_str());
        snprintf(snapshot.footer, sizeof(snapshot.footer), "%s",
                 !has_saved_wifi ? "上+下：进入设置" : "确认/下：重新连接  上+下：设置");
        return snapshot;
    }

    if (maimemo_busy_ && !maimemo_has_card_) {
        snapshot.screen = VocabularyScreen::kDailySummary;
        snprintf(snapshot.word, sizeof(snapshot.word), "墨墨同步中");
        snprintf(snapshot.meaning, sizeof(snapshot.meaning), "%s", maimemo_status_.c_str());
        snprintf(snapshot.example, sizeof(snapshot.example), "请稍候，设备正在和墨墨背单词同步。");
        snprintf(snapshot.footer, sizeof(snapshot.footer), "操作已接收，请稍候");
        snapshot.hint[0] = '\0';
        return snapshot;
    }

    if (!maimemo_has_card_ && maimemo_status_ == kMaimemoDailyTargetCompleteStatus) {
        snapshot.screen = VocabularyScreen::kDailySummary;
        snprintf(snapshot.word, sizeof(snapshot.word), "今日目标已完成");
        snprintf(snapshot.meaning, sizeof(snapshot.meaning), "墨墨今天的目标单词已经背完。");
        snprintf(snapshot.example,
                 sizeof(snapshot.example),
                 "如果想继续学习，请在墨墨 App 或网页端增加今日任务后再同步。");
        snprintf(snapshot.footer, sizeof(snapshot.footer), "上+下：设置");
        snapshot.hint[0] = '\0';
        return snapshot;
    }

    if (!maimemo_has_card_) {
        snapshot.screen = VocabularyScreen::kDailySummary;
        snprintf(snapshot.word, sizeof(snapshot.word), "墨墨模式");
        snprintf(snapshot.meaning, sizeof(snapshot.meaning), "墨墨在线学习未开始。");
        snprintf(snapshot.example, sizeof(snapshot.example), "%s", maimemo_status_.c_str());
        snprintf(snapshot.footer, sizeof(snapshot.footer), "确认：开始同步  上+下：设置");
        snapshot.hint[0] = '\0';
        return snapshot;
    }

    snapshot.has_card = true;
    snapshot.showing_back = maimemo_showing_back_;
    snapshot.screen = maimemo_showing_back_ ? VocabularyScreen::kCardBack : VocabularyScreen::kCardFront;
    CopyText(snapshot.word, sizeof(snapshot.word), maimemo_card_.word.c_str());
    CopyText(snapshot.phonetic, sizeof(snapshot.phonetic), maimemo_card_.phonetic.c_str());
    CopyText(snapshot.meaning, sizeof(snapshot.meaning), maimemo_card_.meaning.c_str());
    CopyText(snapshot.example, sizeof(snapshot.example), maimemo_card_.example.c_str());
    CopyText(snapshot.example_translation,
             sizeof(snapshot.example_translation),
             maimemo_card_.example_translation.c_str());
    snprintf(snapshot.status_line, sizeof(snapshot.status_line), "%s", maimemo_status_.c_str());
    if ((maimemo_busy_ || maimemo_submit_waiting_for_pronunciation_) &&
        maimemo_submit_started_ms_ > 0) {
        snprintf(snapshot.action_status, sizeof(snapshot.action_status), "正在同步");
    } else if (!action_status_.empty()) {
        CopyText(snapshot.action_status, sizeof(snapshot.action_status), action_status_.c_str());
    }
    if (maimemo_showing_back_ && maimemo_revealing_review_) {
        if (const MaimemoReviewMark* mark = FindMaimemoReviewMarkLocked(maimemo_card_)) {
            CopyText(snapshot.review_mark,
                     sizeof(snapshot.review_mark),
                     MaimemoAnswerBadge(mark->answer));
        }
    }
    if (!maimemo_showing_back_ || maimemo_card_.note.empty()) {
        snapshot.hint[0] = '\0';
    } else {
        std::string note = "【助记】" + maimemo_card_.note;
        CopyText(snapshot.hint, sizeof(snapshot.hint), note.c_str());
    }
    if (maimemo_showing_back_) {
        snprintf(snapshot.footer, sizeof(snapshot.footer), "上：忘记  确认：认识  下：模糊");
    } else {
        snprintf(snapshot.footer, sizeof(snapshot.footer), "长按上/下：切词  确认：释义");
    }
    if (maimemo_busy_) {
        if (snapshot.status_line[0] == '\0') {
            snprintf(snapshot.status_line, sizeof(snapshot.status_line), "同步中");
        }
        return snapshot;
    }
    return snapshot;
}

void VocabularyApp::ScoreCurrentLocked(bool known) {
    const size_t word_index = CurrentWordIndexLocked();
    if (word_index >= states_.size()) {
        return;
    }

    scheduler_.ApplyAnswer(&states_[word_index], known, today_);
    stats_.reviewed = static_cast<uint16_t>(
        std::min<uint32_t>(static_cast<uint32_t>(stats_.reviewed) + 1, UINT16_MAX));
    if (known) {
        stats_.correct = static_cast<uint16_t>(
            std::min<uint32_t>(static_cast<uint32_t>(stats_.correct) + 1, UINT16_MAX));
    }
    sync_client_.MarkPending();
    MarkProgressDirtyLocked();

    RemoveWordFromQueueLocked(word_index, nullptr);
    // Keep the study button path display-only. Synchronous audio setup here
    // blocks the app callback long enough for fast repeated clicks to feel lost.
    MoveCardLocked(1);
}

void VocabularyApp::MissCurrentLocked() {
    const size_t word_index = CurrentWordIndexLocked();
    if (word_index >= states_.size()) {
        return;
    }

    scheduler_.ApplyAnswer(&states_[word_index], false, today_);
    sync_client_.MarkPending();
    MarkProgressDirtyLocked();

    RequeueWordIndexLocked(word_index);
    MoveCardLocked(1);
}

void VocabularyApp::DeferCurrentLocked() {
    const size_t word_index = CurrentWordIndexLocked();
    if (word_index >= deck_store_.Count()) {
        return;
    }

    RequeueWordIndexLocked(word_index);
    MoveCardLocked(1);
}

void VocabularyApp::RequeueWordIndexLocked(size_t word_index) {
    size_t removed_pos = queue_pos_;
    const bool was_in_queue = RemoveWordFromQueueLocked(word_index, &removed_pos);
    if (queue_.empty()) {
        queue_.push_back(word_index);
        queue_pos_ = 0;
        showing_back_ = false;
        return;
    }

    size_t anchor_pos = was_in_queue ? removed_pos : queue_pos_;
    if (anchor_pos >= queue_.size()) {
        anchor_pos = 0;
    }
    const size_t insert_pos = std::min(anchor_pos + kAgainDelayCards, queue_.size());
    queue_.insert(queue_.begin() + static_cast<std::ptrdiff_t>(insert_pos), word_index);
    queue_pos_ = std::min(anchor_pos, queue_.size() - 1);
    showing_back_ = false;
}

bool VocabularyApp::RemoveWordFromQueueLocked(size_t word_index, size_t* removed_pos) {
    auto it = std::find(queue_.begin(), queue_.end(), word_index);
    if (it == queue_.end()) {
        return false;
    }

    const size_t pos = static_cast<size_t>(std::distance(queue_.begin(), it));
    if (removed_pos != nullptr) {
        *removed_pos = pos;
    }
    queue_.erase(it);
    if (queue_.empty()) {
        queue_pos_ = 0;
    } else if (pos < queue_pos_) {
        --queue_pos_;
    } else if (pos == queue_pos_ && queue_pos_ >= queue_.size()) {
        queue_pos_ = 0;
    }
    return true;
}

size_t VocabularyApp::CurrentWordIndexLocked() const {
    const size_t word_count = deck_store_.Count();
    if (word_count == 0) {
        return 0;
    }
    return std::min(current_word_index_, word_count - 1);
}

void VocabularyApp::SelectWordLocked(size_t word_index, bool persist_position) {
    const size_t word_count = deck_store_.Count();
    if (word_count == 0) {
        current_word_index_ = 0;
        showing_back_ = false;
        return;
    }

    current_word_index_ = std::min(word_index, word_count - 1);
    auto it = std::find(queue_.begin(), queue_.end(), current_word_index_);
    if (it != queue_.end()) {
        queue_pos_ = static_cast<size_t>(std::distance(queue_.begin(), it));
    }
    showing_back_ = false;
    if (persist_position) {
        MarkCurrentPositionDirtyLocked();
    }
}

void VocabularyApp::RestoreCurrentPositionLocked() {
    size_t saved_index = 0;
    if (!progress_store_.LoadCurrentIndex(settings_.active_deck_id, &saved_index)) {
        return;
    }
    if (saved_index < deck_store_.Count()) {
        SelectWordLocked(saved_index, false);
    }
    current_index_dirty_ = false;
    pending_current_word_index_ = current_word_index_;
    saved_current_word_index_ = current_word_index_;
    current_index_moves_since_save_ = 0;
}

void VocabularyApp::SaveCurrentPositionLocked() {
    if (deck_store_.Count() == 0) {
        current_index_dirty_ = false;
        return;
    }
    const size_t index = current_index_dirty_
        ? std::min(pending_current_word_index_, deck_store_.Count() - 1)
        : CurrentWordIndexLocked();
    if (progress_store_.SaveCurrentIndex(settings_.active_deck_id, index)) {
        current_index_dirty_ = false;
        pending_current_word_index_ = index;
        saved_current_word_index_ = index;
        current_index_moves_since_save_ = 0;
    }
}

void VocabularyApp::MarkCurrentPositionDirtyLocked() {
    if (deck_store_.Count() == 0) {
        current_index_dirty_ = false;
        current_index_moves_since_save_ = 0;
        return;
    }
    pending_current_word_index_ = CurrentWordIndexLocked();
    current_index_dirty_ = true;
}

void VocabularyApp::MarkProgressDirtyLocked() {
    progress_dirty_ = true;
}

std::vector<uint64_t> VocabularyApp::BuildProgressWordKeysLocked() const {
    return BuildProgressWordKeysForStore(deck_store_);
}

void VocabularyApp::ArmProgressSaveTimerLocked() {
    if (!progress_dirty_ || settings_.active_deck_id.empty()) {
        return;
    }
    if (progress_save_timer_ == nullptr) {
        StartProgressSaveTaskLocked();
        return;
    }
    if (esp_timer_is_active(progress_save_timer_)) {
        return;
    }
    const esp_err_t ret = esp_timer_start_once(
        progress_save_timer_,
        static_cast<uint64_t>(kProgressSaveCoalesceMs) * 1000ULL);
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "progress save timer start failed: %s", esp_err_to_name(ret));
        StartProgressSaveTaskLocked();
    }
}

void VocabularyApp::DisarmProgressSaveTimerLocked() {
    if (progress_save_timer_ != nullptr) {
        (void)esp_timer_stop(progress_save_timer_);
    }
}

void VocabularyApp::ShowActionStatusLocked(const char* text) {
    action_status_ = text != nullptr ? text : "";
    if (action_status_timer_ == nullptr) {
        return;
    }
    (void)esp_timer_stop(action_status_timer_);
    if (!action_status_.empty()) {
        (void)esp_timer_start_once(
            action_status_timer_,
            static_cast<uint64_t>(kActionStatusDurationMs) * 1000ULL);
    }
}

void VocabularyApp::HandleActionStatusTimeout() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (action_status_.empty()) {
        return;
    }
    action_status_.clear();
    PublishSnapshotLocked(false);
}

void VocabularyApp::StartProgressSaveTaskLocked() {
    if (!progress_dirty_ || progress_save_busy_ || settings_.active_deck_id.empty()) {
        return;
    }

    DisarmProgressSaveTimerLocked();

    const uint32_t request_id = progress_save_request_.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto* args = new (std::nothrow) ProgressSaveTaskArgs{
        .app = this,
        .request_id = request_id,
        .deck_id = settings_.active_deck_id,
        .word_keys = BuildProgressWordKeysLocked(),
        .states = states_,
        .stats = stats_,
    };
    if (args == nullptr) {
        ESP_LOGW(kTag, "progress save task args allocation failed");
        return;
    }

    progress_dirty_ = false;
    progress_save_busy_ = true;
    BaseType_t created = xTaskCreate(
        &VocabularyApp::ProgressSaveTaskEntry,
        "vocab_progress",
        8192,
        args,
        3,
        nullptr);
    if (created != pdPASS) {
        delete args;
        progress_dirty_ = true;
        progress_save_busy_ = false;
        ESP_LOGW(kTag,
                 "progress save task create failed free_internal=%u min_internal=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
    }
}

void VocabularyApp::MoveCardLocked(int delta) {
    const size_t word_count = deck_store_.Count();
    if (word_count == 0) {
        return;
    }

    if (!queue_.empty()) {
        auto it = std::find(queue_.begin(), queue_.end(), current_word_index_);
        if (it != queue_.end()) {
            queue_pos_ = static_cast<size_t>(std::distance(queue_.begin(), it));
        } else if (queue_pos_ >= queue_.size()) {
            queue_pos_ = 0;
        }

        if (delta < 0) {
            queue_pos_ = (queue_pos_ == 0) ? queue_.size() - 1 : queue_pos_ - 1;
        } else if (delta > 0) {
            queue_pos_ = (queue_pos_ + 1) % queue_.size();
        }
        SelectWordLocked(queue_[queue_pos_]);
        PreloadCurrentPronunciationLocked();
        return;
    }

    const size_t current = CurrentWordIndexLocked();
    if (delta < 0) {
        SelectWordLocked(current == 0 ? word_count - 1 : current - 1);
    } else if (delta > 0) {
        SelectWordLocked((current + 1) % word_count);
    }
    PreloadCurrentPronunciationLocked();
}

void VocabularyApp::BrowseCardLocked(int delta) {
    const size_t word_count = deck_store_.Count();
    if (word_count == 0) {
        return;
    }

    const size_t current = CurrentWordIndexLocked();
    size_t next = current;
    if (delta < 0) {
        next = current == 0 ? word_count - 1 : current - 1;
    } else if (delta > 0) {
        next = (current + 1) % word_count;
    }
    SelectWordLocked(next);
    showing_back_ = true;
    MaybeAutoPlayCurrentPronunciationLocked();
}

#if 0
void VocabularyApp::EnterSettingsLocked() {
    SaveCurrentPositionLocked();
    settings_open_ = true;
    settings_selection_ = 0;
    showing_back_ = false;
    const std::vector<DeckChoice> choices = BuildDeckChoicesLocked();
    settings_deck_index_ = 0;
    for (size_t i = 0; i < choices.size(); ++i) {
        if (choices[i].id == settings_.active_deck_id) {
            settings_deck_index_ = i;
            break;
        }
    }
    (void)sync_client_.TrySync(deck_store_.Manifest(), stats_);
    StartDeckCatalogTaskLocked();
}

void VocabularyApp::HandleSettingsButtonLocked(VocabularyButton button) {
    switch (button) {
        case VocabularyButton::kConfirmClick:
            settings_selection_ = static_cast<uint8_t>((settings_selection_ + 1) % kSettingsItemCount);
            break;
        case VocabularyButton::kConfirmDoubleClick:
            break;
        case VocabularyButton::kConfirmLongPress:
        case VocabularyButton::kSettingsCombo:
            settings_open_ = false;
            showing_back_ = false;
            RebuildQueueLocked();
            break;
        case VocabularyButton::kUpClick:
            ChangeSettingsValueLocked(settings_selection_ == 2 ? ReviewScheduler::kDailyTargetStep : -1);
            break;
        case VocabularyButton::kDownClick:
            ChangeSettingsValueLocked(settings_selection_ == 2 ? -ReviewScheduler::kDailyTargetStep : 1);
            break;
        case VocabularyButton::kUpLongPress:
            ChangeSettingsValueLocked(settings_selection_ == 2 ? ReviewScheduler::kDailyTargetStep : -1);
            break;
        case VocabularyButton::kDownLongPress:
            ChangeSettingsValueLocked(settings_selection_ == 2 ? -ReviewScheduler::kDailyTargetStep : 1);
            break;
        case VocabularyButton::kConfirmLongRelease:
            break;
    }
}

void VocabularyApp::ChangeSettingsValueLocked(int delta) {
    if (settings_selection_ == 0) {
        SelectDeckChoiceLocked(delta);
    } else if (settings_selection_ == 1) {
        settings_.order_mode = settings_.order_mode == VocabularyOrderMode::kSequential
            ? VocabularyOrderMode::kRandom
            : VocabularyOrderMode::kSequential;
    } else if (settings_selection_ == 2) {
        const int step = ReviewScheduler::kDailyTargetStep;
        int next_target = settings_.daily_target;
        if (delta > 0) {
            next_target = ((next_target + step - 1) / step) * step;
            if (next_target == settings_.daily_target) {
                next_target += step;
            }
        } else if (delta < 0) {
            next_target = ((next_target - 1) / step) * step;
        }
        settings_.daily_target = static_cast<uint16_t>(
            std::min<int>(ReviewScheduler::kMaxDailyTarget,
                          std::max<int>(ReviewScheduler::kMinDailyTarget, next_target)));
    } else if (settings_selection_ == 3) {
        settings_.voice_type = settings_.voice_type == VocabularyVoiceType::kAmerican
            ? VocabularyVoiceType::kBritish
            : VocabularyVoiceType::kAmerican;
    } else {
        settings_.study_mode = settings_.study_mode == VocabularyStudyMode::kOffline
            ? VocabularyStudyMode::kMaimemo
            : VocabularyStudyMode::kOffline;
        if (settings_.study_mode == VocabularyStudyMode::kOffline) {
            maimemo_client_.Reset();
            maimemo_has_card_ = false;
            maimemo_busy_ = false;
            maimemo_submit_started_ms_ = 0;
            maimemo_pending_reveal_ = false;
            maimemo_status_ = "墨墨未连接";
        }
    }
    SaveSettingsAndRebuildQueueLocked();
    if (settings_selection_ == 4 && settings_.study_mode == VocabularyStudyMode::kMaimemo) {
        StartMaimemoFetchLocked(false);
    }
}

#endif

#if 0
void VocabularyApp::SaveSettingsAndRebuildQueueLocked() {
    (void)progress_store_.SaveSettings(settings_);
    RebuildQueueLocked(true);
}

std::vector<VocabularyApp::DeckChoice> VocabularyApp::BuildDeckChoicesLocked() const {
    std::vector<DeckChoice> choices;
    const auto& local_decks = deck_store_.Decks();
    choices.reserve(local_decks.size() + remote_decks_.size());
    for (const DeckInfo& deck : local_decks) {
        DeckChoice choice;
        choice.id = deck.id;
        choice.name = deck.name;
        choice.installed = true;
        choice.remote = false;
        choices.push_back(std::move(choice));
    }

    for (size_t i = 0; i < remote_decks_.size(); ++i) {
        const RemoteDeckEntry& remote = remote_decks_[i];
        if (remote.id.empty() || deck_store_.HasDeck(remote.id)) {
            continue;
        }
        DeckChoice choice;
        choice.id = remote.id;
        choice.name = remote.name.empty() ? remote.id : remote.name;
        choice.installed = false;
        choice.remote = true;
        choice.remote_index = i;
        choices.push_back(std::move(choice));
    }
    return choices;
}

void VocabularyApp::SelectDeckChoiceLocked(int delta) {
    const std::vector<DeckChoice> choices = BuildDeckChoicesLocked();
    if (choices.empty()) {
        return;
    }

    const size_t count = choices.size();
    if (settings_deck_index_ >= count) {
        settings_deck_index_ = 0;
    }
    if (delta < 0) {
        settings_deck_index_ = settings_deck_index_ == 0 ? count - 1 : settings_deck_index_ - 1;
    } else if (delta > 0) {
        settings_deck_index_ = (settings_deck_index_ + 1) % count;
    }

    const DeckChoice& choice = choices[settings_deck_index_];
    if (choice.installed) {
        SwitchDeckLocked(choice.id);
    } else if (choice.remote && choice.remote_index < remote_decks_.size()) {
        StartDeckDownloadTaskLocked(remote_decks_[choice.remote_index]);
    }
}

#endif

void VocabularyApp::EnterSettingsLocked() {
    SaveCurrentPositionLocked();
    settings_open_ = true;
    settings_view_ = SettingsView::kMain;
    settings_selection_ = 0;
    showing_back_ = false;

    const std::vector<DeckChoice> local_choices = BuildLocalDeckChoicesLocked();
    settings_local_deck_index_ = 0;
    for (size_t i = 0; i < local_choices.size(); ++i) {
        if (local_choices[i].id == settings_.active_deck_id) {
            settings_local_deck_index_ = i;
            break;
        }
    }
    StartSettingsBackgroundRefreshLocked();
}

void VocabularyApp::HandleSettingsButtonLocked(VocabularyButton button) {
    ESP_LOGI(kTag,
             "settings button=%u view=%u popup=%u sel=%u online=%u busy=%d open=%d",
             static_cast<unsigned>(button),
             static_cast<unsigned>(settings_view_),
             static_cast<unsigned>(settings_popup_action_),
             static_cast<unsigned>(settings_selection_),
             static_cast<unsigned>(settings_online_deck_index_),
             deck_sync_busy_ ? 1 : 0,
             settings_open_ ? 1 : 0);
    switch (settings_view_) {
        case SettingsView::kMain:
            HandleSettingsMainButtonLocked(button);
            break;
        case SettingsView::kLocalDecks:
            HandleLocalDeckSettingsButtonLocked(button);
            break;
        case SettingsView::kOnlineDecks:
            HandleOnlineDeckSettingsButtonLocked(button);
            break;
        case SettingsView::kPopup:
            HandleSettingsPopupButtonLocked(button);
            break;
    }
}

void VocabularyApp::HandleSettingsMainButtonLocked(VocabularyButton button) {
    switch (button) {
        case VocabularyButton::kConfirmClick:
            ActivateSettingsItemLocked();
            break;
        case VocabularyButton::kUpClick:
        case VocabularyButton::kUpLongPress:
            MoveSettingsSelectionLocked(-1);
            break;
        case VocabularyButton::kDownClick:
        case VocabularyButton::kDownLongPress:
            MoveSettingsSelectionLocked(1);
            break;
        case VocabularyButton::kConfirmLongPress:
            settings_open_ = false;
            showing_back_ = false;
            if (settings_.study_mode != VocabularyStudyMode::kMaimemo) {
                StopStationForOfflineLocked();
            }
            break;
        case VocabularyButton::kSettingsCombo:
            break;
        case VocabularyButton::kConfirmDoubleClick:
            break;
        case VocabularyButton::kConfirmLongRelease:
            break;
    }
}

void VocabularyApp::HandleLocalDeckSettingsButtonLocked(VocabularyButton button) {
    switch (button) {
        case VocabularyButton::kConfirmClick:
            OpenSettingsPopupLocked(SettingsPopupAction::kSelectLocalDeck);
            break;
        case VocabularyButton::kUpClick:
        case VocabularyButton::kUpLongPress:
            MoveLocalDeckSelectionLocked(-1);
            break;
        case VocabularyButton::kDownClick:
        case VocabularyButton::kDownLongPress:
            MoveLocalDeckSelectionLocked(1);
            break;
        case VocabularyButton::kConfirmLongPress:
            settings_view_ = SettingsView::kMain;
            break;
        case VocabularyButton::kSettingsCombo:
            settings_view_ = SettingsView::kMain;
            break;
        case VocabularyButton::kConfirmDoubleClick:
            break;
        case VocabularyButton::kConfirmLongRelease:
            break;
    }
}

void VocabularyApp::HandleOnlineDeckSettingsButtonLocked(VocabularyButton button) {
    switch (button) {
        case VocabularyButton::kConfirmClick:
            SelectOnlineDeckChoiceLocked();
            break;
        case VocabularyButton::kUpClick:
        case VocabularyButton::kUpLongPress:
            MoveOnlineDeckSelectionLocked(-1);
            break;
        case VocabularyButton::kDownClick:
        case VocabularyButton::kDownLongPress:
            MoveOnlineDeckSelectionLocked(1);
            break;
        case VocabularyButton::kConfirmLongPress:
            settings_view_ = SettingsView::kMain;
            break;
        case VocabularyButton::kSettingsCombo:
            settings_view_ = SettingsView::kMain;
            break;
        case VocabularyButton::kConfirmDoubleClick:
            break;
        case VocabularyButton::kConfirmLongRelease:
            break;
    }
}

void VocabularyApp::HandleSettingsPopupButtonLocked(VocabularyButton button) {
    switch (button) {
        case VocabularyButton::kConfirmClick:
            ApplySettingsPopupLocked();
            break;
        case VocabularyButton::kUpClick:
        case VocabularyButton::kUpLongPress:
            MoveSettingsPopupValueLocked(1);
            break;
        case VocabularyButton::kDownClick:
        case VocabularyButton::kDownLongPress:
            MoveSettingsPopupValueLocked(-1);
            break;
        case VocabularyButton::kConfirmLongPress:
        case VocabularyButton::kSettingsCombo:
            CancelSettingsPopupLocked(false);
            break;
        case VocabularyButton::kConfirmDoubleClick:
            if (settings_popup_action_ == SettingsPopupAction::kSelectLocalDeck) {
                SelectLocalDeckChoiceLocked();
            } else {
                CancelSettingsPopupLocked(false);
            }
            break;
        case VocabularyButton::kConfirmLongRelease:
            break;
    }
}

void VocabularyApp::HandleDailyTargetSettingsButtonLocked(VocabularyButton button) {
    switch (button) {
        case VocabularyButton::kConfirmClick:
        case VocabularyButton::kConfirmLongPress:
            settings_view_ = SettingsView::kMain;
            break;
        case VocabularyButton::kUpClick:
        case VocabularyButton::kUpLongPress:
            AdjustDailyTargetLocked(ReviewScheduler::kDailyTargetStep);
            break;
        case VocabularyButton::kDownClick:
        case VocabularyButton::kDownLongPress:
            AdjustDailyTargetLocked(-ReviewScheduler::kDailyTargetStep);
            break;
        case VocabularyButton::kSettingsCombo:
            settings_view_ = SettingsView::kMain;
            break;
        case VocabularyButton::kConfirmDoubleClick:
            break;
        case VocabularyButton::kConfirmLongRelease:
            break;
    }
}

void VocabularyApp::MoveSettingsSelectionLocked(int delta) {
    settings_selection_ = MoveSettingsSelection(settings_selection_, delta);
}

void VocabularyApp::MoveLocalDeckSelectionLocked(int delta) {
    const std::vector<DeckChoice> choices = BuildLocalDeckChoicesLocked();
    if (deck_sync_ui_state_ == DeckSyncUiState::kDownloadError ||
        deck_sync_ui_state_ == DeckSyncUiState::kDownloadSuccess) {
        deck_sync_ui_state_ = DeckSyncUiState::kIdle;
        deck_sync_status_.clear();
        deck_sync_detail_.clear();
    }
    if (choices.empty()) {
        settings_local_deck_index_ = 0;
        return;
    }
    if (settings_local_deck_index_ >= choices.size()) {
        settings_local_deck_index_ = 0;
    }
    if (delta < 0) {
        settings_local_deck_index_ = settings_local_deck_index_ == 0
            ? choices.size() - 1
            : settings_local_deck_index_ - 1;
    } else if (delta > 0) {
        settings_local_deck_index_ = (settings_local_deck_index_ + 1) % choices.size();
    }
}

void VocabularyApp::MoveOnlineDeckSelectionLocked(int delta) {
    const std::vector<DeckChoice> choices = BuildOnlineDeckChoicesLocked();
    online_delete_confirm_ = false;
    if (choices.empty()) {
        settings_online_deck_index_ = 0;
        return;
    }
    if (deck_sync_ui_state_ == DeckSyncUiState::kDownloadError) {
        deck_sync_ui_state_ = DeckSyncUiState::kIdle;
        deck_sync_status_ = "选择词库";
        deck_sync_detail_.clear();
    }
    if (settings_online_deck_index_ >= choices.size()) {
        settings_online_deck_index_ = 0;
    }
    if (delta < 0) {
        settings_online_deck_index_ = settings_online_deck_index_ == 0
            ? choices.size() - 1
            : settings_online_deck_index_ - 1;
    } else if (delta > 0) {
        settings_online_deck_index_ = (settings_online_deck_index_ + 1) % choices.size();
    }
}

void VocabularyApp::ActivateSettingsItemLocked() {
    switch (settings_selection_) {
        case 0:
            OpenSettingsPopupLocked(SettingsPopupAction::kStudyMode);
            break;
        case 1:
            OpenSettingsPopupLocked(SettingsPopupAction::kSelectLocalDeck);
            break;
        case 2:
            OpenSettingsPopupLocked(SettingsPopupAction::kDailyTarget);
            break;
        case 3:
            OpenSettingsPopupLocked(SettingsPopupAction::kOrderMode);
            break;
        case 4:
            OpenSettingsPopupLocked(SettingsPopupAction::kVoiceType);
            break;
        case 5:
            OpenSettingsPopupLocked(SettingsPopupAction::kFontMode);
            break;
        case 6:
            if (deck_sync_busy_) {
                return;
            }
            OpenSettingsPopupLocked(SettingsPopupAction::kDownloadOnlineDeck);
            break;
        case 7:
            OpenSettingsPopupLocked(SettingsPopupAction::kOtaUpdate);
            break;
        case 8:
            OpenSettingsPopupLocked(SettingsPopupAction::kOutputVolume);
            break;
        case 9:
            OpenSettingsPopupLocked(SettingsPopupAction::kAutoPronunciation);
            break;
        case 10:
            OpenSettingsPopupLocked(SettingsPopupAction::kWifiStatus);
            break;
        case 11:
            OpenSettingsPopupLocked(SettingsPopupAction::kAiService);
            break;
        case 12:
            OpenSettingsPopupLocked(SettingsPopupAction::kAutoSleep);
            break;
        case 13:
            OpenSettingsPopupLocked(SettingsPopupAction::kButtonHelp);
            break;
        default:
            break;
    }
}

void VocabularyApp::OpenSettingsPopupLocked(SettingsPopupAction action) {
    settings_popup_return_view_ = settings_view_;
    settings_popup_action_ = action;
    online_delete_confirm_ = false;
    if (action == SettingsPopupAction::kSelectLocalDeck && !deck_sync_busy_) {
        deck_sync_ui_state_ = DeckSyncUiState::kIdle;
        deck_sync_status_.clear();
        deck_sync_detail_.clear();
    }
    popup_study_mode_ = settings_.study_mode;
    popup_order_mode_ = settings_.order_mode;
    popup_voice_type_ = settings_.voice_type;
    popup_font_mode_ = settings_.font_mode;
    popup_auto_sleep_timeout_ = settings_.auto_sleep_timeout;
    popup_daily_target_ = settings_.daily_target;
    popup_output_volume_ = static_cast<uint8_t>(CurrentOutputVolume());
    popup_auto_pronunciation_ = settings_.auto_pronunciation;
    if (action == SettingsPopupAction::kAiService) {
        const AiDialogConfig ai_config = AiDialogService::LoadConfig();
        popup_ai_enabled_ = ai_config.enabled && ai_config.entitlement_verified;
        ai_entitlement_status_.clear();
    }
    settings_view_ = SettingsView::kPopup;
}

void VocabularyApp::ApplySettingsPopupLocked() {
    const SettingsPopupAction action = settings_popup_action_;
    const SettingsView return_view = settings_popup_return_view_;

    if (action == SettingsPopupAction::kAiService) {
        if (ai_entitlement_checking_) {
            return;
        }
        AiDialogConfig config = AiDialogService::LoadConfig();
        if (!popup_ai_enabled_) {
            config.enabled = false;
            config.entitlement_verified = false;
            std::string error;
            (void)AiDialogService::SaveConfig(config, &error);
            AiDialogService::GetInstance().ReloadConfig();
            if (ai_overlay_active_) {
                CloseAiOverlayLocked();
            }
            ai_entitlement_status_.clear();
            settings_popup_action_ = SettingsPopupAction::kNone;
            settings_view_ = return_view;
            return;
        }
        if (config.enabled && config.entitlement_verified) {
            settings_popup_action_ = SettingsPopupAction::kNone;
            settings_view_ = return_view;
            return;
        }
        if (!WifiManager::GetInstance().IsConnected()) {
            ai_entitlement_status_ = "请先连接 Wi-Fi，再开启 AI";
            return;
        }
        StartAiEntitlementCheckLocked();
        return;
    }

    if (action == SettingsPopupAction::kDownloadOnlineDeck) {
        settings_popup_action_ = SettingsPopupAction::kDownloadOnlineDeck;
        settings_popup_return_view_ = return_view;
        settings_view_ = SettingsView::kPopup;
        SelectOnlineDeckChoiceLocked();
        return;
    }

    if (action == SettingsPopupAction::kWifiStatus) {
        settings_open_ = false;
        settings_view_ = SettingsView::kMain;
        settings_popup_action_ = SettingsPopupAction::kNone;
        Board::GetInstance().RequestNetwork();
        WifiManager::GetInstance().StartConfigAp();
        return;
    }

    if (action == SettingsPopupAction::kOtaUpdate) {
        // Keep the OTA popup alive while the background task runs so progress
        // remains visible instead of collapsing back to the settings list.
        settings_popup_action_ = SettingsPopupAction::kOtaUpdate;
        settings_popup_return_view_ = return_view;
        settings_view_ = SettingsView::kPopup;
        if (!ota_busy_) {
            StartOnlineWifiLocked("正在打开 WiFi，准备升级...");
            StartOtaTaskLocked();
        }
        return;
    }

    settings_popup_action_ = SettingsPopupAction::kNone;
    settings_view_ = return_view;

    switch (action) {
        case SettingsPopupAction::kStudyMode:
            if (popup_study_mode_ == VocabularyStudyMode::kMaimemo &&
                settings_.study_mode != VocabularyStudyMode::kMaimemo) {
                EnableMaimemoModeLocked();
            } else if (popup_study_mode_ != VocabularyStudyMode::kMaimemo &&
                       settings_.study_mode == VocabularyStudyMode::kMaimemo) {
                DisableMaimemoModeLocked();
                settings_.study_mode = popup_study_mode_;
            } else {
                settings_.study_mode = popup_study_mode_;
            }
            SaveSettingsAndRebuildQueueLocked();
            break;
        case SettingsPopupAction::kOrderMode:
            settings_.order_mode = popup_order_mode_;
            SaveSettingsAndRebuildQueueLocked();
            break;
        case SettingsPopupAction::kVoiceType:
            settings_.voice_type = popup_voice_type_;
            (void)progress_store_.SaveSettings(settings_);
            break;
        case SettingsPopupAction::kFontMode:
            settings_.font_mode = popup_font_mode_;
            SaveSettingsAndRebuildQueueLocked();
            break;
        case SettingsPopupAction::kDailyTarget:
            settings_.daily_target = popup_daily_target_;
            SaveSettingsAndRebuildQueueLocked();
            break;
        case SettingsPopupAction::kOutputVolume:
            if (AudioCodec* codec = Board::GetInstance().GetAudioCodec()) {
                codec->SetOutputVolume(popup_output_volume_);
            }
            break;
        case SettingsPopupAction::kAutoPronunciation:
            settings_.auto_pronunciation = popup_auto_pronunciation_;
            (void)progress_store_.SaveSettings(settings_);
            MaybeAutoPlayCurrentPronunciationLocked();
            break;
        case SettingsPopupAction::kAutoSleep:
            settings_.auto_sleep_timeout = popup_auto_sleep_timeout_;
            (void)progress_store_.SaveSettings(settings_);
            Application::GetInstance().SetIdleSleepDelayMs(
                VocabularyAutoSleepDelayMs(settings_.auto_sleep_timeout));
            break;
        case SettingsPopupAction::kSelectLocalDeck:
            SelectLocalDeckChoiceLocked();
            settings_view_ = SettingsView::kMain;
            break;
        case SettingsPopupAction::kDownloadOnlineDeck:
            SelectOnlineDeckChoiceLocked();
            break;
        case SettingsPopupAction::kRefreshOnlineDecks:
            StartOnlineWifiLocked("正在打开 WiFi，获取在线词库...");
            StartDeckCatalogTaskLocked();
            settings_view_ = SettingsView::kMain;
            break;
        case SettingsPopupAction::kOtaUpdate:
            break;
        case SettingsPopupAction::kAiService:
        case SettingsPopupAction::kWifiStatus:
        case SettingsPopupAction::kButtonHelp:
        case SettingsPopupAction::kNone:
            break;
    }
}

void VocabularyApp::CancelSettingsPopupLocked(bool exit_settings) {
    if (settings_popup_action_ == SettingsPopupAction::kAiService &&
        ai_entitlement_checking_) {
        ai_entitlement_request_.fetch_add(1, std::memory_order_acq_rel);
        ai_entitlement_checking_ = false;
    }
    if (!exit_settings &&
        settings_popup_action_ == SettingsPopupAction::kDownloadOnlineDeck &&
        online_delete_confirm_) {
        online_delete_confirm_ = false;
        deck_sync_ui_state_ = DeckSyncUiState::kIdle;
        deck_sync_status_ = "选择词库";
        deck_sync_detail_.clear();
        return;
    }
    settings_popup_action_ = SettingsPopupAction::kNone;
    online_delete_confirm_ = false;
    settings_view_ = settings_popup_return_view_;
    if (exit_settings) {
        settings_open_ = false;
        if (settings_.study_mode != VocabularyStudyMode::kMaimemo) {
            StopStationForOfflineLocked();
        }
    }
}

void VocabularyApp::MoveSettingsPopupValueLocked(int delta) {
    switch (settings_popup_action_) {
        case SettingsPopupAction::kStudyMode:
            popup_study_mode_ = ToggleStudyMode(popup_study_mode_);
            break;
        case SettingsPopupAction::kOrderMode:
            popup_order_mode_ = ToggleOrderMode(popup_order_mode_);
            break;
        case SettingsPopupAction::kVoiceType:
            popup_voice_type_ = ToggleVoiceType(popup_voice_type_);
            break;
        case SettingsPopupAction::kFontMode:
            popup_font_mode_ = ToggleFontMode(popup_font_mode_);
            break;
        case SettingsPopupAction::kDailyTarget: {
            const int next_target = static_cast<int>(popup_daily_target_) +
                (delta >= 0 ? ReviewScheduler::kDailyTargetStep : -ReviewScheduler::kDailyTargetStep);
            popup_daily_target_ = static_cast<uint16_t>(
                std::min<int>(ReviewScheduler::kMaxDailyTarget,
                              std::max<int>(ReviewScheduler::kMinDailyTarget, next_target)));
            break;
        }
        case SettingsPopupAction::kOutputVolume: {
            const int next_volume = static_cast<int>(popup_output_volume_) +
                (delta >= 0 ? kOutputVolumeStep : -kOutputVolumeStep);
            popup_output_volume_ = static_cast<uint8_t>(
                std::min(kOutputVolumeMax, std::max(kOutputVolumeMin, next_volume)));
            break;
        }
        case SettingsPopupAction::kAutoPronunciation:
            popup_auto_pronunciation_ = !popup_auto_pronunciation_;
            break;
        case SettingsPopupAction::kAutoSleep:
            popup_auto_sleep_timeout_ = MoveAutoSleepTimeout(popup_auto_sleep_timeout_, delta);
            break;
        case SettingsPopupAction::kAiService:
            if (!ai_entitlement_checking_) {
                popup_ai_enabled_ = !popup_ai_enabled_;
                ai_entitlement_status_.clear();
            }
            break;
        case SettingsPopupAction::kWifiStatus:
            break;
        case SettingsPopupAction::kSelectLocalDeck:
            MoveLocalDeckSelectionLocked(delta > 0 ? -1 : 1);
            break;
        case SettingsPopupAction::kDownloadOnlineDeck:
            MoveOnlineDeckSelectionLocked(delta > 0 ? -1 : 1);
            break;
        default:
            break;
    }
}

void VocabularyApp::ToggleStudyModeLocked() {
    const VocabularyStudyMode next_mode = ToggleStudyMode(settings_.study_mode);
    if (settings_.study_mode == VocabularyStudyMode::kMaimemo && next_mode != VocabularyStudyMode::kMaimemo) {
        DisableMaimemoModeLocked();
        settings_.study_mode = next_mode;
    } else if (next_mode == VocabularyStudyMode::kMaimemo) {
        EnableMaimemoModeLocked();
    } else {
        settings_.study_mode = next_mode;
    }
    SaveSettingsAndRebuildQueueLocked();
}

void VocabularyApp::ToggleOrderModeLocked() {
    settings_.order_mode = ToggleOrderMode(settings_.order_mode);
    SaveSettingsAndRebuildQueueLocked();
}

void VocabularyApp::ToggleVoiceTypeLocked() {
    settings_.voice_type = ToggleVoiceType(settings_.voice_type);
    SaveSettingsAndRebuildQueueLocked();
}

void VocabularyApp::AdjustDailyTargetLocked(int delta) {
    const int next_target = static_cast<int>(settings_.daily_target) + delta;
    settings_.daily_target = static_cast<uint16_t>(
        std::min<int>(ReviewScheduler::kMaxDailyTarget,
                      std::max<int>(ReviewScheduler::kMinDailyTarget, next_target)));
    SaveSettingsAndRebuildQueueLocked();
}

VocabularySnapshot VocabularyApp::BuildSettingsSnapshotLocked() const {
    VocabularySnapshot snapshot;
    snapshot.screen = VocabularyScreen::kSettings;
    snapshot.compact_font = settings_.font_mode == VocabularyFontMode::kCompact;
    snapshot.settings_selection = settings_selection_;
    FillBatteryStatus(&snapshot);
    FillWifiStatus(&snapshot);
    FillClockStatus(&snapshot);
    snprintf(snapshot.header, sizeof(snapshot.header), "设置");
    snprintf(snapshot.progress,
             sizeof(snapshot.progress),
             "%s",
             deck_sync_busy_ ? deck_sync_status_.c_str() : StudyModeText(settings_.study_mode));
    snprintf(snapshot.footer, sizeof(snapshot.footer), "上/下：选择  确认：进入  长按：退出");

    const DeckManifest& active_manifest = deck_store_.Manifest();
    const char* active_deck_name = active_manifest.name != nullptr ? active_manifest.name : "未选择";
    auto main_value = [&](uint8_t index) -> std::string {
        switch (index) {
            case 0:
                return StudyModeText(settings_.study_mode);
            case 1:
                return active_deck_name;
            case 2:
                return std::to_string(settings_.daily_target);
            case 3:
                return OrderModeText(settings_.order_mode);
            case 4:
                return settings_.voice_type == VocabularyVoiceType::kBritish ? "英音" : "美音";
            case 5:
                return FontModeText(settings_.font_mode);
            case 6:
                if (deck_sync_busy_) {
                    if (deck_sync_ui_state_ == DeckSyncUiState::kDownloadLoading) {
                        return deck_sync_progress_ >= 0
                            ? "下载" + std::to_string(deck_sync_progress_) + "%"
                            : "下载中";
                    }
                    if (deck_sync_ui_state_ == DeckSyncUiState::kSwitchLoading) {
                        return "加载中";
                    }
                    if (deck_sync_ui_state_ == DeckSyncUiState::kDeleteLoading) {
                        return "删除中";
                    }
                    return "联网中";
                }
                {
                    const size_t local_count = BuildLocalDeckChoicesLocked().size();
                    const std::string online_count = remote_decks_.empty()
                        ? std::string("未获取")
                        : std::to_string(remote_decks_.size()) + "个";
                    std::string value = "本地" + std::to_string(local_count) + "个，在线" + online_count;
                    if (deck_sync_ui_state_ == DeckSyncUiState::kDownloadError) {
                        value += " 下载失败";
                    } else if (deck_sync_ui_state_ == DeckSyncUiState::kCatalogError) {
                        value += " 获取失败";
                    }
                    return value;
                }
#if 0
                if (deck_sync_ui_state_ == DeckSyncUiState::kDownloadSuccess) {
                    return "下载完成";
                }
                if (deck_sync_ui_state_ == DeckSyncUiState::kDownloadError) {
                    return "下载失败";
                }
                if (deck_sync_ui_state_ == DeckSyncUiState::kCatalogError) {
                    return "获取失败";
                }
                if (remote_decks_.empty()) {
                    return "未获取";
                }
                return std::to_string(remote_decks_.size()) + " 个在线";
#endif
            case 7:
                return OtaStatusText(OtaService::GetInstance().GetSnapshot(),
                                     ota_busy_ ? ota_status_ : "");
            case 8:
                return std::to_string(CurrentOutputVolume()) + "%";
            case 9:
                return settings_.auto_pronunciation ? "开" : "关";
            case 10:
                if (WifiManager::GetInstance().IsConfigMode()) {
                    return "配网中";
                }
                return WifiManager::GetInstance().IsConnected() ? "已连接" : "未连接";
            case 11: {
                if (ai_entitlement_checking_) {
                    return "检查中";
                }
                const AiDialogConfig config = AiDialogService::LoadConfig();
                return config.enabled && config.entitlement_verified ? "开" : "关";
            }
            case 12:
                return AutoSleepTimeoutText(settings_.auto_sleep_timeout);
            case 13:
                return "查看";
            default:
                return "";
        }
    };

    std::string settings_list;
    for (uint8_t position = 0; position < kSettingsNavigationItemCount; ++position) {
        const uint8_t i = kSettingsNavigationOrder[position];
        settings_list += SettingsGroupLabel(i);
        settings_list += " ";
        settings_list += i == settings_selection_ ? "→ " : "  ";
        settings_list += SettingsMainLabel(i);
        const std::string value = main_value(i);
        if (i < kVocabularySettingsItemCount) {
            snprintf(snapshot.settings_item_values[i],
                     sizeof(snapshot.settings_item_values[i]),
                     "%s",
                     value.c_str());
        }
        if (!value.empty()) {
            settings_list += "：";
            settings_list += value;
        }
        if (position + 1 < kSettingsNavigationItemCount) {
            settings_list += "\n";
        }
    }
    snprintf(snapshot.settings_list, sizeof(snapshot.settings_list), "%s", settings_list.c_str());

    auto write_popup_options = [&](const char* title,
                                   const std::vector<std::string>& rows,
                                   size_t index) {
        snapshot.settings_popup_open = true;
        snprintf(snapshot.settings_popup_title,
                 sizeof(snapshot.settings_popup_title),
                 "%s",
                 title != nullptr ? title : "");
        std::string body;
        if (rows.empty()) {
            body = "暂无可选项";
        } else {
            if (index >= rows.size()) {
                index = 0;
            }
            if (rows.size() == 1) {
                body = "→ " + rows[index];
            } else if (rows.size() == 2) {
                body = (index == 0 ? "→ " : "  ") + rows[0] + "\n";
                body += (index == 1 ? "→ " : "  ") + rows[1];
            } else {
                const size_t prev = index == 0 ? rows.size() - 1 : index - 1;
                const size_t next = (index + 1) % rows.size();
                body = "  " + rows[prev] + "\n";
                body += "→ " + rows[index] + "\n";
                body += "  " + rows[next];
            }
        }
        snprintf(snapshot.settings_popup_body,
                 sizeof(snapshot.settings_popup_body),
                 "%s",
                 body.c_str());
        snprintf(snapshot.settings_popup_hint,
                 sizeof(snapshot.settings_popup_hint),
                 "确认：保存  长按确认：返回");
    };

    if (settings_view_ == SettingsView::kPopup) {
        switch (settings_popup_action_) {
            case SettingsPopupAction::kStudyMode:
                write_popup_options("学习模式",
                                    {"本地离线", "背词模式", "墨墨在线"},
                                    popup_study_mode_ == VocabularyStudyMode::kOffline
                                        ? 0
                                        : (popup_study_mode_ == VocabularyStudyMode::kBrowse ? 1 : 2));
                break;
            case SettingsPopupAction::kOrderMode:
                write_popup_options("背诵顺序",
                                    {"顺序", "随机"},
                                    popup_order_mode_ == VocabularyOrderMode::kRandom ? 1 : 0);
                break;
            case SettingsPopupAction::kVoiceType:
                write_popup_options("发音口音",
                                    {"美音", "英音"},
                                    popup_voice_type_ == VocabularyVoiceType::kBritish ? 1 : 0);
                break;
            case SettingsPopupAction::kFontMode:
                write_popup_options("显示字体",
                                    {"清晰大字", "紧凑小字"},
                                    popup_font_mode_ == VocabularyFontMode::kCompact ? 1 : 0);
                break;
            case SettingsPopupAction::kDailyTarget: {
                snapshot.settings_popup_open = true;
                snprintf(snapshot.settings_popup_title,
                         sizeof(snapshot.settings_popup_title),
                         "每日目标");
                snprintf(snapshot.settings_popup_body,
                         sizeof(snapshot.settings_popup_body),
                         "%u 个 / 天",
                         static_cast<unsigned>(popup_daily_target_));
                snprintf(snapshot.settings_popup_hint,
                         sizeof(snapshot.settings_popup_hint),
                         "上下调节（每次 5 个）  确认：保存");
                snapshot.settings_popup_progress_visible = true;
                snapshot.settings_popup_progress_min = ReviewScheduler::kMinDailyTarget;
                snapshot.settings_popup_progress_max = ReviewScheduler::kMaxDailyTarget;
                snapshot.settings_popup_progress_value = popup_daily_target_;
                break;
            }
            case SettingsPopupAction::kOutputVolume: {
                snapshot.settings_popup_open = true;
                snprintf(snapshot.settings_popup_title,
                         sizeof(snapshot.settings_popup_title),
                         "音量调节");
                snprintf(snapshot.settings_popup_body,
                         sizeof(snapshot.settings_popup_body),
                         "%u%%",
                         static_cast<unsigned>(popup_output_volume_));
                snprintf(snapshot.settings_popup_hint,
                         sizeof(snapshot.settings_popup_hint),
                         "上下调节（每次 10%%）  确认：保存");
                snapshot.settings_popup_progress_visible = true;
                snapshot.settings_popup_progress_min = kOutputVolumeMin;
                snapshot.settings_popup_progress_max = kOutputVolumeMax;
                snapshot.settings_popup_progress_value = popup_output_volume_;
                break;
            }
            case SettingsPopupAction::kAutoPronunciation:
                write_popup_options("自动发音",
                                    {"关闭", "开启"},
                                    popup_auto_pronunciation_ ? 1 : 0);
                break;
            case SettingsPopupAction::kAutoSleep:
                write_popup_options("自动休眠",
                                    {"30 分钟", "1 小时", "3 小时", "6 小时", "永不（耗电较高）"},
                                    static_cast<size_t>(popup_auto_sleep_timeout_));
                break;
            case SettingsPopupAction::kButtonHelp:
                snapshot.settings_popup_open = true;
                snprintf(snapshot.settings_popup_title,
                         sizeof(snapshot.settings_popup_title),
                         "按键说明");
                snprintf(snapshot.settings_popup_body,
                         sizeof(snapshot.settings_popup_body),
                         "正面：确认释义  长按确认发音\n"
                         "学习背面：上忘记  确认认识  下模糊\n"
                         "长按上/下：切换单词\n"
                         "浏览：上前词  确认/下后词");
                snprintf(snapshot.settings_popup_hint,
                         sizeof(snapshot.settings_popup_hint),
                         "确认/长按确认：返回");
                break;
            case SettingsPopupAction::kAiService: {
                write_popup_options("AI 背词教师",
                                    {"关闭", "开启"},
                                    popup_ai_enabled_ ? 1 : 0);
                if (ai_entitlement_checking_) {
                    snprintf(snapshot.settings_popup_body,
                             sizeof(snapshot.settings_popup_body),
                             "→ 正在检查 AI 开通状态\n  请稍候");
                } else if (!ai_entitlement_status_.empty()) {
                    const size_t used = strlen(snapshot.settings_popup_body);
                    if (used + 2 < sizeof(snapshot.settings_popup_body)) {
                        snprintf(snapshot.settings_popup_body + used,
                                 sizeof(snapshot.settings_popup_body) - used,
                                 "\n%s",
                                 ai_entitlement_status_.c_str());
                    }
                }
                const std::string mac = SystemInfo::GetMacAddress();
                const size_t used = strlen(snapshot.settings_popup_body);
                if (used + mac.size() + 14 < sizeof(snapshot.settings_popup_body)) {
                    snprintf(snapshot.settings_popup_body + used,
                             sizeof(snapshot.settings_popup_body) - used,
                             "\n设备 MAC：%s",
                             mac.c_str());
                }
                break;
            }
            case SettingsPopupAction::kWifiStatus: {
                WifiManager& wifi = WifiManager::GetInstance();
                const bool connected = wifi.IsConnected();
                const bool config_mode = wifi.IsConfigMode();
                const std::string ip_address = connected ? wifi.GetIpAddress() : std::string();
                std::string ssid = connected ? wifi.GetSsid() : std::string();
                std::string password;
                const auto& saved_networks = SsidManager::GetInstance().GetSsidList();
                if (!saved_networks.empty()) {
                    auto selected = saved_networks.begin();
                    if (!ssid.empty()) {
                        const auto match = std::find_if(
                            saved_networks.begin(), saved_networks.end(),
                            [&ssid](const SsidItem& item) { return item.ssid == ssid; });
                        if (match != saved_networks.end()) {
                            selected = match;
                        }
                    }
                    if (ssid.empty()) {
                        ssid = selected->ssid;
                    }
                    password = selected->password;
                }
                snapshot.settings_popup_open = true;
                snprintf(snapshot.settings_popup_title,
                         sizeof(snapshot.settings_popup_title),
                         "Wi-Fi 状态");
                snprintf(snapshot.settings_popup_badge,
                         sizeof(snapshot.settings_popup_badge),
                         "%s",
                         config_mode ? "配网中" : (connected ? "已连接" : "未连接"));
                snprintf(snapshot.settings_popup_body,
                         sizeof(snapshot.settings_popup_body),
                         "状态：%s\n账号：%s\n密码：%s%s%s",
                         connected ? "连接成功" : (config_mode ? "正在配网" : "尚未连接"),
                         ssid.empty() ? "未保存" : ssid.c_str(),
                         ssid.empty() ? "未保存" : (password.empty() ? "无密码" : password.c_str()),
                         !ip_address.empty() ? "\nIP：" : "",
                         ip_address.c_str());
                snprintf(snapshot.settings_popup_hint,
                         sizeof(snapshot.settings_popup_hint),
                         "确认：重新配网  长按确认：返回");
                break;
            }
            case SettingsPopupAction::kSelectLocalDeck: {
                if (deck_sync_busy_ && deck_sync_ui_state_ == DeckSyncUiState::kSwitchLoading) {
                    snapshot.settings_popup_open = true;
                    snprintf(snapshot.settings_popup_title,
                             sizeof(snapshot.settings_popup_title),
                             "背诵词库");
                    snprintf(snapshot.settings_popup_badge,
                             sizeof(snapshot.settings_popup_badge),
                             "切换中");
                    snprintf(snapshot.settings_popup_body,
                             sizeof(snapshot.settings_popup_body),
                             "正在后台加载并切换词库\n%s\n完成后会自动生效",
                             deck_sync_detail_.c_str());
                    snprintf(snapshot.settings_popup_hint,
                             sizeof(snapshot.settings_popup_hint),
                             "任务在后台执行  长按确认：返回");
                    break;
                }
                const std::vector<DeckChoice> choices = BuildLocalDeckChoicesLocked();
                std::vector<std::string> rows;
                rows.reserve(choices.size());
                for (const DeckChoice& choice : choices) {
                    std::string row = choice.name;
                    if (choice.id == settings_.active_deck_id) {
                        row = "[当前] " + row;
                    }
                    if (choice.builtin) {
                        row += " [内置]";
                    }
                    rows.push_back(std::move(row));
                }
                const size_t index = choices.empty()
                    ? 0
                    : std::min(settings_local_deck_index_, choices.size() - 1);
                write_popup_options("背诵词库", rows, index);
                if ((deck_sync_ui_state_ == DeckSyncUiState::kDownloadError ||
                     deck_sync_ui_state_ == DeckSyncUiState::kDownloadSuccess) &&
                    !deck_sync_detail_.empty()) {
                    const size_t used = strlen(snapshot.settings_popup_body);
                    if (used + 4 < sizeof(snapshot.settings_popup_body)) {
                        snprintf(snapshot.settings_popup_body + used,
                                 sizeof(snapshot.settings_popup_body) - used,
                                 "\n%s：%s",
                                 deck_sync_status_.empty() ? "状态" : deck_sync_status_.c_str(),
                                 deck_sync_detail_.c_str());
                    }
                }
                break;
            }
            case SettingsPopupAction::kDownloadOnlineDeck: {
                const std::vector<DeckChoice> choices = BuildOnlineDeckChoicesLocked();
                const std::string storage_badge = OnlineDeckStorageBadgeLocked();
                snprintf(snapshot.settings_popup_badge,
                         sizeof(snapshot.settings_popup_badge),
                         "%s",
                         storage_badge.c_str());
                if (deck_sync_busy_) {
                    snapshot.settings_popup_open = true;
                    snprintf(snapshot.settings_popup_title,
                             sizeof(snapshot.settings_popup_title),
                             "我的词库");
                    const char* phase = deck_sync_ui_state_ == DeckSyncUiState::kDownloadLoading
                        ? "正在下载到本地"
                        : (deck_sync_ui_state_ == DeckSyncUiState::kSwitchLoading
                            ? "正在加载并切换词库"
                            : (deck_sync_ui_state_ == DeckSyncUiState::kDeleteLoading
                                ? "正在删除并整理词库"
                                : "正在获取在线词库列表"));
                    snprintf(snapshot.settings_popup_badge,
                             sizeof(snapshot.settings_popup_badge),
                             "%s",
                             deck_sync_ui_state_ == DeckSyncUiState::kDownloadLoading &&
                                     deck_sync_progress_ >= 0
                                 ? (std::to_string(deck_sync_progress_) + "%").c_str()
                                 : (deck_sync_ui_state_ == DeckSyncUiState::kSwitchLoading
                                     ? "加载中"
                                     : (deck_sync_ui_state_ == DeckSyncUiState::kDeleteLoading
                                         ? "删除中"
                                         : "联网中")));
                    snprintf(snapshot.settings_popup_body,
                             sizeof(snapshot.settings_popup_body),
                             "%s\n%s",
                             phase,
                             deck_sync_detail_.empty()
                                 ? sync_client_.LastStatus().c_str()
                                 : deck_sync_detail_.c_str());
                    snprintf(snapshot.settings_popup_hint,
                             sizeof(snapshot.settings_popup_hint),
                             "任务在后台执行  长按确认：返回");
                    if (deck_sync_ui_state_ == DeckSyncUiState::kDownloadLoading &&
                        deck_sync_progress_ >= 0) {
                        snapshot.settings_popup_progress_visible = true;
                        snapshot.settings_popup_progress_min = 0;
                        snapshot.settings_popup_progress_max = 100;
                        snapshot.settings_popup_progress_value = deck_sync_progress_;
                    }
                    break;
                }
                if (deck_sync_ui_state_ == DeckSyncUiState::kCatalogError && choices.empty()) {
                    snapshot.settings_popup_open = true;
                    snprintf(snapshot.settings_popup_title,
                             sizeof(snapshot.settings_popup_title),
                             "我的词库");
                    snprintf(snapshot.settings_popup_body,
                             sizeof(snapshot.settings_popup_body),
                             "→ 获取失败\n  %s",
                             deck_sync_detail_.empty() ? sync_client_.LastStatus().c_str() : deck_sync_detail_.c_str());
                    snapshot.settings_popup_hint[0] = '\0';
                    break;
                }
                if (choices.empty()) {
                    snapshot.settings_popup_open = true;
                    snprintf(snapshot.settings_popup_title,
                             sizeof(snapshot.settings_popup_title),
                             "我的词库");
                    snprintf(snapshot.settings_popup_body,
                             sizeof(snapshot.settings_popup_body),
                             "→ 获取在线词库列表\n  仅下载选中的词库");
                    snapshot.settings_popup_hint[0] = '\0';
                    break;
                }

                const size_t index = choices.empty()
                    ? 0
                    : std::min(settings_online_deck_index_, choices.size() - 1);
                const DeckChoice& choice = choices[index];
                const bool confirm_delete = choice.installed && online_delete_confirm_;

                std::vector<std::string> rows;
                rows.reserve(3);
                size_t row_index = 0;
                const size_t begin = index > 0 ? index - 1 : index;
                const size_t end = std::min(choices.size(), begin + 3);
                for (size_t i = begin; i < end; ++i) {
                    const DeckChoice& row_choice = choices[i];
                    std::string row = row_choice.builtin
                        ? std::string("[内置] ") + row_choice.name
                        : row_choice.installed
                        ? std::string("[已下载] ") + row_choice.name
                        : row_choice.name;
                    if (i == index) {
                        row_index = rows.size();
                        if (deck_sync_ui_state_ == DeckSyncUiState::kDownloadError) {
                            row = "[下载失败] " + choice.name;
                        } else if (confirm_delete) {
                            row = "[待删除] " + choice.name;
                        }
                    }
                    rows.push_back(std::move(row));
                }
                write_popup_options("我的词库", rows, row_index);
                snprintf(snapshot.settings_popup_hint,
                         sizeof(snapshot.settings_popup_hint),
                         "%s",
                         choice.builtin
                             ? "上下选择  内置词库不可删除  长按确认：返回"
                             : (choice.installed
                                 ? "上下选择  确认：删除词库  长按确认：返回"
                                 : "上下选择  确认：下载词库  长按确认：返回"));
                if ((deck_sync_ui_state_ == DeckSyncUiState::kDownloadError ||
                     deck_sync_ui_state_ == DeckSyncUiState::kDownloadSuccess ||
                     confirm_delete) &&
                    !deck_sync_detail_.empty()) {
                    const size_t used = strlen(snapshot.settings_popup_body);
                    if (used + 4 < sizeof(snapshot.settings_popup_body)) {
                        snprintf(snapshot.settings_popup_body + used,
                                 sizeof(snapshot.settings_popup_body) - used,
                                 "\n%s：%s",
                                 deck_sync_status_.empty() ? "状态" : deck_sync_status_.c_str(),
                                 deck_sync_detail_.c_str());
                    }
                }
                break;
            }
            case SettingsPopupAction::kRefreshOnlineDecks:
                snapshot.settings_popup_open = true;
                snprintf(snapshot.settings_popup_title,
                         sizeof(snapshot.settings_popup_title),
                         "我的词库");
                {
                    const std::string storage_badge = OnlineDeckStorageBadgeLocked();
                    snprintf(snapshot.settings_popup_badge,
                             sizeof(snapshot.settings_popup_badge),
                             "%s",
                             storage_badge.c_str());
                }
                snprintf(snapshot.settings_popup_body,
                         sizeof(snapshot.settings_popup_body),
                         "→ 获取在线词库列表\n  仅下载选中的词库");
                snapshot.settings_popup_hint[0] = '\0';
                break;
            case SettingsPopupAction::kOtaUpdate: {
                const OtaService::Snapshot ota_snapshot = OtaService::GetInstance().GetSnapshot();
                const std::string status = OtaStatusText(ota_snapshot, ota_busy_ ? ota_status_ : "");
                const bool ota_active = OtaSnapshotActive(ota_snapshot) || ota_busy_;
                const int progress = std::clamp(ota_snapshot.progress, 0, 100);
                std::string body = "→ " + status;
                if (!ota_snapshot.current_version.empty()) {
                    body += "\n  当前 " + ota_snapshot.current_version;
                }
                if (!ota_snapshot.latest_version.empty()) {
                    body += "\n  最新 " + ota_snapshot.latest_version;
                } else if (!ota_busy_) {
                    body += "\n  进入设置会自动联网检查";
                }
                snapshot.settings_popup_open = true;
                snprintf(snapshot.settings_popup_title,
                         sizeof(snapshot.settings_popup_title),
                         "系统升级");
                snprintf(snapshot.settings_popup_body,
                         sizeof(snapshot.settings_popup_body),
                         "%s",
                         body.c_str());
                if (ota_active && ota_snapshot.progress >= 0) {
                    snapshot.settings_popup_progress_visible = true;
                    snapshot.settings_popup_progress_min = 0;
                    snapshot.settings_popup_progress_max = 100;
                    snapshot.settings_popup_progress_value = progress;
                    snprintf(snapshot.settings_popup_badge,
                             sizeof(snapshot.settings_popup_badge),
                             "%d%%",
                             progress);
                }
                snprintf(snapshot.settings_popup_hint,
                         sizeof(snapshot.settings_popup_hint),
                         "%s",
                         ota_active ? "升级中，请勿断电" : "确认：开始升级  长按确认：返回");
                break;
            }
            default:
                break;
        }
    }
    return snapshot;
}

void VocabularyApp::SaveSettingsAndRebuildQueueLocked() {
    SaveCurrentPositionLocked();
    (void)progress_store_.SaveSettings(settings_);
    RebuildQueueLocked(true);
    RestoreCurrentPositionLocked();
    if (settings_.study_mode == VocabularyStudyMode::kBrowse) {
        showing_back_ = true;
    }
    SaveCurrentPositionLocked();
}

std::vector<VocabularyApp::DeckChoice> VocabularyApp::BuildLocalDeckChoicesLocked() const {
    std::vector<DeckChoice> choices;
    const auto& local_decks = deck_store_.Decks();
    choices.reserve(local_decks.size());
    for (const DeckInfo& deck : local_decks) {
        DeckChoice choice;
        choice.id = deck.id;
        choice.name = deck.name.empty() ? deck.id : deck.name;
        choice.builtin = deck.builtin;
        choice.installed = true;
        choice.remote = false;
        choice.detail = std::to_string(deck.word_count) + " 词";
        if (deck.builtin) {
            choice.detail += " · 内置";
        } else {
            choice.detail += " · 已下载";
        }
        choices.push_back(std::move(choice));
    }
    return choices;
}

std::vector<VocabularyApp::DeckChoice> VocabularyApp::BuildOnlineDeckChoicesLocked() const {
    std::vector<DeckChoice> choices;
    choices.reserve(remote_decks_.size());
    for (size_t i = 0; i < remote_decks_.size(); ++i) {
        const RemoteDeckEntry& remote = remote_decks_[i];
        if (remote.id.empty()) {
            continue;
        }
        const DeckInfo* installed_deck = nullptr;
        for (const DeckInfo& deck : deck_store_.Decks()) {
            if (deck.id == remote.id) {
                installed_deck = &deck;
                break;
            }
        }
        DeckChoice choice;
        choice.id = remote.id;
        choice.name = remote.name.empty() ? remote.id : remote.name;
        choice.installed = installed_deck != nullptr;
        choice.builtin = installed_deck != nullptr && installed_deck->builtin;
        choice.remote = true;
        choice.remote_index = i;
        if (remote.entry_count > 0) {
            choice.detail = std::to_string(remote.entry_count) + " 词";
        }
        if (remote.size > 0) {
            if (!choice.detail.empty()) {
                choice.detail += " · ";
            }
            choice.detail += FormatBytes(remote.size);
        }
        if (choice.detail.empty()) {
            choice.detail = "在线词库";
        }
        choices.push_back(std::move(choice));
    }
    return choices;
}

std::string VocabularyApp::OnlineDeckStorageBadgeLocked() const {
    const int free_percent = AssetsStore::GetInstance().FreePercent();
    if (free_percent < 0) {
        return "空间--";
    }
    return "剩余" + std::to_string(free_percent) + "%";
}

void VocabularyApp::SelectLocalDeckChoiceLocked() {
    const std::vector<DeckChoice> choices = BuildLocalDeckChoicesLocked();
    if (deck_sync_busy_) {
        return;
    }
    if (choices.empty()) {
        return;
    }
    const size_t index = std::min(settings_local_deck_index_, choices.size() - 1);
    StartDeckSwitchTaskLocked(choices[index].id, choices[index].name);
}

void VocabularyApp::DeleteLocalDeckChoiceLocked() {
    const std::vector<DeckChoice> choices = BuildLocalDeckChoicesLocked();
    if (deck_sync_busy_) {
        return;
    }
    if (choices.empty()) {
        return;
    }

    const size_t index = std::min(settings_local_deck_index_, choices.size() - 1);
    const DeckChoice& choice = choices[index];
    if (choice.builtin) {
        deck_sync_ui_state_ = DeckSyncUiState::kDownloadError;
        deck_sync_status_ = "无法删除";
        deck_sync_detail_ = "内置词库不能删除";
        return;
    }

    const std::string active_before = settings_.active_deck_id;
    if (choice.id == active_before) {
        SaveCurrentPositionLocked();
        (void)progress_store_.Save(settings_.active_deck_id,
                                   BuildProgressWordKeysLocked(), states_, stats_);
    }

    if (!deck_store_.DeleteDeck(choice.id)) {
        deck_sync_ui_state_ = DeckSyncUiState::kDownloadError;
        deck_sync_status_ = "删除失败";
        deck_sync_detail_ = choice.name;
        return;
    }

    (void)progress_store_.ClearProgress(choice.id);
    deck_sync_ui_state_ = DeckSyncUiState::kDownloadSuccess;
    deck_sync_status_ = "已删除";
    deck_sync_detail_ = choice.name;

    if (choice.id == active_before) {
        settings_.active_deck_id = deck_store_.Manifest().deck_id != nullptr
            ? deck_store_.Manifest().deck_id
            : "";
        (void)progress_store_.SaveSettings(settings_);
        const size_t word_count = deck_store_.Count();
        if (!progress_store_.Load(settings_.active_deck_id,
                                  BuildProgressWordKeysLocked(), &states_, &stats_)) {
            states_.assign(word_count, ReviewState{});
            stats_ = VocabularyDailyStats{};
        }
        progress_store_.ResetDailyStatsIfNeeded(today_, &states_, &stats_);
        RebuildQueueLocked();
        RestoreCurrentPositionLocked();
    }

    const std::vector<DeckChoice> next_choices = BuildLocalDeckChoicesLocked();
    if (next_choices.empty()) {
        settings_local_deck_index_ = 0;
    } else {
        settings_local_deck_index_ = std::min(index, next_choices.size() - 1);
    }
}

void VocabularyApp::SelectOnlineDeckChoiceLocked() {
    const std::vector<DeckChoice> choices = BuildOnlineDeckChoicesLocked();
    if (deck_sync_busy_) {
        return;
    }
    if (choices.empty()) {
        online_delete_confirm_ = false;
        StartOnlineWifiLocked("正在打开 WiFi，获取在线词库...");
        StartDeckCatalogTaskLocked();
        return;
    }
    const size_t index = std::min(settings_online_deck_index_, choices.size() - 1);
    const DeckChoice& choice = choices[index];
    ESP_LOGI(kTag,
             "online deck select index=%u/%u id=%s installed=%d builtin=%d remote=%d remote_index=%u",
             static_cast<unsigned>(index + 1),
             static_cast<unsigned>(choices.size()),
             choice.id.c_str(),
             choice.installed ? 1 : 0,
             choice.builtin ? 1 : 0,
             choice.remote ? 1 : 0,
             static_cast<unsigned>(choice.remote_index));
    if (choice.installed) {
        if (choice.builtin) {
            deck_sync_ui_state_ = DeckSyncUiState::kDownloadError;
            deck_sync_status_ = "无法删除";
            deck_sync_detail_ = "内置词库不能删除";
            return;
        }
        online_delete_confirm_ = false;
        StartDeckDeleteTaskLocked(choice.id, choice.name);
        return;
    }
    if (choice.remote && choice.remote_index < remote_decks_.size()) {
        online_delete_confirm_ = false;
        StartOnlineWifiLocked("正在打开 WiFi，下载词库...");
        StartDeckDownloadTaskLocked(remote_decks_[choice.remote_index]);
    }
}

void VocabularyApp::DeleteOnlineDeckChoiceLocked() {
    const std::vector<DeckChoice> choices = BuildOnlineDeckChoicesLocked();
    if (choices.empty()) {
        online_delete_confirm_ = false;
        return;
    }

    const size_t index = std::min(settings_online_deck_index_, choices.size() - 1);
    const DeckChoice& choice = choices[index];
    if (!choice.installed) {
        online_delete_confirm_ = false;
        return;
    }
    if (choice.builtin) {
        online_delete_confirm_ = false;
        deck_sync_ui_state_ = DeckSyncUiState::kDownloadError;
        deck_sync_status_ = "无法删除";
        deck_sync_detail_ = "内置词库不能删除";
        return;
    }

    const std::string active_before = settings_.active_deck_id;
    if (choice.id == active_before) {
        (void)progress_store_.Save(settings_.active_deck_id,
                                   BuildProgressWordKeysLocked(), states_, stats_);
    }

    if (!deck_store_.DeleteDeck(choice.id)) {
        online_delete_confirm_ = false;
        deck_sync_ui_state_ = DeckSyncUiState::kDownloadError;
        deck_sync_status_ = "删除失败";
        deck_sync_detail_ = "无法删除内置词库或本地文件";
        return;
    }

    (void)progress_store_.ClearProgress(choice.id);
    online_delete_confirm_ = false;
    deck_sync_ui_state_ = DeckSyncUiState::kDownloadSuccess;
    deck_sync_status_ = "已删除";
    deck_sync_detail_ = choice.name;

    if (choice.id == active_before) {
        settings_.active_deck_id = deck_store_.Manifest().deck_id != nullptr
            ? deck_store_.Manifest().deck_id
            : "";
        (void)progress_store_.SaveSettings(settings_);
        const size_t word_count = deck_store_.Count();
        if (!progress_store_.Load(settings_.active_deck_id,
                                  BuildProgressWordKeysLocked(), &states_, &stats_)) {
            states_.assign(word_count, ReviewState{});
            stats_ = VocabularyDailyStats{};
        }
        progress_store_.ResetDailyStatsIfNeeded(today_, &states_, &stats_);
        RebuildQueueLocked();
        RestoreCurrentPositionLocked();
    }

    const std::vector<DeckChoice> local_choices = BuildLocalDeckChoicesLocked();
    settings_local_deck_index_ = 0;
    for (size_t i = 0; i < local_choices.size(); ++i) {
        if (local_choices[i].id == settings_.active_deck_id) {
            settings_local_deck_index_ = i;
            break;
        }
    }
}

bool VocabularyApp::SwitchDeckLocked(const std::string& deck_id) {
    if (deck_id.empty() || deck_id == settings_.active_deck_id) {
        return !deck_id.empty();
    }
    if (!deck_store_.HasDeck(deck_id)) {
        ESP_LOGW(kTag, "switch deck ignored, not installed: %s", deck_id.c_str());
        return false;
    }

    const std::string previous_deck_id = settings_.active_deck_id;
    SaveCurrentPositionLocked();
    (void)progress_store_.Save(settings_.active_deck_id,
                               BuildProgressWordKeysLocked(), states_, stats_);
    if (!deck_store_.SelectDeck(deck_id) ||
        deck_store_.Manifest().deck_id == nullptr ||
        deck_id != deck_store_.Manifest().deck_id) {
        ESP_LOGW(kTag, "switch deck failed: %s", deck_id.c_str());
        if (!previous_deck_id.empty() && deck_store_.HasDeck(previous_deck_id)) {
            (void)deck_store_.SelectDeck(previous_deck_id);
        }
        return false;
    }
    settings_.active_deck_id = deck_store_.Manifest().deck_id != nullptr
        ? deck_store_.Manifest().deck_id
        : "";
    (void)progress_store_.SaveSettings(settings_);

    const size_t word_count = deck_store_.Count();
    if (!progress_store_.Load(settings_.active_deck_id,
                              BuildProgressWordKeysLocked(), &states_, &stats_)) {
        states_.assign(word_count, ReviewState{});
        stats_ = VocabularyDailyStats{};
    }
    progress_store_.ResetDailyStatsIfNeeded(today_, &states_, &stats_);
    RebuildQueueLocked();
    RestoreCurrentPositionLocked();
    return true;
}

void VocabularyApp::StartDeckSwitchTaskLocked(const std::string& deck_id, const std::string& name) {
    if (deck_sync_busy_ || deck_id.empty()) {
        return;
    }

    const bool save_progress = progress_dirty_;
    online_delete_confirm_ = false;
    SaveCurrentPositionLocked();
    deck_sync_busy_ = true;
    deck_sync_progress_ = -1;
    deck_sync_ui_state_ = DeckSyncUiState::kSwitchLoading;
    deck_sync_status_ = "正在加载词库";
    deck_sync_detail_ = name.empty() ? deck_id : name;
    const uint32_t request_id = deck_sync_request_.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto* args = new (std::nothrow) DeckSwitchTaskArgs{
        .app = this,
        .request_id = request_id,
        .deck_id = deck_id,
        .name = name,
        .active_deck_id = settings_.active_deck_id,
        .progress_word_keys = save_progress
            ? BuildProgressWordKeysLocked()
            : std::vector<uint64_t>{},
        .progress_states = save_progress ? states_ : std::vector<ReviewState>{},
        .progress_stats = stats_,
        .save_progress = save_progress,
    };
    if (args == nullptr) {
        deck_sync_busy_ = false;
        deck_sync_ui_state_ = DeckSyncUiState::kDownloadError;
        deck_sync_status_ = "切换失败";
        deck_sync_detail_ = "无法创建切换任务";
        return;
    }

    BaseType_t created = xTaskCreate(
        &VocabularyApp::DeckSwitchTaskEntry,
        "deck_switch",
        kDeckSyncTaskStackBytes,
        args,
        4,
        nullptr);
    if (created != pdPASS) {
        delete args;
        deck_sync_busy_ = false;
        deck_sync_ui_state_ = DeckSyncUiState::kDownloadError;
        deck_sync_status_ = "切换失败";
        deck_sync_detail_ = "无法启动切换任务";
        ESP_LOGW(kTag,
                 "deck switch task create failed free_internal=%u min_internal=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
    } else {
        if (save_progress) {
            progress_dirty_ = false;
        }
        ESP_LOGI(kTag, "deck switch task started id=%s", deck_id.c_str());
    }
}

void VocabularyApp::StartDeckDeleteTaskLocked(const std::string& deck_id, const std::string& name) {
    if (deck_sync_busy_ || deck_id.empty()) {
        return;
    }

    const bool save_progress = deck_id == settings_.active_deck_id && progress_dirty_;
    SaveCurrentPositionLocked();
    online_delete_confirm_ = false;
    deck_sync_busy_ = true;
    deck_sync_progress_ = -1;
    deck_sync_ui_state_ = DeckSyncUiState::kDeleteLoading;
    deck_sync_status_ = "正在删除词库";
    deck_sync_detail_ = name.empty() ? deck_id : name;
    const uint32_t request_id = deck_sync_request_.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto* args = new (std::nothrow) DeckSwitchTaskArgs{
        .app = this,
        .request_id = request_id,
        .deck_id = deck_id,
        .name = name,
        .active_deck_id = settings_.active_deck_id,
        .progress_word_keys = save_progress
            ? BuildProgressWordKeysLocked()
            : std::vector<uint64_t>{},
        .progress_states = save_progress ? states_ : std::vector<ReviewState>{},
        .progress_stats = stats_,
        .save_progress = save_progress,
        .delete_deck = true,
    };
    if (args == nullptr || xTaskCreate(&VocabularyApp::DeckSwitchTaskEntry,
                                      "deck_delete",
                                      kDeckSyncTaskStackBytes,
                                      args,
                                      4,
                                      nullptr) != pdPASS) {
        delete args;
        deck_sync_busy_ = false;
        deck_sync_ui_state_ = DeckSyncUiState::kDownloadError;
        deck_sync_status_ = "删除失败";
        deck_sync_detail_ = "无法启动删除任务";
    } else if (save_progress) {
        progress_dirty_ = false;
    }
}

void VocabularyApp::StartDeckCatalogTaskLocked() {
    if (deck_sync_busy_) {
        return;
    }

    online_delete_confirm_ = false;
    deck_sync_busy_ = true;
    deck_sync_progress_ = -1;
    deck_sync_ui_state_ = DeckSyncUiState::kCatalogLoading;
    deck_sync_status_ = "正在获取列表";
    deck_sync_detail_ = "正在连接在线词库";
    const uint32_t request_id = deck_sync_request_.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto* args = new (std::nothrow) DeckSyncTaskArgs{
        .app = this,
        .request_id = request_id,
        .download = false,
    };
    if (args == nullptr) {
        deck_sync_busy_ = false;
        deck_sync_ui_state_ = DeckSyncUiState::kCatalogError;
        deck_sync_status_ = "获取失败";
        deck_sync_detail_ = "无法创建获取任务";
        return;
    }

    BaseType_t created = xTaskCreate(
        &VocabularyApp::DeckSyncTaskEntry,
        "deck_catalog",
        kDeckSyncTaskStackBytes,
        args,
        4,
        nullptr);
    if (created != pdPASS) {
        delete args;
        deck_sync_busy_ = false;
        deck_sync_ui_state_ = DeckSyncUiState::kCatalogError;
        deck_sync_status_ = "获取失败";
        deck_sync_detail_ = "无法启动获取任务";
    }
}

void VocabularyApp::StartDeckDownloadTaskLocked(const RemoteDeckEntry& entry) {
    if (deck_sync_busy_) {
        ESP_LOGI(kTag, "deck download ignored while busy: %s", entry.id.c_str());
        return;
    }

    ESP_LOGI(kTag,
             "start deck download id=%s name=%s size=%u free_internal=%u min_internal=%u",
             entry.id.c_str(),
             entry.name.c_str(),
             static_cast<unsigned>(entry.size),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
    online_delete_confirm_ = false;
    deck_sync_busy_ = true;
    deck_sync_progress_ = 0;
    deck_sync_ui_state_ = DeckSyncUiState::kDownloadLoading;
    deck_sync_status_ = "正在下载";
    deck_sync_detail_ = entry.name.empty() ? entry.id : entry.name;
    const uint32_t request_id = deck_sync_request_.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto* args = new (std::nothrow) DeckSyncTaskArgs{
        .app = this,
        .request_id = request_id,
        .download = true,
        .entry = entry,
    };
    if (args == nullptr) {
        deck_sync_busy_ = false;
        deck_sync_ui_state_ = DeckSyncUiState::kDownloadError;
        deck_sync_status_ = "下载失败";
        deck_sync_detail_ = "无法创建下载任务";
        ESP_LOGW(kTag, "deck download args allocation failed");
        return;
    }

    BaseType_t created = xTaskCreate(
        &VocabularyApp::DeckSyncTaskEntry,
        "deck_download",
        kDeckSyncTaskStackBytes,
        args,
        4,
        nullptr);
    if (created != pdPASS) {
        delete args;
        deck_sync_busy_ = false;
        deck_sync_ui_state_ = DeckSyncUiState::kDownloadError;
        deck_sync_status_ = "下载失败";
        deck_sync_detail_ = "无法启动下载任务";
        ESP_LOGW(kTag,
                 "deck download task create failed free_internal=%u min_internal=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
    } else {
        ESP_LOGI(kTag, "deck download task started id=%s", entry.id.c_str());
    }
}

bool VocabularyApp::IsCurrentDeckSyncRequest(uint32_t request_id) const {
    return deck_sync_request_.load(std::memory_order_acquire) == request_id;
}

void VocabularyApp::WifiIdleTimerEntry(void* arg) {
    auto* app = static_cast<VocabularyApp*>(arg);
    if (app == nullptr) {
        return;
    }
    Application::GetInstance().Schedule([app]() {
        app->HandleWifiIdleTimeout();
    });
}

void VocabularyApp::ProgressSaveTimerEntry(void* arg) {
    auto* app = static_cast<VocabularyApp*>(arg);
    if (app == nullptr) {
        return;
    }
    Application::GetInstance().Schedule([app]() {
        std::lock_guard<std::mutex> lock(app->mutex_);
        app->StartProgressSaveTaskLocked();
    });
}

void VocabularyApp::ActionStatusTimerEntry(void* arg) {
    auto* app = static_cast<VocabularyApp*>(arg);
    if (app == nullptr) {
        return;
    }
    Application::GetInstance().Schedule([app]() {
        app->HandleActionStatusTimeout();
    });
}

void VocabularyApp::RtcTimeSyncTaskEntry(void* arg) {
    auto* app = static_cast<VocabularyApp*>(arg);
    if (app != nullptr) {
        app->RunRtcTimeSyncTask();
    }
    vTaskDelete(nullptr);
}

void VocabularyApp::RunRtcTimeSyncTask() {
    // The product currently uses China local time. SNTP supplies UTC to the
    // system clock; POSIX TZ then gives the civil time written to the RTC.
    setenv("TZ", "CST-8", 1);
    tzset();

    bool synchronized = false;
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    const esp_err_t init_ret = esp_netif_sntp_init(&config);
    if (init_ret == ESP_OK) {
        synchronized = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(8000)) == ESP_OK;
        esp_netif_sntp_deinit();
    } else {
        ESP_LOGW(kTag, "SNTP init failed: %s", esp_err_to_name(init_ret));
    }

    if (synchronized) {
        const time_t now = time(nullptr);
        tm local_tm = {};
        if (now >= 1704067200 && localtime_r(&now, &local_tm) != nullptr) {
            RtcPcf8563* rtc = EggaGetRtc();
            synchronized = rtc != nullptr && rtc->SetTime(local_tm);
            if (synchronized) {
                ESP_LOGI(kTag,
                         "RTC synchronized to %04d-%02d-%02d %02d:%02d:%02d",
                         local_tm.tm_year + 1900,
                         local_tm.tm_mon + 1,
                         local_tm.tm_mday,
                         local_tm.tm_hour,
                         local_tm.tm_min,
                         local_tm.tm_sec);
            }
        } else {
            synchronized = false;
        }
    }
    if (!synchronized) {
        ESP_LOGW(kTag, "RTC time synchronization did not complete");
    }

    rtc_time_sync_busy_.store(false, std::memory_order_release);
    Application::GetInstance().Schedule([this, synchronized]() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (synchronized) {
            // Re-evaluate the natural day immediately after correcting the RTC;
            // this also refreshes the persistent header clock.
            RefreshCurrentDayLocked();
        }
        if (rtc_time_repair_pending_) {
            rtc_time_repair_pending_ = false;
            if (settings_.study_mode != VocabularyStudyMode::kMaimemo) {
                StopStationForOfflineLocked();
            }
        }
        PublishSnapshotLocked(false);
    });
}

void VocabularyApp::DeckSyncTaskEntry(void* arg) {
    std::unique_ptr<DeckSyncTaskArgs> args(static_cast<DeckSyncTaskArgs*>(arg));
    if (args && args->app != nullptr) {
        args->app->RunDeckSyncTask(args->request_id, args->download, std::move(args->entry));
    }
    vTaskDelete(nullptr);
}

void VocabularyApp::DeckSwitchTaskEntry(void* arg) {
    std::unique_ptr<DeckSwitchTaskArgs> args(static_cast<DeckSwitchTaskArgs*>(arg));
    if (args && args->app != nullptr) {
        args->app->RunDeckSwitchTask(args->request_id,
                                     std::move(args->deck_id),
                                     std::move(args->name),
                                     std::move(args->active_deck_id),
                                     std::move(args->progress_word_keys),
                                     std::move(args->progress_states),
                                     args->progress_stats,
                                     args->save_progress,
                                     args->delete_deck);
    }
    vTaskDelete(nullptr);
}

void VocabularyApp::ProgressSaveTaskEntry(void* arg) {
    std::unique_ptr<ProgressSaveTaskArgs> args(static_cast<ProgressSaveTaskArgs*>(arg));
    if (args && args->app != nullptr) {
        args->app->RunProgressSaveTask(args->request_id,
                                       std::move(args->deck_id),
                                       std::move(args->word_keys),
                                       std::move(args->states),
                                       args->stats);
    }
    vTaskDelete(nullptr);
}

void VocabularyApp::OtaTaskEntry(void* arg) {
    std::unique_ptr<OtaTaskArgs> args(static_cast<OtaTaskArgs*>(arg));
    if (args && args->app != nullptr) {
        args->app->RunOtaTask(args->request_id, args->start_update);
    }
    vTaskDelete(nullptr);
}

void VocabularyApp::AiEntitlementTaskEntry(void* arg) {
    std::unique_ptr<AiEntitlementTaskArgs> args(
        static_cast<AiEntitlementTaskArgs*>(arg));
    if (args && args->app != nullptr) {
        args->app->RunAiEntitlementCheck(args->request_id);
    }
    vTaskDelete(nullptr);
}

void VocabularyApp::RunProgressSaveTask(uint32_t request_id,
                                        std::string deck_id,
                                        std::vector<uint64_t> word_keys,
                                        std::vector<ReviewState> states,
                                        VocabularyDailyStats stats) {
    const bool saved = progress_store_.Save(deck_id, word_keys, states, stats);
    std::lock_guard<std::mutex> lock(mutex_);
    if (progress_save_request_.load(std::memory_order_acquire) == request_id) {
        progress_save_busy_ = false;
    }
    if (!saved) {
        progress_dirty_ = true;
    }
    if (progress_dirty_) {
        ArmProgressSaveTimerLocked();
    }
}

bool VocabularyApp::CommitPreparedDeckLocked(DeckStore&& prepared,
                                             const std::string& deck_id,
                                             std::vector<ReviewState>&& states,
                                             VocabularyDailyStats stats) {
    const DeckManifest& prepared_manifest = prepared.Manifest();
    if (prepared_manifest.deck_id == nullptr ||
        (!deck_id.empty() && deck_id != prepared_manifest.deck_id)) {
        return false;
    }

    deck_store_.AdoptPrepared(std::move(prepared));
    settings_.active_deck_id = deck_store_.Manifest().deck_id != nullptr
        ? deck_store_.Manifest().deck_id
        : "";
    (void)progress_store_.SaveSettings(settings_);
    states_ = std::move(states);
    stats_ = stats;
    RebuildQueueLocked();
    RestoreCurrentPositionLocked();
    return true;
}

void VocabularyApp::RunDeckSwitchTask(uint32_t request_id,
                                      std::string deck_id,
                                      std::string name,
                                      std::string active_deck_id,
                                      std::vector<uint64_t> progress_word_keys,
                                      std::vector<ReviewState> progress_states,
                                      VocabularyDailyStats progress_stats,
                                      bool save_progress,
                                      bool delete_deck) {
    ESP_LOGI(kTag,
             "deck task begin request=%u id=%s delete=%d",
             static_cast<unsigned>(request_id),
             deck_id.c_str(),
             delete_deck ? 1 : 0);

    const bool progress_saved = !save_progress ||
        progress_store_.Save(active_deck_id,
                             progress_word_keys,
                             progress_states,
                             progress_stats);

    DeckStore prepared;
    bool prepared_ok = false;
    if (delete_deck) {
        prepared.Initialize(active_deck_id);
        prepared_ok = prepared.DeleteDeck(deck_id);
        if (prepared_ok) {
            (void)progress_store_.ClearProgress(deck_id);
        }
    } else {
        prepared.Initialize(deck_id);
        prepared_ok = prepared.Manifest().deck_id != nullptr &&
            deck_id == prepared.Manifest().deck_id;
    }

    std::vector<ReviewState> next_states;
    VocabularyDailyStats next_stats;
    std::string next_deck_id;
    if (prepared_ok && prepared.Manifest().deck_id != nullptr) {
        next_deck_id = prepared.Manifest().deck_id;
        const std::vector<uint64_t> next_word_keys = BuildProgressWordKeysForStore(prepared);
        if (!progress_store_.Load(next_deck_id, next_word_keys, &next_states, &next_stats)) {
            next_states.assign(prepared.Count(), ReviewState{});
            next_stats = VocabularyDailyStats{};
        }
        progress_store_.ResetDailyStatsIfNeeded(today_, &next_states, &next_stats);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!IsCurrentDeckSyncRequest(request_id)) {
        return;
    }
    if (!progress_saved) {
        progress_dirty_ = true;
        ArmProgressSaveTimerLocked();
    }

    bool switched = false;
    if (prepared_ok) {
        switched = CommitPreparedDeckLocked(std::move(prepared),
                                            next_deck_id,
                                            std::move(next_states),
                                            next_stats);
    }
    deck_sync_busy_ = false;
    deck_sync_progress_ = -1;
    deck_sync_ui_state_ = switched ? DeckSyncUiState::kDownloadSuccess : DeckSyncUiState::kDownloadError;
    deck_sync_status_ = delete_deck
        ? (switched ? "已删除" : "删除失败")
        : (switched ? "已切换" : "切换失败");
    deck_sync_detail_ = name.empty() ? deck_id : name;
    if (switched) {
        const std::vector<DeckChoice> local_choices = BuildLocalDeckChoicesLocked();
        for (size_t i = 0; i < local_choices.size(); ++i) {
            if (local_choices[i].id == settings_.active_deck_id) {
                settings_local_deck_index_ = i;
                break;
            }
        }
        const std::vector<DeckChoice> online_choices = BuildOnlineDeckChoicesLocked();
        if (settings_online_deck_index_ >= online_choices.size()) {
            settings_online_deck_index_ = online_choices.empty() ? 0 : online_choices.size() - 1;
        }
    }
    ESP_LOGI(kTag,
             "deck task finish request=%u id=%s delete=%d ok=%d",
             static_cast<unsigned>(request_id),
             deck_id.c_str(),
             delete_deck ? 1 : 0,
             switched ? 1 : 0);
    PublishSnapshotLocked(false);
}

void VocabularyApp::RunDeckSyncTask(uint32_t request_id, bool download, RemoteDeckEntry entry) {
    ESP_LOGI(kTag,
             "deck sync task begin request=%u download=%d id=%s",
             static_cast<unsigned>(request_id),
             download ? 1 : 0,
             entry.id.c_str());
    if (!WaitForOnlineWifiReady()) {
        ESP_LOGW(kTag,
                 "deck sync WiFi wait failed request=%u download=%d id=%s",
                 static_cast<unsigned>(request_id),
                 download ? 1 : 0,
                 entry.id.c_str());
        std::lock_guard<std::mutex> lock(mutex_);
        if (!IsCurrentDeckSyncRequest(request_id)) {
            return;
        }
        deck_sync_busy_ = false;
        deck_sync_ui_state_ = download ? DeckSyncUiState::kDownloadError : DeckSyncUiState::kCatalogError;
        deck_sync_status_ = download ? "下载失败" : "获取失败";
        deck_sync_detail_ = "WiFi 未连接，稍后再试";
        RestoreWifiAfterNetworkTaskLocked();
        PublishSnapshotLocked(false);
        return;
    }

    std::vector<RemoteDeckEntry> remote_decks;
    bool ok = false;
    if (download) {
        const std::string deck_name = entry.name.empty() ? entry.id : entry.name;
        auto last_percent = std::make_shared<int>(-1);
        ok = sync_client_.DownloadDeck(
            entry,
            nullptr,
            [this, request_id, deck_name, last_percent](size_t written, size_t total) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!IsCurrentDeckSyncRequest(request_id)) {
                    return;
                }
                const int percent = total > 0
                    ? static_cast<int>(std::min<size_t>(100, (written * 100) / total))
                    : 0;
                const int displayed_percent = total > 0 && written < total
                    ? (percent / 5) * 5
                    : percent;
                if (displayed_percent == *last_percent && (total == 0 || written < total)) {
                    return;
                }
                *last_percent = displayed_percent;
                deck_sync_progress_ = displayed_percent;
                deck_sync_ui_state_ = DeckSyncUiState::kDownloadLoading;
                deck_sync_status_ = "正在下载";
                deck_sync_detail_ = deck_name + " " + std::to_string(displayed_percent) + "% (" +
                    std::to_string(written / 1024) + "/" +
                    (total > 0 ? std::to_string(total / 1024) : std::string("--")) + "KB)";
                PublishSnapshotLocked(false);
            });
    } else {
        ok = sync_client_.FetchDeckManifest(&remote_decks);
    }

    std::unique_ptr<DeckStore> downloaded_store;
    std::vector<ReviewState> downloaded_states;
    VocabularyDailyStats downloaded_stats;
    if (ok && download) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!IsCurrentDeckSyncRequest(request_id)) {
                return;
            }
            deck_sync_progress_ = 100;
            deck_sync_ui_state_ = DeckSyncUiState::kSwitchLoading;
            deck_sync_status_ = "下载完成，正在加载";
            deck_sync_detail_ = entry.name.empty() ? entry.id : entry.name;
            PublishSnapshotLocked(false);
        }
        downloaded_store = std::make_unique<DeckStore>();
        downloaded_store->Initialize(entry.id);
        ok = downloaded_store->Manifest().deck_id != nullptr &&
            entry.id == downloaded_store->Manifest().deck_id;
        if (ok) {
            const std::vector<uint64_t> word_keys =
                BuildProgressWordKeysForStore(*downloaded_store);
            if (!progress_store_.Load(entry.id,
                                      word_keys,
                                      &downloaded_states,
                                      &downloaded_stats)) {
                downloaded_states.assign(downloaded_store->Count(), ReviewState{});
                downloaded_stats = VocabularyDailyStats{};
            }
            progress_store_.ResetDailyStatsIfNeeded(
                today_, &downloaded_states, &downloaded_stats);
        }
    }
    ESP_LOGI(kTag,
             "deck sync task finish request=%u download=%d ok=%d status=%s",
             static_cast<unsigned>(request_id),
             download ? 1 : 0,
             ok ? 1 : 0,
             sync_client_.LastStatus().c_str());

    std::lock_guard<std::mutex> lock(mutex_);
    if (!IsCurrentDeckSyncRequest(request_id)) {
        return;
    }

    if (ok && download) {
        online_delete_confirm_ = false;
        const bool switched = downloaded_store != nullptr &&
            CommitPreparedDeckLocked(std::move(*downloaded_store),
                                     entry.id,
                                     std::move(downloaded_states),
                                     downloaded_stats);
        deck_sync_status_ = switched ? "下载完成，已切换" : "下载完成，切换失败";
        deck_sync_busy_ = false;
        deck_sync_progress_ = -1;
        deck_sync_ui_state_ = switched ? DeckSyncUiState::kDownloadSuccess : DeckSyncUiState::kDownloadError;
        const std::vector<DeckChoice> online_choices = BuildOnlineDeckChoicesLocked();
        for (size_t i = 0; i < online_choices.size(); ++i) {
            if (online_choices[i].id == entry.id) {
                settings_online_deck_index_ = i;
                break;
            }
        }
        const std::vector<DeckChoice> local_choices = BuildLocalDeckChoicesLocked();
        for (size_t i = 0; i < local_choices.size(); ++i) {
            if (local_choices[i].id == entry.id) {
                settings_local_deck_index_ = i;
                break;
            }
        }
    } else if (ok) {
        online_delete_confirm_ = false;
        deck_sync_busy_ = false;
        deck_sync_progress_ = -1;
        deck_sync_ui_state_ = DeckSyncUiState::kIdle;
        deck_sync_status_ = "列表已更新";
        deck_sync_detail_ = std::to_string(remote_decks.size()) + " 个在线词库";
        remote_decks_ = std::move(remote_decks);
        const std::vector<DeckChoice> online_choices = BuildOnlineDeckChoicesLocked();
        if (settings_online_deck_index_ >= online_choices.size()) {
            settings_online_deck_index_ = 0;
        }
    } else if (download) {
        online_delete_confirm_ = false;
        deck_sync_busy_ = false;
        deck_sync_progress_ = -1;
        deck_sync_ui_state_ = DeckSyncUiState::kDownloadError;
        deck_sync_status_ = "下载失败";
        deck_sync_detail_ = sync_client_.LastStatus();
        if (deck_sync_detail_.empty()) {
            deck_sync_detail_ = entry.name.empty() ? entry.id : entry.name;
        }
        if (IsPermissionFailure(deck_sync_detail_)) {
            deck_sync_detail_ = std::string("无词库权限，请联系开发者获取权限。") + kDeveloperContact;
        }
    } else {
        online_delete_confirm_ = false;
        deck_sync_busy_ = false;
        deck_sync_progress_ = -1;
        deck_sync_ui_state_ = DeckSyncUiState::kCatalogError;
        deck_sync_status_ = "获取失败";
        deck_sync_detail_ = sync_client_.LastStatus();
        if (deck_sync_detail_.empty()) {
            deck_sync_detail_ = "请检查 WiFi 或在线词库服务";
        }
        if (IsPermissionFailure(deck_sync_detail_)) {
            deck_sync_detail_ = std::string("无词库权限，请联系开发者获取权限。") + kDeveloperContact;
        }
    }
    RestoreWifiAfterNetworkTaskLocked();
    PublishSnapshotLocked(false);
}

void VocabularyApp::StartOtaTaskLocked() {
    StartOtaTaskLocked(true);
}

void VocabularyApp::StartOtaCheckTaskLocked() {
    StartOtaTaskLocked(false);
}

void VocabularyApp::StartSettingsBackgroundRefreshLocked() {
    StartOnlineWifiLocked("正在打开 WiFi...");
    // Do not start the deck catalog and OTA check concurrently merely by
    // opening settings.  Both are large TLS/network tasks; the catalog is
    // started on demand when the user opens “我的词库”.
    StartOtaCheckTaskLocked();
}

void VocabularyApp::StartOtaTaskLocked(bool start_update) {
    if (ota_busy_) {
        return;
    }

    const OtaService::Snapshot snapshot = OtaService::GetInstance().GetSnapshot();
    if (OtaSnapshotBusy(snapshot)) {
        ota_status_ = OtaStatusText(snapshot, "升级处理中");
        return;
    }

    ota_busy_ = true;
    ota_status_ = start_update ? "正在连接 WiFi..." : "正在后台检查升级...";
    const uint32_t request_id = ota_request_.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto* args = new (std::nothrow) OtaTaskArgs{
        .app = this,
        .request_id = request_id,
        .start_update = start_update,
    };
    if (args == nullptr) {
        ota_busy_ = false;
        ota_status_ = start_update ? "升级任务启动失败" : "检查任务启动失败";
        return;
    }

    BaseType_t created = xTaskCreate(
        &VocabularyApp::OtaTaskEntry,
        "ota_ui",
        kOtaUiTaskStackBytes,
        args,
        4,
        nullptr);
    if (created != pdPASS) {
        delete args;
        ota_busy_ = false;
        ota_status_ = start_update ? "升级任务启动失败" : "检查任务启动失败";
    }
}

bool VocabularyApp::IsCurrentOtaRequest(uint32_t request_id) const {
    return ota_request_.load(std::memory_order_acquire) == request_id;
}

void VocabularyApp::RunOtaTask(uint32_t request_id, bool start_update) {
    auto publish = [&](const std::string& status, bool busy, bool restore_wifi) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!IsCurrentOtaRequest(request_id)) {
            return false;
        }
        ota_status_ = status;
        ota_busy_ = busy;
        if (restore_wifi) {
            RestoreWifiAfterNetworkTaskLocked();
        }
        PublishSnapshotLocked(false, true);
        return true;
    };

    if (!publish("正在连接 WiFi...", true, false)) {
        return;
    }
    if (!WaitForOnlineWifiReady()) {
        publish(start_update ? "WiFi 未连接，无法升级" : "WiFi 未连接，稍后再检查", false, true);
        return;
    }

    if (!publish(start_update ? "正在检查升级..." : "后台检查升级...", true, false)) {
        return;
    }
    std::string error;
    if (!start_update) {
        OtaService::GetInstance().Check("", &error);
        const OtaService::Snapshot snapshot = OtaService::GetInstance().GetSnapshot();
        publish(OtaStatusText(snapshot, error.empty() ? "检查完成" : error), false, true);
        return;
    }

    const bool started = OtaService::GetInstance().Start("", &error);
    if (!started) {
        publish(error.empty() ? "升级启动失败" : error, false, true);
        return;
    }

    bool timed_out = true;
    int available_poll_count = 0;
    int last_published_progress = -1;
    int64_t last_published_ms = 0;
    std::string last_published_state;
    for (int i = 0; i < kOtaMaxPollSeconds; ++i) {
        const OtaService::Snapshot snapshot = OtaService::GetInstance().GetSnapshot();
        const int64_t now_ms = NowMs();
        const bool state_changed = snapshot.state != last_published_state;
        const bool progress_advanced = snapshot.progress >= 0 &&
            (last_published_progress < 0 ||
             snapshot.progress >= last_published_progress + kOtaProgressRefreshStep);
        const bool refresh_due = last_published_ms == 0 ||
            now_ms - last_published_ms >= kOtaProgressRefreshMaxIntervalMs;
        if (state_changed || progress_advanced || refresh_due) {
            if (!publish(OtaStatusText(snapshot, "升级处理中"), true, false)) {
                return;
            }
            last_published_state = snapshot.state;
            last_published_progress = snapshot.progress;
            last_published_ms = now_ms;
        }
        if (!OtaSnapshotActive(snapshot)) {
            timed_out = false;
            break;
        }
        if (snapshot.state == "available" && !snapshot.running) {
            ++available_poll_count;
            if (available_poll_count > 15) {
                timed_out = false;
                break;
            }
        } else {
            available_poll_count = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    const OtaService::Snapshot final_snapshot = OtaService::GetInstance().GetSnapshot();
    const bool active = OtaSnapshotActive(final_snapshot);
    const bool stuck_available = final_snapshot.state == "available" && !final_snapshot.running;
    const bool keep_wifi = active && !stuck_available;
    if (timed_out && keep_wifi) {
        publish("升级仍在后台进行", false, false);
        return;
    }
    publish(OtaStatusText(final_snapshot, "升级完成"), false, !keep_wifi);
}

void VocabularyApp::MarkUserActivityLocked() {
    last_activity_ms_ = NowMs();
    if (settings_.study_mode != VocabularyStudyMode::kMaimemo) {
        return;
    }
    ArmMaimemoWifiIdleTimerLocked();
    if (!wake_wifi_restore_pending_ &&
        maimemo_wifi_suspended_ && !WifiStationConnected()) {
        StartOnlineWifiLocked("正在重新连接 WiFi...");
    }
}

void VocabularyApp::StartOnlineWifiLocked(const char* status) {
    maimemo_wifi_suspended_ = false;
    if (status != nullptr && status[0] != '\0') {
        maimemo_status_ = status;
    }
    if (!HasSavedWifiCredentials()) {
        ESP_LOGI(kTag, "Skip online WiFi start: no saved SSID");
        maimemo_status_ = "请先配置 WiFi";
        if (settings_.study_mode == VocabularyStudyMode::kMaimemo) {
            ArmMaimemoWifiIdleTimerLocked();
        }
        return;
    }
    Board::GetInstance().RequestNetwork();
    WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::LOW_POWER);
    if (settings_.study_mode == VocabularyStudyMode::kMaimemo) {
        ArmMaimemoWifiIdleTimerLocked();
    }
}

void VocabularyApp::StartTemporaryPronunciationWifiLocked() {
    if (settings_.study_mode == VocabularyStudyMode::kMaimemo) {
        return;
    }
    if (!HasSavedWifiCredentials()) {
        offline_pronunciation_wifi_active_ = false;
        offline_pronunciation_wifi_deadline_ms_ = 0;
        return;
    }

    offline_pronunciation_wifi_active_ = true;
    offline_pronunciation_wifi_deadline_ms_ = NowMs() + kOfflinePronunciationWifiIdleMs;
    Board::GetInstance().RequestNetwork();
    WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::PERFORMANCE);
    ArmMaimemoWifiIdleTimerLocked();
}

void VocabularyApp::StopStationForOfflineLocked() {
    DisarmMaimemoWifiIdleTimerLocked();
    maimemo_wifi_suspended_ = false;
    offline_pronunciation_wifi_active_ = false;
    offline_pronunciation_wifi_deadline_ms_ = 0;
    WifiManager::GetInstance().StopStation();
}

void VocabularyApp::RestoreWifiAfterNetworkTaskLocked() {
    if (settings_open_) {
        return;
    }
    if (settings_.study_mode != VocabularyStudyMode::kMaimemo) {
        StopStationForOfflineLocked();
    } else {
        ArmMaimemoWifiIdleTimerLocked();
    }
}

bool VocabularyApp::EnsureMaimemoWifiForUserActionLocked() {
    if (WifiStationConnected()) {
        maimemo_wifi_suspended_ = false;
        ArmMaimemoWifiIdleTimerLocked();
        return true;
    }

    StartOnlineWifiLocked("正在重新连接 WiFi...");
    return false;
}

void VocabularyApp::EnableMaimemoModeLocked() {
    settings_.study_mode = VocabularyStudyMode::kMaimemo;
    ResetMaimemoSessionLocked(false);
    if (maimemo_offline_state_.snapshot_day_index == today_ &&
        !maimemo_offline_state_.today_items.empty()) {
        (void)LoadMaimemoOfflineItemLocked(maimemo_offline_state_.current_index,
                                           "离线快照已就绪，正在联网校验");
    } else {
        maimemo_has_card_ = false;
        maimemo_showing_back_ = false;
    }
    maimemo_status_ = "正在打开 WiFi...";
    StartOnlineWifiLocked("正在打开 WiFi...");
    if (WifiStationConnected()) {
        maimemo_status_ = "WiFi 已连接，正在加载墨墨单词...";
        StartMaimemoFetchLocked(false);
    }
}

void VocabularyApp::DisableMaimemoModeLocked() {
    settings_.study_mode = VocabularyStudyMode::kOffline;
    ResetMaimemoSessionLocked(true);
    maimemo_status_ = "墨墨未连接";
    StopStationForOfflineLocked();
}

void VocabularyApp::ResetMaimemoSessionLocked(bool clear_card) {
    maimemo_request_.fetch_add(1, std::memory_order_acq_rel);
    maimemo_client_.Reset();
    if (clear_card) {
        maimemo_has_card_ = false;
        maimemo_showing_back_ = false;
        maimemo_revealing_review_ = false;
    }
    maimemo_busy_ = false;
    maimemo_submit_waiting_for_pronunciation_ = false;
    maimemo_submit_started_ms_ = 0;
    maimemo_pending_reveal_ = false;
}

void VocabularyApp::ArmMaimemoWifiIdleTimerLocked() {
    if (wifi_idle_timer_ == nullptr) {
        return;
    }

    int64_t delay_ms = 0;
    if (settings_.study_mode == VocabularyStudyMode::kMaimemo) {
        delay_ms = kMaimemoWifiIdleMs;
    } else if (offline_pronunciation_wifi_active_) {
        delay_ms = std::max<int64_t>(1000, offline_pronunciation_wifi_deadline_ms_ - NowMs());
    } else {
        return;
    }

    (void)esp_timer_stop(wifi_idle_timer_);
    (void)esp_timer_start_once(wifi_idle_timer_, static_cast<uint64_t>(delay_ms) * 1000ULL);
}

void VocabularyApp::DisarmMaimemoWifiIdleTimerLocked() {
    if (wifi_idle_timer_ != nullptr) {
        (void)esp_timer_stop(wifi_idle_timer_);
    }
}

void VocabularyApp::HandleWifiIdleTimeout() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (settings_.study_mode != VocabularyStudyMode::kMaimemo &&
        offline_pronunciation_wifi_active_) {
        const int64_t now = NowMs();
        if (settings_open_ ||
            now < offline_pronunciation_wifi_deadline_ms_ ||
            pronunciation_busy_ || deck_sync_busy_ ||
            OtaSnapshotActive(OtaService::GetInstance().GetSnapshot())) {
            ArmMaimemoWifiIdleTimerLocked();
            return;
        }

        offline_pronunciation_wifi_active_ = false;
        offline_pronunciation_wifi_deadline_ms_ = 0;
        WifiManager::GetInstance().StopStation();
        PublishSnapshotLocked(false);
        return;
    }

    if (settings_.study_mode != VocabularyStudyMode::kMaimemo) {
        return;
    }
    if (settings_open_) {
        ArmMaimemoWifiIdleTimerLocked();
        return;
    }

    const int64_t now = NowMs();
    if (now - last_activity_ms_ < kMaimemoWifiIdleMs) {
        ArmMaimemoWifiIdleTimerLocked();
        return;
    }
    if (maimemo_busy_ || deck_sync_busy_ ||
        OtaSnapshotActive(OtaService::GetInstance().GetSnapshot())) {
        ArmMaimemoWifiIdleTimerLocked();
        return;
    }
    if (!WifiStationConnected()) {
        maimemo_wifi_suspended_ = true;
        maimemo_status_ = "WiFi 已休眠";
        PublishSnapshotLocked(false);
        return;
    }

    ResetMaimemoSessionLocked(false);
    maimemo_wifi_suspended_ = true;
    maimemo_status_ = "已空闲 30 分钟，WiFi 已关闭";
    WifiManager::GetInstance().StopStation();
    PublishSnapshotLocked(false);
}

void VocabularyApp::HandleMaimemoButtonLocked(VocabularyButton button) {
    if (button == VocabularyButton::kSettingsCombo) {
        EnterSettingsLocked();
        return;
    }

    if (maimemo_busy_ || maimemo_submit_waiting_for_pronunciation_) {
        QueuePendingMaimemoButtonLocked(button, "操作已记录，将在当前任务完成后继续");
        return;
    }

    if (button == VocabularyButton::kConfirmLongPress && maimemo_has_card_) {
        ShowActionStatusLocked("播放发音");
        PlayMaimemoPronunciationLocked();
        return;
    }

    if (!maimemo_has_card_) {
        if (button == VocabularyButton::kConfirmClick || button == VocabularyButton::kDownClick) {
            StartMaimemoFetchLocked(false);
        }
        return;
    }

    switch (button) {
        case VocabularyButton::kConfirmClick:
            if (!maimemo_showing_back_) {
                ShowActionStatusLocked("查看释义");
                maimemo_showing_back_ = true;
                maimemo_revealing_review_ = false;
                maimemo_study_started_ms_ = NowMs();
                MaybeAutoPlayCurrentPronunciationLocked();
            } else {
                ShowActionStatusLocked("认识");
                StartMaimemoSubmitLocked(MaimemoAnswer::kFamiliar);
            }
            break;
        case VocabularyButton::kConfirmDoubleClick:
            break;
        case VocabularyButton::kUpClick:
            if (maimemo_showing_back_) {
                ShowActionStatusLocked("忘记");
                StartMaimemoSubmitLocked(MaimemoAnswer::kForget);
            } else {
                ShowActionStatusLocked("请先查看释义");
            }
            break;
        case VocabularyButton::kDownClick:
            if (maimemo_showing_back_) {
                ShowActionStatusLocked("模糊");
                StartMaimemoSubmitLocked(MaimemoAnswer::kVague);
            } else {
                ShowActionStatusLocked("请先查看释义");
            }
            break;
        case VocabularyButton::kUpLongPress:
            ShowActionStatusLocked("上一个");
            StartMaimemoFetchLocked(true, true);
            break;
        case VocabularyButton::kDownLongPress:
            ShowActionStatusLocked("下一个");
            StartMaimemoFetchLocked(false, true);
            break;
        default:
            break;
    }
}

bool VocabularyApp::HandleMaimemoOfflineButtonLocked(VocabularyButton button) {
    const bool snapshot_available = maimemo_offline_state_.snapshot_day_index == today_ &&
        !maimemo_offline_state_.today_items.empty();
    if (!maimemo_has_card_ && snapshot_available) {
        auto next = std::find_if(maimemo_offline_state_.today_items.begin(),
                                 maimemo_offline_state_.today_items.end(),
                                 [](const auto& item) { return !item.is_finished; });
        if (next != maimemo_offline_state_.today_items.end()) {
            (void)LoadMaimemoOfflineItemLocked(
                static_cast<size_t>(std::distance(maimemo_offline_state_.today_items.begin(), next)),
                "当前离线，使用今日快照");
        }
    }
    if (!maimemo_has_card_) {
        return false;
    }

    auto move_snapshot = [this](int delta) {
        if (maimemo_offline_state_.today_items.empty()) {
            return false;
        }
        const size_t count = maimemo_offline_state_.today_items.size();
        const size_t next = delta < 0
            ? (maimemo_offline_state_.current_index + count - 1) % count
            : (maimemo_offline_state_.current_index + 1) % count;
        return LoadMaimemoOfflineItemLocked(next, "当前离线，浏览今日快照", true);
    };

    auto save_answer = [this](MaimemoAnswer answer) {
        constexpr size_t kMaxPendingMaimemoAnswers = 1000;
        if (maimemo_offline_state_.pending_answers.size() >= kMaxPendingMaimemoAnswers) {
            maimemo_status_ = "离线记录已满，请联网同步后继续";
            return;
        }
        if (maimemo_offline_state_.current_index < maimemo_offline_state_.today_items.size() &&
            maimemo_offline_state_.today_items[maimemo_offline_state_.current_index].is_finished) {
            maimemo_status_ = "此词已经离线记录";
            return;
        }
        const int64_t now = NowMs();
        MaimemoPendingAnswer pending;
        char operation_id[40] = {};
        snprintf(operation_id,
                 sizeof(operation_id),
                 "%08lx-%08lx",
                 static_cast<unsigned long>(esp_random()),
                 static_cast<unsigned long>(esp_random()));
        pending.operation_id = operation_id;
        pending.voc_id = maimemo_card_.voc_id;
        pending.word = maimemo_card_.word;
        pending.answer = answer;
        pending.recall_duration_ms = static_cast<uint32_t>(
            std::max<int64_t>(0, now - maimemo_recall_started_ms_));
        pending.study_duration_ms = static_cast<uint32_t>(
            std::max<int64_t>(0, now - maimemo_study_started_ms_));
        pending.day_index = today_;
        maimemo_offline_state_.pending_answers.push_back(std::move(pending));
        RecordMaimemoReviewMarkLocked(maimemo_card_, answer);
        if (maimemo_offline_state_.current_index < maimemo_offline_state_.today_items.size()) {
            maimemo_offline_state_.today_items[maimemo_offline_state_.current_index].is_finished = true;
        }
        ScheduleMaimemoPersistenceLocked(false, true);
        if (maimemo_offline_state_.current_index + 1 <
            maimemo_offline_state_.today_items.size()) {
            const size_t next = maimemo_offline_state_.current_index + 1;
            (void)LoadMaimemoOfflineItemLocked(next, nullptr);
        } else {
            maimemo_has_card_ = false;
            maimemo_showing_back_ = false;
        }
        maimemo_status_ = "离线记录待同步 " +
            std::to_string(maimemo_offline_state_.pending_answers.size()) + " 条";
    };

    switch (button) {
        case VocabularyButton::kConfirmClick:
            if (!maimemo_showing_back_) {
                ShowActionStatusLocked("查看释义");
                maimemo_showing_back_ = true;
                maimemo_revealing_review_ = false;
                maimemo_study_started_ms_ = NowMs();
                MaybeAutoPlayCurrentPronunciationLocked();
            } else {
                ShowActionStatusLocked("认识");
                save_answer(MaimemoAnswer::kFamiliar);
            }
            return true;
        case VocabularyButton::kConfirmDoubleClick:
            return true;
        case VocabularyButton::kUpLongPress:
            ShowActionStatusLocked("上一个");
            return move_snapshot(-1);
        case VocabularyButton::kDownLongPress:
            ShowActionStatusLocked("下一个");
            return move_snapshot(1);
        case VocabularyButton::kUpClick:
            if (!maimemo_showing_back_) {
                ShowActionStatusLocked("请先查看释义");
                return true;
            }
            ShowActionStatusLocked("忘记");
            save_answer(MaimemoAnswer::kForget);
            return true;
        case VocabularyButton::kDownClick:
            if (!maimemo_showing_back_) {
                ShowActionStatusLocked("请先查看释义");
                return true;
            }
            ShowActionStatusLocked("模糊");
            save_answer(MaimemoAnswer::kVague);
            return true;
        case VocabularyButton::kConfirmLongPress:
            ShowActionStatusLocked("播放发音");
            PlayMaimemoPronunciationLocked();
            return true;
        default:
            return false;
    }
}

bool VocabularyApp::LoadMaimemoOfflineItemLocked(size_t index,
                                                 const char* status,
                                                 bool reveal_review) {
    if (maimemo_offline_state_.snapshot_day_index != today_ ||
        index >= maimemo_offline_state_.today_items.size()) {
        return false;
    }
    maimemo_offline_state_.current_index = index;
    const MaimemoTodayItem& item = maimemo_offline_state_.today_items[index];
    maimemo_card_ = item.card;
    if (!item.details_cached) {
        for (size_t local_index = 0; local_index < deck_store_.Count(); ++local_index) {
            const WordCard* local = deck_store_.Get(local_index);
            if (local == nullptr || !AsciiEqualsIgnoreCase(local->word, item.card.word)) {
                continue;
            }
            maimemo_card_.phonetic = local->phonetic != nullptr ? local->phonetic : "";
            maimemo_card_.meaning = local->meaning != nullptr ? local->meaning : "";
            maimemo_card_.example = local->example != nullptr ? local->example : "";
            maimemo_card_.example_translation = local->example_translation != nullptr
                ? local->example_translation
                : "";
            break;
        }
        if (maimemo_card_.meaning.empty()) {
            maimemo_card_.meaning = "离线快照已缓存，释义需联网加载";
        }
    }
    maimemo_has_card_ = true;
    maimemo_showing_back_ = reveal_review;
    maimemo_revealing_review_ = reveal_review;
    maimemo_recall_started_ms_ = NowMs();
    maimemo_study_started_ms_ = maimemo_recall_started_ms_;
    if (status != nullptr) {
        maimemo_status_ = status;
    }
    return true;
}

const MaimemoReviewMark* VocabularyApp::FindMaimemoReviewMarkLocked(
    const MaimemoWordCard& card) const {
    auto mark = std::find_if(
        maimemo_offline_state_.review_marks.rbegin(),
        maimemo_offline_state_.review_marks.rend(),
        [this, &card](const auto& item) {
            if (item.day_index != today_) {
                return false;
            }
            return (!card.voc_id.empty() && item.voc_id == card.voc_id) ||
                AsciiEqualsIgnoreCase(item.word.c_str(), card.word);
        });
    return mark != maimemo_offline_state_.review_marks.rend() ? &*mark : nullptr;
}

void VocabularyApp::RecordMaimemoReviewMarkLocked(const MaimemoWordCard& card,
                                                   MaimemoAnswer answer) {
    auto mark = std::find_if(
        maimemo_offline_state_.review_marks.begin(),
        maimemo_offline_state_.review_marks.end(),
        [this, &card](const auto& item) {
            return item.day_index == today_ &&
                ((!card.voc_id.empty() && item.voc_id == card.voc_id) ||
                 AsciiEqualsIgnoreCase(item.word.c_str(), card.word));
        });
    if (mark == maimemo_offline_state_.review_marks.end()) {
        constexpr size_t kMaxStoredMaimemoReviewMarks = 1000;
        if (maimemo_offline_state_.review_marks.size() >= kMaxStoredMaimemoReviewMarks) {
            maimemo_offline_state_.review_marks.erase(
                maimemo_offline_state_.review_marks.begin());
        }
        MaimemoReviewMark new_mark;
        new_mark.voc_id = card.voc_id;
        new_mark.word = card.word;
        new_mark.answer = answer;
        new_mark.day_index = today_;
        maimemo_offline_state_.review_marks.push_back(std::move(new_mark));
    } else {
        mark->answer = answer;
        mark->voc_id = card.voc_id;
        mark->word = card.word;
    }
    // Review marks share the compact pending-answer journal. Do not rewrite
    // the much larger daily card snapshot after every answer.
    maimemo_pending_answers_dirty_ = true;
}

void VocabularyApp::CacheMaimemoCardLocked(const MaimemoWordCard& card) {
    if (card.voc_id.empty() || card.word.empty()) {
        return;
    }
    if (maimemo_offline_state_.snapshot_day_index != today_) {
        maimemo_offline_state_.snapshot_day_index = today_;
        maimemo_offline_state_.refreshed_at_ms = 0;
        maimemo_offline_state_.current_index = 0;
        maimemo_offline_state_.today_items.clear();
    }
    auto found = std::find_if(maimemo_offline_state_.today_items.begin(),
                              maimemo_offline_state_.today_items.end(),
                              [&card](const auto& item) {
                                  return item.card.voc_id == card.voc_id;
                              });
    if (found == maimemo_offline_state_.today_items.end()) {
        auto word_match = std::find_if(maimemo_offline_state_.today_items.begin(),
                                       maimemo_offline_state_.today_items.end(),
                                       [&card](const auto& item) {
                                           return AsciiEqualsIgnoreCase(item.card.word.c_str(),
                                                                         card.word);
                                       });
        if (word_match != maimemo_offline_state_.today_items.end() &&
            std::find_if(std::next(word_match),
                         maimemo_offline_state_.today_items.end(),
                         [&card](const auto& item) {
                             return AsciiEqualsIgnoreCase(item.card.word.c_str(), card.word);
                         }) == maimemo_offline_state_.today_items.end()) {
            found = word_match;
        }
    }
    if (found == maimemo_offline_state_.today_items.end()) {
        MaimemoTodayItem item;
        item.card = card;
        item.order = static_cast<uint16_t>(maimemo_offline_state_.today_items.size());
        item.details_cached = true;
        maimemo_offline_state_.today_items.push_back(std::move(item));
        maimemo_offline_state_.current_index = maimemo_offline_state_.today_items.size() - 1;
    } else {
        found->card = card;
        found->details_cached = true;
        maimemo_offline_state_.current_index = static_cast<size_t>(
            std::distance(maimemo_offline_state_.today_items.begin(), found));
    }
}

void VocabularyApp::MergeMaimemoTodayItemsLocked(std::vector<MaimemoTodayItem> items) {
    for (auto& item : items) {
        item.is_finished = std::any_of(maimemo_offline_state_.pending_answers.begin(),
                                       maimemo_offline_state_.pending_answers.end(),
                                       [&item](const auto& pending) {
                                           return pending.voc_id == item.card.voc_id ||
                                               AsciiEqualsIgnoreCase(pending.word.c_str(),
                                                                     item.card.word);
                                       });
        auto cached = std::find_if(maimemo_offline_state_.today_items.begin(),
                                   maimemo_offline_state_.today_items.end(),
                                   [&item](const auto& existing) {
                                       return existing.details_cached &&
                                           (existing.card.voc_id == item.card.voc_id ||
                                            AsciiEqualsIgnoreCase(existing.card.word.c_str(),
                                                                  item.card.word));
                                   });
        if (cached != maimemo_offline_state_.today_items.end()) {
            item.card = cached->card;
            item.details_cached = true;
            continue;
        }
    }
    const std::string current_voc_id = maimemo_has_card_ ? maimemo_card_.voc_id : "";
    const std::string current_word = maimemo_has_card_ ? maimemo_card_.word : "";
    maimemo_offline_state_.snapshot_day_index = today_;
    maimemo_offline_state_.refreshed_at_ms = NowMs();
    maimemo_offline_state_.today_items = std::move(items);
    maimemo_offline_state_.current_index = 0;
    if (!current_voc_id.empty()) {
        auto current = std::find_if(maimemo_offline_state_.today_items.begin(),
                                    maimemo_offline_state_.today_items.end(),
                                    [&current_voc_id, &current_word](const auto& item) {
                                        return item.card.voc_id == current_voc_id ||
                                            AsciiEqualsIgnoreCase(item.card.word.c_str(),
                                                                  current_word);
                                    });
        if (current != maimemo_offline_state_.today_items.end()) {
            maimemo_offline_state_.current_index = static_cast<size_t>(
                std::distance(maimemo_offline_state_.today_items.begin(), current));
        }
    }
}

void VocabularyApp::MoveMaimemoPendingToConflictsLocked() {
    maimemo_offline_state_.conflicts.insert(maimemo_offline_state_.conflicts.end(),
                                            maimemo_offline_state_.pending_answers.begin(),
                                            maimemo_offline_state_.pending_answers.end());
    maimemo_offline_state_.pending_answers.clear();
    constexpr size_t kMaxStoredMaimemoConflicts = 200;
    if (maimemo_offline_state_.conflicts.size() > kMaxStoredMaimemoConflicts) {
        maimemo_offline_state_.conflicts.erase(
            maimemo_offline_state_.conflicts.begin(),
            maimemo_offline_state_.conflicts.end() - kMaxStoredMaimemoConflicts);
    }
    ScheduleMaimemoPersistenceLocked(false, true);
}

bool VocabularyApp::SaveMaimemoOfflineStateLocked() {
    if (!maimemo_offline_store_.Save(maimemo_offline_state_)) {
        ESP_LOGW(kTag, "Failed to save Maimemo offline state");
        return false;
    }
    return true;
}

bool VocabularyApp::SaveMaimemoPendingAnswersLocked() {
    if (!maimemo_offline_store_.SavePendingAnswers(maimemo_offline_state_)) {
        ESP_LOGW(kTag, "Failed to save Maimemo pending answers");
        return false;
    }
    return true;
}

void VocabularyApp::ScheduleMaimemoPersistenceLocked(bool save_full_state,
                                                      bool save_pending_answers) {
    maimemo_full_state_dirty_ = maimemo_full_state_dirty_ || save_full_state;
    maimemo_pending_answers_dirty_ =
        maimemo_pending_answers_dirty_ || save_pending_answers;
    StartMaimemoPersistenceTaskLocked();
}

void VocabularyApp::StartMaimemoPersistenceTaskLocked() {
    if (maimemo_persistence_busy_ ||
        (!maimemo_full_state_dirty_ && !maimemo_pending_answers_dirty_)) {
        return;
    }

    const bool save_full_state = maimemo_full_state_dirty_;
    const bool save_pending_answers = maimemo_pending_answers_dirty_;
    maimemo_full_state_dirty_ = false;
    maimemo_pending_answers_dirty_ = false;

    MaimemoOfflineState state;
    if (save_full_state) {
        state = maimemo_offline_state_;
    } else {
        // Pending-answer persistence does not read the (potentially large)
        // daily snapshot. Keep the foreground copy bounded to the small data
        // that the worker will actually serialize.
        state.pending_answers = maimemo_offline_state_.pending_answers;
        state.conflicts = maimemo_offline_state_.conflicts;
        state.review_marks = maimemo_offline_state_.review_marks;
    }

    const uint32_t request_id =
        maimemo_persistence_request_.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto* args = new (std::nothrow) MaimemoPersistenceTaskArgs{
        .app = this,
        .request_id = request_id,
        .state = std::move(state),
        .save_full_state = save_full_state,
        .save_pending_answers = save_pending_answers,
    };
    if (args == nullptr) {
        maimemo_full_state_dirty_ = maimemo_full_state_dirty_ || save_full_state;
        maimemo_pending_answers_dirty_ =
            maimemo_pending_answers_dirty_ || save_pending_answers;
        ESP_LOGW(kTag, "Unable to allocate Maimemo persistence task");
        return;
    }

    maimemo_persistence_busy_ = true;
    const BaseType_t created = xTaskCreate(
        &VocabularyApp::MaimemoPersistenceTaskEntry,
        "maimemo_persist",
        kMaimemoPersistenceTaskStackBytes,
        args,
        3,
        nullptr);
    if (created != pdPASS) {
        delete args;
        maimemo_persistence_busy_ = false;
        maimemo_full_state_dirty_ = maimemo_full_state_dirty_ || save_full_state;
        maimemo_pending_answers_dirty_ =
            maimemo_pending_answers_dirty_ || save_pending_answers;
        ESP_LOGW(kTag, "Unable to start Maimemo persistence task");
    }
}

void VocabularyApp::RunMaimemoPersistenceTask(uint32_t request_id,
                                               MaimemoOfflineState state,
                                               bool save_full_state,
                                               bool save_pending_answers) {
    const int64_t started_ms = NowMs();
    bool full_saved = true;
    bool pending_saved = true;
    if (save_full_state) {
        full_saved = maimemo_offline_store_.Save(state);
    }
    if (save_pending_answers) {
        pending_saved = maimemo_offline_store_.SavePendingAnswers(state);
    }
    ESP_LOGI(kTag,
             "Maimemo persistence finished request=%u full_requested=%d full_ok=%d journal_requested=%d journal_ok=%d took=%lld ms",
             static_cast<unsigned>(request_id),
             save_full_state ? 1 : 0,
             full_saved ? 1 : 0,
             save_pending_answers ? 1 : 0,
             pending_saved ? 1 : 0,
             static_cast<long long>(NowMs() - started_ms));

    std::lock_guard<std::mutex> lock(mutex_);
    maimemo_persistence_busy_ = false;
    if (!full_saved) {
        maimemo_full_state_dirty_ = true;
    }
    if (!pending_saved) {
        maimemo_pending_answers_dirty_ = true;
    }
    if (full_saved && pending_saved) {
        StartMaimemoPersistenceTaskLocked();
    }
}

void VocabularyApp::QueuePendingMaimemoButtonLocked(VocabularyButton button, const char* status) {
    (void)button;
    // A press made while a network operation is active refers to an unknown
    // future card. Replaying it later caused automatic flips/submits.
    maimemo_pending_buttons_.clear();
    if (status != nullptr && status[0] != '\0') {
        maimemo_status_ = status;
    }
}

void VocabularyApp::ReplayPendingMaimemoButtonLocked() {
    maimemo_pending_buttons_.clear();
}

void VocabularyApp::StartMaimemoFetchLocked(bool back, bool reveal_review) {
    if (maimemo_busy_) {
        return;
    }
    maimemo_pending_reveal_ = reveal_review;
    StartMaimemoFetchAvoidingLocked(back, "", "");
}

void VocabularyApp::StartMaimemoFetchAvoidingLocked(bool back,
                                                   std::string avoid_voc_id,
                                                   std::string avoid_word) {
    if (maimemo_busy_) {
        return;
    }

    // WebStudy responses are latency-sensitive.  Do not leave the station in
    // modem-sleep while a user is waiting for the next card; it is restored
    // to LOW_POWER as soon as this task finishes.
    WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::PERFORMANCE);
    maimemo_busy_ = true;
    const bool had_card = maimemo_has_card_;
    if (!had_card) {
        maimemo_showing_back_ = false;
    }
    maimemo_status_ = had_card ? "正在同步单词..." : "正在加载墨墨单词...";
    const MaimemoAuthConfig auth = MaimemoAuthStore::GetInstance().Load();
    const bool refresh_snapshot = auth.open_api_configured &&
        (maimemo_offline_state_.snapshot_day_index != today_ ||
         maimemo_offline_state_.refreshed_at_ms == 0);
    const uint32_t request_id = maimemo_request_.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto* args = new (std::nothrow) MaimemoTaskArgs{
        .app = this,
        .request_id = request_id,
        .action = MaimemoTaskAction::kFetch,
        .voc_id = std::move(avoid_voc_id),
        .word = std::move(avoid_word),
        .back = back,
        .refresh_snapshot = refresh_snapshot,
    };
    if (args == nullptr) {
        maimemo_busy_ = false;
        maimemo_status_ = "墨墨任务内存不足";
        return;
    }

    BaseType_t created = xTaskCreate(
        &VocabularyApp::MaimemoTaskEntry,
        "maimemo_study",
        kMaimemoTaskStackBytes,
        args,
        4,
        nullptr);
    if (created != pdPASS) {
        delete args;
        maimemo_busy_ = false;
        maimemo_status_ = "墨墨任务启动失败";
    }
}

void VocabularyApp::StartMaimemoSubmitLocked(MaimemoAnswer answer) {
    if (maimemo_busy_ || maimemo_submit_waiting_for_pronunciation_ || !maimemo_has_card_) {
        return;
    }
    if (pronunciation_busy_) {
        if (!pronunciation_play_audio_) {
            // Every freshly fetched card starts a background pronunciation
            // preload. Treating that preload as active playback made fast
            // review answers wait for an unrelated HTTP/cache operation, so
            // the button badge changed while submission appeared to do
            // nothing. Cancel preloading and let WebStudy start immediately.
            pronunciation_cancel_requested_.store(true, std::memory_order_release);
            ESP_LOGI(kTag, "Maimemo submit bypasses active pronunciation preload");
        } else {
            // The active pronunciation task owns the audio decoder and a sizeable
            // network stack. Tell it to stop, show the sync state immediately,
            // then start WebStudy only after that task has released its resources.
            maimemo_submit_waiting_for_pronunciation_ = true;
            maimemo_pending_submit_answer_ = answer;
            maimemo_submit_started_ms_ = NowMs();
            maimemo_status_ = "正在同步...";
            pronunciation_cancel_requested_.store(true, std::memory_order_release);
            Application::GetInstance().StopSound();
            ESP_LOGI(kTag, "Maimemo submit requested; cancelling active pronunciation first");
            return;
        }
    }
    constexpr size_t kMaxPendingMaimemoAnswers = 1000;
    if (maimemo_offline_state_.pending_answers.size() >= kMaxPendingMaimemoAnswers) {
        maimemo_status_ = "待同步记录已满，请保持联网完成同步";
        return;
    }
    const auto existing_pending = std::find_if(
        maimemo_offline_state_.pending_answers.begin(),
        maimemo_offline_state_.pending_answers.end(),
        [this](const auto& pending) {
            return pending.voc_id == maimemo_card_.voc_id ||
                AsciiEqualsIgnoreCase(pending.word.c_str(), maimemo_card_.word);
        });
    if (existing_pending != maimemo_offline_state_.pending_answers.end()) {
        maimemo_status_ = "此词已有待同步答案";
        if (WifiStationConnected()) {
            StartMaimemoPendingSyncLocked();
        }
        return;
    }

    const int64_t now = NowMs();
    const uint32_t recall_duration_ms = static_cast<uint32_t>(
        std::max<int64_t>(0, now - maimemo_recall_started_ms_));
    const uint32_t study_duration_ms = static_cast<uint32_t>(
        std::max<int64_t>(0, now - maimemo_study_started_ms_));

    MaimemoPendingAnswer pending;
    char operation_id[40] = {};
    snprintf(operation_id,
             sizeof(operation_id),
             "%08lx-%08lx",
             static_cast<unsigned long>(esp_random()),
             static_cast<unsigned long>(esp_random()));
    pending.operation_id = operation_id;
    pending.voc_id = maimemo_card_.voc_id;
    pending.word = maimemo_card_.word;
    pending.answer = answer;
    pending.recall_duration_ms = recall_duration_ms;
    pending.study_duration_ms = study_duration_ms;
    pending.day_index = today_;
    maimemo_offline_state_.pending_answers.push_back(pending);
    RecordMaimemoReviewMarkLocked(maimemo_card_, answer);
    auto snapshot_item = std::find_if(maimemo_offline_state_.today_items.begin(),
                                      maimemo_offline_state_.today_items.end(),
                                      [&pending](const auto& item) {
                                          return item.card.voc_id == pending.voc_id ||
                                              AsciiEqualsIgnoreCase(item.card.word.c_str(),
                                                                    pending.word);
                                      });
    if (snapshot_item != maimemo_offline_state_.today_items.end()) {
        snapshot_item->is_finished = true;
    }
    // Keep the answer in RAM while the foreground WebStudy request is active.
    // Starting a SPIFFS write here used to make flash GC compete with the
    // latency-sensitive WebSocket round trip. Every terminal task path below
    // persists immediately when the submit is not acknowledged; successful
    // submits only need to clear an older on-disk pending file.
    maimemo_pending_answers_dirty_ = true;

    // Keep the WebSocket responsive only during the actual submit.  The
    // completion path switches back to LOW_POWER, so the 30-minute idle
    // policy does not keep the radio in its high-power state.
    WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::PERFORMANCE);
    maimemo_busy_ = true;
    maimemo_submit_started_ms_ = NowMs();
    maimemo_pending_reveal_ = false;
    maimemo_status_.clear();
    maimemo_status_ = "正在提交...";
    const MaimemoAuthConfig auth = MaimemoAuthStore::GetInstance().Load();
    const uint32_t request_id = maimemo_request_.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto* args = new (std::nothrow) MaimemoTaskArgs{
        .app = this,
        .request_id = request_id,
        .action = MaimemoTaskAction::kSubmit,
        .answer = answer,
        .voc_id = maimemo_card_.voc_id,
        .word = maimemo_card_.word,
        .recall_duration_ms = recall_duration_ms,
        .study_duration_ms = study_duration_ms,
        .operation_id = pending.operation_id,
        .refresh_snapshot = auth.open_api_configured &&
            (maimemo_offline_state_.snapshot_day_index != today_ ||
             maimemo_offline_state_.refreshed_at_ms == 0),
    };
    if (args == nullptr) {
        maimemo_busy_ = false;
        maimemo_submit_started_ms_ = 0;
        maimemo_status_ = "墨墨任务内存不足";
        StartMaimemoPersistenceTaskLocked();
        return;
    }

    BaseType_t created = xTaskCreate(
        &VocabularyApp::MaimemoTaskEntry,
        "maimemo_submit",
        kMaimemoTaskStackBytes,
        args,
        4,
        nullptr);
    if (created != pdPASS) {
        delete args;
        maimemo_busy_ = false;
        maimemo_submit_started_ms_ = 0;
        maimemo_status_ = "墨墨任务启动失败";
        StartMaimemoPersistenceTaskLocked();
    }
}

void VocabularyApp::StartMaimemoPendingSyncLocked() {
    if (maimemo_busy_ || maimemo_offline_state_.pending_answers.empty() ||
        !WifiStationConnected()) {
        return;
    }
    const MaimemoPendingAnswer& pending = maimemo_offline_state_.pending_answers.front();
    const MaimemoAuthConfig auth = MaimemoAuthStore::GetInstance().Load();
    WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::PERFORMANCE);
    maimemo_busy_ = true;
    maimemo_pending_reveal_ = false;
    maimemo_status_ = "正在核对离线记录 " +
        std::to_string(maimemo_offline_state_.pending_answers.size()) + " 条";
    const uint32_t request_id = maimemo_request_.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto* args = new (std::nothrow) MaimemoTaskArgs{
        .app = this,
        .request_id = request_id,
        .action = MaimemoTaskAction::kSyncPending,
        .answer = pending.answer,
        .voc_id = pending.voc_id,
        .word = pending.word,
        .recall_duration_ms = pending.recall_duration_ms,
        .study_duration_ms = pending.study_duration_ms,
        .operation_id = pending.operation_id,
        .refresh_snapshot = auth.open_api_configured &&
            (maimemo_offline_state_.snapshot_day_index != today_ ||
             maimemo_offline_state_.refreshed_at_ms == 0),
    };
    if (args == nullptr) {
        maimemo_busy_ = false;
        maimemo_status_ = "离线同步内存不足";
        return;
    }
    if (xTaskCreate(&VocabularyApp::MaimemoTaskEntry,
                    "maimemo_sync",
                    kMaimemoTaskStackBytes,
                    args,
                    4,
                    nullptr) != pdPASS) {
        delete args;
        maimemo_busy_ = false;
        maimemo_status_ = "离线同步任务启动失败";
    }
}

bool VocabularyApp::IsCurrentMaimemoRequest(uint32_t request_id) const {
    return maimemo_request_.load(std::memory_order_acquire) == request_id;
}

void VocabularyApp::MaimemoTaskEntry(void* arg) {
    std::unique_ptr<MaimemoTaskArgs> args(static_cast<MaimemoTaskArgs*>(arg));
    if (args && args->app != nullptr) {
        args->app->RunMaimemoTask(args->request_id,
                                  static_cast<uint8_t>(args->action),
                                  args->answer,
                                  std::move(args->voc_id),
                                  std::move(args->word),
                                  args->recall_duration_ms,
                                  args->study_duration_ms,
                                  args->back,
                                  std::move(args->operation_id),
                                  args->refresh_snapshot);
    }
    vTaskDelete(nullptr);
}

void VocabularyApp::MaimemoPersistenceTaskEntry(void* arg) {
    std::unique_ptr<MaimemoPersistenceTaskArgs> args(
        static_cast<MaimemoPersistenceTaskArgs*>(arg));
    if (args && args->app != nullptr) {
        args->app->RunMaimemoPersistenceTask(args->request_id,
                                             std::move(args->state),
                                             args->save_full_state,
                                             args->save_pending_answers);
    }
    vTaskDelete(nullptr);
}

void VocabularyApp::RunMaimemoTask(uint32_t request_id,
                                   uint8_t action,
                                   MaimemoAnswer answer,
                                   std::string voc_id,
                                   std::string word,
                                   uint32_t recall_duration_ms,
                                   uint32_t study_duration_ms,
                                   bool back,
                                   std::string operation_id,
                                   bool refresh_snapshot) {
    const int64_t task_started_ms = NowMs();
    MaimemoWordCard card;
    bool ok = false;
    bool submitted = false;
    const auto task_action = static_cast<MaimemoTaskAction>(action);
    if (task_action == MaimemoTaskAction::kSyncPending) {
        // A new WebStudy session must fetch its current word before accepting
        // an answer. Reconcile persisted work before attempting the replay.
        MaimemoWordCard current_card;
        ok = maimemo_client_.GetWord(&current_card, false);
        if (ok) {
            const bool same_card = current_card.voc_id == voc_id ||
                AsciiEqualsIgnoreCase(current_card.word.c_str(), word);
            if (same_card) {
                ok = maimemo_client_.SubmitResponse(current_card.voc_id,
                                                    current_card.word,
                                                    answer,
                                                    recall_duration_ms,
                                                    study_duration_ms,
                                                    &card,
                                                    &submitted);
            } else {
                // The server has already advanced. Archive this one uncertain
                // answer, but immediately expose the current server card.
                card = std::move(current_card);
            }
        }
    } else if (task_action == MaimemoTaskAction::kSubmit) {
        ok = maimemo_client_.SubmitResponse(voc_id,
                                            word,
                                            answer,
                                            recall_duration_ms,
                                            study_duration_ms,
                                            &card,
                                            &submitted);
    } else if (!voc_id.empty() || !word.empty()) {
        ok = maimemo_client_.GetWordAvoiding(&card, back, voc_id, word);
    } else {
        ok = maimemo_client_.GetWord(&card, back);
    }
    const int64_t network_finished_ms = NowMs();
    const std::string status = maimemo_client_.LastStatus();
    std::vector<MaimemoTodayItem> today_items;
    std::string snapshot_error;
    bool snapshot_ok = false;
    if (refresh_snapshot) {
        const MaimemoAuthConfig auth = MaimemoAuthStore::GetInstance().Load();
        snapshot_ok = maimemo_open_api_client_.FetchTodayItems(auth.open_api_token,
                                                                &today_items,
                                                                &snapshot_error);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!IsCurrentMaimemoRequest(request_id)) {
        return;
    }

    const bool reveal_after_fetch =
        !IsMaimemoSubmitAction(task_action) && maimemo_pending_reveal_;
    maimemo_busy_ = false;
    maimemo_submit_waiting_for_pronunciation_ = false;
    maimemo_submit_started_ms_ = 0;
    // Maimemo operations temporarily use PERFORMANCE to avoid Wi-Fi modem
    // sleep adding seconds to a WebSocket round trip.  Return to LOW_POWER
    // after every terminal task path; idle shutdown remains governed by the
    // existing 30-minute timer.
    if (settings_.study_mode == VocabularyStudyMode::kMaimemo) {
        WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::LOW_POWER);
    }
    maimemo_pending_reveal_ = false;
    maimemo_status_ = status;
    bool persist_full_state = snapshot_ok;
    bool persist_pending_answers = false;
    if (snapshot_ok) {
        MergeMaimemoTodayItemsLocked(std::move(today_items));
    } else if (refresh_snapshot && !snapshot_error.empty()) {
        ESP_LOGW(kTag, "Maimemo snapshot refresh failed: %s", snapshot_error.c_str());
    }
    if (IsMaimemoSubmitAction(task_action) && !operation_id.empty()) {
        auto pending = std::find_if(maimemo_offline_state_.pending_answers.begin(),
                                    maimemo_offline_state_.pending_answers.end(),
                                    [&operation_id](const auto& item) {
                                        return item.operation_id == operation_id;
                                    });
        if (submitted && pending != maimemo_offline_state_.pending_answers.end()) {
            maimemo_offline_state_.pending_answers.erase(pending);
            persist_pending_answers = true;
        } else if (ok && !submitted &&
                   pending != maimemo_offline_state_.pending_answers.end()) {
            maimemo_offline_state_.conflicts.push_back(std::move(*pending));
            maimemo_offline_state_.pending_answers.erase(pending);
            constexpr size_t kMaxStoredMaimemoConflicts = 100;
            if (maimemo_offline_state_.conflicts.size() > kMaxStoredMaimemoConflicts) {
                maimemo_offline_state_.conflicts.erase(
                    maimemo_offline_state_.conflicts.begin(),
                    maimemo_offline_state_.conflicts.end() - kMaxStoredMaimemoConflicts);
            }
            persist_pending_answers = true;
            maimemo_status_ = "服务端进度已推进，旧答案已归档";
        }
    }
    if (!ok && submitted) {
        maimemo_has_card_ = false;
        maimemo_showing_back_ = false;
        maimemo_status_ = status == kMaimemoDailyTargetCompleteStatus
            ? status
            : "答案已同步，正在重新加载";
        if (WifiStationConnected()) {
            StartMaimemoFetchLocked(false);
        }
        PublishSnapshotLocked(false);
        ScheduleMaimemoPersistenceLocked(persist_full_state, persist_pending_answers);
        ESP_LOGI(kTag,
                 "Maimemo switch published request=%u action=%u network=%lld ms total=%lld ms fallback_fetch=1",
                 static_cast<unsigned>(request_id),
                 static_cast<unsigned>(action),
                 static_cast<long long>(network_finished_ms - task_started_ms),
                 static_cast<long long>(NowMs() - task_started_ms));
        return;
    }
    if (!ok && IsMaimemoSubmitAction(task_action) && !submitted) {
        maimemo_status_ = "提交未确认，答案已保留待同步";
        ArmMaimemoWifiIdleTimerLocked();
        PublishSnapshotLocked(false);
        ScheduleMaimemoPersistenceLocked(persist_full_state, persist_pending_answers);
        return;
    }
    if (!ok && status == kMaimemoDailyTargetCompleteStatus) {
        maimemo_has_card_ = false;
        maimemo_showing_back_ = false;
    }
    if (ok) {
        if (!IsMaimemoSubmitAction(task_action) || submitted) {
            maimemo_status_.clear();
        }
        maimemo_card_ = std::move(card);
        maimemo_has_card_ = true;
        CacheMaimemoCardLocked(maimemo_card_);
        maimemo_showing_back_ = reveal_after_fetch;
        maimemo_revealing_review_ = reveal_after_fetch;
        maimemo_recall_started_ms_ = NowMs();
        maimemo_study_started_ms_ = maimemo_recall_started_ms_;
        if (maimemo_showing_back_) {
            MaybeAutoPlayCurrentPronunciationLocked();
        } else {
            PreloadCurrentPronunciationLocked();
        }
        if (ai_overlay_active_) {
            UpdateAiWordContextLocked();
            AiDialogService::GetInstance().RefreshLearningContextConnection();
        }
    }
    ArmMaimemoWifiIdleTimerLocked();
    if (!maimemo_offline_state_.pending_answers.empty() && WifiStationConnected()) {
        StartMaimemoPendingSyncLocked();
    } else {
        ReplayPendingMaimemoButtonLocked();
    }
    PublishSnapshotLocked(false);
    // The next card is now queued for the e-paper refresh. Snapshot and answer
    // files are committed afterwards by a lower-priority coalescing worker.
    ScheduleMaimemoPersistenceLocked(persist_full_state, persist_pending_answers);
    // Avoid a synchronous UART write immediately after the Wi-Fi/PM transition.
    // On ESP32-S3 this path can run while the MSPI clock-switch critical section
    // is active and otherwise trip the interrupt watchdog.
    ESP_LOGI(kTag,
             "Maimemo switch published request=%u action=%u network=%lld ms total=%lld ms auto_audio=%d",
             static_cast<unsigned>(request_id),
             static_cast<unsigned>(action),
             static_cast<long long>(network_finished_ms - task_started_ms),
             static_cast<long long>(NowMs() - task_started_ms),
             settings_.auto_pronunciation ? 1 : 0);
}

void VocabularyApp::WriteNfcEntryLink() {
    EggaNfc* nfc = EggaGetNfc();
    if (nfc == nullptr) {
        ESP_LOGW(kTag, "NFC unavailable for entry link");
        return;
    }
    if (!nfc->IsPowered() && !nfc->PowerOn()) {
        ESP_LOGW(kTag, "NFC power on failed");
        return;
    }
    const esp_err_t ret = nfc->WriteUriNdef(kNfcEntryUrl);
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "NFC entry write failed: %s", esp_err_to_name(ret));
    }
}

void VocabularyApp::PlayCurrentPronunciationLocked() {
    const size_t word_index = CurrentWordIndexLocked();
    if (word_index >= deck_store_.Count()) {
        return;
    }

    const WordCard* card = deck_store_.Get(word_index);
    if (card == nullptr || card->word == nullptr) {
        return;
    }

    ESP_LOGI(kTag, "Pronunciation requested for %s", card->word);

    const std::string next_word = NextOfflinePronunciationWordLocked();
    StartPronunciationTaskLocked(card->deck_id != nullptr ? card->deck_id : "",
                                 card->word,
                                 YoudaoVoiceType(settings_.voice_type),
                                 "",
                                 next_word);
}

void VocabularyApp::PreloadCurrentPronunciationLocked() {
    if (!settings_.auto_pronunciation || settings_open_) {
        return;
    }
    if (settings_.study_mode == VocabularyStudyMode::kMaimemo) {
        if (!maimemo_has_card_ || maimemo_card_.word.empty()) {
            return;
        }
        // A Maimemo response is already handled by a 12 KB network task.
        // Starting another 12 KB pronunciation prefetch task here creates a
        // short but dangerous heap/stack peak on ESP32-S3. Pronunciation is
        // still fetched and played when the user reveals the card or long
        // presses it; only the invisible speculative prefetch is skipped.
        return;
    }

    const size_t word_index = CurrentWordIndexLocked();
    if (word_index >= deck_store_.Count()) {
        return;
    }
    const WordCard* card = deck_store_.Get(word_index);
    if (card == nullptr || card->word == nullptr) {
        return;
    }
    StartTemporaryPronunciationWifiLocked();
    StartPronunciationTaskLocked(card->deck_id != nullptr ? card->deck_id : "",
                                 card->word,
                                 YoudaoVoiceType(settings_.voice_type),
                                 "",
                                 "",
                                 "",
                                 false);
}

std::string VocabularyApp::MaimemoPronunciationUrlLocked() const {
    std::string pronunciation_url = settings_.voice_type == VocabularyVoiceType::kAmerican
        ? maimemo_card_.pronunciation_url_us
        : maimemo_card_.pronunciation_url_uk;
    if (pronunciation_url.empty()) {
        pronunciation_url = settings_.voice_type == VocabularyVoiceType::kAmerican
            ? maimemo_card_.pronunciation_url_uk
            : maimemo_card_.pronunciation_url_us;
    }
    return pronunciation_url;
}

void VocabularyApp::PlayMaimemoPronunciationLocked() {
    if (!maimemo_has_card_ || maimemo_card_.word.empty()) {
        return;
    }
    if (!WifiStationConnected() &&
        PlayLocalPronunciation(settings_.active_deck_id, maimemo_card_.word, false)) {
        return;
    }
    StartPronunciationTaskLocked("",
                                 maimemo_card_.word,
                                 YoudaoVoiceType(settings_.voice_type),
                                 MaimemoPronunciationUrlLocked());
}

std::string VocabularyApp::NextOfflinePronunciationWordLocked() const {
    const size_t word_count = deck_store_.Count();
    if (word_count <= 1) {
        return {};
    }

    size_t next_index = CurrentWordIndexLocked();
    if (!queue_.empty()) {
        auto it = std::find(queue_.begin(), queue_.end(), current_word_index_);
        size_t pos = it != queue_.end()
            ? static_cast<size_t>(std::distance(queue_.begin(), it))
            : queue_pos_;
        if (pos >= queue_.size()) {
            pos = 0;
        }
        next_index = queue_[(pos + 1) % queue_.size()];
    } else {
        next_index = (CurrentWordIndexLocked() + 1) % word_count;
    }

    if (next_index == CurrentWordIndexLocked()) {
        return {};
    }
    const WordCard* next_card = deck_store_.Get(next_index);
    if (next_card == nullptr || next_card->word == nullptr) {
        return {};
    }
    return next_card->word;
}

void VocabularyApp::MaybeAutoPlayCurrentPronunciationLocked() {
    if (!settings_.auto_pronunciation || settings_open_) {
        return;
    }
    if (settings_.study_mode == VocabularyStudyMode::kMaimemo) {
        if (!maimemo_busy_ && maimemo_has_card_ && maimemo_showing_back_) {
            PlayMaimemoPronunciationLocked();
        }
        return;
    }
    if (deck_store_.Count() == 0) {
        return;
    }
    if (settings_.study_mode != VocabularyStudyMode::kBrowse && !showing_back_) {
        return;
    }
    StartTemporaryPronunciationWifiLocked();
    PlayCurrentPronunciationLocked();
}

void VocabularyApp::StartPronunciationTaskLocked(const std::string& deck_id,
                                                 const std::string& word,
                                                 int voice_type,
                                                 const std::string& pronunciation_url,
                                                 const std::string& prefetch_word,
                                                 const std::string& prefetch_url,
                                                 bool play_audio) {
    if (pronunciation_busy_) {
        // Never create a second 12 KB pronunciation task while the first one
        // still owns the decoder/network buffers.  A Maimemo submit can cancel
        // it explicitly and will resume from RunPronunciationTask::finish().
        ESP_LOGI(kTag, "Pronunciation request ignored while another task is active");
        return;
    }
    pronunciation_cancel_requested_.store(false, std::memory_order_release);
    const uint32_t request_id = pronunciation_request_.fetch_add(1, std::memory_order_acq_rel) + 1;
    pronunciation_busy_ = true;
    pronunciation_play_audio_ = play_audio;
    auto* args = new (std::nothrow) PronunciationTaskArgs{
        .app = this,
        .request_id = request_id,
        .voice_type = voice_type,
        .deck_id = deck_id,
        .word = word,
        .pronunciation_url = pronunciation_url,
        .prefetch_word = prefetch_word,
        .prefetch_url = prefetch_url,
        .play_audio = play_audio,
    };
    if (args == nullptr) {
        pronunciation_busy_ = false;
        pronunciation_play_audio_ = false;
        ESP_LOGW(kTag, "Unable to allocate pronunciation task");
        if (play_audio) {
            PlayLocalPronunciation(deck_id, word);
        }
        return;
    }

    BaseType_t created = xTaskCreate(
        &VocabularyApp::PronunciationTaskEntry,
        "pronunciation",
        kPronunciationTaskStackBytes,
        args,
        4,
        nullptr);
    if (created != pdPASS) {
        ESP_LOGW(kTag, "Unable to start pronunciation task");
        delete args;
        pronunciation_busy_ = false;
        pronunciation_play_audio_ = false;
        if (play_audio) {
            PlayLocalPronunciation(deck_id, word);
        }
    }
}

void VocabularyApp::PronunciationTaskEntry(void* arg) {
    std::unique_ptr<PronunciationTaskArgs> args(static_cast<PronunciationTaskArgs*>(arg));
    if (args && args->app != nullptr) {
        args->app->RunPronunciationTask(args->request_id,
                                        args->voice_type,
                                        std::move(args->deck_id),
                                        std::move(args->word),
                                        std::move(args->pronunciation_url),
                                        std::move(args->prefetch_word),
                                        std::move(args->prefetch_url),
                                        args->play_audio);
    }
    vTaskDelete(nullptr);
}

bool VocabularyApp::IsCurrentPronunciationRequest(uint32_t request_id) const {
    return pronunciation_request_.load(std::memory_order_acquire) == request_id;
}

void VocabularyApp::RunPronunciationTask(uint32_t request_id,
                                         int voice_type,
                                         std::string deck_id,
                                         std::string word,
                                         std::string pronunciation_url,
                                         std::string prefetch_word,
                                         std::string prefetch_url,
                                         bool play_audio) {
    auto finish = [this, request_id]() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!IsCurrentPronunciationRequest(request_id)) {
            return;
        }
        pronunciation_busy_ = false;
        pronunciation_play_audio_ = false;
        pronunciation_cancel_requested_.store(false, std::memory_order_release);
        if (settings_.study_mode != VocabularyStudyMode::kMaimemo && offline_pronunciation_wifi_active_) {
            WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::LOW_POWER);
            ArmMaimemoWifiIdleTimerLocked();
        }
        if (maimemo_submit_waiting_for_pronunciation_) {
            const MaimemoAnswer pending_answer = maimemo_pending_submit_answer_;
            ESP_LOGI(kTag,
                     "Maimemo pronunciation released submit after %lld ms",
                     static_cast<long long>(
                         maimemo_submit_started_ms_ > 0
                             ? NowMs() - maimemo_submit_started_ms_
                             : 0));
            maimemo_submit_waiting_for_pronunciation_ = false;
            StartMaimemoSubmitLocked(pending_answer);
        }
    };

    if (!IsCurrentPronunciationRequest(request_id)) {
        finish();
        return;
    }

    if (play_audio && PlayLocalPronunciation(deck_id, word, false)) {
        ESP_LOGI(kTag, "Played local pronunciation for %s", word.c_str());
        finish();
        return;
    }

    std::string online_error;
    bool online_played = false;
    if (!play_audio && !WaitForOnlineWifiReady()) {
        ESP_LOGW(kTag, "Pronunciation preload skipped for %s: wifi not connected", word.c_str());
        finish();
        return;
    }
    if (WaitForOnlineWifiReady()) {
        auto should_continue = [this, request_id]() {
            return IsCurrentPronunciationRequest(request_id) &&
                !pronunciation_cancel_requested_.load(std::memory_order_acquire);
        };
        if (!play_audio) {
            const bool prefetched_current = !pronunciation_url.empty()
                ? OnlinePronunciationService::PrefetchUrl(pronunciation_url, should_continue, &online_error)
                : OnlinePronunciationService::PrefetchWord(word,
                                                           voice_type,
                                                           should_continue,
                                                           &online_error);
            if (!prefetched_current && IsCurrentPronunciationRequest(request_id)) {
                ESP_LOGW(kTag, "Pronunciation preload unavailable for %s: %s",
                         word.c_str(),
                         online_error.empty() ? "unknown error" : online_error.c_str());
            }
            finish();
            return;
        }
        if (!pronunciation_url.empty()) {
            online_played = OnlinePronunciationService::FetchUrlAndPlay(
                pronunciation_url,
                should_continue,
                &online_error);
            if (!online_played) {
                ESP_LOGW(kTag, "Direct pronunciation unavailable for %s: %s",
                         word.c_str(),
                         online_error.empty() ? "unknown error" : online_error.c_str());
            }
        }
        if (!online_played) {
            online_played = OnlinePronunciationService::FetchAndPlay(
                word,
                voice_type,
                should_continue,
                &online_error);
        }
    } else {
        online_error = "wifi not connected";
    }
    if (online_played) {
        ESP_LOGI(kTag, "Played online pronunciation for %s", word.c_str());
        if (!prefetch_url.empty() || !prefetch_word.empty()) {
            std::string prefetch_error;
            auto should_continue = [this, request_id]() {
                return IsCurrentPronunciationRequest(request_id) &&
                    !pronunciation_cancel_requested_.load(std::memory_order_acquire);
            };
            const bool prefetched = !prefetch_url.empty()
                ? OnlinePronunciationService::PrefetchUrl(prefetch_url, should_continue, &prefetch_error)
                : OnlinePronunciationService::PrefetchWord(prefetch_word,
                                                           voice_type,
                                                           should_continue,
                                                           &prefetch_error);
            if (!prefetched && IsCurrentPronunciationRequest(request_id)) {
                ESP_LOGW(kTag, "Pronunciation prefetch unavailable: %s",
                         prefetch_error.empty() ? "unknown error" : prefetch_error.c_str());
            }
        }
        finish();
        return;
    }

    if (!IsCurrentPronunciationRequest(request_id) ||
        pronunciation_cancel_requested_.load(std::memory_order_acquire)) {
        ESP_LOGI(kTag, "Pronunciation request for %s was cancelled", word.c_str());
        finish();
        return;
    }

    ESP_LOGW(kTag, "Online pronunciation unavailable for %s: %s",
             word.c_str(),
             online_error.empty() ? "unknown error" : online_error.c_str());
    PlayLocalPronunciation(deck_id, word);
    finish();
}

bool VocabularyApp::PlayLocalPronunciation(const std::string& deck_id,
                                           const std::string& word,
                                           bool play_missing_cue) {
    const std::string word_name = SanitizeAssetName(word.c_str());
    if (word_name.empty()) {
        ESP_LOGW(kTag, "Cannot build pronunciation path for current word");
        if (play_missing_cue) {
            PlayMissingPronunciationCueLocked();
        }
        return false;
    }

    struct PronunciationCandidate {
        std::string path;
        bool wav = false;
    };

    std::vector<PronunciationCandidate> candidates;
    const std::string deck_name = SanitizeAssetName(deck_id.c_str());
    if (!deck_name.empty()) {
        const std::string deck_path = "/audio/decks/" + deck_name + "/" + word_name;
        candidates.push_back({deck_path + ".ogg", false});
        candidates.push_back({deck_path + ".wav", true});
    }
    const std::string global_path = "/audio/words/" + word_name;
    candidates.push_back({global_path + ".ogg", false});
    candidates.push_back({global_path + ".wav", true});

    AssetsStore& assets = AssetsStore::GetInstance();
    std::string audio;
    for (const PronunciationCandidate& candidate : candidates) {
        if (!assets.ReadFile(candidate.path, &audio, kMaxPronunciationBytes)) {
            continue;
        }

        ESP_LOGI(kTag, "Play pronunciation: %s", candidate.path.c_str());
        Application::GetInstance().StopSound();
        if (candidate.wav) {
            if (PlayWavPronunciationLocked(audio)) {
                return true;
            }
            ESP_LOGW(kTag, "Unsupported wav pronunciation: %s", candidate.path.c_str());
            continue;
        }
        Application::GetInstance().PlaySound(std::string_view(audio.data(), audio.size()));
        return true;
    }

    ESP_LOGW(kTag, "Pronunciation not found for %s", word.c_str());
    if (play_missing_cue) {
        PlayMissingPronunciationCueLocked();
    }
    return false;
}

bool VocabularyApp::PlayWavPronunciationLocked(const std::string& wav) {
    if (wav.size() < 44) {
        return false;
    }

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(wav.data());
    const size_t size = wav.size();
    if (!ChunkIdEquals(bytes, "RIFF") || !ChunkIdEquals(bytes + 8, "WAVE")) {
        return false;
    }

    uint16_t audio_format = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;
    const uint8_t* pcm_data = nullptr;
    size_t pcm_bytes = 0;

    size_t offset = 12;
    while (offset + 8 <= size) {
        const uint8_t* chunk = bytes + offset;
        const uint32_t chunk_size = ReadLe32(chunk + 4);
        const size_t data_offset = offset + 8;
        if (data_offset + chunk_size > size) {
            return false;
        }

        if (ChunkIdEquals(chunk, "fmt ")) {
            if (chunk_size < 16) {
                return false;
            }
            audio_format = ReadLe16(bytes + data_offset);
            channels = ReadLe16(bytes + data_offset + 2);
            sample_rate = ReadLe32(bytes + data_offset + 4);
            bits_per_sample = ReadLe16(bytes + data_offset + 14);
        } else if (ChunkIdEquals(chunk, "data")) {
            pcm_data = bytes + data_offset;
            pcm_bytes = chunk_size;
        }

        offset = data_offset + chunk_size + (chunk_size & 1);
    }

    if (audio_format != 1 || channels == 0 || channels > 2 ||
        sample_rate == 0 || bits_per_sample != 16 ||
        pcm_data == nullptr || pcm_bytes < channels * sizeof(int16_t)) {
        return false;
    }

    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return false;
    }

    const int output_rate = codec->output_sample_rate() > 0 ? codec->output_sample_rate() : 16000;
    const size_t input_frames = pcm_bytes / (channels * sizeof(int16_t));
    const size_t output_frames = std::max<size_t>(1, input_frames * output_rate / sample_rate);
    std::vector<int16_t> pcm(output_frames);

    for (size_t i = 0; i < output_frames; ++i) {
        const size_t src_frame = std::min(
            input_frames - 1,
            i * static_cast<size_t>(sample_rate) / static_cast<size_t>(output_rate));
        const int16_t left = static_cast<int16_t>(
            pcm_data[src_frame * channels * 2] |
            (pcm_data[src_frame * channels * 2 + 1] << 8));
        if (channels == 1) {
            pcm[i] = left;
        } else {
            const int16_t right = static_cast<int16_t>(
                pcm_data[src_frame * channels * 2 + 2] |
                (pcm_data[src_frame * channels * 2 + 3] << 8));
            pcm[i] = static_cast<int16_t>((static_cast<int>(left) + static_cast<int>(right)) / 2);
        }
    }

    const bool output_was_enabled = codec->output_enabled();
    if (!output_was_enabled) {
        codec->EnableOutput(true);
    }
    codec->OutputData(pcm);
    if (!output_was_enabled) {
        codec->EnableOutput(false);
    }
    return true;
}

void VocabularyApp::PlayMissingPronunciationCueLocked() {
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return;
    }

    const int sample_rate = codec->output_sample_rate() > 0 ? codec->output_sample_rate() : 16000;
    struct Tone {
        int frequency;
        int duration_ms;
        int amplitude;
    };
    constexpr Tone kTones[] = {
        {880, 45, 2400},
        {0, 25, 0},
        {660, 65, 2200},
    };

    size_t total_samples = 0;
    for (const Tone& tone : kTones) {
        total_samples += static_cast<size_t>(sample_rate * tone.duration_ms / 1000);
    }

    std::vector<int16_t> pcm;
    pcm.reserve(total_samples);
    for (const Tone& tone : kTones) {
        const int samples = sample_rate * tone.duration_ms / 1000;
        const int half_period = tone.frequency > 0
            ? std::max(1, sample_rate / (tone.frequency * 2))
            : 1;
        for (int i = 0; i < samples; ++i) {
            if (tone.frequency <= 0) {
                pcm.push_back(0);
                continue;
            }
            const bool high = ((i / half_period) % 2) == 0;
            pcm.push_back(static_cast<int16_t>(high ? tone.amplitude : -tone.amplitude));
        }
    }

    const bool output_was_enabled = codec->output_enabled();
    if (!output_was_enabled) {
        codec->EnableOutput(true);
    }
    codec->OutputData(pcm);
    if (!output_was_enabled) {
        codec->EnableOutput(false);
    }
}

void VocabularyApp::PlayCueLocked(bool known, bool complete) {
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        return;
    }

    const int sample_rate = codec->output_sample_rate() > 0 ? codec->output_sample_rate() : 16000;
    const int duration_ms = complete ? 180 : (known ? 70 : 120);
    const int frequency = complete ? 1040 : (known ? 880 : 220);
    const int samples = sample_rate * duration_ms / 1000;
    const int half_period = std::max(1, sample_rate / (frequency * 2));

    std::vector<int16_t> pcm(samples);
    for (int i = 0; i < samples; ++i) {
        const bool high = ((i / half_period) % 2) == 0;
        const int amplitude = complete ? 5000 : (known ? 3800 : 3200);
        pcm[i] = static_cast<int16_t>(high ? amplitude : -amplitude);
    }

    const bool output_was_enabled = codec->output_enabled();
    if (!output_was_enabled) {
        codec->EnableOutput(true);
    }
    codec->OutputData(pcm);
    if (!output_was_enabled) {
        codec->EnableOutput(false);
    }
}
