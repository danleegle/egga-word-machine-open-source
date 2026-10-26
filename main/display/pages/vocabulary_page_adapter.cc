#include "pages/vocabulary_page_adapter.h"

#include "ai/ai_dialog_service.h"
#include "lcd_display.h"
#include "lvgl.h"
#include "lvgl_theme.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <esp_log.h>
#include <esp_timer.h>
#include <string>
#include <vector>

LV_FONT_DECLARE(DejaVuSans_Phonetic_16);
LV_FONT_DECLARE(CalibriLight_Eink_36);
LV_FONT_DECLARE(Zfull_GB_16);
LV_FONT_DECLARE(Zfull_GB_20);

namespace {

constexpr char kAiPageTag[] = "AiPage";
constexpr lv_coord_t kPageWidth = 400;
constexpr lv_coord_t kPageHeight = 300;
constexpr lv_coord_t kFrameInset = 1;
constexpr lv_coord_t kFrameRadius = 12;
constexpr lv_coord_t kFrameWidth = kPageWidth - kFrameInset * 2;
constexpr lv_coord_t kFrameHeight = kPageHeight - kFrameInset * 2;
constexpr lv_coord_t kHeaderHeight = 36;
constexpr lv_coord_t kDividerHeight = 1;
constexpr lv_coord_t kContentHeight = kFrameHeight - kHeaderHeight - kDividerHeight;
// Reserve a permanent HH:MM slot and keep enough room for six 16 px CJK
// status glyphs beside the battery.
constexpr lv_coord_t kStatusGroupWidth = 200;
constexpr lv_coord_t kStatusGroupHeight = 30;
constexpr lv_coord_t kWifiIconWidth = 24;
constexpr lv_coord_t kBatteryIconWidth = 31;
constexpr lv_coord_t kClockLabelWidth = 46;
constexpr lv_coord_t kStatusIconGap = 8;
constexpr lv_coord_t kFooterHeight = 28;
constexpr lv_coord_t kCardBottomPad = 4;
constexpr lv_coord_t kMeaningTwoLineHeight = 48;
// Three 20 px meaning lines plus the two 2 px line gaps. Removing the static
// footer only buys one additional line on this 300 px panel, so keep it bounded.
constexpr lv_coord_t kMeaningThreeLineHeight = 64;
constexpr lv_coord_t kSettingsPanelHeight = kContentHeight - kFooterHeight - 4;
constexpr lv_coord_t kSettingsSidebarLabelWidth = 78;
constexpr lv_coord_t kSettingsSidebarLabelHeight = 34;
constexpr lv_coord_t kSettingsSidebarX = 4;
constexpr lv_coord_t kSettingsSidebarY = 26;
constexpr lv_coord_t kSettingsSidebarGap = 16;
constexpr lv_coord_t kSettingsSplitterX = 100;
constexpr lv_coord_t kSettingsSplitterY = 20;
constexpr lv_coord_t kSettingsRowX = 114;
constexpr lv_coord_t kSettingsRowY = 22;
constexpr lv_coord_t kSettingsRowWidth = kFrameWidth - 142;
constexpr lv_coord_t kSettingsRowHeight = 36;
constexpr lv_coord_t kSettingsRowGap = 14;
constexpr lv_coord_t kSettingsValueWidth = 118;
constexpr lv_coord_t kPhoneticLiftY = -12;
constexpr lv_coord_t kWifiSetupPanelHeight = 154;
constexpr lv_coord_t kWifiQrSize = 108;
constexpr lv_coord_t kWifiTextWidth = 236;
constexpr lv_coord_t kAiPanelX = 15;
constexpr lv_coord_t kAiPanelY = 112;
constexpr lv_coord_t kAiPanelWidth = 370;
constexpr lv_coord_t kAiPanelHeight = 178;
constexpr lv_coord_t kAiAnswerWidth = 346;
constexpr lv_coord_t kAiAnswerHeight = 136;
constexpr uint8_t kHintMaxLines = 3;
constexpr lv_coord_t kHintLineSpace = 2;

struct SettingsGroup {
    const char* label;
    uint8_t items[5];
    uint8_t count;
};

constexpr SettingsGroup kSettingsGroups[] = {
    {"学习", {0, 1, 2, 3, 0}, 4},
    {"发音", {9, 4, 8, 0, 0}, 3},
    {"联网", {10, 6, 0, 0, 0}, 2},
    {"系统", {5, 11, 12, 13, 7}, 5},
};

void StyleScreen(lv_obj_t* obj) {
    lv_obj_set_size(obj, kPageWidth, kPageHeight);
    lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

void StyleFramePanel(lv_obj_t* obj) {
    lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_black(), 0);
    lv_obj_set_style_radius(obj, kFrameRadius, 0);
    lv_obj_set_style_clip_corner(obj, true, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

void StyleHeaderPanel(lv_obj_t* obj) {
    lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_left(obj, 10, 0);
    lv_obj_set_style_pad_right(obj, 10, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

void StyleContentPanel(lv_obj_t* obj) {
    lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_left(obj, 10, 0);
    lv_obj_set_style_pad_right(obj, 10, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

void StyleDivider(lv_obj_t* obj) {
    lv_obj_set_style_bg_color(obj, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

void StyleWordLabel(lv_obj_t* obj) {
    lv_obj_set_width(obj, LV_PCT(100));
    lv_obj_set_height(obj, 46);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(obj, 0, 0);
}

void StyleContentLabel(lv_obj_t* obj, const lv_font_t* font = nullptr) {
    if (font != nullptr) {
        lv_obj_set_style_text_font(obj, font, 0);
    }
    lv_obj_set_width(obj, LV_PCT(100));
    lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_line_space(obj, 2, 0);
    lv_obj_set_style_pad_left(obj, 0, 0);
    lv_obj_set_style_pad_top(obj, 0, 0);
}

void StyleHintLabel(lv_obj_t* obj, const lv_font_t* font = nullptr) {
    StyleContentLabel(obj, font);
    lv_obj_set_style_margin_top(obj, 3, 0);
    lv_obj_set_height(obj, LV_SIZE_CONTENT);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
}

void StylePlainObject(lv_obj_t* obj);

void StyleSettingsPanel(lv_obj_t* obj) {
    StylePlainObject(obj);
    lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
}

void StyleSettingsCategoryItem(lv_obj_t* obj) {
    StylePlainObject(obj);
    lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(obj, 9, 0);
}

void StyleSettingsCategoryLabel(lv_obj_t* obj, const lv_font_t* font = nullptr) {
    StylePlainObject(obj);
    if (font != nullptr) {
        lv_obj_set_style_text_font(obj, font, 0);
    }
    lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_CENTER, 0);
}

void StyleSettingsRow(lv_obj_t* obj) {
    StylePlainObject(obj);
    lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, lv_color_black(), 0);
    lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_pad_left(obj, 6, 0);
    lv_obj_set_style_pad_right(obj, 6, 0);
}

void StyleSettingsRowLabel(lv_obj_t* obj, const lv_font_t* font = nullptr) {
    StylePlainObject(obj);
    if (font != nullptr) {
        lv_obj_set_style_text_font(obj, font, 0);
    }
    lv_label_set_long_mode(obj, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, 0);
}

void StylePopupPanel(lv_obj_t* obj) {
    lv_obj_set_style_bg_color(obj, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 2, 0);
    lv_obj_set_style_border_color(obj, lv_color_black(), 0);
    lv_obj_set_style_radius(obj, kFrameRadius, 0);
    lv_obj_set_style_clip_corner(obj, true, 0);
    lv_obj_set_style_pad_all(obj, 12, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

void StylePlainObject(lv_obj_t* obj) {
    lv_obj_remove_style_all(obj);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
}

void StyleWifiSetupPanel(lv_obj_t* obj) {
    StylePlainObject(obj);
    lv_obj_set_width(obj, LV_PCT(100));
    lv_obj_set_height(obj, kWifiSetupPanelHeight);
    lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(obj, 18, 0);
}

void StyleWifiTextLabel(lv_obj_t* obj, const lv_font_t* font = nullptr) {
    StyleContentLabel(obj, font);
    lv_obj_set_width(obj, kWifiTextWidth);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(obj, 5, 0);
    lv_obj_set_style_text_align(obj, LV_TEXT_ALIGN_LEFT, 0);
}

void StyleStatusIconContainer(lv_obj_t* obj) {
    StylePlainObject(obj);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
}

lv_obj_t* CreateBlackRect(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h) {
    lv_obj_t* obj = lv_obj_create(parent);
    StylePlainObject(obj);
    lv_obj_set_style_bg_color(obj, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_pos(obj, x, y);
    return obj;
}

void BuildSleepMoon(lv_obj_t* parent) {
    lv_obj_t* disc = lv_obj_create(parent);
    StylePlainObject(disc);
    lv_obj_set_style_bg_color(disc, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_size(disc, 18, 18);
    lv_obj_set_pos(disc, 1, 5);

    lv_obj_t* cutout = lv_obj_create(parent);
    StylePlainObject(cutout);
    lv_obj_set_style_bg_color(cutout, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(cutout, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cutout, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_size(cutout, 15, 15);
    lv_obj_set_pos(cutout, 7, 2);
}

void SetLabel(lv_obj_t* label, const char* text) {
    if (label != nullptr) {
        lv_label_set_text(label, text != nullptr ? text : "");
    }
}

std::string TruncateHintText(const char* text,
                             const lv_font_t* font,
                             lv_coord_t width,
                             lv_coord_t height) {
    const std::string source = text != nullptr ? text : "";
    if (source.empty() || font == nullptr || width <= 0 || height <= 0) {
        return source;
    }

    auto fits = [&](const std::string& candidate) {
        lv_point_t size = {};
        lv_text_get_size(&size,
                         candidate.c_str(),
                         font,
                         0,
                         kHintLineSpace,
                         width,
                         LV_TEXT_FLAG_NONE);
        return size.y <= height;
    };
    if (fits(source)) {
        return source;
    }

    std::vector<size_t> offsets;
    offsets.reserve(source.size() + 1);
    for (size_t offset = 0; offset < source.size();) {
        offsets.push_back(offset);
        const unsigned char first = static_cast<unsigned char>(source[offset]);
        size_t length = 1;
        if ((first & 0xE0) == 0xC0) length = 2;
        else if ((first & 0xF0) == 0xE0) length = 3;
        else if ((first & 0xF8) == 0xF0) length = 4;
        offset += std::min(length, source.size() - offset);
    }
    offsets.push_back(source.size());

    size_t low = 0;
    size_t high = offsets.size() - 1;
    size_t best = 0;
    while (low <= high) {
        const size_t mid = low + (high - low) / 2;
        const std::string candidate = source.substr(0, offsets[mid]) + "。";
        if (fits(candidate)) {
            best = mid;
            low = mid + 1;
        } else {
            if (mid == 0) break;
            high = mid - 1;
        }
    }

    std::string prefix = source.substr(0, offsets[best]);
    const size_t minimum_natural_end = prefix.size() / 2;
    size_t natural_end = std::string::npos;
    size_t natural_length = 0;
    for (const char* ending : {"。", "！", "？", "!", "?"}) {
        const size_t pos = prefix.rfind(ending);
        if (pos != std::string::npos && pos >= minimum_natural_end &&
            (natural_end == std::string::npos || pos > natural_end)) {
            natural_end = pos;
            natural_length = strlen(ending);
        }
    }
    if (natural_end != std::string::npos) {
        return prefix.substr(0, natural_end + natural_length);
    }

    for (const char* separator : {"；", ";", "，", ",", "、", "\n", " "}) {
        const size_t pos = prefix.rfind(separator);
        if (pos != std::string::npos && pos >= minimum_natural_end &&
            (natural_end == std::string::npos || pos > natural_end)) {
            natural_end = pos;
        }
    }
    if (natural_end != std::string::npos) {
        prefix.resize(natural_end);
    }
    while (!prefix.empty() &&
           (prefix.back() == ' ' || prefix.back() == '\r' || prefix.back() == '\n' ||
            prefix.back() == '\t')) {
        prefix.pop_back();
    }
    return prefix.empty() ? std::string() : prefix + "。";
}

void SetTruncatedHintLabel(lv_obj_t* label, const char* text, const lv_font_t* font) {
    if (label == nullptr) {
        return;
    }

    // Let flex layout place the full label first, then use only the complete
    // lines that still fit inside the card. LVGL adds dots when text remains.
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    SetLabel(label, text);

    lv_obj_t* parent = lv_obj_get_parent(label);
    if (parent == nullptr || font == nullptr) {
        return;
    }
    lv_obj_update_layout(parent);

    lv_area_t label_coords = {};
    lv_area_t content_coords = {};
    lv_obj_get_coords(label, &label_coords);
    lv_obj_get_content_coords(parent, &content_coords);
    const lv_coord_t available_height = content_coords.y2 - label_coords.y1 + 1;
    const lv_coord_t line_height = font->line_height;
    if (available_height < line_height) {
        lv_obj_set_height(label, 0);
        return;
    }

    const uint8_t available_lines = static_cast<uint8_t>(
        (available_height + kHintLineSpace) / (line_height + kHintLineSpace));
    const uint8_t visible_lines = std::min(kHintMaxLines, available_lines);
    const lv_coord_t visible_height =
        visible_lines * line_height + (visible_lines - 1) * kHintLineSpace;
    const std::string visible_text =
        TruncateHintText(text, font, lv_obj_get_content_width(label), visible_height);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    SetLabel(label, visible_text.c_str());
}

void SetVisible(lv_obj_t* obj, bool visible) {
    if (obj == nullptr) {
        return;
    }
    if (visible) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

void ClampLabelHeight(lv_obj_t* label, lv_coord_t max_height) {
    if (label == nullptr || max_height <= 0) {
        return;
    }
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_height(label, LV_SIZE_CONTENT);
    lv_obj_update_layout(label);
    if (lv_obj_get_height(label) > max_height) {
        lv_obj_set_height(label, max_height);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    }
}

struct MeaningPresentation {
    std::string text;
    bool horizontal = false;
};

MeaningPresentation FormatMeaningForCardBack(const char* text) {
    MeaningPresentation presentation;
    const std::string source = text != nullptr ? text : "";
    presentation.text = source;

    std::vector<std::string> entries;
    for (size_t start = 0; start <= source.size();) {
        const size_t newline = source.find('\n', start);
        std::string entry = source.substr(
            start,
            newline == std::string::npos ? std::string::npos : newline - start);
        size_t first = 0;
        while (first < entry.size() &&
               (entry[first] == ' ' || entry[first] == '\t' || entry[first] == '\r')) {
            ++first;
        }
        size_t last = entry.size();
        while (last > first &&
               (entry[last - 1] == ' ' || entry[last - 1] == '\t' || entry[last - 1] == '\r')) {
            --last;
        }
        if (last > first) {
            entries.push_back(entry.substr(first, last - first));
        }
        if (newline == std::string::npos) {
            break;
        }
        start = newline + 1;
    }

    if (entries.size() < 3) {
        return presentation;
    }

    presentation.text.clear();
    for (const std::string& entry : entries) {
        if (!presentation.text.empty()) {
            presentation.text += "  ";
        }
        presentation.text += entry;
    }
    presentation.horizontal = true;
    return presentation;
}

std::vector<std::string> PaginateText(const char* text,
                                      const lv_font_t* font,
                                      lv_coord_t width,
                                      lv_coord_t height,
                                      int32_t line_space) {
    const std::string source = text != nullptr ? text : "";
    if (source.empty() || font == nullptr) {
        return {source};
    }

    auto fits = [&](const std::string& candidate) {
        lv_point_t size = {};
        lv_text_get_size(&size,
                         candidate.c_str(),
                         font,
                         0,
                         line_space,
                         width,
                         LV_TEXT_FLAG_NONE);
        return size.y <= height;
    };
    std::vector<size_t> character_offsets;
    character_offsets.reserve(source.size() + 1);
    for (size_t offset = 0; offset < source.size();) {
        character_offsets.push_back(offset);
        const unsigned char first = static_cast<unsigned char>(source[offset]);
        size_t length = 1;
        if ((first & 0xE0) == 0xC0) length = 2;
        else if ((first & 0xF0) == 0xE0) length = 3;
        else if ((first & 0xF8) == 0xF0) length = 4;
        offset += std::min(length, source.size() - offset);
    }
    character_offsets.push_back(source.size());

    std::vector<std::string> pages;
    size_t start = 0;
    const size_t character_count = character_offsets.size() - 1;
    auto is_character = [&](size_t index, const char* value) {
        if (index >= character_count) return false;
        const size_t begin = character_offsets[index];
        const size_t end = character_offsets[index + 1];
        return source.compare(begin, end - begin, value) == 0;
    };
    auto is_sentence_boundary = [&](size_t split) {
        if (split == 0 || split > character_count) return false;
        const size_t last = split - 1;
        if (is_character(last, "。") || is_character(last, "！") ||
            is_character(last, "？") || is_character(last, "；") ||
            is_character(last, ".") || is_character(last, "!") ||
            is_character(last, "?") || is_character(last, ";") ||
            is_character(last, "\n")) {
            return true;
        }
        // Keep a closing quote on the same page as the sentence punctuation.
        if ((is_character(last, "”") || is_character(last, "’") ||
             is_character(last, "\"") || is_character(last, "'")) &&
            last > 0) {
            return is_character(last - 1, "。") || is_character(last - 1, "！") ||
                is_character(last - 1, "？") || is_character(last - 1, ".") ||
                is_character(last - 1, "!") || is_character(last - 1, "?");
        }
        return false;
    };
    while (start < character_count) {
        size_t low = start + 1;
        size_t high = character_count;
        size_t best = start + 1;
        while (low <= high) {
            const size_t mid = low + (high - low) / 2;
            const std::string candidate = source.substr(
                character_offsets[start],
                character_offsets[mid] - character_offsets[start]);
            if (fits(candidate)) {
                best = mid;
                low = mid + 1;
            } else {
                if (mid == 0) break;
                high = mid - 1;
            }
        }
        if (best < character_count) {
            // Do not split a normal sentence across e-paper pages. The AI TTS
            // stream is synchronized at sentence boundaries, so character-level
            // page breaks can otherwise show page 2 while the speaker is still
            // reading the visible final sentence on page 1.
            const size_t minimum_sentence_break =
                start + std::max<size_t>(1, (best - start) / 2);
            for (size_t split = best; split > minimum_sentence_break; --split) {
                if (is_sentence_boundary(split)) {
                    best = split;
                    break;
                }
            }
        }
        pages.push_back(source.substr(character_offsets[start],
                                      character_offsets[best] - character_offsets[start]));
        start = best;
    }
    if (pages.empty()) {
        pages.push_back(source);
    }
    return pages;
}

bool IsActionStatusOnlyUpdate(const VocabularySnapshot& previous,
                              const VocabularySnapshot& next) {
    // The Maimemo status line is hidden on word cards. Treat its background
    // transition ("syncing" -> "submitting") together with the feedback update,
    // so we do not redraw the entire card just to show the top-right state.
    VocabularySnapshot before = previous;
    VocabularySnapshot after = next;
    memset(before.action_status, 0, sizeof(before.action_status));
    memset(after.action_status, 0, sizeof(after.action_status));
    memset(before.status_line, 0, sizeof(before.status_line));
    memset(after.status_line, 0, sizeof(after.status_line));
    return memcmp(&before, &after, sizeof(VocabularySnapshot)) == 0;
}

const char* SettingsItemLabel(uint8_t index) {
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

size_t SettingsGroupIndexForItem(uint8_t item_index) {
    for (size_t group = 0; group < sizeof(kSettingsGroups) / sizeof(kSettingsGroups[0]); ++group) {
        for (uint8_t row = 0; row < kSettingsGroups[group].count; ++row) {
            if (kSettingsGroups[group].items[row] == item_index) {
                return group;
            }
        }
    }
    return 0;
}

const lv_font_t* ContentFont() {
    return &Zfull_GB_16;
}

const lv_font_t* UiFont() {
    static lv_font_t font;
    static bool initialized = false;
    if (!initialized) {
        font = Zfull_GB_16;
        // ZFULL intentionally focuses on low-resolution CJK/ASCII glyphs.
        // AI answers can contain IPA, so preserve those glyphs with the same
        // phonetic fallback used by the dedicated pronunciation line.
        font.fallback = &DejaVuSans_Phonetic_16;
        initialized = true;
    }
    return &font;
}

const lv_font_t* ExampleFont() {
    static lv_font_t font;
    static bool initialized = false;
    if (!initialized) {
        font = Zfull_GB_20;
        font.fallback = &Zfull_GB_16;
        initialized = true;
    }
    return &font;
}

}  // namespace

VocabularyPageAdapter::VocabularyPageAdapter(LcdDisplay* host)
    : host_(host) {}

UiPageId VocabularyPageAdapter::Id() const {
    return UiPageId::Vocabulary;
}

const char* VocabularyPageAdapter::Name() const {
    return "Vocabulary";
}

void VocabularyPageAdapter::Build() {
    if (built_ || host_ == nullptr) {
        built_ = true;
        return;
    }
    if (host_->vocabulary_screen_ != nullptr) {
        screen_ = host_->vocabulary_screen_;
        built_ = true;
        return;
    }

    auto* lvgl_theme = static_cast<LvglTheme*>(host_->current_theme_);
    const lv_font_t* text_font = lvgl_theme->text_font() ? lvgl_theme->text_font()->font() : nullptr;
    const lv_font_t* body_font = lvgl_theme->reminder_text_font()
        ? lvgl_theme->reminder_text_font()->font()
        : text_font;
    const lv_font_t* word_font = &CalibriLight_Eink_36;
    content_font_ = ContentFont();
    ui_font_ = UiFont();
    const lv_font_t* content_font = content_font_;
    const lv_font_t* ui_font = ui_font_ != nullptr ? ui_font_ : body_font;
    const lv_font_t* header_font = ui_font != nullptr ? ui_font : text_font;
    const lv_font_t* phonetic_font = &DejaVuSans_Phonetic_16;

    screen_ = lv_obj_create(nullptr);
    host_->vocabulary_screen_ = screen_;
    StyleScreen(screen_);

    sleep_overlay_ = lv_obj_create(screen_);
    StylePlainObject(sleep_overlay_);
    lv_obj_set_size(sleep_overlay_, kPageWidth, kPageHeight);

    // Replace the inactive Wi-Fi indicator with a compact sleep marker while
    // leaving the last useful page visible, matching common e-paper behavior.
    lv_obj_t* sleep_moon = lv_obj_create(sleep_overlay_);
    StylePlainObject(sleep_moon);
    lv_obj_set_style_bg_color(sleep_moon, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(sleep_moon, LV_OPA_COVER, 0);
    lv_obj_set_size(sleep_moon, 28, 28);
    lv_obj_align(sleep_moon, LV_ALIGN_TOP_LEFT, 5, 4);
    BuildSleepMoon(sleep_moon);

    lv_obj_t* sleep_status_bar = lv_obj_create(sleep_overlay_);
    StylePlainObject(sleep_status_bar);
    lv_obj_set_style_bg_color(sleep_status_bar, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(sleep_status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(sleep_status_bar, lv_color_black(), 0);
    lv_obj_set_style_border_side(sleep_status_bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(sleep_status_bar, 1, 0);
    lv_obj_set_size(sleep_status_bar, kFrameWidth - 2, 30);
    lv_obj_align(sleep_status_bar, LV_ALIGN_BOTTOM_MID, 0, -2);

    lv_obj_t* sleep_status = lv_label_create(sleep_status_bar);
    lv_obj_set_style_text_font(sleep_status, &Zfull_GB_16, 0);
    lv_label_set_text(sleep_status, "休眠中 · 按任意键唤醒");
    lv_obj_align(sleep_status, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(sleep_overlay_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* frame = lv_obj_create(screen_);
    StyleFramePanel(frame);
    lv_obj_set_size(frame, kFrameWidth, kFrameHeight);
    lv_obj_align(frame, LV_ALIGN_TOP_LEFT, kFrameInset, kFrameInset);

    lv_obj_t* header = lv_obj_create(frame);
    StyleHeaderPanel(header);
    lv_obj_set_size(header, kFrameWidth, kHeaderHeight);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t* divider = lv_obj_create(frame);
    StyleDivider(divider);
    lv_obj_set_size(divider, kFrameWidth, kDividerHeight);
    lv_obj_align(divider, LV_ALIGN_TOP_MID, 0, kHeaderHeight);

    header_label_ = lv_label_create(header);
    if (header_font) {
        lv_obj_set_style_text_font(header_label_, header_font, 0);
    }
    lv_obj_set_width(header_label_,
                     kFrameWidth - kWifiIconWidth - kStatusGroupWidth - kStatusIconGap * 2);
    lv_label_set_long_mode(header_label_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(header_label_, LV_ALIGN_LEFT_MID, kWifiIconWidth + kStatusIconGap, 0);

    wifi_icon_ = lv_obj_create(header);
    StyleStatusIconContainer(wifi_icon_);
    lv_obj_set_size(wifi_icon_, kWifiIconWidth, 18);
    lv_obj_align(wifi_icon_, LV_ALIGN_LEFT_MID, 0, 0);

    for (size_t i = 0; i < kWifiBarCount; ++i) {
        wifi_bars_[i] = lv_obj_create(wifi_icon_);
        StylePlainObject(wifi_bars_[i]);
        lv_obj_set_style_bg_color(wifi_bars_[i], lv_color_black(), 0);
        lv_obj_set_style_bg_opa(wifi_bars_[i], LV_OPA_COVER, 0);
        const lv_coord_t bar_w = 3;
        const lv_coord_t bar_h = 4 + static_cast<lv_coord_t>(i) * 4;
        lv_obj_set_size(wifi_bars_[i], bar_w, bar_h);
        lv_obj_align(wifi_bars_[i], LV_ALIGN_BOTTOM_LEFT, 1 + static_cast<lv_coord_t>(i) * 5, 0);
    }

    status_group_ = lv_obj_create(header);
    StylePlainObject(status_group_);
    lv_obj_set_size(status_group_, kStatusGroupWidth, kStatusGroupHeight);
    lv_obj_align(status_group_, LV_ALIGN_RIGHT_MID, 0, 0);

    clock_label_ = lv_label_create(status_group_);
    if (header_font) {
        lv_obj_set_style_text_font(clock_label_, header_font, 0);
    }
    lv_obj_set_width(clock_label_, kClockLabelWidth);
    lv_label_set_long_mode(clock_label_, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(clock_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(clock_label_, LV_ALIGN_LEFT_MID, 0, 0);

    progress_label_ = lv_label_create(status_group_);
    if (header_font) {
        lv_obj_set_style_text_font(progress_label_, header_font, 0);
    }
    lv_obj_set_width(progress_label_,
                     kStatusGroupWidth - kClockLabelWidth - kBatteryIconWidth -
                         kStatusIconGap * 2);
    lv_label_set_long_mode(progress_label_, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(progress_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(progress_label_, LV_ALIGN_LEFT_MID,
                 kClockLabelWidth + kStatusIconGap, 0);

    battery_icon_ = lv_obj_create(status_group_);
    StyleStatusIconContainer(battery_icon_);
    lv_obj_set_size(battery_icon_, kBatteryIconWidth, 18);
    lv_obj_align(battery_icon_, LV_ALIGN_RIGHT_MID, 0, 0);

    CreateBlackRect(battery_icon_, 0, 2, 27, 2);
    CreateBlackRect(battery_icon_, 0, 13, 27, 2);
    CreateBlackRect(battery_icon_, 0, 2, 2, 13);
    CreateBlackRect(battery_icon_, 25, 2, 2, 13);

    for (size_t i = 0; i < kBatteryCellCount; ++i) {
        battery_cells_[i] =
            CreateBlackRect(battery_icon_, 4 + static_cast<lv_coord_t>(i) * 4, 5, 3, 7);
    }

    CreateBlackRect(battery_icon_, 27, 5, 3, 7);

    lv_obj_t* card = lv_obj_create(frame);
    StyleContentPanel(card);
    lv_obj_set_size(card, kFrameWidth, kContentHeight);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, kHeaderHeight + kDividerHeight);
    lv_obj_set_style_pad_top(card, 8, 0);
    lv_obj_set_style_pad_bottom(card, kFooterHeight + kCardBottomPad, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    word_label_ = lv_label_create(card);
    if (word_font) {
        lv_obj_set_style_text_font(word_label_, word_font, 0);
    }
    StyleWordLabel(word_label_);

    review_mark_label_ = lv_label_create(card);
    StylePlainObject(review_mark_label_);
    if (ui_font) {
        lv_obj_set_style_text_font(review_mark_label_, ui_font, 0);
    }
    lv_obj_set_size(review_mark_label_, 76, 28);
    lv_label_set_long_mode(review_mark_label_, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(review_mark_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(review_mark_label_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(review_mark_label_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(review_mark_label_, 0, 0);
    lv_obj_set_style_radius(review_mark_label_, 0, 0);
    lv_obj_set_style_pad_all(review_mark_label_, 2, 0);
    lv_obj_add_flag(review_mark_label_, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(review_mark_label_, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_flag(review_mark_label_, LV_OBJ_FLAG_HIDDEN);

    // Keep transient button feedback in the same top-right status slot on
    // both sides of the card. A review mark yields this slot while feedback
    // is visible, so the bubble never jumps or overlaps another badge.
    action_status_label_ = lv_label_create(card);
    if (ui_font) {
        lv_obj_set_style_text_font(action_status_label_, ui_font, 0);
    }
    lv_obj_set_size(action_status_label_, 94, kFooterHeight);
    lv_label_set_long_mode(action_status_label_, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(action_status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(action_status_label_, lv_color_white(), 0);
    lv_obj_set_style_bg_color(action_status_label_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(action_status_label_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(action_status_label_, 4, 0);
    lv_obj_set_style_pad_all(action_status_label_, 0, 0);
    lv_obj_set_style_pad_top(action_status_label_, 5, 0);
    lv_obj_add_flag(action_status_label_, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(action_status_label_, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_flag(action_status_label_, LV_OBJ_FLAG_HIDDEN);

    wifi_setup_panel_ = lv_obj_create(card);
    StyleWifiSetupPanel(wifi_setup_panel_);
    lv_obj_add_flag(wifi_setup_panel_, LV_OBJ_FLAG_HIDDEN);

#if LV_USE_QRCODE
    wifi_qrcode_ = lv_qrcode_create(wifi_setup_panel_);
    lv_qrcode_set_size(wifi_qrcode_, kWifiQrSize);
    lv_qrcode_set_dark_color(wifi_qrcode_, lv_color_black());
    lv_qrcode_set_light_color(wifi_qrcode_, lv_color_white());
    lv_obj_set_style_border_width(wifi_qrcode_, 4, 0);
    lv_obj_set_style_border_color(wifi_qrcode_, lv_color_white(), 0);
    lv_qrcode_update(wifi_qrcode_, "http://192.168.4.1", strlen("http://192.168.4.1"));
    lv_obj_add_flag(wifi_qrcode_, LV_OBJ_FLAG_HIDDEN);
#endif

    wifi_text_label_ = lv_label_create(wifi_setup_panel_);
    StyleWifiTextLabel(wifi_text_label_, content_font);

    phonetic_label_ = lv_label_create(card);
    if (phonetic_font) {
        lv_obj_set_style_text_font(phonetic_label_, phonetic_font, 0);
    }
    lv_obj_set_width(phonetic_label_, LV_PCT(100));
    lv_obj_set_style_text_align(phonetic_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_translate_y(phonetic_label_, kPhoneticLiftY, 0);

    meaning_label_ = lv_label_create(card);
    StyleContentLabel(meaning_label_, content_font);

    example_label_ = lv_label_create(card);
    StyleContentLabel(example_label_, ExampleFont());

    translation_label_ = lv_label_create(card);
    StyleContentLabel(translation_label_, content_font);

    hint_label_ = lv_label_create(card);
    StyleHintLabel(hint_label_, content_font);

    status_label_ = lv_label_create(card);
    StyleContentLabel(status_label_, content_font);

    footer_label_ = lv_label_create(card);
    StylePlainObject(footer_label_);
    if (ui_font) {
        lv_obj_set_style_text_font(footer_label_, ui_font, 0);
    }
    lv_obj_set_size(footer_label_, LV_PCT(100), kFooterHeight);
    lv_label_set_long_mode(footer_label_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(footer_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_border_color(footer_label_, lv_color_black(), 0);
    lv_obj_set_style_border_side(footer_label_, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(footer_label_, 1, 0);
    lv_obj_set_style_pad_top(footer_label_, 5, 0);
    lv_obj_add_flag(footer_label_, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(footer_label_, LV_ALIGN_BOTTOM_MID, 0, 0);
    // The card reserves footer space with bottom padding so flex content never
    // overlaps the action bar. LVGL aligns floating children to that padded
    // content box, so translate the bar back down to the real panel edge.
    lv_obj_set_style_translate_y(footer_label_, kFooterHeight + 4, 0);

    ai_panel_ = lv_obj_create(screen_);
    StylePopupPanel(ai_panel_);
    // Keep the requested outer size and use the formerly reserved bottom
    // padding for answer text instead of making the whole dialog taller.
    lv_obj_set_size(ai_panel_, kAiPanelWidth, kAiPanelHeight);
    lv_obj_set_pos(ai_panel_, kAiPanelX, kAiPanelY);
    lv_obj_set_style_pad_all(ai_panel_, 10, 0);
    lv_obj_set_style_pad_bottom(ai_panel_, 2, 0);
    lv_obj_add_flag(ai_panel_, LV_OBJ_FLAG_HIDDEN);

    ai_status_label_ = lv_label_create(ai_panel_);
    lv_obj_set_style_text_font(ai_status_label_, ui_font, 0);
    lv_obj_set_width(ai_status_label_, 150);
    lv_label_set_long_mode(ai_status_label_, LV_LABEL_LONG_CLIP);
    lv_obj_align(ai_status_label_, LV_ALIGN_TOP_LEFT, 0, 0);

    ai_question_label_ = lv_label_create(ai_panel_);
    lv_obj_set_style_text_font(ai_question_label_, ui_font, 0);
    lv_obj_set_width(ai_question_label_, 190);
    lv_label_set_long_mode(ai_question_label_, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(ai_question_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(ai_question_label_, LV_ALIGN_TOP_RIGHT, 0, 0);

    ai_answer_label_ = lv_label_create(ai_panel_);
    lv_obj_set_style_text_font(ai_answer_label_, ui_font, 0);
    lv_obj_set_size(ai_answer_label_, kAiAnswerWidth, kAiAnswerHeight);
    lv_label_set_long_mode(ai_answer_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(ai_answer_label_, 3, 0);
    lv_obj_set_style_text_align(ai_answer_label_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(ai_answer_label_, LV_ALIGN_BOTTOM_MID, 0, 0);

    settings_panel_ = lv_obj_create(card);
    StyleSettingsPanel(settings_panel_);
    lv_obj_set_size(settings_panel_, LV_PCT(100), kSettingsPanelHeight);
    lv_obj_add_flag(settings_panel_, LV_OBJ_FLAG_HIDDEN);

    for (size_t i = 0; i < kSettingsGroupCount; ++i) {
        settings_sidebar_items_[i] = lv_obj_create(settings_panel_);
        StyleSettingsCategoryItem(settings_sidebar_items_[i]);
        lv_obj_set_size(settings_sidebar_items_[i], kSettingsSidebarLabelWidth, kSettingsSidebarLabelHeight);
        lv_obj_align(settings_sidebar_items_[i],
                     LV_ALIGN_TOP_LEFT,
                     kSettingsSidebarX,
                     kSettingsSidebarY + static_cast<lv_coord_t>(i) *
                          (kSettingsSidebarLabelHeight + kSettingsSidebarGap));

        settings_sidebar_labels_[i] = lv_label_create(settings_sidebar_items_[i]);
        StyleSettingsCategoryLabel(settings_sidebar_labels_[i], ui_font);
        lv_obj_set_width(settings_sidebar_labels_[i], kSettingsSidebarLabelWidth - 8);
        lv_obj_align(settings_sidebar_labels_[i], LV_ALIGN_CENTER, 0, 0);
    }

    settings_splitter_ = lv_obj_create(settings_panel_);
    StyleDivider(settings_splitter_);
    lv_obj_set_size(settings_splitter_, 1, kSettingsPanelHeight - kSettingsSplitterY * 2);
    lv_obj_align(settings_splitter_, LV_ALIGN_TOP_LEFT, kSettingsSplitterX, kSettingsSplitterY);

    for (size_t i = 0; i < kSettingsVisibleRows; ++i) {
        settings_row_[i] = lv_obj_create(settings_panel_);
        StyleSettingsRow(settings_row_[i]);
        lv_obj_set_size(settings_row_[i], kSettingsRowWidth, kSettingsRowHeight);
        lv_obj_align(settings_row_[i],
                     LV_ALIGN_TOP_LEFT,
                     kSettingsRowX,
                     kSettingsRowY + static_cast<lv_coord_t>(i) *
                         (kSettingsRowHeight + kSettingsRowGap));

        settings_row_label_[i] = lv_label_create(settings_row_[i]);
        StyleSettingsRowLabel(settings_row_label_[i], ui_font);
        lv_obj_set_width(settings_row_label_[i], kSettingsRowWidth - kSettingsValueWidth - 18);
        lv_obj_align(settings_row_label_[i], LV_ALIGN_LEFT_MID, 4, 0);

        settings_row_value_[i] = lv_label_create(settings_row_[i]);
        StyleSettingsRowLabel(settings_row_value_[i], ui_font);
        lv_obj_set_width(settings_row_value_[i], kSettingsValueWidth);
        lv_obj_set_style_text_align(settings_row_value_[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(settings_row_value_[i], LV_ALIGN_RIGHT_MID, -4, 0);
    }

    settings_popup_ = lv_obj_create(screen_);
    StylePopupPanel(settings_popup_);
    lv_obj_set_size(settings_popup_, 348, 188);
    lv_obj_align(settings_popup_, LV_ALIGN_CENTER, 0, 18);
    lv_obj_add_flag(settings_popup_, LV_OBJ_FLAG_HIDDEN);

    settings_popup_title_ = lv_label_create(settings_popup_);
    if (ui_font) {
        lv_obj_set_style_text_font(settings_popup_title_, ui_font, 0);
    }
    lv_obj_set_width(settings_popup_title_, 220);
    lv_label_set_long_mode(settings_popup_title_, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(settings_popup_title_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(settings_popup_title_, LV_ALIGN_TOP_MID, 0, 0);

    settings_popup_badge_ = lv_label_create(settings_popup_);
    if (ui_font) {
        lv_obj_set_style_text_font(settings_popup_badge_, ui_font, 0);
    }
    lv_obj_set_width(settings_popup_badge_, 92);
    lv_label_set_long_mode(settings_popup_badge_, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(settings_popup_badge_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(settings_popup_badge_, LV_ALIGN_TOP_RIGHT, 0, 0);

    settings_popup_body_ = lv_label_create(settings_popup_);
    if (ui_font) {
        lv_obj_set_style_text_font(settings_popup_body_, ui_font, 0);
    }
    lv_obj_set_width(settings_popup_body_, LV_PCT(100));
    lv_label_set_long_mode(settings_popup_body_, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(settings_popup_body_, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_line_space(settings_popup_body_, 6, 0);
    lv_obj_align(settings_popup_body_, LV_ALIGN_TOP_LEFT, 0, 38);

    settings_popup_progress_ = lv_bar_create(settings_popup_);
    lv_obj_set_size(settings_popup_progress_, 280, 18);
    lv_obj_align(settings_popup_progress_, LV_ALIGN_TOP_MID, 0, 88);
    lv_obj_set_style_radius(settings_popup_progress_, 9, LV_PART_MAIN);
    lv_obj_set_style_radius(settings_popup_progress_, 9, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(settings_popup_progress_, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(settings_popup_progress_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(settings_popup_progress_, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(settings_popup_progress_, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(settings_popup_progress_, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(settings_popup_progress_, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_add_flag(settings_popup_progress_, LV_OBJ_FLAG_HIDDEN);

    settings_popup_hint_ = lv_label_create(settings_popup_);
    if (content_font) {
        lv_obj_set_style_text_font(settings_popup_hint_, content_font, 0);
    }
    lv_obj_set_width(settings_popup_hint_, LV_PCT(100));
    lv_label_set_long_mode(settings_popup_hint_, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(settings_popup_hint_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(settings_popup_hint_, LV_ALIGN_BOTTOM_MID, 0, 0);

    built_ = true;
    UpdateSnapshot(snapshot_);
}

lv_obj_t* VocabularyPageAdapter::Screen() const {
    return screen_;
}

void VocabularyPageAdapter::OnShow() {
    UpdateSnapshot(snapshot_);
}

bool VocabularyPageAdapter::HandleEvent(const UiPageEvent& event) {
    (void)event;
    return false;
}

void VocabularyPageAdapter::UpdateSnapshot(const VocabularySnapshot& snapshot) {
    const bool action_status_only = built_ && screen_ != nullptr &&
        IsActionStatusOnlyUpdate(snapshot_, snapshot);
    snapshot_ = snapshot;
    if (!built_ || screen_ == nullptr) {
        return;
    }

    SetVisible(sleep_overlay_, snapshot_.sleeping);
    if (snapshot_.sleeping) {
        lv_obj_move_foreground(sleep_overlay_);
        CancelAiPageTurn();
        return;
    }

    if (action_status_only) {
        SetLabel(action_status_label_, snapshot_.action_status);
        SetVisible(action_status_label_, snapshot_.action_status[0] != '\0');
        if (snapshot_.screen == VocabularyScreen::kCardFront ||
            snapshot_.screen == VocabularyScreen::kCardBack) {
            SetVisible(review_mark_label_,
                       snapshot_.action_status[0] == '\0' &&
                           snapshot_.review_mark[0] != '\0');
        }
        if (snapshot_.action_status[0] != '\0') {
            lv_obj_move_foreground(action_status_label_);
        }
        return;
    }

    const lv_font_t* header_font = snapshot_.compact_font ? content_font_ : ui_font_;
    if (header_font != nullptr) {
        lv_obj_set_style_text_font(header_label_, header_font, 0);
        lv_obj_set_style_text_font(clock_label_, header_font, 0);
        lv_obj_set_style_text_font(progress_label_, header_font, 0);
    }
    SetLabel(header_label_, snapshot_.header);
    SetLabel(clock_label_, snapshot_.clock);
    SetLabel(progress_label_, snapshot_.progress);
    SetLabel(review_mark_label_, snapshot_.review_mark);
    SetLabel(action_status_label_, snapshot_.action_status);
    UpdateStatusIcons();
    SetLabel(footer_label_, snapshot_.footer);
    SetVisible(settings_popup_, false);

    switch (snapshot_.screen) {
        case VocabularyScreen::kWelcome:
            ApplyWelcome();
            break;
        case VocabularyScreen::kCardFront:
            ApplyCardFront();
            break;
        case VocabularyScreen::kCardBack:
            ApplyCardBack();
            break;
        case VocabularyScreen::kDailySummary:
            ApplySummary();
            break;
        case VocabularyScreen::kSettings:
            ApplySettings();
            break;
    }

    UpdatePopupOverlay(snapshot_.compact_font ? content_font_ : ui_font_);
    UpdateAiOverlay();
}

void VocabularyPageAdapter::UpdateAiOverlay() {
    SetVisible(ai_panel_, snapshot_.ai_overlay_active);
    if (!snapshot_.ai_overlay_active) {
        CancelAiPageTurn();
        ai_page_follow_latest_ = true;
        ai_page_index_ = 0;
        ai_rendered_page_text_.clear();
        last_ai_page_request_id_ = snapshot_.ai_page_request_id;
        return;
    }

    const AiDialogState state = static_cast<AiDialogState>(snapshot_.ai_state);
    std::string status = "小墨";
    if (state == AiDialogState::kConnecting || state == AiDialogState::kListening) {
        status = "小墨 · 正在听";
    } else if (state == AiDialogState::kThinking) status = "小墨 · 正在思考";
    else if (state == AiDialogState::kSpeaking) status = "小墨 · 正在讲解";
    else if (state == AiDialogState::kError) status = "小墨 · 暂不可用";
    SetLabel(ai_question_label_, snapshot_.ai_question);
    const AiDialogSnapshot live_ai = AiDialogService::GetInstance().GetSnapshot();
    const char* answer = !live_ai.answer.empty()
        ? live_ai.answer.c_str()
        : snapshot_.ai_message;
    const std::vector<std::string> pages =
        PaginateText(answer, ui_font_, kAiAnswerWidth, kAiAnswerHeight, 3);
    const size_t last_page = pages.size() - 1;
    if (snapshot_.ai_page_request_id != last_ai_page_request_id_) {
        CancelAiPageTurn();
        last_ai_page_request_id_ = snapshot_.ai_page_request_id;
        if (snapshot_.ai_page_delta < 0) {
            if (ai_page_index_ > 0) {
                --ai_page_index_;
            }
            ai_page_follow_latest_ = false;
        } else if (snapshot_.ai_page_delta > 0) {
            if (ai_page_index_ < last_page) {
                ++ai_page_index_;
            }
            ai_page_follow_latest_ = ai_page_index_ >= last_page;
        } else {
            ai_page_index_ = 0;
            ai_page_follow_latest_ = true;
        }
    }
    if (ai_page_index_ > last_page) {
        ai_page_index_ = last_page;
    }
    if (ai_page_follow_latest_) {
        if (ai_page_index_ < last_page) {
            ScheduleAiPageTurn();
        }
    }
    if (pages.size() > 1) {
        status = "小墨 " + std::to_string(ai_page_index_ + 1) + "/" +
            std::to_string(pages.size());
    }
    SetLabel(ai_status_label_, status.c_str());
    const std::string& visible_page = pages[ai_page_index_];
    const bool visible_page_changed = visible_page != ai_rendered_page_text_;
    SetLabel(ai_answer_label_, visible_page.c_str());
    if (visible_page_changed) {
        ai_rendered_page_text_ = visible_page;
        // UpdateSnapshot changes LVGL objects before RequestUrgentRefresh().
        // Force LVGL to rasterize this exact page into the 1-bpp framebuffer
        // now, otherwise the asynchronous e-paper task can sample the old
        // frame, report an empty refresh, and release the page-turn gate.
        lv_refr_now(lv_display_get_default());
        ESP_LOGI(kAiPageTag,
                 "rendered page=%u/%u bytes=%u t_ms=%lld",
                 static_cast<unsigned>(ai_page_index_ + 1),
                 static_cast<unsigned>(pages.size()),
                 static_cast<unsigned>(visible_page.size()),
                 static_cast<long long>(esp_timer_get_time() / 1000));
    }

    SetVisible(meaning_label_, false);
    SetVisible(example_label_, false);
    SetVisible(translation_label_, false);
    SetVisible(status_label_, false);
    SetVisible(settings_panel_, false);
}

void VocabularyPageAdapter::ScheduleAiPageTurn() {
    if (ai_page_turn_pending_) {
        return;
    }
    ai_page_turn_pending_ = true;
    ESP_LOGI(kAiPageTag,
             "page turn queued from=%u t_ms=%lld",
             static_cast<unsigned>(ai_page_index_ + 1),
             static_cast<long long>(esp_timer_get_time() / 1000));
    ai_page_turn_timer_ =
        lv_timer_create(&VocabularyPageAdapter::AiPageTurnTimerCallback, 100, this);
    if (ai_page_turn_timer_ == nullptr) {
        ai_page_turn_pending_ = false;
    }
}

void VocabularyPageAdapter::CancelAiPageTurn() {
    ai_page_turn_pending_ = false;
    if (ai_page_turn_timer_ != nullptr) {
        lv_timer_delete(ai_page_turn_timer_);
        ai_page_turn_timer_ = nullptr;
    }
}

void VocabularyPageAdapter::AiPageTurnTimerCallback(lv_timer_t* timer) {
    auto* adapter =
        static_cast<VocabularyPageAdapter*>(lv_timer_get_user_data(timer));
    if (adapter == nullptr) {
        lv_timer_delete(timer);
        return;
    }
    if (!adapter->snapshot_.ai_overlay_active) {
        adapter->CancelAiPageTurn();
        return;
    }
    if (adapter->host_ != nullptr && adapter->host_->IsRefreshPending()) {
        return;
    }

    adapter->ai_page_turn_pending_ = false;
    adapter->ai_page_turn_timer_ = nullptr;
    lv_timer_delete(timer);
    ++adapter->ai_page_index_;
    ESP_LOGI(kAiPageTag,
             "page turn released to=%u after display idle t_ms=%lld",
             static_cast<unsigned>(adapter->ai_page_index_ + 1),
             static_cast<long long>(esp_timer_get_time() / 1000));
    adapter->UpdateAiOverlay();
    if (adapter->host_ != nullptr) {
        adapter->host_->RequestUrgentRefresh();
    }
}

void VocabularyPageAdapter::ApplyWelcome() {
    lv_obj_set_style_pad_bottom(lv_obj_get_parent(meaning_label_),
                                kFooterHeight + kCardBottomPad,
                                0);
    SetVisible(word_label_, true);
    SetVisible(wifi_setup_panel_, false);
    SetVisible(wifi_qrcode_, false);
    SetVisible(wifi_text_label_, false);
    SetVisible(phonetic_label_, false);
    SetVisible(meaning_label_, true);
    SetVisible(example_label_, true);
    SetVisible(translation_label_, false);
    SetVisible(status_label_, false);
    SetVisible(review_mark_label_, false);
    SetVisible(action_status_label_, false);
    SetVisible(hint_label_, true);
    SetVisible(settings_panel_, false);
    SetVisible(footer_label_, true);

    lv_obj_set_style_text_font(word_label_, &CalibriLight_Eink_36, 0);
    lv_label_set_long_mode(word_label_, LV_LABEL_LONG_MODE_DOTS);
    StyleContentLabel(meaning_label_, ui_font_);
    StyleContentLabel(example_label_, content_font_);
    StyleHintLabel(hint_label_, content_font_);
    SetLabel(word_label_, snapshot_.word);
    SetLabel(meaning_label_, snapshot_.meaning);
    SetLabel(example_label_, snapshot_.example);
    SetTruncatedHintLabel(hint_label_, snapshot_.hint, content_font_);
}

void VocabularyPageAdapter::ApplyCardFront() {
    lv_obj_set_style_pad_bottom(lv_obj_get_parent(meaning_label_),
                                kFooterHeight + kCardBottomPad,
                                0);
    SetVisible(word_label_, true);
    SetVisible(wifi_setup_panel_, false);
    SetVisible(wifi_qrcode_, false);
    SetVisible(wifi_text_label_, false);
    SetVisible(phonetic_label_, true);
    SetVisible(meaning_label_, false);
    SetVisible(example_label_, false);
    SetVisible(translation_label_, false);
    SetVisible(status_label_, false);
    SetVisible(review_mark_label_,
               snapshot_.action_status[0] == '\0' && snapshot_.review_mark[0] != '\0');
    SetVisible(action_status_label_, snapshot_.action_status[0] != '\0');
    if (snapshot_.action_status[0] != '\0') {
        lv_obj_move_foreground(action_status_label_);
    }
    SetVisible(hint_label_, snapshot_.hint[0] != '\0');
    SetVisible(settings_panel_, false);
    SetVisible(footer_label_, snapshot_.footer[0] != '\0');
    lv_obj_align(action_status_label_, LV_ALIGN_TOP_RIGHT, 0, 0);

    const lv_font_t* ui_font = snapshot_.compact_font ? content_font_ : ui_font_;
    lv_obj_set_style_text_font(word_label_, strlen(snapshot_.word) > 18
        ? ExampleFont()
        : &CalibriLight_Eink_36, 0);
    lv_label_set_long_mode(word_label_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(phonetic_label_, &DejaVuSans_Phonetic_16, 0);
    StyleHintLabel(hint_label_, ui_font);
    SetLabel(word_label_, snapshot_.word);
    SetLabel(phonetic_label_, snapshot_.phonetic);
    SetLabel(status_label_, snapshot_.status_line);
    SetTruncatedHintLabel(hint_label_, snapshot_.hint, ui_font);
}

void VocabularyPageAdapter::ApplyCardBack() {
    // Three-or-more entry meanings flow horizontally first. Most then fit the
    // original two-line budget and keep the footer; only a real third display
    // line reclaims the footer area. The transient action badge shares the
    // top-right overlay slot with the review mark, so it never changes layout.
    SetVisible(word_label_, true);
    SetVisible(wifi_setup_panel_, false);
    SetVisible(wifi_qrcode_, false);
    SetVisible(wifi_text_label_, false);
    SetVisible(phonetic_label_, true);
    SetVisible(meaning_label_, true);
    SetVisible(example_label_, true);
    SetVisible(translation_label_, snapshot_.example_translation[0] != '\0');
    SetVisible(status_label_, false);
    SetVisible(review_mark_label_,
               snapshot_.action_status[0] == '\0' && snapshot_.review_mark[0] != '\0');
    SetVisible(action_status_label_, snapshot_.action_status[0] != '\0');
    if (snapshot_.action_status[0] != '\0') {
        lv_obj_move_foreground(action_status_label_);
    }
    SetVisible(hint_label_, snapshot_.hint[0] != '\0');
    SetVisible(settings_panel_, false);
    lv_obj_align(action_status_label_, LV_ALIGN_TOP_RIGHT, 0, 0);

    lv_obj_set_style_text_font(word_label_, strlen(snapshot_.word) > 18
        ? ExampleFont()
        : &CalibriLight_Eink_36, 0);
    lv_label_set_long_mode(word_label_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(phonetic_label_, &DejaVuSans_Phonetic_16, 0);
    StyleContentLabel(meaning_label_, ExampleFont());
    StyleContentLabel(example_label_, content_font_);
    StyleContentLabel(translation_label_, content_font_);
    StyleContentLabel(status_label_, content_font_);
    StyleHintLabel(hint_label_, content_font_);
    const MeaningPresentation meaning = FormatMeaningForCardBack(snapshot_.meaning);
    SetLabel(word_label_, snapshot_.word);
    SetLabel(phonetic_label_, snapshot_.phonetic);
    SetLabel(meaning_label_, meaning.text.c_str());
    SetLabel(example_label_, snapshot_.example);
    SetLabel(translation_label_, snapshot_.example_translation);
    SetLabel(status_label_, snapshot_.status_line);
    lv_obj_t* card = lv_obj_get_parent(meaning_label_);
    lv_label_set_long_mode(meaning_label_, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_height(meaning_label_, LV_SIZE_CONTENT);
    lv_obj_update_layout(card);
    lv_coord_t natural_meaning_height = lv_obj_get_height(meaning_label_);
    if (meaning.horizontal && natural_meaning_height > kMeaningThreeLineHeight) {
        StyleContentLabel(meaning_label_, content_font_);
        lv_obj_set_height(meaning_label_, LV_SIZE_CONTENT);
        lv_obj_update_layout(card);
        natural_meaning_height = lv_obj_get_height(meaning_label_);
    }
    const bool needs_third_line =
        meaning.horizontal && natural_meaning_height > kMeaningTwoLineHeight;
    SetVisible(footer_label_, snapshot_.footer[0] != '\0' && !needs_third_line);
    lv_obj_set_style_pad_bottom(card,
                                needs_third_line
                                    ? kCardBottomPad
                                    : kFooterHeight + kCardBottomPad,
                                0);
    ClampLabelHeight(meaning_label_,
                     needs_third_line ? kMeaningThreeLineHeight : kMeaningTwoLineHeight);
    ClampLabelHeight(example_label_, 52);
    ClampLabelHeight(translation_label_, 38);
    SetTruncatedHintLabel(hint_label_, snapshot_.hint, content_font_);
}

void VocabularyPageAdapter::ApplySummary() {
    lv_obj_set_style_pad_bottom(lv_obj_get_parent(meaning_label_),
                                kFooterHeight + kCardBottomPad,
                                0);
    const bool wifi_setup = snapshot_.wifi_ap_mode;
    SetVisible(word_label_, true);
    SetVisible(wifi_setup_panel_, wifi_setup);
    SetVisible(wifi_qrcode_, wifi_setup);
    SetVisible(wifi_text_label_, wifi_setup);
    SetVisible(phonetic_label_, false);
    SetVisible(meaning_label_, !wifi_setup);
    SetVisible(example_label_, !wifi_setup);
    SetVisible(translation_label_, false);
    SetVisible(status_label_, false);
    SetVisible(review_mark_label_, false);
    SetVisible(action_status_label_, false);
    SetVisible(hint_label_, !wifi_setup && snapshot_.hint[0] != '\0');
    SetVisible(settings_panel_, false);
    SetVisible(footer_label_, snapshot_.footer[0] != '\0');

    const lv_font_t* ui_font = snapshot_.compact_font ? content_font_ : ui_font_;
    lv_obj_set_style_text_font(word_label_, ExampleFont(), 0);
    lv_label_set_long_mode(word_label_, LV_LABEL_LONG_MODE_DOTS);
    StyleContentLabel(meaning_label_, ui_font);
    StyleContentLabel(example_label_, ui_font);
    StyleHintLabel(hint_label_, ui_font);
    StyleWifiTextLabel(wifi_text_label_, ui_font);
    SetLabel(word_label_, snapshot_.word);
    if (wifi_setup) {
        std::string wifi_text = snapshot_.meaning;
        if (snapshot_.example[0] != '\0') {
            if (!wifi_text.empty()) {
                wifi_text.push_back('\n');
            }
            wifi_text += snapshot_.example;
        }
        if (snapshot_.hint[0] != '\0') {
            if (!wifi_text.empty()) {
                wifi_text.push_back('\n');
            }
            wifi_text += snapshot_.hint;
        }
        SetLabel(wifi_text_label_, wifi_text.c_str());
    } else {
        SetLabel(meaning_label_, snapshot_.meaning);
        SetLabel(example_label_, snapshot_.example);
        SetTruncatedHintLabel(hint_label_, snapshot_.hint, ui_font);
    }
}

void VocabularyPageAdapter::ApplySettings() {
    lv_obj_set_style_pad_bottom(lv_obj_get_parent(meaning_label_),
                                kFooterHeight + kCardBottomPad,
                                0);
    const lv_font_t* settings_font = snapshot_.compact_font ? content_font_ : ui_font_;
    SetVisible(word_label_, false);
    SetVisible(wifi_setup_panel_, false);
    SetVisible(wifi_qrcode_, false);
    SetVisible(wifi_text_label_, false);
    SetVisible(phonetic_label_, false);
    SetVisible(meaning_label_, false);
    SetVisible(example_label_, false);
    SetVisible(translation_label_, false);
    SetVisible(status_label_, false);
    SetVisible(review_mark_label_, false);
    SetVisible(action_status_label_, false);
    SetVisible(hint_label_, false);
    SetVisible(settings_panel_, true);
    SetVisible(footer_label_, snapshot_.footer[0] != '\0');

    SetLabel(word_label_, "");
    SetLabel(meaning_label_, "");
    UpdateSettingsPanel(settings_font);
}

void VocabularyPageAdapter::UpdatePopupOverlay(const lv_font_t* font) {
    if (snapshot_.settings_popup_open) {
        SetVisible(settings_popup_, true);
        if (font != nullptr) {
            lv_obj_set_style_text_font(settings_popup_title_, font, 0);
            lv_obj_set_style_text_font(settings_popup_badge_, font, 0);
            lv_obj_set_style_text_font(settings_popup_body_, font, 0);
        }
        SetLabel(settings_popup_title_, snapshot_.settings_popup_title);
        SetLabel(settings_popup_badge_, snapshot_.settings_popup_badge);
        SetVisible(settings_popup_badge_, snapshot_.settings_popup_badge[0] != '\0');
        SetLabel(settings_popup_body_, snapshot_.settings_popup_body);
        SetVisible(settings_popup_progress_, snapshot_.settings_popup_progress_visible);
        if (snapshot_.settings_popup_progress_visible) {
            lv_bar_set_range(settings_popup_progress_,
                             snapshot_.settings_popup_progress_min,
                             snapshot_.settings_popup_progress_max);
            lv_bar_set_value(settings_popup_progress_,
                             snapshot_.settings_popup_progress_value,
                             LV_ANIM_OFF);
        }
        SetLabel(settings_popup_hint_, snapshot_.settings_popup_hint);
        SetVisible(settings_popup_hint_, snapshot_.settings_popup_hint[0] != '\0');
        lv_obj_move_foreground(settings_popup_);
    }
}

void VocabularyPageAdapter::UpdateSettingsPanel(const lv_font_t* font) {
    const size_t selected_group = SettingsGroupIndexForItem(snapshot_.settings_selection);

    for (size_t i = 0; i < kSettingsGroupCount; ++i) {
        lv_obj_t* item = settings_sidebar_items_[i];
        lv_obj_t* label = settings_sidebar_labels_[i];
        if (item == nullptr || label == nullptr) {
            continue;
        }
        const bool selected = i == selected_group;
        if (font != nullptr) {
            lv_obj_set_style_text_font(label, font, 0);
        }
        lv_obj_set_style_bg_color(item, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(item, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(label, selected ? lv_color_white() : lv_color_black(), 0);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);
        SetLabel(label, kSettingsGroups[i].label);
    }

    const SettingsGroup& group = kSettingsGroups[selected_group];
    size_t selected_row = 0;
    for (size_t i = 0; i < group.count; ++i) {
        if (group.items[i] == snapshot_.settings_selection) {
            selected_row = i;
            break;
        }
    }
    const size_t first_row = selected_row < kSettingsVisibleRows
        ? 0
        : selected_row - kSettingsVisibleRows + 1;
    for (size_t row = 0; row < kSettingsVisibleRows; ++row) {
        const size_t group_row = first_row + row;
        const bool visible = group_row < group.count;
        SetVisible(settings_row_[row], visible);
        if (!visible) {
            continue;
        }

        const uint8_t item_index = group.items[group_row];
        const bool selected = item_index == snapshot_.settings_selection;
        lv_obj_t* row_obj = settings_row_[row];
        lv_obj_set_style_bg_color(row_obj, selected ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_bg_opa(row_obj, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(row_obj, selected ? 9 : 0, 0);
        lv_obj_set_style_border_width(row_obj, selected ? 0 : 1, 0);
        lv_obj_set_style_border_side(row_obj, LV_BORDER_SIDE_BOTTOM, 0);

        lv_obj_t* row_label = settings_row_label_[row];
        lv_obj_t* row_value = settings_row_value_[row];
        if (font != nullptr) {
            lv_obj_set_style_text_font(row_label, font, 0);
            lv_obj_set_style_text_font(row_value, font, 0);
        }
        lv_obj_set_style_text_color(row_label, selected ? lv_color_white() : lv_color_black(), 0);
        lv_obj_set_style_text_color(row_value, selected ? lv_color_white() : lv_color_black(), 0);
        SetLabel(row_label, SettingsItemLabel(item_index));
        SetLabel(row_value, item_index < kVocabularySettingsItemCount
            ? snapshot_.settings_item_values[item_index]
            : "");
    }
}

void VocabularyPageAdapter::UpdateStatusIcons() {
    uint8_t visible_bars = 0;
    if (snapshot_.wifi_connected) {
        visible_bars = snapshot_.wifi_signal_bars == 0 ? 1 : snapshot_.wifi_signal_bars;
    }
    if (visible_bars > kWifiBarCount) {
        visible_bars = kWifiBarCount;
    }
    for (size_t i = 0; i < kWifiBarCount; ++i) {
        SetVisible(wifi_bars_[i], i < visible_bars);
    }

    uint8_t visible_cells = 0;
    if (snapshot_.battery_charging) {
        visible_cells = kBatteryCellCount;
    } else if (snapshot_.battery_available && snapshot_.battery_level > 5) {
        visible_cells = 1;
        if (snapshot_.battery_level >= 25) {
            visible_cells = 2;
        }
        if (snapshot_.battery_level >= 50) {
            visible_cells = 3;
        }
        if (snapshot_.battery_level >= 75) {
            visible_cells = 4;
        }
        if (snapshot_.battery_level >= 90) {
            visible_cells = 5;
        }
    }
    for (size_t i = 0; i < kBatteryCellCount; ++i) {
        SetVisible(battery_cells_[i], i < visible_cells);
    }
}
