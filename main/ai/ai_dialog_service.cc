#include "ai_dialog_service.h"

#include "application.h"
#include "assets_store.h"
#include "audio/audio_codec.h"
#include "audio/audio_service.h"
#include "boards/common/board.h"
#include "settings.h"
#include "system_info.h"
#include "wifi_manager.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <memory>
#include <new>
#include <set>
#include <utility>

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_timer.h>

namespace {
constexpr char kTag[] = "AiDialog";
constexpr char kAiPurchaseRequiredMessage[] =
    "AI 教师未开通，请联系开发者获取权限。小红书号：4449420200";
constexpr char kNamespace[] = "ai_dialog";
constexpr char kEntitlementVerifiedKey[] = "entitled";
constexpr char kDefaultUrl[] = "wss://xrobo-io.qiniuapi.com/v1/ws/";
constexpr char kQiniuOtaUrl[] = "https://xrobo.qiniuapi.com/v1/ota/";
constexpr int kMinDailyLimit = 1;
constexpr int kMaxDailyLimit = 100;
constexpr uint32_t kSendTaskStack = 6144;
constexpr size_t kMaxPttPrebufferPackets = 250;  // 15 seconds at 60 ms/packet.

void PlayPttReadyBeep(AudioService* audio_service) {
    if (audio_service == nullptr) return;
    // Play a short two-note chime and wait for it to drain so the cue does not
    // leak into the first microphone packet.
    const bool played = audio_service->PlayPromptChime();
    ESP_LOGI(kTag, "PTT ready cue %s", played ? "played" : "timed out");
}

int32_t CurrentUsageDay() {
    const std::time_t now = std::time(nullptr);
    // Before SNTP/RTC synchronization, keep the quota for this boot only.
    if (now < 1704067200) {
        return -1;
    }
    return static_cast<int32_t>(now / 86400);
}

const char* JsonString(cJSON* object, const char* key) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : nullptr;
}

int JsonInt(cJSON* object, const char* key, int fallback) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

std::string JsonEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const unsigned char ch : value) {
        switch (ch) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (ch >= 0x20) escaped.push_back(static_cast<char>(ch));
                break;
        }
    }
    return escaped;
}

bool DecodeUtf8(const std::string& text, size_t offset, uint32_t* codepoint, size_t* length) {
    if (offset >= text.size()) return false;
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80) {
        *codepoint = first;
        *length = 1;
        return true;
    }
    size_t count = 0;
    uint32_t value = 0;
    if ((first & 0xE0) == 0xC0) { count = 2; value = first & 0x1F; }
    else if ((first & 0xF0) == 0xE0) { count = 3; value = first & 0x0F; }
    else if ((first & 0xF8) == 0xF0) { count = 4; value = first & 0x07; }
    else return false;
    if (offset + count > text.size()) return false;
    for (size_t i = 1; i < count; ++i) {
        const auto next = static_cast<unsigned char>(text[offset + i]);
        if ((next & 0xC0) != 0x80) return false;
        value = (value << 6) | (next & 0x3F);
    }
    *codepoint = value;
    *length = count;
    return true;
}

bool IsCjk(uint32_t codepoint) {
    return (codepoint >= 0x3400 && codepoint <= 0x4DBF) ||
           (codepoint >= 0x4E00 && codepoint <= 0x9FFF) ||
           (codepoint >= 0xF900 && codepoint <= 0xFAFF);
}

std::string ExtractRequestedHanzi(const std::string& question) {
    struct CharacterAt {
        std::string value;
        uint32_t codepoint = 0;
        size_t offset = 0;
    };
    std::vector<CharacterAt> characters;
    for (size_t offset = 0; offset < question.size();) {
        uint32_t cp = 0;
        size_t len = 0;
        if (!DecodeUtf8(question, offset, &cp, &len)) {
            ++offset;
            continue;
        }
        if (IsCjk(cp)) characters.push_back({question.substr(offset, len), cp, offset});
        offset += len;
    }

    // Only create a learning card when the user explicitly names a character
    // next to a reading/writing/meaning intent. A generic mention of “字” is not
    // enough: otherwise “现在学的这个字叫什么” and “AI 名字叫什么” are
    // incorrectly parsed as requests for “现” and “名”.
    static const char* kIntentPhrases[] = {
        "怎么写", "如何写", "怎样写", "怎么读", "如何读", "怎样读",
        "怎么念", "如何念", "怎样念", "是什么意思", "什么意思", "是什么字"
    };
    static const std::set<std::string> kTargetSeparators = {
        "的", "是", "字", "个", "这", "那", "请", "问", "我", "想", "要", "把", "将"
    };
    size_t intent_offset = std::string::npos;
    for (const char* phrase : kIntentPhrases) {
        const size_t found = question.find(phrase);
        if (found != std::string::npos &&
            (intent_offset == std::string::npos || found < intent_offset)) {
            intent_offset = found;
        }
    }
    if (intent_offset == std::string::npos) return {};

    const size_t left_quote = question.find("“");
    if (left_quote != std::string::npos && left_quote < intent_offset) {
        const size_t start = left_quote + std::strlen("“");
        uint32_t cp = 0;
        size_t len = 0;
        if (DecodeUtf8(question, start, &cp, &len) && IsCjk(cp) &&
            question.compare(start + len, std::strlen("”"), "”") == 0) {
            return question.substr(start, len);
        }
    }

    // The named target must be the final CJK character before the intent, with
    // an optional “字” suffix. Do not scan farther back into ordinary sentence
    // text because that turns referential questions into unrelated cards.
    size_t candidate_index = characters.size();
    while (candidate_index > 0 && characters[candidate_index - 1].offset >= intent_offset) {
        --candidate_index;
    }
    if (candidate_index == 0) return {};
    --candidate_index;
    if (characters[candidate_index].value == "字") {
        if (candidate_index == 0) return {};
        --candidate_index;
    }
    const std::string& candidate = characters[candidate_index].value;
    return kTargetSeparators.find(candidate) == kTargetSeparators.end() ? candidate : std::string();
}
bool RefersToCurrentHanzi(const std::string& question) {
    static const char* kReferences[] = {
        "这个字", "这字", "它", "当前这个字", "现在这个字",
        "屏幕上的字", "左边这个字"
    };
    static const char* kLearningIntents[] = {
        "怎么读", "如何读", "怎样读", "怎么念", "如何念", "怎样念",
        "怎么写", "如何写", "怎样写", "是什么意思", "什么意思",
        "是什么字", "叫什么"
    };
    bool has_reference = false;
    for (const char* phrase : kReferences) {
        if (question.find(phrase) != std::string::npos) {
            has_reference = true;
            break;
        }
    }
    if (!has_reference) return false;
    for (const char* phrase : kLearningIntents) {
        if (question.find(phrase) != std::string::npos) return true;
    }
    return false;
}

AiNavigationAction DetectNavigationAction(const std::string& transcript) {
    std::string command;
    command.reserve(transcript.size());
    static const std::string kIgnored = " \t\r\n，。！？、,.!?~～";
    for (size_t offset = 0; offset < transcript.size();) {
        uint32_t codepoint = 0;
        size_t length = 0;
        if (!DecodeUtf8(transcript, offset, &codepoint, &length)) {
            ++offset;
            continue;
        }
        const std::string token = transcript.substr(offset, length);
        if (kIgnored.find(token) == std::string::npos) command += token;
        offset += length;
    }

    static const std::set<std::string> kNextCommands = {
        "下一个字", "下个字", "下一个单词", "下个单词", "下一个", "下一张", "下一张卡片", "下一页",
        "换下一个字", "换下一个单词", "换下一个", "换一个字", "换一个单词", "请下一个字", "请换下一个字",
        "切换到下一个字", "切换到下一个单词", "切换到下一张卡片",
        "帮我切换到下一个字", "帮我切换到下一个单词", "帮我切换到下一张卡片",
        "小墨下一个", "小墨下一个单词", "小墨下一张"
    };
    static const std::set<std::string> kPreviousCommands = {
        "上一个字", "上个字", "上一个单词", "上个单词", "上一个", "上一张", "上一张卡片", "上一页",
        "换上一个字", "换上一个单词", "换上一个", "请上一个字",
        "切换到上一个字", "切换到上一个单词", "切换到上一张卡片",
        "帮我切换到上一个字", "帮我切换到上一个单词", "帮我切换到上一张卡片",
        "小墨上一个", "小墨上一个单词", "小墨上一张"
    };
    if (kNextCommands.find(command) != kNextCommands.end()) {
        return AiNavigationAction::kNextCard;
    }
    if (kPreviousCommands.find(command) != kPreviousCommands.end()) {
        return AiNavigationAction::kPreviousCard;
    }
    return AiNavigationAction::kNone;
}

bool HasMeaningfulTranscript(const char* transcript) {
    if (transcript == nullptr || transcript[0] == '\0') return false;
    const std::string text(transcript);
    static const std::string kIgnored = " \t\r\n，。！？、,.!?~～…;；:：\"'“”‘’";
    for (size_t offset = 0; offset < text.size();) {
        uint32_t codepoint = 0;
        size_t length = 0;
        if (!DecodeUtf8(text, offset, &codepoint, &length)) {
            ++offset;
            continue;
        }
        const std::string token = text.substr(offset, length);
        if (kIgnored.find(token) == std::string::npos) return true;
        offset += length;
    }
    return false;
}

void VerifyHanziRequestParser() {
    static const std::pair<const char*, const char*> kCases[] = {
        {"天地的地怎么写", "地"},
        {"地字怎么写", "地"},
        {"“地”怎么写", "地"},
        {"请问这个地怎么读", "地"},
        {"天地的地是什么意思", "地"},
        {"“地”这个字怎么写", "地"},
        {"现在学的这个字叫什么", ""},
        {"AI 名字叫什么", ""},
        {"你叫什么名字", ""},
        {"这个字怎么读", ""},
        {"足球的足怎么写", "足"},
        {"足球的足字怎么写", "足"},
        {"香蕉的蕉怎么写啊", "蕉"},
    };
    for (const auto& [question, expected] : kCases) {
        const std::string actual = ExtractRequestedHanzi(question);
        if (actual != expected) {
            ESP_LOGE(kTag, "Hanzi parser self-test failed question=%s expected=%s actual=%s",
                     question, expected, actual.c_str());
            return;
        }
    }
    static const std::pair<const char*, bool> kCurrentCases[] = {
        {"这个字怎么读", true},
        {"它怎么念", true},
        {"当前这个字怎么写", true},
        {"屏幕上的字是什么意思", true},
        {"AI 名字叫什么", false},
        {"今天天气怎么样", false},
    };
    for (const auto& [question, expected] : kCurrentCases) {
        const bool actual = RefersToCurrentHanzi(question);
        if (actual != expected) {
            ESP_LOGE(kTag, "Current Hanzi parser self-test failed question=%s expected=%d actual=%d",
                     question, expected, actual);
            return;
        }
    }
    static const std::pair<const char*, AiNavigationAction> kNavigationCases[] = {
        {"下一个字", AiNavigationAction::kNextCard},
        {"下一张。", AiNavigationAction::kNextCard},
        {"请换下一个字！", AiNavigationAction::kNextCard},
        {"帮我切换到下一个单词", AiNavigationAction::kNextCard},
        {"上一个字", AiNavigationAction::kPreviousCard},
        {"小墨，上一张", AiNavigationAction::kPreviousCard},
        {"下一个字怎么读", AiNavigationAction::kNone},
        {"讲一下下一个字", AiNavigationAction::kNone},
    };
    for (const auto& [transcript, expected] : kNavigationCases) {
        const AiNavigationAction actual = DetectNavigationAction(transcript);
        if (actual != expected) {
            ESP_LOGE(kTag, "Navigation parser self-test failed transcript=%s expected=%u actual=%u",
                     transcript, static_cast<unsigned>(expected), static_cast<unsigned>(actual));
            return;
        }
    }
    ESP_LOGI(kTag, "AI parser self-test passed cases=%u",
              static_cast<unsigned>(sizeof(kCases) / sizeof(kCases[0]) +
                                    sizeof(kCurrentCases) / sizeof(kCurrentCases[0]) +
                                    sizeof(kNavigationCases) / sizeof(kNavigationCases[0])));
}

bool ValidWebSocketUrl(const std::string& value) {
    return value.rfind("wss://", 0) == 0 || value.rfind("ws://", 0) == 0;
}

uint16_t ReadBe16(const uint8_t* data) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

uint32_t ReadBe32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

void WriteBe16(uint8_t* data, uint16_t value) {
    data[0] = static_cast<uint8_t>(value >> 8);
    data[1] = static_cast<uint8_t>(value);
}

void WriteBe32(uint8_t* data, uint32_t value) {
    data[0] = static_cast<uint8_t>(value >> 24);
    data[1] = static_cast<uint8_t>(value >> 16);
    data[2] = static_cast<uint8_t>(value >> 8);
    data[3] = static_cast<uint8_t>(value);
}

bool PostJson(const std::string& url, const std::string& body,
              const std::string& mac, const std::string& client_id,
              std::string* response, int* status) {
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 15000;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) return false;
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Activation-Version", "1");
    esp_http_client_set_header(client, "User-Agent", "shiziji/0.5.1");
    esp_http_client_set_header(client, "Accept-Language", "zh-CN");
    esp_http_client_set_header(client, "Device-Id", mac.c_str());
    esp_http_client_set_header(client, "Client-Id", client_id.c_str());
    esp_err_t result = esp_http_client_open(client, static_cast<int>(body.size()));
    if (result == ESP_OK) {
        const int written = esp_http_client_write(client, body.data(), static_cast<int>(body.size()));
        if (written != static_cast<int>(body.size())) result = ESP_FAIL;
    }
    if (result == ESP_OK && esp_http_client_fetch_headers(client) < 0) result = ESP_FAIL;
    *status = esp_http_client_get_status_code(client);
    response->clear();
    char buffer[1024];
    int read = 0;
    while (result == ESP_OK && (read = esp_http_client_read(client, buffer, sizeof(buffer))) > 0) {
        if (response->size() + static_cast<size_t>(read) > 32768) break;
        response->append(buffer, static_cast<size_t>(read));
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result == ESP_OK && !response->empty();
}
}  // namespace

AiDialogService& AiDialogService::GetInstance() {
    static AiDialogService instance;
    return instance;
}

AiDialogConfig AiDialogService::LoadConfig() {
    Settings settings(kNamespace);
    AiDialogConfig config;
    config.enabled = settings.GetBool("enabled", false);
    config.entitlement_verified = settings.GetBool(kEntitlementVerifiedKey, false);
    config.provider = settings.GetString("provider", "qiniu_xrobot");
    config.url = settings.GetString("url", kDefaultUrl);
    config.token = settings.GetString("token");
    config.agent_id = settings.GetString("agent_id");
    config.daily_limit = std::clamp(static_cast<int>(settings.GetInt("daily_limit", 10)),
                                    kMinDailyLimit, kMaxDailyLimit);
    return config;
}

bool AiDialogService::SaveConfig(const AiDialogConfig& config, std::string* error) {
    if (config.provider != "qiniu_xrobot" && !ValidWebSocketUrl(config.url)) {
        if (error != nullptr) *error = "url must start with ws:// or wss://";
        return false;
    }
    if (config.daily_limit < kMinDailyLimit || config.daily_limit > kMaxDailyLimit) {
        if (error != nullptr) *error = "daily_limit must be between 1 and 100";
        return false;
    }
    if (config.provider.size() > 32 || config.url.size() > 255 ||
        config.token.size() > 1024 || config.agent_id.size() > 128) {
        if (error != nullptr) *error = "AI configuration value is too long";
        return false;
    }
    Settings settings(kNamespace, true);
    // This namespace only contains AI configuration. Clear stale copies before
    // rewriting so repeated provisioning does not consume scarce NVS entries.
    settings.EraseAll();
    settings.SetBool("enabled", config.enabled);
    settings.SetBool(kEntitlementVerifiedKey, config.entitlement_verified);
    settings.SetString("provider", config.provider.empty() ? "custom" : config.provider);
    settings.SetString("url", config.url);
    settings.SetString("token", config.token);
    settings.SetString("agent_id", config.agent_id);
    settings.SetInt("daily_limit", config.daily_limit);
    return true;
}

void AiDialogService::ResetUsage() {
    Settings settings(kNamespace, true);
    settings.SetInt("usage_day", CurrentUsageDay());
    settings.SetInt("usage_count", 0);
}

void AiDialogService::Initialize(AudioService* audio_service, std::function<void()> on_changed) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return;
    audio_service_ = audio_service;
    on_changed_ = std::move(on_changed);
    config_ = LoadConfig();
    snapshot_.provider = config_.provider;
    snapshot_.daily_limit = config_.daily_limit;
    snapshot_.configured = config_.enabled && config_.entitlement_verified &&
        (config_.provider == "qiniu_xrobot" ||
         (!config_.token.empty() && ValidWebSocketUrl(config_.url)));
    snapshot_.state = snapshot_.configured ? AiDialogState::kReady : AiDialogState::kDisabled;
    snapshot_.message = snapshot_.configured ? "长按确认键，听到提示音后说话" : "请先在设置中开启 AI 背词教师";
    RefreshUsageLocked();
    initialized_ = true;
    VerifyHanziRequestParser();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        if (send_task_ != nullptr) xTaskNotifyGive(send_task_);
    };
    callbacks.on_playback_frame = [this]() {
        HandlePlaybackFrame();
    };
    audio_service_->SetCallbacks(callbacks);
    xTaskCreate(&AiDialogService::SendTaskEntry, "ai_audio_send", kSendTaskStack,
                this, 4, &send_task_);
}

void AiDialogService::SetLearningContext(const AiLearningContext& context) {
    std::lock_guard<std::mutex> lock(mutex_);
    learning_context_ = context;
}

void AiDialogService::RefreshLearningContextConnection() {
    bool should_start = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!context_refresh_running_ && snapshot_.configured) {
            context_refresh_running_ = true;
            snapshot_.state = AiDialogState::kConnecting;
            snapshot_.message = "正在同步新卡片给 AI";
            should_start = true;
        }
    }
    if (!should_start) return;
    NotifyChanged();
    if (xTaskCreate(&AiDialogService::ContextRefreshTaskEntry, "ai_ctx_refresh",
                    12288, this, 4, nullptr) != pdPASS) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            context_refresh_running_ = false;
        }
        SetState(AiDialogState::kError, "无法同步新卡片，请重试");
    }
}

void AiDialogService::ReloadConfig() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = LoadConfig();
        snapshot_.provider = config_.provider;
        snapshot_.daily_limit = config_.daily_limit;
        snapshot_.configured = config_.enabled && config_.entitlement_verified &&
            (config_.provider == "qiniu_xrobot" ||
             (!config_.token.empty() && ValidWebSocketUrl(config_.url)));
        RefreshUsageLocked();
        const bool busy = snapshot_.state == AiDialogState::kConnecting ||
            snapshot_.state == AiDialogState::kListening ||
            snapshot_.state == AiDialogState::kThinking ||
            snapshot_.state == AiDialogState::kSpeaking;
        if (!busy) {
            snapshot_.state = snapshot_.configured ? AiDialogState::kReady : AiDialogState::kDisabled;
            snapshot_.message = snapshot_.configured ? "长按确认键，听到提示音后说话" :
                                                       "请先在设置中开启 AI 背词教师";
        }
    }
    NotifyChanged();
}

bool AiDialogService::StartTurn() {
    bool open_connection = false;
    bool can_start = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = LoadConfig();
        snapshot_.provider = config_.provider;
        snapshot_.daily_limit = config_.daily_limit;
        snapshot_.configured = config_.enabled && config_.entitlement_verified &&
            (config_.provider == "qiniu_xrobot" ||
            (!config_.token.empty() && ValidWebSocketUrl(config_.url)));
        RefreshUsageLocked();
        if (!snapshot_.configured) {
            snapshot_.state = AiDialogState::kDisabled;
            snapshot_.message = "请先在设置中开启 AI 背词教师";
        } else if (config_.provider != "qiniu_xrobot" &&
                   snapshot_.used_today >= snapshot_.daily_limit) {
            snapshot_.state = AiDialogState::kError;
            snapshot_.message = "今天的 AI 对话次数已用完";
        } else if (close_pending_) {
            snapshot_.state = AiDialogState::kReady;
            snapshot_.message = "正在结束上一轮，请稍候";
        } else if (snapshot_.state == AiDialogState::kListening ||
                   snapshot_.state == AiDialogState::kConnecting ||
                   snapshot_.state == AiDialogState::kThinking ||
                   snapshot_.state == AiDialogState::kSpeaking) {
            return false;
        } else {
            snapshot_.question.clear();
            snapshot_.answer.clear();
            pending_tts_text_.clear();
            tts_audio_packets_ = 0;
            tts_played_packets_ = 0;
            tts_stream_active_ = false;
            snapshot_.display_type = AiLearningDisplayType::kText;
            snapshot_.display_character.clear();
            snapshot_.display_pinyin.clear();
            snapshot_.display_caption.clear();
            // Do not let provider-generated idle/farewell speech reach the
            // speaker. Cloud TTS is enabled only after a real STT transcript.
            suppress_cloud_tts_ = true;
            // Authorization and WebSocket setup are deliberately silent in the
            // UI. PTT capture has already started and buffers audio meanwhile.
            snapshot_.message.clear();
            snapshot_.state = AiDialogState::kConnecting;
            turn_counted_ = false;
            open_connection = true;
            can_start = true;
        }
    }
    NotifyChanged();
    if (!can_start) return false;
    idle_close_generation_.fetch_add(1, std::memory_order_acq_rel);
    return open_connection ? OpenConnection() : BeginListening();
}

void AiDialogService::SetPttButtonHeld(bool held) {
    ptt_button_held_.store(held, std::memory_order_release);
    if (!held) {
        bool ptt_active = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ptt_active = ptt_mode_;
            if (ptt_active) ptt_release_pending_ = true;
        }
        if (ptt_active && audio_service_ != nullptr) {
            audio_service_->EnableVoiceProcessing(false);
        }
    }
}

bool AiDialogService::StartPtt() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = LoadConfig();
        if (!config_.enabled || !config_.entitlement_verified) {
            snapshot_.configured = false;
            snapshot_.state = AiDialogState::kDisabled;
            snapshot_.message = "请先在设置中开启 AI 背词教师";
            return false;
        }
        if (ptt_mode_ || close_pending_ ||
            snapshot_.state == AiDialogState::kConnecting ||
            snapshot_.state == AiDialogState::kListening ||
            snapshot_.state == AiDialogState::kThinking ||
            snapshot_.state == AiDialogState::kSpeaking) {
            return false;
        }
        ptt_mode_ = true;
        ptt_release_pending_ = false;
        ptt_prebuffer_.clear();
        snapshot_.message.clear();
    }
    idle_close_generation_.fetch_add(1, std::memory_order_acq_rel);
    PlayPttReadyBeep(audio_service_);
    if (audio_service_ != nullptr) {
        audio_service_->MarkPttStart(esp_timer_get_time() / 1000);
        audio_service_->EnableVoiceProcessing(true);
    }
    NotifyChanged();
    if (xTaskCreate(&AiDialogService::PttConnectTaskEntry, "ai_ptt_connect",
                    16384, this, 4, nullptr) != pdPASS) {
        if (audio_service_ != nullptr) audio_service_->EnableVoiceProcessing(false);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ptt_mode_ = false;
            snapshot_.state = AiDialogState::kError;
            snapshot_.message = "无法启动 AI 连接任务";
        }
        NotifyChanged();
        return false;
    }
    return true;
}

void AiDialogService::FinishPtt() {
    ptt_button_held_.store(false, std::memory_order_release);
    bool should_submit = false;
    std::string session;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ptt_mode_) return;
        ptt_release_pending_ = true;
        if (snapshot_.state == AiDialogState::kListening && connected_) {
            session = session_id_;
            should_submit = true;
        } else if (snapshot_.state == AiDialogState::kConnecting ||
                   snapshot_.state == AiDialogState::kReady) {
            snapshot_.message.clear();
        }
    }
    if (!should_submit) return;

    if (audio_service_ != nullptr) {
        audio_service_->EnableVoiceProcessing(false);
        // Let the encoder and WebSocket sender flush the last microphone frame
        // before the manual listen/stop control message.
        vTaskDelay(pdMS_TO_TICKS(120));
        for (int attempt = 0; attempt < 100; ++attempt) {
            bool prebuffer_empty = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                prebuffer_empty = ptt_prebuffer_.empty();
            }
            if (audio_service_->GetSendQueueSize() == 0 && prebuffer_empty) break;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    // Arm the STT deadline before listen/stop so an unusually fast server
    // response cannot race ahead of the timeout generation.
    ScheduleTranscriptTimeout();
    SendJson("{\"session_id\":\"" + session +
             "\",\"type\":\"listen\",\"state\":\"stop\",\"mode\":\"manual\"}");
    ESP_LOGI(kTag, "PTT recording submitted");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshot_.state == AiDialogState::kListening) {
            snapshot_.state = AiDialogState::kThinking;
            snapshot_.message = "已发送，AI 正在理解";
        }
    }
    NotifyChanged();
}

void AiDialogService::CancelTurn() {
    idle_close_generation_.fetch_add(1, std::memory_order_acq_rel);
    turn_timeout_generation_.fetch_add(1, std::memory_order_acq_rel);
    SendJson("{\"type\":\"abort\",\"reason\":\"user_cancelled\"}");
    if (audio_service_ != nullptr) {
        audio_service_->EnableVoiceProcessing(false);
        audio_service_->ResetDecoder();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tts_stream_active_ = false;
        pending_tts_text_.clear();
        ptt_mode_ = false;
        ptt_release_pending_ = false;
        ptt_prebuffer_.clear();
        ptt_button_held_.store(false, std::memory_order_release);
    }
    ScheduleClose();
    SetState(AiDialogState::kReady, "已取消，请短按确认返回学习");
}

void AiDialogService::Stop() {
    idle_close_generation_.fetch_add(1, std::memory_order_acq_rel);
    turn_timeout_generation_.fetch_add(1, std::memory_order_acq_rel);
    if (audio_service_ != nullptr) {
        audio_service_->EnableVoiceProcessing(false);
        audio_service_->ResetDecoder();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tts_stream_active_ = false;
        pending_tts_text_.clear();
        ptt_mode_ = false;
        ptt_release_pending_ = false;
        ptt_prebuffer_.clear();
        ptt_button_held_.store(false, std::memory_order_release);
    }
    if (!ScheduleClose()) {
        CloseClient();
    }
    SetState(AiDialogState::kReady, "长按确认键，听到提示音后说话");
}

bool AiDialogService::ScheduleClose() {
    bool should_start = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!close_pending_ && client_ != nullptr) {
            close_pending_ = true;
            should_start = true;
        }
    }
    if (!should_start) return false;
    if (xTaskCreate(&AiDialogService::CloseConnectionTaskEntry, "ai_ws_close",
                    4096, this, 3, nullptr) != pdPASS) {
        std::lock_guard<std::mutex> lock(mutex_);
        close_pending_ = false;
        return false;
    }
    return true;
}

bool AiDialogService::IsBusy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_.state == AiDialogState::kConnecting ||
           snapshot_.state == AiDialogState::kListening ||
           snapshot_.state == AiDialogState::kThinking ||
           snapshot_.state == AiDialogState::kSpeaking;
}

AiDialogSnapshot AiDialogService::GetSnapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

void AiDialogService::ResolveNavigationRequest(uint32_t request_id,
                                               const std::string& response) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (request_id == 0 || snapshot_.navigation_request_id != request_id) {
            return;
        }
        snapshot_.state = AiDialogState::kReady;
        snapshot_.answer = response;
        snapshot_.message = response;
    }
    NotifyChanged();
}

bool AiDialogService::OpenConnection(bool reserve_turn) {
    std::lock_guard<std::mutex> connection_lock(connection_mutex_);
    bool already_connected = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        already_connected = connected_ && client_ != nullptr && !session_id_.empty();
    }
    if (already_connected) {
        ESP_LOGI(kTag, "Reusing direct Qiniu WebSocket");
        return reserve_turn ? BeginListening() : true;
    }
    std::string websocket_url = config_.url;
    std::string token = config_.token;
    std::string credential_error;
    if (config_.provider == "qiniu_xrobot" &&
        !AcquireCloudCredentials(&websocket_url, &token, &credential_error, reserve_turn)) {
        SetState(AiDialogState::kError,
                 credential_error.empty() ? "AI 授权失败" : credential_error);
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        already_connected = connected_ && client_ != nullptr && !session_id_.empty();
    }
    if (already_connected) {
        ESP_LOGI(kTag, "Reusing warm Qiniu WebSocket reserve_turn=%d", reserve_turn ? 1 : 0);
        return reserve_turn ? BeginListening() : true;
    }
    CloseClient();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        preparing_connection_ = !reserve_turn;
        headers_ = "Authorization: Bearer " + token + "\r\n";
        headers_ += "Protocol-Version: " + std::to_string(protocol_version_) + "\r\n";
        headers_ += "Device-Id: " + SystemInfo::GetMacAddress() + "\r\n";
        headers_ += "Client-Id: " + Board::GetInstance().GetUuid() + "\r\n";
        // Qiniu binds a device to an agent in its control plane. Agent-ID is
        // only forwarded for custom gateways that explicitly support it.
        if (config_.provider != "qiniu_xrobot" && !config_.agent_id.empty()) {
            headers_ += "X-Agent-ID: " + config_.agent_id + "\r\n";
        }

        esp_websocket_client_config_t ws_config = {};
        ws_config.uri = websocket_url.c_str();
        ws_config.headers = headers_.c_str();
        ws_config.user_context = this;
        ws_config.disable_auto_reconnect = true;
        ws_config.task_stack = 8192;
        ws_config.buffer_size = 2048;
        ws_config.network_timeout_ms = 10000;
        ws_config.ping_interval_sec = 10;
        ws_config.crt_bundle_attach = esp_crt_bundle_attach;
        client_ = esp_websocket_client_init(&ws_config);
        if (client_ == nullptr) {
            snapshot_.state = AiDialogState::kError;
            snapshot_.message = "无法创建 AI 连接";
        } else {
            esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY,
                                          &AiDialogService::WebSocketEventEntry, this);
        }
    }
    if (client_ == nullptr) {
        NotifyChanged();
        return false;
    }
    if (esp_websocket_client_start(client_) != ESP_OK) {
        CloseClient();
        SetState(AiDialogState::kError, "AI 连接启动失败");
        return false;
    }
    if (!reserve_turn) {
        const int64_t deadline = esp_timer_get_time() + 12LL * 1000 * 1000;
        while (esp_timer_get_time() < deadline) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (connected_ && !session_id_.empty()) return true;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        CloseClient();
        return false;
    }
    return true;
}

bool AiDialogService::AcquireCloudCredentials(std::string* websocket_url,
                                               std::string* token,
                                               std::string* error,
                                               bool reserve_turn,
                                               int* http_status) {
    (void)reserve_turn;
    Board::GetInstance().RequestNetwork();
    // AI setup is dominated by TLS/WebSocket round trips. Disable modem sleep
    // while the AI overlay owns the connection, then restore it on close.
    WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::PERFORMANCE);
    for (int attempt = 0; attempt < 60 && !WifiManager::GetInstance().IsConnected(); ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (!WifiManager::GetInstance().IsConnected()) {
        *error = "Wi-Fi 连接超时";
        return false;
    }
    const std::string mac = SystemInfo::GetMacAddress();
    const std::string client_id = Board::GetInstance().GetUuid();
    const esp_app_desc_t* app = esp_app_get_description();
    const std::string version = app != nullptr ? app->version : "unknown";
    std::string response;
    int status = 0;
    const std::string ota_body = "{\"version\":0,\"uuid\":\"" + client_id +
        "\",\"application\":{\"name\":\"egga_word_machine\",\"version\":\"" + version +
        "\",\"elf_sha256\":\"\"},\"board\":{\"type\":\"egga-s3-epaper-4.2\",\"name\":\"egga-word-machine\"},"
        "\"mac_address\":\"" + mac + "\",\"chip_model_name\":\"esp32s3\"}";
    const bool posted = PostJson(kQiniuOtaUrl, ota_body, mac, client_id, &response, &status);
    if (http_status != nullptr) {
        *http_status = status;
    }
    if (!posted ||
        status != 200) {
        if (status == 429) {
            *error = "AI 额度已用完，学习功能不受影响";
        } else if (status >= 400 && status < 500) {
            *error = kAiPurchaseRequiredMessage;
        } else {
            *error = "AI 服务暂时不可用，请稍后重试";
        }
        return false;
    }
    cJSON* root = cJSON_ParseWithLength(response.data(), response.size());
    cJSON* websocket = root != nullptr ? cJSON_GetObjectItemCaseSensitive(root, "websocket") : nullptr;
    const char* url_value = cJSON_IsObject(websocket) ? JsonString(websocket, "url") : nullptr;
    const char* token_value = cJSON_IsObject(websocket) ? JsonString(websocket, "token") : nullptr;
    const int protocol_version = cJSON_IsObject(websocket) ? JsonInt(websocket, "version", 1) : 1;
    if (token_value == nullptr && root != nullptr) token_value = JsonString(root, "token");
    if (url_value == nullptr || token_value == nullptr || !ValidWebSocketUrl(url_value)) {
        *error = kAiPurchaseRequiredMessage;
        if (root != nullptr) cJSON_Delete(root);
        return false;
    }
    *websocket_url = url_value;
    *token = token_value;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        protocol_version_ = std::clamp(protocol_version, 1, 3);
    }
    ESP_LOGI(kTag, "Qiniu credentials ready protocol_version=%d", protocol_version_);
    cJSON_Delete(root);
    return true;
}

AiEntitlementStatus AiDialogService::CheckEntitlement(std::string* message) {
    std::string websocket_url;
    std::string token;
    std::string error;
    int http_status = 0;
    if (AcquireCloudCredentials(&websocket_url, &token, &error, false, &http_status)) {
        if (message != nullptr) {
            *message = "AI 服务已开通，可以使用";
        }
        return AiEntitlementStatus::kAvailable;
    }
    if (http_status == 429) {
        if (message != nullptr) {
            *message = "AI 已开通，今日额度已用完";
        }
        return AiEntitlementStatus::kQuotaExceeded;
    }
    if (http_status >= 400 && http_status < 500) {
        if (message != nullptr) {
            *message = kAiPurchaseRequiredMessage;
        }
        return AiEntitlementStatus::kNotEntitled;
    }
    if (error == kAiPurchaseRequiredMessage) {
        if (message != nullptr) {
            *message = kAiPurchaseRequiredMessage;
        }
        return AiEntitlementStatus::kNotEntitled;
    }
    if (message != nullptr) {
        *message = error.empty() ? "检查失败，请确认 Wi-Fi 后重试" : error;
    }
    return AiEntitlementStatus::kNetworkError;
}

bool AiDialogService::BeginListening() {
    std::string session;
    bool manual = false;
    bool released = false;
    size_t buffered_count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_) return false;
        manual = ptt_mode_;
        released = manual && (ptt_release_pending_ ||
            !ptt_button_held_.load(std::memory_order_acquire));
        buffered_count = ptt_prebuffer_.size();
        session = session_id_;
        snapshot_.state = AiDialogState::kListening;
        snapshot_.message = manual
            ? (released ? "录音完成，正在发送" : "正在录音，松开确认键发送")
            : "请说话，说完后稍等";
    }
    const std::string json = "{\"session_id\":\"" + session +
        "\",\"type\":\"listen\",\"state\":\"start\",\"mode\":\"" +
        (manual ? "manual" : "auto") + "\"}";
    SendJson(json);
    if (!manual && audio_service_ != nullptr) {
        audio_service_->EnableVoiceProcessing(true);
    }
    if (manual) ESP_LOGI(kTag, "PTT server listening started, buffered=%u",
                         static_cast<unsigned>(buffered_count));
    if (send_task_ != nullptr) xTaskNotifyGive(send_task_);
    NotifyChanged();
    if (released && xTaskCreate(&AiDialogService::PttSubmitTaskEntry, "ai_ptt_submit",
                                6144, this, 4, nullptr) != pdPASS) {
        SetState(AiDialogState::kError, "无法提交录音，请重试");
    }
    return true;
}

void AiDialogService::SendJson(const std::string& json) {
    esp_websocket_client_handle_t client = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_ || client_ == nullptr) return;
        client = client_;
    }
    esp_websocket_client_send_text(client, json.data(), static_cast<int>(json.size()),
                                   pdMS_TO_TICKS(1000));
}

void AiDialogService::WebSocketEventEntry(void* handler_args, esp_event_base_t,
                                          int32_t event_id, void* event_data) {
    static_cast<AiDialogService*>(handler_args)->HandleWebSocketEvent(
        event_id, static_cast<esp_websocket_event_data_t*>(event_data));
}

void AiDialogService::HandleWebSocketEvent(int32_t event_id, esp_websocket_event_data_t* event) {
    if (event != nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (event->client != client_) return;
    }
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        int protocol_version = 1;
        AiLearningContext context;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connected_ = true;
            snapshot_.message.clear();
            protocol_version = protocol_version_;
            context = learning_context_;
        }
        NotifyChanged();
        const std::string current_item_prompt = context.current_character.empty()
            ? std::string()
            : "当前屏幕正在学习的英语单词是“" + context.current_character +
              "”。用户说“这个单词”“它”或“当前这个”时，都指单词“" +
              context.current_character + "”。讲解时不要主动分析、展示或朗读音标，不要输出 IPA 字符。"
              "只有用户明确询问怎么读或要求示范发音时，才在回答中单独写出完整英文单词“" +
              context.current_character + "”供语音合成直接发音；不要把单词拆成字母、音节或音标。"
              "其余内容围绕词性与义项、词根词缀、搭配、例句、易混词和记忆方法讲解。";
        // XRobot sends the model response straight to TTS. Put the spoken-text
        // contract into an existing prompt replacement that the live agent
        // already consumes, so dictionary shorthand never reaches TTS as
        // isolated letters even if the console prompt has not been republished.
        const std::string card_state_prompt =
            std::string(context.revealed ? "revealed" : "question") +
            "。只有用户确实说出问题、指令或学习内容时才回答。"
            "如果用户没有说话、输入为空、只有环境噪声或没有提出任何内容，必须完全保持沉默，"
            "不要主动问候、寒暄、提醒、挽留或告别，也不要说再见、舍不得再见之类的话。"
            "以下回答会直接用于语音合成，禁止输出会被逐字母朗读的词典缩写："
            "sb、sb.、s.b. 必须写成 somebody；sth、sth.、s.th. 必须写成 something；"
            "sb's 必须写成 somebody's；n. 写成 noun；v. 写成 verb；"
            "vt. 写成 transitive verb；vi. 写成 intransitive verb；"
            "adj. 写成 adjective；adv. 写成 adverb；prep. 写成 preposition；"
            "pron. 写成 pronoun；conj. 写成 conjunction；det. 写成 determiner；"
            "pl. 写成 plural；e.g. 写成 for example；i.e. 写成 that is；"
            "etc. 写成 and so on；BrE 写成 British English；AmE 写成 American English。"
            "如需解释缩写，先说完整形式；除非用户明确询问字母拼写，否则不要逐个朗读字母。"
            "讲解单词时不要主动输出、分析或朗读 IPA 音标。用户明确要求示范发音时，"
            "只说完整英文单词“" + context.current_character +
            "”，不要输出音标、音节拆分或字母拆分。";
        const std::string hello =
            "{\"type\":\"hello\",\"version\":" + std::to_string(protocol_version) +
            ",\"transport\":\"websocket\",\"agent_params\":{\"custom_replace_prompt\":{"
            "\"learning_course\":\"" + JsonEscape(context.course) +
            "\",\"learning_lesson\":\"" + JsonEscape(context.lesson) +
            "\",\"current_learning_item\":\"" + JsonEscape(current_item_prompt) +
            "\",\"current_pinyin\":\"" + JsonEscape(context.current_pinyin) +
            "\",\"visible_words\":\"" + JsonEscape(context.visible_words) +
            "\",\"card_state\":\"" + JsonEscape(card_state_prompt) +
            "\"}},\"audio_params\":{\"format\":\"opus\",\"sample_rate\":16000,"
            "\"channels\":1,\"frame_duration\":60,\"play_buffer_duration\":1000}}";
        SendJson(hello);
        return;
    }
    if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        if (audio_service_ != nullptr) audio_service_->EnableVoiceProcessing(false);
        bool turn_was_active = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connected_ = false;
            session_id_.clear();
            turn_was_active = ptt_mode_ || snapshot_.state == AiDialogState::kConnecting ||
                snapshot_.state == AiDialogState::kListening ||
                snapshot_.state == AiDialogState::kThinking ||
                snapshot_.state == AiDialogState::kSpeaking;
            if (snapshot_.state != AiDialogState::kReady) {
                snapshot_.state = AiDialogState::kError;
                snapshot_.message = "AI 连接已断开，请重试";
            }
        }
        NotifyChanged();
        (void)turn_was_active;
        return;
    }
    if (event_id == WEBSOCKET_EVENT_ERROR) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (snapshot_.state == AiDialogState::kReady && !ptt_mode_) {
                ESP_LOGW(kTag, "Idle Qiniu WebSocket closed; scheduling reconnect");
                return;
            }
        }
        const int status = event != nullptr ? event->error_handle.esp_ws_handshake_status_code : 0;
        SetState(AiDialogState::kError,
                 status > 0 ? "AI 鉴权或握手失败（HTTP " + std::to_string(status) + "）"
                            : "AI 网络连接失败");
        return;
    }
    if (event_id != WEBSOCKET_EVENT_DATA || event == nullptr || event->data_len <= 0) return;
    if (event->op_code == 0x2 || (event->op_code == 0x0 && receiving_binary_)) {
        if (event->payload_offset == 0) {
            binary_buffer_.clear();
            receiving_binary_ = true;
        }
        binary_buffer_.insert(binary_buffer_.end(), event->data_ptr, event->data_ptr + event->data_len);
        if (!event->fin || event->payload_offset + event->data_len < event->payload_len) return;
        receiving_binary_ = false;
        const uint8_t* payload = binary_buffer_.data();
        size_t payload_size = binary_buffer_.size();
        int protocol_version = 1;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            protocol_version = protocol_version_;
        }
        if (protocol_version == 2 && payload_size >= 16) {
            const uint32_t framed_size = ReadBe32(payload + 12);
            if (framed_size > payload_size - 16) {
                ESP_LOGW(kTag, "Invalid protocol v2 audio frame bytes=%u payload=%u",
                         static_cast<unsigned>(payload_size), static_cast<unsigned>(framed_size));
                binary_buffer_.clear();
                return;
            }
            payload += 16;
            payload_size = framed_size;
        } else if (protocol_version == 3 && payload_size >= 4) {
            const uint16_t framed_size = ReadBe16(payload + 2);
            if (framed_size > payload_size - 4) {
                ESP_LOGW(kTag, "Invalid protocol v3 audio frame bytes=%u payload=%u",
                         static_cast<unsigned>(payload_size), static_cast<unsigned>(framed_size));
                binary_buffer_.clear();
                return;
            }
            payload += 4;
            payload_size = framed_size;
        }
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = server_sample_rate_;
        packet->frame_duration = server_frame_duration_;
        packet->payload.assign(payload, payload + payload_size);
        bool first_tts_packet = false;
        size_t tts_packet_count = 0;
        bool suppress = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            suppress = suppress_cloud_tts_;
            tts_packet_count = ++tts_audio_packets_;
            first_tts_packet = (tts_packet_count == 1);
        }
        if (suppress) {
            binary_buffer_.clear();
            return;
        }
        if (first_tts_packet) {
            ESP_LOGI(kTag, "First TTS audio packet protocol=%d opus_bytes=%u",
                     protocol_version, static_cast<unsigned>(payload_size));
        } else if (tts_packet_count % 25 == 0) {
            ESP_LOGI(kTag, "TTS audio progress packets=%u",
                     static_cast<unsigned>(tts_packet_count));
        }
        if (audio_service_ != nullptr &&
            !audio_service_->PushPacketToDecodeQueue(std::move(packet), true)) {
            ESP_LOGE(kTag, "Unable to queue TTS audio packet");
        }
        binary_buffer_.clear();
    } else if (event->op_code == 0x1 || event->op_code == 0x0) {
        if (event->payload_offset == 0) text_buffer_.clear();
        text_buffer_.append(event->data_ptr, event->data_len);
        if (event->fin && event->payload_offset + event->data_len >= event->payload_len) {
            HandleText(text_buffer_);
            text_buffer_.clear();
        }
    }
}

void AiDialogService::DeriveLearningDisplayLocked(const std::string& question) {
    snapshot_.display_type = AiLearningDisplayType::kText;
    snapshot_.display_character.clear();
    snapshot_.display_pinyin.clear();
    snapshot_.display_caption.clear();
    std::string requested = ExtractRequestedHanzi(question);
    if (requested.empty() && RefersToCurrentHanzi(question)) {
        requested = learning_context_.current_character;
    }
    if (requested.empty()) return;
    snapshot_.display_type = AiLearningDisplayType::kHanzi;
    snapshot_.display_character = requested;
    if (requested == learning_context_.current_character) {
        snapshot_.display_pinyin = learning_context_.current_pinyin;
    }
    // Metadata is filled from the local card catalogue by LiteracyApp. Even
    // when the catalogue has no entry, keep the requested character so the UI
    // can still show it prominently instead of falling back to plain dialogue.
}

void AiDialogService::HandleText(const std::string& text) {
    cJSON* root = cJSON_ParseWithLength(text.data(), text.size());
    if (root == nullptr) return;
    const char* type = JsonString(root, "type");
    if (type != nullptr && std::strcmp(type, "hello") == 0) {
        cJSON* params = cJSON_GetObjectItemCaseSensitive(root, "audio_params");
        bool prepared_only = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (const char* session = JsonString(root, "session_id")) session_id_ = session;
            protocol_version_ = std::clamp(JsonInt(root, "version", protocol_version_), 1, 3);
            if (cJSON_IsObject(params)) {
                server_sample_rate_ = JsonInt(params, "sample_rate", 16000);
                server_frame_duration_ = JsonInt(params, "frame_duration", 60);
            }
            ESP_LOGI(kTag, "Server hello protocol=%d sample_rate=%d frame_ms=%d",
                     protocol_version_, server_sample_rate_, server_frame_duration_);
            prepared_only = preparing_connection_;
            preparing_connection_ = false;
            if (prepared_only && !ptt_mode_) {
                snapshot_.state = AiDialogState::kReady;
                snapshot_.message = "AI 已就绪，长按确认键说话";
            }
        }
        if (prepared_only) {
            NotifyChanged();
        } else if (!BeginListening()) {
            CancelTurn();
        }
    } else if (type != nullptr && std::strcmp(type, "stt") == 0) {
        const char* value = JsonString(root, "text");
        ESP_LOGI(kTag, "STT received: %s", value != nullptr ? value : "");
        if (audio_service_ != nullptr) audio_service_->EnableVoiceProcessing(false);
        const bool has_question = HasMeaningfulTranscript(value);
        if (!has_question) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot_.question.clear();
                snapshot_.answer.clear();
                snapshot_.state = AiDialogState::kReady;
                snapshot_.message.clear();
                suppress_cloud_tts_ = true;
                ptt_mode_ = false;
                ptt_release_pending_ = false;
                ptt_prebuffer_.clear();
                ptt_button_held_.store(false, std::memory_order_release);
            }
            idle_close_generation_.fetch_add(1, std::memory_order_acq_rel);
            turn_timeout_generation_.fetch_add(1, std::memory_order_acq_rel);
            SendJson("{\"type\":\"abort\",\"reason\":\"empty_transcript\"}");
            if (audio_service_ != nullptr) audio_service_->ResetDecoder();
            ESP_LOGI(kTag, "Empty STT transcript; closing AI session silently");
            NotifyChanged();
            ScheduleClose();
            cJSON_Delete(root);
            return;
        }
        bool navigation_command = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            suppress_cloud_tts_ = false;
            snapshot_.question = value;
            DeriveLearningDisplayLocked(snapshot_.question);
            const AiNavigationAction navigation = DetectNavigationAction(snapshot_.question);
            if (navigation != AiNavigationAction::kNone) {
                snapshot_.navigation_action = navigation;
                ++snapshot_.navigation_request_id;
                navigation_command = true;
                suppress_cloud_tts_ = true;
                ptt_mode_ = false;
                ptt_release_pending_ = false;
                ptt_prebuffer_.clear();
                ptt_button_held_.store(false, std::memory_order_release);
            }
            snapshot_.state = AiDialogState::kThinking;
            snapshot_.message = "AI 正在想";
            if (navigation_command) {
                snapshot_.state = AiDialogState::kReady;
                snapshot_.message = "正在切换学习卡片";
            }
            if (!turn_counted_ && config_.provider != "qiniu_xrobot") {
                IncrementUsageLocked();
                turn_counted_ = true;
            }
        }
        if (navigation_command) {
            idle_close_generation_.fetch_add(1, std::memory_order_acq_rel);
            turn_timeout_generation_.fetch_add(1, std::memory_order_acq_rel);
            SendJson("{\"type\":\"abort\",\"reason\":\"local_navigation\"}");
            if (audio_service_ != nullptr) audio_service_->ResetDecoder();
        }
        NotifyChanged();
        if (!navigation_command) {
            ScheduleResponseTimeout();
        }
    } else if (type != nullptr && std::strcmp(type, "tts") == 0) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (suppress_cloud_tts_) {
                cJSON_Delete(root);
                return;
            }
        }
        const char* state = JsonString(root, "state");
        const char* value = JsonString(root, "text");
        ESP_LOGI(kTag, "TTS event state=%s text=%s",
                 state != nullptr ? state : "", value != nullptr ? value : "");
        size_t audio_packets = 0;
        bool answer_changed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state != nullptr && std::strcmp(state, "start") == 0) {
                turn_timeout_generation_.fetch_add(1, std::memory_order_acq_rel);
                tts_audio_packets_ = 0;
                tts_played_packets_ = 0;
                pending_tts_text_.clear();
                tts_stream_active_ = true;
                if (audio_service_ != nullptr) audio_service_->BeginStreamingPlayback();
            }
            if (value != nullptr && value[0] != '\0' &&
                (state == nullptr || std::strcmp(state, "sentence_start") == 0)) {
                // Text arrives ahead of the speaker. Reveal each new sentence
                // only after all audio packets received before it have played,
                // so a page turn cannot hide the sentence still being spoken
                // at the bottom of the preceding e-paper page.
                pending_tts_text_.push_back({tts_audio_packets_, value});
                answer_changed = FlushPendingTtsTextLocked();
            }
            if (state != nullptr && std::strcmp(state, "stop") == 0) {
                audio_packets = tts_audio_packets_;
                snapshot_.state = AiDialogState::kSpeaking;
                snapshot_.message = "AI 正在回答";
                ptt_mode_ = false;
                ptt_release_pending_ = false;
                ptt_prebuffer_.clear();
                ptt_button_held_.store(false, std::memory_order_release);
            } else {
                snapshot_.state = AiDialogState::kSpeaking;
                snapshot_.message = "AI 正在回答";
            }
        }
        if (state != nullptr && std::strcmp(state, "stop") == 0) {
            ESP_LOGI(kTag, "TTS completed audio_packets=%u",
                     static_cast<unsigned>(audio_packets));
            if (audio_service_ != nullptr) audio_service_->EndStreamingPlayback();
        }
        if (answer_changed ||
            state == nullptr ||
            std::strcmp(state, "sentence_start") != 0) {
            NotifyChanged();
        }
        if (state != nullptr && std::strcmp(state, "stop") == 0) {
            StartPlaybackDrain();
        }
    } else if (type != nullptr && std::strcmp(type, "error") == 0) {
        const char* message = JsonString(root, "message");
        SetState(AiDialogState::kError, message != nullptr ? message : "AI 服务返回错误");
    }
    cJSON_Delete(root);
}

void AiDialogService::SendTaskEntry(void* arg) {
    static_cast<AiDialogService*>(arg)->SendTask();
}

void AiDialogService::PttConnectTaskEntry(void* arg) {
    AiDialogService* service = static_cast<AiDialogService*>(arg);
    if (!service->StartTurn()) {
        const std::string error = service->GetSnapshot().message;
        service->CancelTurn();
        service->SetState(AiDialogState::kError,
                          error.empty() ? "AI 连接失败，请重试" : error);
    }
    vTaskDelete(nullptr);
}

void AiDialogService::PttSubmitTaskEntry(void* arg) {
    static_cast<AiDialogService*>(arg)->FinishPtt();
    vTaskDelete(nullptr);
}

void AiDialogService::PrewarmTaskEntry(void* arg) {
    static_cast<AiDialogService*>(arg)->PrewarmConnection();
    vTaskDelete(nullptr);
}

void AiDialogService::ContextRefreshTaskEntry(void* arg) {
    static_cast<AiDialogService*>(arg)->RefreshContextConnection();
    vTaskDelete(nullptr);
}

void AiDialogService::PrewarmConnection() {
    vTaskDelay(pdMS_TO_TICKS(250));
    ESP_LOGI(kTag, "AI WebSocket prewarm started");
    if (OpenConnection(false)) {
        ESP_LOGI(kTag, "AI WebSocket prewarm ready");
        std::lock_guard<std::mutex> lock(mutex_);
        prewarm_running_ = false;
        return;
    }
    ESP_LOGW(kTag, "AI WebSocket prewarm unavailable; first turn will reconnect");
    {
        std::lock_guard<std::mutex> lock(mutex_);
        prewarm_running_ = false;
        if (!ptt_mode_) {
            snapshot_.state = AiDialogState::kReady;
            snapshot_.message = "长按确认键，听到提示音后说话";
        }
    }
    NotifyChanged();
}

void AiDialogService::RefreshContextConnection() {
    idle_close_generation_.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tts_stream_active_ = false;
        pending_tts_text_.clear();
    }
    if (audio_service_ != nullptr) {
        audio_service_->EnableVoiceProcessing(false);
        audio_service_->ResetDecoder();
    }
    CloseClient();
    const bool ready = OpenConnection(false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        context_refresh_running_ = false;
        if (!ready && snapshot_.state != AiDialogState::kError) {
            snapshot_.state = AiDialogState::kError;
            snapshot_.message = "新卡片 AI 上下文同步失败，请重试";
        }
    }
    ESP_LOGI(kTag, "AI context refresh %s", ready ? "ready" : "failed");
    NotifyChanged();
}

void AiDialogService::CloseConnectionTaskEntry(void* arg) {
    static_cast<AiDialogService*>(arg)->CloseConnection();
    vTaskDelete(nullptr);
}

void AiDialogService::PlaybackDrainTaskEntry(void* arg) {
    static_cast<AiDialogService*>(arg)->WaitForPlaybackDrain();
    vTaskDelete(nullptr);
}

void AiDialogService::IdleCloseTaskEntry(void* arg) {
    std::unique_ptr<std::pair<AiDialogService*, uint32_t>> context(
        static_cast<std::pair<AiDialogService*, uint32_t>*>(arg));
    context->first->WaitForIdleClose(context->second);
    vTaskDelete(nullptr);
}

void AiDialogService::ResponseTimeoutTaskEntry(void* arg) {
    std::unique_ptr<std::pair<AiDialogService*, uint32_t>> context(
        static_cast<std::pair<AiDialogService*, uint32_t>*>(arg));
    context->first->WaitForResponseTimeout(context->second);
    vTaskDelete(nullptr);
}

void AiDialogService::TranscriptTimeoutTaskEntry(void* arg) {
    std::unique_ptr<std::pair<AiDialogService*, uint32_t>> context(
        static_cast<std::pair<AiDialogService*, uint32_t>*>(arg));
    context->first->WaitForTranscriptTimeout(context->second);
    vTaskDelete(nullptr);
}

bool AiDialogService::StartPlaybackDrain() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (playback_drain_pending_) return true;
        playback_drain_pending_ = true;
    }
    if (xTaskCreate(&AiDialogService::PlaybackDrainTaskEntry, "ai_play_drain",
                    4096, this, 3, nullptr) == pdPASS) {
        return true;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    playback_drain_pending_ = false;
    snapshot_.state = AiDialogState::kReady;
    snapshot_.message = "回答完毕，确认键继续提问";
    return false;
}

void AiDialogService::WaitForPlaybackDrain() {
    // Some providers emit the stop event immediately before their last binary
    // frame. Give the WebSocket event task a short grace period, then require
    // the playback pipeline to remain empty for several checks.
    vTaskDelay(pdMS_TO_TICKS(300));
    int stable_idle_checks = 0;
    for (int attempt = 0; attempt < 1200 && stable_idle_checks < 4; ++attempt) {
        if (audio_service_ == nullptr || audio_service_->IsIdle()) {
            ++stable_idle_checks;
        } else {
            stable_idle_checks = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        FlushPendingTtsTextLocked(true);
        tts_stream_active_ = false;
        suppress_cloud_tts_ = true;
        playback_drain_pending_ = false;
        if (snapshot_.state == AiDialogState::kSpeaking) {
            snapshot_.state = AiDialogState::kReady;
            snapshot_.message = "回答完毕，确认键继续提问";
        }
    }
    ESP_LOGI(kTag, "TTS playback drained");
    NotifyChanged();
    ScheduleIdleClose();
}

void AiDialogService::HandlePlaybackFrame() {
    bool answer_changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!tts_stream_active_) {
            return;
        }
        ++tts_played_packets_;
        answer_changed = FlushPendingTtsTextLocked();
    }
    if (answer_changed) {
        NotifyChanged();
    }
}

bool AiDialogService::FlushPendingTtsTextLocked(bool force) {
    bool changed = false;
    while (!pending_tts_text_.empty() &&
           (force ||
            pending_tts_text_.front().reveal_after_packet <= tts_played_packets_)) {
        AppendVisibleAnswerLocked(pending_tts_text_.front().text);
        pending_tts_text_.pop_front();
        changed = true;
    }
    return changed;
}

void AiDialogService::AppendVisibleAnswerLocked(const std::string& text) {
    snapshot_.answer += text;

    // Retain the complete practical answer for page-by-page reading. The
    // display layer measures pages with the actual font.
    constexpr size_t kMaxVisibleAnswerBytes = 4096;
    if (snapshot_.answer.size() <= kMaxVisibleAnswerBytes) {
        return;
    }
    size_t first = snapshot_.answer.size() - kMaxVisibleAnswerBytes;
    while (first < snapshot_.answer.size() &&
           (static_cast<unsigned char>(snapshot_.answer[first]) & 0xC0) == 0x80) {
        ++first;
    }
    snapshot_.answer = "…" + snapshot_.answer.substr(first);
}

bool AiDialogService::ScheduleIdleClose() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_ || client_ == nullptr) return false;
    }
    const uint32_t generation =
        idle_close_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto* context = new (std::nothrow) std::pair<AiDialogService*, uint32_t>(this, generation);
    if (context == nullptr) return false;
    if (xTaskCreate(&AiDialogService::IdleCloseTaskEntry, "ai_idle_close",
                    3072, context, 2, nullptr) == pdPASS) {
        return true;
    }
    delete context;
    return false;
}

void AiDialogService::WaitForIdleClose(uint32_t generation) {
    // Keep a short follow-up window for fast consecutive questions, but close
    // well before the cloud agent's configured 120-second idle timeout. This guarantees
    // that provider-side farewell prompts can never reach the speaker.
    vTaskDelay(pdMS_TO_TICKS(25000));
    bool should_close = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        should_close = generation == idle_close_generation_.load(std::memory_order_acquire) &&
            connected_ && client_ != nullptr && !close_pending_ && !ptt_mode_ &&
            snapshot_.state == AiDialogState::kReady;
    }
    if (should_close) {
        ESP_LOGI(kTag, "Closing idle AI session silently before provider timeout");
        ScheduleClose();
    }
}

bool AiDialogService::ScheduleResponseTimeout() {
    const uint32_t generation =
        turn_timeout_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto* context = new (std::nothrow) std::pair<AiDialogService*, uint32_t>(this, generation);
    if (context == nullptr) return false;
    if (xTaskCreate(&AiDialogService::ResponseTimeoutTaskEntry, "ai_rsp_timeout",
                    3072, context, 2, nullptr) == pdPASS) {
        return true;
    }
    delete context;
    return false;
}

void AiDialogService::WaitForResponseTimeout(uint32_t generation) {
    vTaskDelay(pdMS_TO_TICKS(20000));
    bool timed_out = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        timed_out = generation == turn_timeout_generation_.load(std::memory_order_acquire) &&
            connected_ && client_ != nullptr && !close_pending_ &&
            snapshot_.state == AiDialogState::kThinking && !snapshot_.question.empty();
        if (timed_out) {
            const bool had_question = !snapshot_.question.empty();
            ptt_mode_ = false;
            ptt_release_pending_ = false;
            ptt_prebuffer_.clear();
            ptt_button_held_.store(false, std::memory_order_release);
            snapshot_.state = AiDialogState::kReady;
            // A silent turn must end silently. Only show retry guidance when a
            // real question was recognized but the provider failed to answer.
            snapshot_.message = had_question ? "AI 暂未回答，请长按确认键重试" : "";
        }
    }
    if (!timed_out) return;
    ESP_LOGW(kTag, "AI response timeout; closing session before provider farewell");
    SendJson("{\"type\":\"abort\",\"reason\":\"response_timeout\"}");
    if (audio_service_ != nullptr) {
        audio_service_->EnableVoiceProcessing(false);
        audio_service_->ResetDecoder();
    }
    NotifyChanged();
    ScheduleClose();
}

bool AiDialogService::ScheduleTranscriptTimeout() {
    const uint32_t generation =
        turn_timeout_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto* context = new (std::nothrow) std::pair<AiDialogService*, uint32_t>(this, generation);
    if (context == nullptr) return false;
    if (xTaskCreate(&AiDialogService::TranscriptTimeoutTaskEntry, "ai_stt_timeout",
                    3072, context, 2, nullptr) == pdPASS) {
        return true;
    }
    delete context;
    return false;
}

void AiDialogService::WaitForTranscriptTimeout(uint32_t generation) {
    // Final STT normally arrives quickly after manual listen/stop. A separate
    // short deadline prevents a silent press from occupying the AI session,
    // while the answer deadline starts only after meaningful text is received.
    vTaskDelay(pdMS_TO_TICKS(6000));
    bool timed_out = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        timed_out = generation == turn_timeout_generation_.load(std::memory_order_acquire) &&
            connected_ && client_ != nullptr && !close_pending_ &&
            snapshot_.state == AiDialogState::kThinking && snapshot_.question.empty();
        if (timed_out) {
            suppress_cloud_tts_ = true;
            ptt_mode_ = false;
            ptt_release_pending_ = false;
            ptt_prebuffer_.clear();
            ptt_button_held_.store(false, std::memory_order_release);
            snapshot_.state = AiDialogState::kReady;
            snapshot_.message.clear();
        }
    }
    if (!timed_out) return;
    ESP_LOGI(kTag, "No STT transcript after PTT; closing AI session silently");
    SendJson("{\"type\":\"abort\",\"reason\":\"transcript_timeout\"}");
    if (audio_service_ != nullptr) {
        audio_service_->EnableVoiceProcessing(false);
        audio_service_->ResetDecoder();
    }
    NotifyChanged();
    ScheduleClose();
}

void AiDialogService::CloseConnection() {
    idle_close_generation_.fetch_add(1, std::memory_order_acq_rel);
    turn_timeout_generation_.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ptt_mode_ = false;
        ptt_release_pending_ = false;
        ptt_prebuffer_.clear();
        ptt_button_held_.store(false, std::memory_order_release);
    }
    CloseClient();
    WifiManager::GetInstance().SetPowerSaveLevel(WifiPowerSaveLevel::LOW_POWER);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        close_pending_ = false;
    }
    NotifyChanged();
}

void AiDialogService::SendTask() {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (audio_service_ != nullptr) {
            std::vector<uint8_t> payload;
            esp_websocket_client_handle_t client = nullptr;
            bool can_send = false;
            bool from_prebuffer = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (connected_ && snapshot_.state == AiDialogState::kListening &&
                    !ptt_prebuffer_.empty()) {
                    payload = std::move(ptt_prebuffer_.front());
                    ptt_prebuffer_.pop_front();
                    from_prebuffer = true;
                }
                client = client_;
                can_send = connected_ && snapshot_.state == AiDialogState::kListening;
            }
            if (payload.empty()) {
                auto packet = audio_service_->PopPacketFromSendQueue();
                if (packet == nullptr) break;
                payload = std::move(packet->payload);
                bool buffered = false;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (ptt_mode_ &&
                        !(connected_ && snapshot_.state == AiDialogState::kListening)) {
                        if (ptt_prebuffer_.size() >= kMaxPttPrebufferPackets) {
                            ptt_prebuffer_.pop_front();
                        }
                        ptt_prebuffer_.push_back(std::move(payload));
                        buffered = true;
                    }
                    client = client_;
                    can_send = connected_ && snapshot_.state == AiDialogState::kListening;
                }
                if (buffered) continue;
            }
            if (can_send && client != nullptr) {
                int protocol_version = 1;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    protocol_version = protocol_version_;
                }
                std::vector<uint8_t> framed;
                const uint8_t* send_data = payload.data();
                size_t send_size = payload.size();
                if (protocol_version == 2) {
                    framed.resize(16 + payload.size());
                    WriteBe16(framed.data(), 2);
                    WriteBe16(framed.data() + 2, 0);
                    WriteBe32(framed.data() + 4, 0);
                    WriteBe32(framed.data() + 8, 0);
                    WriteBe32(framed.data() + 12, static_cast<uint32_t>(payload.size()));
                    std::memcpy(framed.data() + 16, payload.data(), payload.size());
                    send_data = framed.data();
                    send_size = framed.size();
                } else if (protocol_version == 3) {
                    framed.resize(4 + payload.size());
                    framed[0] = 0;
                    framed[1] = 0;
                    WriteBe16(framed.data() + 2, static_cast<uint16_t>(payload.size()));
                    std::memcpy(framed.data() + 4, payload.data(), payload.size());
                    send_data = framed.data();
                    send_size = framed.size();
                }
                const int sent = esp_websocket_client_send_bin(
                    client, reinterpret_cast<const char*>(send_data),
                    static_cast<int>(send_size), pdMS_TO_TICKS(1000));
                if (sent <= 0 && from_prebuffer) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ptt_prebuffer_.push_front(std::move(payload));
                    ESP_LOGW(kTag, "PTT buffered audio send failed; retrying");
                    vTaskDelay(pdMS_TO_TICKS(30));
                    continue;
                }
                // A cold WebSocket can reject a large burst even though each
                // Opus packet is small. Replay buffered speech faster than
                // real time, but leave enough room for the TLS task to drain.
                if (from_prebuffer) vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }
}

void AiDialogService::SetState(AiDialogState state, const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.state = state;
        if (!message.empty()) snapshot_.message = message;
    }
    NotifyChanged();
}

void AiDialogService::NotifyChanged() {
    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = on_changed_;
    }
    if (callback) callback();
}

void AiDialogService::RefreshUsageLocked() {
    const int32_t today = CurrentUsageDay();
    Settings settings(kNamespace);
    const int32_t stored_day = settings.GetInt("usage_day", today);
    snapshot_.used_today = stored_day == today
        ? std::max(0, static_cast<int>(settings.GetInt("usage_count", 0))) : 0;
    if (stored_day != today) {
        Settings writable(kNamespace, true);
        writable.SetInt("usage_day", today);
        writable.SetInt("usage_count", 0);
    }
}

void AiDialogService::IncrementUsageLocked() {
    ++snapshot_.used_today;
    Settings settings(kNamespace, true);
    settings.SetInt("usage_day", CurrentUsageDay());
    settings.SetInt("usage_count", snapshot_.used_today);
}

void AiDialogService::CloseClient() {
    esp_websocket_client_handle_t client = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        client = client_;
        client_ = nullptr;
        connected_ = false;
        session_id_.clear();
    }
    if (client != nullptr) {
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
    }
}
