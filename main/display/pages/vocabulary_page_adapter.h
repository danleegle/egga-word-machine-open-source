#ifndef VOCABULARY_PAGE_ADAPTER_H
#define VOCABULARY_PAGE_ADAPTER_H

#include "ui_page.h"
#include "vocabulary/vocabulary_types.h"

#include <string>

class LcdDisplay;

class VocabularyPageAdapter : public IUiPage {
public:
    explicit VocabularyPageAdapter(LcdDisplay* host);

    UiPageId Id() const override;
    const char* Name() const override;
    void Build() override;
    lv_obj_t* Screen() const override;
    void OnShow() override;
    bool HandleEvent(const UiPageEvent& event) override;

    void UpdateSnapshot(const VocabularySnapshot& snapshot);

private:
    void ApplyWelcome();
    void ApplyCardFront();
    void ApplyCardBack();
    void ApplySummary();
    void ApplySettings();
    void UpdatePopupOverlay(const lv_font_t* font);
    void UpdateSettingsPanel(const lv_font_t* font);
    void UpdateStatusIcons();
    void UpdateAiOverlay();
    void ScheduleAiPageTurn();
    void CancelAiPageTurn();
    static void AiPageTurnTimerCallback(lv_timer_t* timer);

    static constexpr size_t kSettingsGroupCount = 4;
    static constexpr size_t kSettingsVisibleRows = 4;
    static constexpr size_t kWifiBarCount = 4;
    static constexpr size_t kBatteryCellCount = 5;

    LcdDisplay* host_ = nullptr;
    bool built_ = false;
    VocabularySnapshot snapshot_;
    const lv_font_t* content_font_ = nullptr;
    const lv_font_t* ui_font_ = nullptr;
    bool ai_page_follow_latest_ = true;
    size_t ai_page_index_ = 0;
    uint32_t last_ai_page_request_id_ = 0;
    bool ai_page_turn_pending_ = false;
    lv_timer_t* ai_page_turn_timer_ = nullptr;
    std::string ai_rendered_page_text_;

    lv_obj_t* screen_ = nullptr;
    lv_obj_t* sleep_overlay_ = nullptr;
    lv_obj_t* header_label_ = nullptr;
    lv_obj_t* status_group_ = nullptr;
    lv_obj_t* clock_label_ = nullptr;
    lv_obj_t* progress_label_ = nullptr;
    lv_obj_t* wifi_icon_ = nullptr;
    lv_obj_t* wifi_bars_[kWifiBarCount] = {};
    lv_obj_t* battery_icon_ = nullptr;
    lv_obj_t* battery_cells_[kBatteryCellCount] = {};
    lv_obj_t* word_label_ = nullptr;
    lv_obj_t* review_mark_label_ = nullptr;
    lv_obj_t* action_status_label_ = nullptr;
    lv_obj_t* wifi_setup_panel_ = nullptr;
    lv_obj_t* wifi_qrcode_ = nullptr;
    lv_obj_t* wifi_text_label_ = nullptr;
    lv_obj_t* phonetic_label_ = nullptr;
    lv_obj_t* meaning_label_ = nullptr;
    lv_obj_t* example_label_ = nullptr;
    lv_obj_t* translation_label_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* hint_label_ = nullptr;
    lv_obj_t* footer_label_ = nullptr;
    lv_obj_t* ai_panel_ = nullptr;
    lv_obj_t* ai_status_label_ = nullptr;
    lv_obj_t* ai_question_label_ = nullptr;
    lv_obj_t* ai_answer_label_ = nullptr;
    lv_obj_t* settings_panel_ = nullptr;
    lv_obj_t* settings_sidebar_items_[kSettingsGroupCount] = {};
    lv_obj_t* settings_sidebar_labels_[kSettingsGroupCount] = {};
    lv_obj_t* settings_splitter_ = nullptr;
    lv_obj_t* settings_row_[kSettingsVisibleRows] = {};
    lv_obj_t* settings_row_label_[kSettingsVisibleRows] = {};
    lv_obj_t* settings_row_value_[kSettingsVisibleRows] = {};
    lv_obj_t* settings_popup_ = nullptr;
    lv_obj_t* settings_popup_title_ = nullptr;
    lv_obj_t* settings_popup_badge_ = nullptr;
    lv_obj_t* settings_popup_body_ = nullptr;
    lv_obj_t* settings_popup_progress_ = nullptr;
    lv_obj_t* settings_popup_hint_ = nullptr;
};

#endif  // VOCABULARY_PAGE_ADAPTER_H
