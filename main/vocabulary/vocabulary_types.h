#ifndef VOCABULARY_TYPES_H
#define VOCABULARY_TYPES_H

#include <cstddef>
#include <cstdint>
#include <string>

enum class ReviewStatus : uint8_t {
    kNew = 0,
    kLearning,
    kReview,
    kMastered,
};

enum class VocabularyScreen : uint8_t {
    kCardFront = 0,
    kCardBack,
    kDailySummary,
    kSettings,
    kWelcome,
};

enum class VocabularyButton : uint8_t {
    kConfirmClick = 0,
    kConfirmLongPress,
    kUpClick,
    kDownClick,
    kUpLongPress,
    kDownLongPress,
    kSettingsCombo,
    kConfirmDoubleClick,
    kConfirmLongRelease,
};

enum class VocabularyOrderMode : uint8_t {
    kSequential = 0,
    kRandom,
};

enum class VocabularyVoiceType : uint8_t {
    kAmerican = 0,
    kBritish,
};

enum class VocabularyStudyMode : uint8_t {
    kOffline = 0,
    kMaimemo,
    kBrowse,
};

enum class VocabularyFontMode : uint8_t {
    kClear = 0,
    kCompact,
};

enum class VocabularyAutoSleepTimeout : uint8_t {
    k30Minutes = 0,
    k1Hour,
    k3Hours,
    k6Hours,
    kNever,
};

constexpr uint32_t VocabularyAutoSleepDelayMs(VocabularyAutoSleepTimeout timeout) {
    switch (timeout) {
        case VocabularyAutoSleepTimeout::k30Minutes:
            return 30U * 60U * 1000U;
        case VocabularyAutoSleepTimeout::k1Hour:
            return 60U * 60U * 1000U;
        case VocabularyAutoSleepTimeout::k6Hours:
            return 6U * 60U * 60U * 1000U;
        case VocabularyAutoSleepTimeout::kNever:
            return 0;
        case VocabularyAutoSleepTimeout::k3Hours:
        default:
            return 3U * 60U * 60U * 1000U;
    }
}

struct WordCard {
    uint16_t id = 0;
    const char* deck_id = nullptr;
    const char* word = nullptr;
    const char* phonetic = nullptr;
    const char* meaning = nullptr;
    const char* example = nullptr;
    const char* example_translation = nullptr;
};

struct ReviewState {
    ReviewStatus status = ReviewStatus::kNew;
    int32_t due_day = 0;
    int32_t last_review_day = 0;
    uint16_t interval_days = 0;
    uint8_t correct_streak = 0;
    uint16_t lapses = 0;
};

struct DeckManifest {
    const char* deck_id = nullptr;
    const char* name = nullptr;
    const char* language = nullptr;
    uint16_t version = 1;
    size_t word_count = 0;
};

struct VocabularySettings {
    VocabularyOrderMode order_mode = VocabularyOrderMode::kSequential;
    uint16_t daily_target = 20;
    VocabularyVoiceType voice_type = VocabularyVoiceType::kAmerican;
    VocabularyStudyMode study_mode = VocabularyStudyMode::kOffline;
    VocabularyFontMode font_mode = VocabularyFontMode::kClear;
    bool auto_pronunciation = false;
    bool onboarding_completed = false;
    VocabularyAutoSleepTimeout auto_sleep_timeout = VocabularyAutoSleepTimeout::k3Hours;
    std::string active_deck_id;
};

struct VocabularyDailyStats {
    int32_t day = 0;
    uint16_t reviewed = 0;
    uint16_t correct = 0;
    uint16_t streak_days = 0;
};

static constexpr size_t kVocabularySettingsItemCount = 13;

struct VocabularySnapshot {
    VocabularyScreen screen = VocabularyScreen::kCardFront;
    bool sleeping = false;
    bool has_card = false;
    bool showing_back = false;
    ReviewStatus status = ReviewStatus::kNew;
    uint16_t interval_days = 0;
    uint16_t lapses = 0;
    uint16_t reviewed_today = 0;
    uint16_t correct_today = 0;
    uint16_t streak_days = 0;
    uint16_t due_count = 0;
    uint16_t total_count = 0;
    uint16_t card_position = 0;
    uint16_t card_count = 0;
    bool battery_available = false;
    bool battery_charging = false;
    uint8_t battery_level = 0;
    bool wifi_connected = false;
    bool wifi_ap_mode = false;
    uint8_t wifi_signal_bars = 0;
    char header[64] = {0};
    char clock[6] = {0};
    char word[48] = {0};
    char phonetic[64] = {0};
    char meaning[192] = {0};
    char example[448] = {0};
    char example_translation[256] = {0};
    char progress[96] = {0};
    char hint[256] = {0};
    char footer[128] = {0};
    char status_line[96] = {0};
    char action_status[32] = {0};
    char review_mark[24] = {0};
    bool ai_overlay_active = false;
    uint8_t ai_state = 0;
    int8_t ai_page_delta = 0;
    uint32_t ai_page_request_id = 0;
    char ai_question[192] = {0};
    // Chinese explanations use up to three UTF-8 bytes per character.  Keep
    // enough data to populate the full-height answer label before clipping.
    char ai_answer[512] = {0};
    char ai_message[160] = {0};
    bool settings_popup_open = false;
    bool compact_font = false;
    uint8_t settings_selection = 0;
    char settings_list[512] = {0};
    char settings_item_values[kVocabularySettingsItemCount][72] = {};
    char settings_popup_title[64] = {0};
    char settings_popup_badge[32] = {0};
    char settings_popup_body[320] = {0};
    char settings_popup_hint[128] = {0};
    bool settings_popup_progress_visible = false;
    int16_t settings_popup_progress_min = 0;
    int16_t settings_popup_progress_max = 100;
    int16_t settings_popup_progress_value = 0;
};

#endif  // VOCABULARY_TYPES_H
