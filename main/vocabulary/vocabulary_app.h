#ifndef VOCABULARY_APP_H
#define VOCABULARY_APP_H

#include "deck_store.h"
#include "maimemo_study_client.h"
#include "maimemo_offline_store.h"
#include "maimemo_open_api_client.h"
#include "progress_store.h"
#include "review_scheduler.h"
#include "sync_client.h"
#include "vocabulary_types.h"

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

class Display;

class VocabularyApp {
public:
    void Initialize(Display* display);
    void HandleButton(VocabularyButton button);
    void RefreshSnapshot();
    void NotifyNetworkConnected();
    VocabularySnapshot GetSnapshot() const;
    std::string DeckStatusJson() const;
    std::string DiagnosticsJson() const;
    std::string MaimemoOfflineStatusJson() const;
    bool SelectDeck(const std::string& deck_id);
    bool ClearProgress();
    bool ClearMaimemoOfflineState();
    void NotifyMaimemoAuthChanged();
    bool CanEnterLowPowerMode() const;
    void PrepareForLowPowerMode();
    void HandleUserWakeFromLowPowerMode(bool publish_snapshot = true);
    void RestoreNetworkAfterUserWake();

private:
    enum class SettingsView : uint8_t {
        kMain = 0,
        kLocalDecks,
        kOnlineDecks,
        kPopup,
    };

    enum class SettingsPopupAction : uint8_t {
        kNone = 0,
        kStudyMode,
        kOrderMode,
        kVoiceType,
        kFontMode,
        kDailyTarget,
        kOutputVolume,
        kAutoPronunciation,
        kSelectLocalDeck,
        kDownloadOnlineDeck,
        kRefreshOnlineDecks,
        kOtaUpdate,
        kAiService,
        kWifiStatus,
        kAutoSleep,
        kButtonHelp,
    };

    enum class DeckSyncUiState : uint8_t {
        kIdle = 0,
        kCatalogLoading,
        kCatalogError,
        kDownloadLoading,
        kSwitchLoading,
        kDeleteLoading,
        kDownloadSuccess,
        kDownloadError,
    };

    struct DeckChoice {
        std::string id;
        std::string name;
        std::string detail;
        bool builtin = false;
        bool installed = false;
        bool remote = false;
        size_t remote_index = 0;
    };

    int32_t TodayDayIndex() const;
    bool RefreshCurrentDayLocked();
    void RebuildQueueLocked(bool preserve_current = false);
    void PublishSnapshotLocked(bool full_refresh, bool render_now = false);
    VocabularySnapshot BuildSnapshotLocked() const;
    VocabularySnapshot BuildMaimemoSnapshotLocked() const;
    void ScoreCurrentLocked(bool known);
    void MissCurrentLocked();
    void DeferCurrentLocked();
    void RequeueWordIndexLocked(size_t word_index);
    bool RemoveWordFromQueueLocked(size_t word_index, size_t* removed_pos);
    size_t CurrentWordIndexLocked() const;
    void SelectWordLocked(size_t word_index, bool persist_position = true);
    void RestoreCurrentPositionLocked();
    void SaveCurrentPositionLocked();
    void MarkCurrentPositionDirtyLocked();
    void MarkProgressDirtyLocked();
    std::vector<uint64_t> BuildProgressWordKeysLocked() const;
    void ArmProgressSaveTimerLocked();
    void DisarmProgressSaveTimerLocked();
    void ShowActionStatusLocked(const char* text);
    void HandleActionStatusTimeout();
    void StartProgressSaveTaskLocked();
    void RunProgressSaveTask(uint32_t request_id,
                             std::string deck_id,
                             std::vector<uint64_t> word_keys,
                             std::vector<ReviewState> states,
                             VocabularyDailyStats stats);
    void MoveCardLocked(int delta);
    void BrowseCardLocked(int delta);
    void EnterSettingsLocked();
    void HandleSettingsButtonLocked(VocabularyButton button);
    void HandleSettingsMainButtonLocked(VocabularyButton button);
    void HandleLocalDeckSettingsButtonLocked(VocabularyButton button);
    void HandleOnlineDeckSettingsButtonLocked(VocabularyButton button);
    void HandleDailyTargetSettingsButtonLocked(VocabularyButton button);
    void HandleSettingsPopupButtonLocked(VocabularyButton button);
    void MoveSettingsSelectionLocked(int delta);
    void MoveLocalDeckSelectionLocked(int delta);
    void MoveOnlineDeckSelectionLocked(int delta);
    void ActivateSettingsItemLocked();
    void OpenSettingsPopupLocked(SettingsPopupAction action);
    void ApplySettingsPopupLocked();
    void CancelSettingsPopupLocked(bool exit_settings);
    void MoveSettingsPopupValueLocked(int delta);
    void ToggleStudyModeLocked();
    void ToggleOrderModeLocked();
    void ToggleVoiceTypeLocked();
    void AdjustDailyTargetLocked(int delta);
    VocabularySnapshot BuildSettingsSnapshotLocked() const;
    void SaveSettingsAndRebuildQueueLocked();
    std::vector<DeckChoice> BuildLocalDeckChoicesLocked() const;
    std::vector<DeckChoice> BuildOnlineDeckChoicesLocked() const;
    std::string OnlineDeckStorageBadgeLocked() const;
    void SelectLocalDeckChoiceLocked();
    void DeleteLocalDeckChoiceLocked();
    void SelectOnlineDeckChoiceLocked();
    void DeleteOnlineDeckChoiceLocked();
    bool SwitchDeckLocked(const std::string& deck_id);
    void StartDeckSwitchTaskLocked(const std::string& deck_id, const std::string& name);
    void StartDeckDeleteTaskLocked(const std::string& deck_id, const std::string& name);
    void StartDeckCatalogTaskLocked();
    void StartDeckDownloadTaskLocked(const RemoteDeckEntry& entry);
    bool IsCurrentDeckSyncRequest(uint32_t request_id) const;
    void RunDeckSwitchTask(uint32_t request_id,
                           std::string deck_id,
                           std::string name,
                           std::string active_deck_id,
                           std::vector<uint64_t> progress_word_keys,
                           std::vector<ReviewState> progress_states,
                           VocabularyDailyStats progress_stats,
                           bool save_progress,
                           bool delete_deck);
    bool CommitPreparedDeckLocked(DeckStore&& prepared,
                                  const std::string& deck_id,
                                  std::vector<ReviewState>&& states,
                                  VocabularyDailyStats stats);
    void RunDeckSyncTask(uint32_t request_id, bool download, RemoteDeckEntry entry);
    void StartSettingsBackgroundRefreshLocked();
    void StartOtaCheckTaskLocked();
    void StartOtaTaskLocked();
    void StartOtaTaskLocked(bool start_update);
    bool IsCurrentOtaRequest(uint32_t request_id) const;
    void RunOtaTask(uint32_t request_id, bool start_update);
    void UpdateLowBatteryAlertLocked(VocabularySnapshot* snapshot);
    void ApplyLowBatteryPopupLocked(VocabularySnapshot* snapshot) const;
    void MarkUserActivityLocked();
    void HandleButtonLocked(VocabularyButton button);
    void UpdateAiWordContextLocked();
    void OpenAiOverlayLocked();
    void CloseAiOverlayLocked();
    void StartAiEntitlementCheckLocked();
    void RunAiEntitlementCheck(uint32_t request_id);
    void StartOnlineWifiLocked(const char* status);
    void StartTemporaryPronunciationWifiLocked();
    void StopStationForOfflineLocked();
    void RestoreWifiAfterNetworkTaskLocked();
    bool EnsureMaimemoWifiForUserActionLocked();
    void EnableMaimemoModeLocked();
    void DisableMaimemoModeLocked();
    void ResetMaimemoSessionLocked(bool clear_card);
    void ArmMaimemoWifiIdleTimerLocked();
    void DisarmMaimemoWifiIdleTimerLocked();
    void HandleWifiIdleTimeout();
    void HandleMaimemoButtonLocked(VocabularyButton button);
    void QueuePendingMaimemoButtonLocked(VocabularyButton button, const char* status);
    void ReplayPendingMaimemoButtonLocked();
    void StartMaimemoFetchLocked(bool back, bool reveal_review = false);
    void StartMaimemoFetchAvoidingLocked(bool back,
                                         std::string avoid_voc_id,
                                         std::string avoid_word);
    void StartMaimemoSubmitLocked(MaimemoAnswer answer);
    void StartMaimemoPendingSyncLocked();
    bool HandleMaimemoOfflineButtonLocked(VocabularyButton button);
    bool LoadMaimemoOfflineItemLocked(size_t index,
                                      const char* status,
                                      bool reveal_review = false);
    const MaimemoReviewMark* FindMaimemoReviewMarkLocked(const MaimemoWordCard& card) const;
    void RecordMaimemoReviewMarkLocked(const MaimemoWordCard& card, MaimemoAnswer answer);
    void CacheMaimemoCardLocked(const MaimemoWordCard& card);
    void MergeMaimemoTodayItemsLocked(std::vector<MaimemoTodayItem> items);
    void MoveMaimemoPendingToConflictsLocked();
    bool SaveMaimemoOfflineStateLocked();
    bool SaveMaimemoPendingAnswersLocked();
    void ScheduleMaimemoPersistenceLocked(bool save_full_state,
                                          bool save_pending_answers);
    void StartMaimemoPersistenceTaskLocked();
    void RunMaimemoPersistenceTask(uint32_t request_id,
                                   MaimemoOfflineState state,
                                   bool save_full_state,
                                   bool save_pending_answers);
    bool IsCurrentMaimemoRequest(uint32_t request_id) const;
    void RunMaimemoTask(uint32_t request_id,
                        uint8_t action,
                        MaimemoAnswer answer,
                        std::string voc_id,
                        std::string word,
                        uint32_t recall_duration_ms,
                        uint32_t study_duration_ms,
                        bool back,
                        std::string operation_id,
                        bool refresh_snapshot);
    void WriteNfcEntryLink();
    void PlayCurrentPronunciationLocked();
    void PreloadCurrentPronunciationLocked();
    void MaybeAutoPlayCurrentPronunciationLocked();
    void PlayMaimemoPronunciationLocked();
    std::string MaimemoPronunciationUrlLocked() const;
    std::string NextOfflinePronunciationWordLocked() const;
    void StartPronunciationTaskLocked(const std::string& deck_id,
                                      const std::string& word,
                                      int voice_type,
                                      const std::string& pronunciation_url = "",
                                      const std::string& prefetch_word = "",
                                      const std::string& prefetch_url = "",
                                      bool play_audio = true);
    void RunPronunciationTask(uint32_t request_id,
                              int voice_type,
                              std::string deck_id,
                              std::string word,
                              std::string pronunciation_url,
                              std::string prefetch_word,
                              std::string prefetch_url,
                              bool play_audio);
    bool IsCurrentPronunciationRequest(uint32_t request_id) const;
    bool PlayLocalPronunciation(const std::string& deck_id,
                                const std::string& word,
                                bool play_missing_cue = true);
    bool PlayWavPronunciationLocked(const std::string& wav);
    void PlayMissingPronunciationCueLocked();
    void PlayCueLocked(bool known, bool complete);
    void StartRtcTimeSyncLocked();
    void RunRtcTimeSyncTask();
    static void WifiIdleTimerEntry(void* arg);
    static void ProgressSaveTimerEntry(void* arg);
    static void ActionStatusTimerEntry(void* arg);
    static void ProgressSaveTaskEntry(void* arg);
    static void PronunciationTaskEntry(void* arg);
    static void MaimemoTaskEntry(void* arg);
    static void MaimemoPersistenceTaskEntry(void* arg);
    static void DeckSyncTaskEntry(void* arg);
    static void DeckSwitchTaskEntry(void* arg);
    static void OtaTaskEntry(void* arg);
    static void AiEntitlementTaskEntry(void* arg);
    static void RtcTimeSyncTaskEntry(void* arg);

    Display* display_ = nullptr;
    DeckStore deck_store_;
    ProgressStore progress_store_;
    ReviewScheduler scheduler_;
    SyncClient sync_client_;
    MaimemoStudyClient maimemo_client_;
    MaimemoOpenApiClient maimemo_open_api_client_;
    MaimemoOfflineStore maimemo_offline_store_;
    MaimemoOfflineState maimemo_offline_state_;
    std::vector<RemoteDeckEntry> remote_decks_;
    std::vector<ReviewState> states_;
    std::vector<size_t> queue_;
    VocabularySettings settings_;
    VocabularyDailyStats stats_;
    bool ai_overlay_active_ = false;
    int8_t ai_page_delta_ = 0;
    uint32_t ai_page_request_id_ = 0;
    uint32_t last_ai_navigation_request_id_ = 0;
    SettingsView settings_view_ = SettingsView::kMain;
    SettingsView settings_popup_return_view_ = SettingsView::kMain;
    SettingsPopupAction settings_popup_action_ = SettingsPopupAction::kNone;
    VocabularyStudyMode popup_study_mode_ = VocabularyStudyMode::kOffline;
    VocabularyOrderMode popup_order_mode_ = VocabularyOrderMode::kSequential;
    VocabularyVoiceType popup_voice_type_ = VocabularyVoiceType::kAmerican;
    VocabularyFontMode popup_font_mode_ = VocabularyFontMode::kClear;
    VocabularyAutoSleepTimeout popup_auto_sleep_timeout_ = VocabularyAutoSleepTimeout::k3Hours;
    uint16_t popup_daily_target_ = 20;
    uint8_t popup_output_volume_ = 70;
    bool popup_auto_pronunciation_ = false;
    bool popup_ai_enabled_ = false;
    bool ai_entitlement_checking_ = false;
    std::string ai_entitlement_status_;
    size_t current_word_index_ = 0;
    size_t pending_current_word_index_ = 0;
    size_t saved_current_word_index_ = 0;
    uint8_t current_index_moves_since_save_ = 0;
    size_t queue_pos_ = 0;
    bool showing_back_ = false;
    bool current_index_dirty_ = false;
    bool progress_dirty_ = false;
    bool progress_save_busy_ = false;
    bool onboarding_active_ = false;
    bool settings_open_ = false;
    uint8_t settings_selection_ = 0;
    size_t settings_local_deck_index_ = 0;
    size_t settings_online_deck_index_ = 0;
    int32_t today_ = 0;
    std::atomic<uint32_t> pronunciation_request_{0};
    std::atomic<bool> pronunciation_cancel_requested_{false};
    std::atomic<uint32_t> progress_save_request_{0};
    std::atomic<uint32_t> maimemo_request_{0};
    std::atomic<uint32_t> maimemo_persistence_request_{0};
    std::atomic<uint32_t> deck_sync_request_{0};
    std::atomic<uint32_t> ota_request_{0};
    std::atomic<uint32_t> ai_entitlement_request_{0};
    std::atomic<bool> rtc_time_sync_busy_{false};
    bool rtc_time_repair_pending_ = false;
    bool deck_sync_busy_ = false;
    int deck_sync_progress_ = -1;
    bool online_delete_confirm_ = false;
    DeckSyncUiState deck_sync_ui_state_ = DeckSyncUiState::kIdle;
    std::string deck_sync_status_ = "未获取";
    std::string deck_sync_detail_;
    bool ota_busy_ = false;
    std::string ota_status_ = "待检查升级";
    bool low_battery_popup_open_ = false;
    bool low_battery_alert_dismissed_ = false;
    esp_timer_handle_t wifi_idle_timer_ = nullptr;
    esp_timer_handle_t progress_save_timer_ = nullptr;
    esp_timer_handle_t action_status_timer_ = nullptr;
    std::string action_status_;
    int64_t last_activity_ms_ = 0;
    int64_t offline_pronunciation_wifi_deadline_ms_ = 0;
    bool offline_pronunciation_wifi_active_ = false;
    bool pronunciation_busy_ = false;
    bool pronunciation_play_audio_ = false;
    bool maimemo_wifi_suspended_ = false;
    int64_t maimemo_wake_wifi_start_ms_ = 0;
    bool wake_wifi_restore_pending_ = false;
    MaimemoWordCard maimemo_card_;
    bool maimemo_has_card_ = false;
    bool maimemo_showing_back_ = false;
    bool maimemo_revealing_review_ = false;
    bool maimemo_busy_ = false;
    bool maimemo_persistence_busy_ = false;
    bool maimemo_full_state_dirty_ = false;
    bool maimemo_pending_answers_dirty_ = false;
    bool maimemo_submit_waiting_for_pronunciation_ = false;
    MaimemoAnswer maimemo_pending_submit_answer_ = MaimemoAnswer::kFamiliar;
    int64_t maimemo_submit_started_ms_ = 0;
    bool maimemo_pending_reveal_ = false;
    std::deque<VocabularyButton> maimemo_pending_buttons_;
    int64_t maimemo_recall_started_ms_ = 0;
    int64_t maimemo_study_started_ms_ = 0;
    std::string maimemo_status_ = "墨墨未连接";
    mutable std::mutex mutex_;
    mutable std::mutex published_mutex_;
    VocabularySnapshot snapshot_;
    std::string published_deck_id_;
    std::string published_deck_name_;
    std::string published_deck_language_;
    uint16_t published_deck_version_ = 1;
    size_t published_deck_word_count_ = 0;
};

#endif  // VOCABULARY_APP_H
