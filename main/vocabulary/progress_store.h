#ifndef VOCABULARY_PROGRESS_STORE_H
#define VOCABULARY_PROGRESS_STORE_H

#include "vocabulary_types.h"

#include <cstddef>
#include <string>
#include <vector>

class ProgressStore {
public:
    bool Load(const std::string& deck_id,
              const std::vector<uint64_t>& word_keys,
              std::vector<ReviewState>* states,
              VocabularyDailyStats* stats);
    bool Load(const std::string& deck_id,
              size_t word_count,
              std::vector<ReviewState>* states,
              VocabularyDailyStats* stats);
    bool Load(size_t word_count,
              std::vector<ReviewState>* states,
              VocabularyDailyStats* stats);
    bool Save(const std::string& deck_id,
              const std::vector<uint64_t>& word_keys,
              const std::vector<ReviewState>& states,
              const VocabularyDailyStats& stats);
    bool Save(const std::string& deck_id,
              const std::vector<ReviewState>& states,
              const VocabularyDailyStats& stats);
    bool Save(const std::vector<ReviewState>& states,
              const VocabularyDailyStats& stats);
    bool LoadSettings(VocabularySettings* settings);
    bool SaveSettings(const VocabularySettings& settings);
    bool LoadCurrentIndex(const std::string& deck_id, size_t* index);
    bool SaveCurrentIndex(const std::string& deck_id, size_t index);
    bool ClearProgress(const std::string& deck_id);
    bool ClearProgress();
    void ResetDailyStatsIfNeeded(int32_t today,
                                 std::vector<ReviewState>* states,
                                 VocabularyDailyStats* stats);
};

#endif  // VOCABULARY_PROGRESS_STORE_H
