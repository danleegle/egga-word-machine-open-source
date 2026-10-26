#ifndef AI_DIALOG_SERVICE_H
#define AI_DIALOG_SERVICE_H

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <atomic>
#include <deque>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_websocket_client.h"

class AudioService;

enum class AiDialogState : uint8_t {
    kDisabled = 0,
    kReady,
    kConnecting,
    kListening,
    kThinking,
    kSpeaking,
    kError,
};

enum class AiLearningDisplayType : uint8_t {
    kText = 0,
    kHanzi,
};

enum class AiNavigationAction : uint8_t {
    kNone = 0,
    kNextCard,
    kPreviousCard,
};

struct AiLearningContext {
    std::string course;
    std::string lesson;
    std::string learning_type;
    std::string current_character;
    std::string current_pinyin;
    std::string visible_words;
    bool revealed = false;
};

struct AiDialogConfig {
    bool enabled = false;
    bool entitlement_verified = false;
    std::string provider = "qiniu_xrobot";
    std::string url = "wss://xrobo-io.qiniuapi.com/v1/ws/";
    std::string token;
    std::string agent_id;
    int daily_limit = 10;
};

enum class AiEntitlementStatus : uint8_t {
    kAvailable = 0,
    kQuotaExceeded,
    kNotEntitled,
    kNetworkError,
};

struct AiDialogSnapshot {
    AiDialogState state = AiDialogState::kDisabled;
    bool configured = false;
    int used_today = 0;
    int daily_limit = 10;
    std::string provider;
    std::string question;
    std::string answer;
    std::string message;
    AiLearningDisplayType display_type = AiLearningDisplayType::kText;
    std::string display_character;
    std::string display_pinyin;
    std::string display_caption;
    AiNavigationAction navigation_action = AiNavigationAction::kNone;
    uint32_t navigation_request_id = 0;
};

class AiDialogService {
public:
    static AiDialogService& GetInstance();

    void Initialize(AudioService* audio_service, std::function<void()> on_changed);
    void ReloadConfig();
    void SetLearningContext(const AiLearningContext& context);
    void RefreshLearningContextConnection();
    bool StartTurn();
    bool StartPtt();
    void FinishPtt();
    void SetPttButtonHeld(bool held);
    void CancelTurn();
    void Stop();
    bool IsBusy() const;
    AiDialogSnapshot GetSnapshot() const;
    void ResolveNavigationRequest(uint32_t request_id, const std::string& response);

    static AiDialogConfig LoadConfig();
    static bool SaveConfig(const AiDialogConfig& config, std::string* error);
    static void ResetUsage();
    AiEntitlementStatus CheckEntitlement(std::string* message);

private:
    AiDialogService() = default;
    ~AiDialogService() = default;
    AiDialogService(const AiDialogService&) = delete;
    AiDialogService& operator=(const AiDialogService&) = delete;

    static void WebSocketEventEntry(void* handler_args, esp_event_base_t base,
                                    int32_t event_id, void* event_data);
    static void SendTaskEntry(void* arg);
    static void PttConnectTaskEntry(void* arg);
    static void PttSubmitTaskEntry(void* arg);
    static void CloseConnectionTaskEntry(void* arg);
    static void PlaybackDrainTaskEntry(void* arg);
    static void IdleCloseTaskEntry(void* arg);
    static void TranscriptTimeoutTaskEntry(void* arg);
    static void ResponseTimeoutTaskEntry(void* arg);
    static void PrewarmTaskEntry(void* arg);
    static void ContextRefreshTaskEntry(void* arg);
    void HandleWebSocketEvent(int32_t event_id, esp_websocket_event_data_t* event);
    void HandleText(const std::string& text);
    void SendTask();
    bool OpenConnection(bool reserve_turn = true);
    bool AcquireCloudCredentials(std::string* websocket_url, std::string* token,
                                 std::string* error, bool reserve_turn,
                                 int* http_status = nullptr);
    void PrewarmConnection();
    void RefreshContextConnection();
    void CloseConnection();
    bool ScheduleClose();
    void WaitForPlaybackDrain();
    bool StartPlaybackDrain();
    void WaitForIdleClose(uint32_t generation);
    bool ScheduleIdleClose();
    void WaitForTranscriptTimeout(uint32_t generation);
    bool ScheduleTranscriptTimeout();
    void WaitForResponseTimeout(uint32_t generation);
    bool ScheduleResponseTimeout();
    void DeriveLearningDisplayLocked(const std::string& question);
    void HandlePlaybackFrame();
    bool FlushPendingTtsTextLocked(bool force = false);
    void AppendVisibleAnswerLocked(const std::string& text);
    bool BeginListening();
    void SendJson(const std::string& json);
    void SetState(AiDialogState state, const std::string& message = "");
    void NotifyChanged();
    void RefreshUsageLocked();
    void IncrementUsageLocked();
    void CloseClient();

    mutable std::mutex mutex_;
    std::mutex connection_mutex_;
    AudioService* audio_service_ = nullptr;
    std::function<void()> on_changed_;
    esp_websocket_client_handle_t client_ = nullptr;
    TaskHandle_t send_task_ = nullptr;
    AiDialogConfig config_;
    AiDialogSnapshot snapshot_;
    std::string headers_;
    std::string session_id_;
    std::string text_buffer_;
    std::vector<uint8_t> binary_buffer_;
    AiLearningContext learning_context_;
    int server_sample_rate_ = 16000;
    int server_frame_duration_ = 60;
    int protocol_version_ = 1;
    bool receiving_binary_ = false;
    bool initialized_ = false;
    bool connected_ = false;
    bool turn_counted_ = false;
    bool close_pending_ = false;
    bool playback_drain_pending_ = false;
    bool ptt_mode_ = false;
    bool ptt_release_pending_ = false;
    bool preparing_connection_ = false;
    bool prewarm_running_ = false;
    bool context_refresh_running_ = false;
    bool suppress_cloud_tts_ = false;
    size_t tts_audio_packets_ = 0;
    size_t tts_played_packets_ = 0;
    bool tts_stream_active_ = false;
    struct PendingTtsText {
        size_t reveal_after_packet = 0;
        std::string text;
    };
    std::deque<PendingTtsText> pending_tts_text_;
    std::atomic<bool> ptt_button_held_{false};
    std::atomic<uint32_t> idle_close_generation_{0};
    std::atomic<uint32_t> turn_timeout_generation_{0};
    std::deque<std::vector<uint8_t>> ptt_prebuffer_;
};

#endif  // AI_DIALOG_SERVICE_H
