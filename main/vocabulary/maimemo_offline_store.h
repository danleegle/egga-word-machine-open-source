#ifndef MAIMEMO_OFFLINE_STORE_H
#define MAIMEMO_OFFLINE_STORE_H

#include "maimemo_study_client.h"

#include <cstdint>
#include <string>
#include <vector>

struct MaimemoTodayItem {
    MaimemoWordCard card;
    uint16_t order = 0;
    bool is_new = false;
    bool is_finished = false;
    bool details_cached = false;
};

struct MaimemoPendingAnswer {
    std::string operation_id;
    std::string voc_id;
    std::string word;
    MaimemoAnswer answer = MaimemoAnswer::kFamiliar;
    uint32_t recall_duration_ms = 0;
    uint32_t study_duration_ms = 0;
    int32_t day_index = 0;
};

struct MaimemoReviewMark {
    std::string voc_id;
    std::string word;
    MaimemoAnswer answer = MaimemoAnswer::kFamiliar;
    int32_t day_index = 0;
};

struct MaimemoOfflineState {
    int32_t snapshot_day_index = 0;
    int64_t refreshed_at_ms = 0;
    size_t current_index = 0;
    std::vector<MaimemoTodayItem> today_items;
    std::vector<MaimemoPendingAnswer> pending_answers;
    std::vector<MaimemoPendingAnswer> conflicts;
    std::vector<MaimemoReviewMark> review_marks;
};

class MaimemoOfflineStore {
public:
    bool Load(MaimemoOfflineState* state) const;
    bool Save(const MaimemoOfflineState& state) const;
    bool SavePendingAnswers(const MaimemoOfflineState& state) const;
    bool Clear() const;
};

#endif  // MAIMEMO_OFFLINE_STORE_H
