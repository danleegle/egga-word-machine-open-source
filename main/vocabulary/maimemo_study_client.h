#ifndef MAIMEMO_STUDY_CLIENT_H
#define MAIMEMO_STUDY_CLIENT_H

#include <cstdint>
#include <chrono>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <string>
#include <vector>

class WebSocket;

enum class MaimemoAnswer : uint8_t {
    kFamiliar = 1,
    kVague = 2,
    kForget = 3,
};

struct MaimemoWordCard {
    std::string voc_id;
    std::string word;
    std::string phonetic;
    std::string pronunciation_url_us;
    std::string pronunciation_url_uk;
    std::string meaning;
    std::string example;
    std::string example_translation;
    std::string note_type;
    std::string note;
};

class MaimemoStudyClient {
public:
    MaimemoStudyClient();
    ~MaimemoStudyClient();

    struct WsMessage {
        std::string id;
        std::string reply_id;
        int event = 0;
        bool success = false;
        bool has_success = false;
        std::string error_code;
        std::string error_message;
        std::string error_info;
        std::vector<uint8_t> data;
    };

    bool GetWord(MaimemoWordCard* card, bool back = false);
    bool GetWordAvoiding(MaimemoWordCard* card,
                         bool back,
                         const std::string& avoid_voc_id,
                         const std::string& avoid_word);
    bool SubmitResponse(const std::string& voc_id,
                        const std::string& word,
                        MaimemoAnswer answer,
                        uint32_t recall_duration_ms,
                        uint32_t study_duration_ms,
                        MaimemoWordCard* next_card,
                        bool* submitted = nullptr);
    std::string LastStatus() const;
    uint16_t ProgressCurrent() const;
    uint16_t DailyTarget() const;
    void Reset();

private:
    bool EnsureReadyLocked(std::unique_lock<std::mutex>& lock);
    bool ConnectLocked(std::unique_lock<std::mutex>& lock);
    bool InitStudyLocked(std::unique_lock<std::mutex>& lock);
    bool SendRequestWaitLocked(std::unique_lock<std::mutex>& lock,
                               int event,
                               const std::vector<uint8_t>& payload,
                               WsMessage* response);
    void HandleWsData(const char* data, size_t len, bool binary);
    void SetLastStatusLocked(const std::string& status);

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unique_ptr<WebSocket> websocket_;
    std::string token_;
    std::string pending_id_;
    WsMessage pending_response_;
    bool pending_done_ = false;
    bool system_ready_ = false;
    bool initialized_ = false;
    uint32_t request_seq_ = 0;
    uint16_t progress_current_ = 0;
    uint16_t daily_target_ = 0;
    uint8_t study_method_ = 0;
    bool init_payload_logged_ = false;
    bool word_payload_logged_ = false;
    bool submit_payload_logged_ = false;
    std::chrono::steady_clock::time_point last_server_word_received_at_{};
    std::string last_status_ = "未连接墨墨";
};

#endif  // MAIMEMO_STUDY_CLIENT_H
