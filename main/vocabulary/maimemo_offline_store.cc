#include "maimemo_offline_store.h"

#include "assets_store.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_spiffs.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace {

constexpr char kTag[] = "MaimemoOffline";
constexpr char kStateFile[] = "maimemo-offline-state.json";
constexpr char kTempFile[] = "maimemo-offline-state.tmp";
constexpr char kBackupFile[] = "maimemo-offline-state.bak";
constexpr char kAnswersFile[] = "maimemo-offline-answers.json";
constexpr char kAnswersTempFile[] = "maimemo-offline-answers.tmp";
constexpr char kAnswersBackupFile[] = "maimemo-offline-answers.bak";
constexpr size_t kMaxStateBytes = 512 * 1024;
constexpr size_t kMaxTodayItems = 1000;
constexpr size_t kMaxPendingAnswers = 1000;
constexpr size_t kMaxConflicts = 200;
constexpr size_t kMaxReviewMarks = 1000;

const char* JsonStringValue(cJSON* object, const char* key) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : "";
}

bool JsonBoolValue(cJSON* object, const char* key, bool fallback = false) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

int JsonIntValue(cJSON* object, const char* key, int fallback = 0) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

MaimemoWordCard ParseCard(cJSON* object) {
    MaimemoWordCard card;
    card.voc_id = JsonStringValue(object, "voc_id");
    card.word = JsonStringValue(object, "word");
    card.phonetic = JsonStringValue(object, "phonetic");
    card.pronunciation_url_us = JsonStringValue(object, "audio_us");
    card.pronunciation_url_uk = JsonStringValue(object, "audio_uk");
    card.meaning = JsonStringValue(object, "meaning");
    card.example = JsonStringValue(object, "example");
    card.example_translation = JsonStringValue(object, "example_translation");
    card.note_type = JsonStringValue(object, "note_type");
    card.note = JsonStringValue(object, "note");
    return card;
}

bool ParseAnswer(cJSON* object, MaimemoPendingAnswer* answer) {
    if (!cJSON_IsObject(object) || answer == nullptr) {
        return false;
    }
    answer->operation_id = JsonStringValue(object, "operation_id");
    answer->voc_id = JsonStringValue(object, "voc_id");
    answer->word = JsonStringValue(object, "word");
    const int answer_value = JsonIntValue(object, "answer", 0);
    if (answer->operation_id.empty() || answer->voc_id.empty() ||
        answer_value < static_cast<int>(MaimemoAnswer::kFamiliar) ||
        answer_value > static_cast<int>(MaimemoAnswer::kForget)) {
        return false;
    }
    answer->answer = static_cast<MaimemoAnswer>(answer_value);
    answer->recall_duration_ms = static_cast<uint32_t>(
        std::max(0, JsonIntValue(object, "recall_ms", 0)));
    answer->study_duration_ms = static_cast<uint32_t>(
        std::max(0, JsonIntValue(object, "study_ms", 0)));
    answer->day_index = JsonIntValue(object, "day", 0);
    return true;
}

bool ParseReviewMark(cJSON* object, MaimemoReviewMark* mark) {
    if (!cJSON_IsObject(object) || mark == nullptr) {
        return false;
    }
    mark->voc_id = JsonStringValue(object, "voc_id");
    mark->word = JsonStringValue(object, "word");
    const int answer_value = JsonIntValue(object, "answer", 0);
    if (mark->word.empty() ||
        answer_value < static_cast<int>(MaimemoAnswer::kFamiliar) ||
        answer_value > static_cast<int>(MaimemoAnswer::kForget)) {
        return false;
    }
    mark->answer = static_cast<MaimemoAnswer>(answer_value);
    mark->day_index = JsonIntValue(object, "day", 0);
    return true;
}

bool ReadStateFile(const std::string& relative_path, std::string* json) {
    return AssetsStore::GetInstance().ReadFile(relative_path, json, kMaxStateBytes);
}

void AppendJsonString(std::string* output, const std::string& value) {
    output->push_back('"');
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"': output->append("\\\""); break;
            case '\\': output->append("\\\\"); break;
            case '\b': output->append("\\b"); break;
            case '\f': output->append("\\f"); break;
            case '\n': output->append("\\n"); break;
            case '\r': output->append("\\r"); break;
            case '\t': output->append("\\t"); break;
            default:
                if (ch < 0x20) {
                    char escaped[7] = {};
                    snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
                    output->append(escaped);
                } else {
                    output->push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    output->push_back('"');
}

void AppendStringField(std::string* output,
                       const char* key,
                       const std::string& value,
                       bool* first,
                       bool optional = false) {
    if (optional && value.empty()) {
        return;
    }
    if (!*first) {
        output->push_back(',');
    }
    *first = false;
    output->push_back('"');
    output->append(key);
    output->append("\":");
    AppendJsonString(output, value);
}

void AppendCardJson(std::string* output, const MaimemoTodayItem& item) {
    output->push_back('{');
    bool first = true;
    AppendStringField(output, "voc_id", item.card.voc_id, &first);
    AppendStringField(output, "word", item.card.word, &first);
    AppendStringField(output, "phonetic", item.card.phonetic, &first, true);
    AppendStringField(output, "audio_us", item.card.pronunciation_url_us, &first, true);
    AppendStringField(output, "audio_uk", item.card.pronunciation_url_uk, &first, true);
    AppendStringField(output, "meaning", item.card.meaning, &first, true);
    AppendStringField(output, "example", item.card.example, &first, true);
    AppendStringField(output, "example_translation", item.card.example_translation, &first, true);
    AppendStringField(output, "note_type", item.card.note_type, &first, true);
    AppendStringField(output, "note", item.card.note, &first, true);
    output->append(",\"order\":");
    output->append(std::to_string(item.order));
    output->append(",\"is_new\":");
    output->append(item.is_new ? "true" : "false");
    output->append(",\"is_finished\":");
    output->append(item.is_finished ? "true" : "false");
    output->append(",\"details_cached\":");
    output->append(item.details_cached ? "true" : "false");
    output->push_back('}');
}

void AppendAnswerJson(std::string* output, const MaimemoPendingAnswer& answer) {
    output->append("{\"operation_id\":");
    AppendJsonString(output, answer.operation_id);
    output->append(",\"voc_id\":");
    AppendJsonString(output, answer.voc_id);
    output->append(",\"word\":");
    AppendJsonString(output, answer.word);
    output->append(",\"answer\":");
    output->append(std::to_string(static_cast<int>(answer.answer)));
    output->append(",\"recall_ms\":");
    output->append(std::to_string(answer.recall_duration_ms));
    output->append(",\"study_ms\":");
    output->append(std::to_string(answer.study_duration_ms));
    output->append(",\"day\":");
    output->append(std::to_string(answer.day_index));
    output->push_back('}');
}

void AppendReviewMarkJson(std::string* output, const MaimemoReviewMark& mark) {
    output->append("{\"voc_id\":");
    AppendJsonString(output, mark.voc_id);
    output->append(",\"word\":");
    AppendJsonString(output, mark.word);
    output->append(",\"answer\":");
    output->append(std::to_string(static_cast<int>(mark.answer)));
    output->append(",\"day\":");
    output->append(std::to_string(mark.day_index));
    output->push_back('}');
}

bool WriteAll(FILE* file, const char* data, size_t length) {
    // SPIFFS tries to reserve enough blocks for a single write operation. A
    // large snapshot written in one fwrite can therefore hit its GC-run limit
    // and report ENOSPC even with megabytes free. Small sequential writes let
    // GC make bounded progress and avoid holding the vocabulary lock for long
    // failed retries.
    constexpr size_t kWriteChunkBytes = 4096;
    size_t offset = 0;
    while (offset < length) {
        const size_t chunk = std::min(kWriteChunkBytes, length - offset);
        if (fwrite(data + offset, 1, chunk, file) != chunk) {
            return false;
        }
        offset += chunk;
    }
    return fflush(file) == 0;
}

bool WriteStateFile(const std::string& path,
                    const MaimemoOfflineState& state,
                    size_t* bytes_written) {
    std::string json;
    json.reserve(std::min(kMaxStateBytes, state.today_items.size() * 128));
    json.append("{\"version\":1,\"snapshot_day\":");
    json.append(std::to_string(state.snapshot_day_index));
    json.append(",\"refreshed_at_ms\":");
    json.append(std::to_string(state.refreshed_at_ms));
    json.append(",\"current_index\":");
    json.append(std::to_string(state.current_index));
    json.append(",\"today_items\":[");

    bool first = true;
    for (const auto& item : state.today_items) {
        if (!first) json.push_back(',');
        first = false;
        AppendCardJson(&json, item);
        if (json.size() > kMaxStateBytes) return false;
    }

    json.append("],\"pending\":[");
    first = true;
    for (const auto& answer : state.pending_answers) {
        if (!first) json.push_back(',');
        first = false;
        AppendAnswerJson(&json, answer);
        if (json.size() > kMaxStateBytes) return false;
    }

    json.append("],\"conflicts\":[");
    first = true;
    for (const auto& answer : state.conflicts) {
        if (!first) json.push_back(',');
        first = false;
        AppendAnswerJson(&json, answer);
        if (json.size() > kMaxStateBytes) return false;
    }
    json.append("],\"review_marks\":[");
    first = true;
    for (const auto& mark : state.review_marks) {
        if (!first) json.push_back(',');
        first = false;
        AppendReviewMarkJson(&json, mark);
        if (json.size() > kMaxStateBytes) return false;
    }
    json.append("]}");
    if (json.size() > kMaxStateBytes) {
        return false;
    }
    if (bytes_written != nullptr) {
        *bytes_written = json.size();
    }
    FILE* file = fopen(path.c_str(), "wb");
    if (file == nullptr) return false;
    const bool wrote = WriteAll(file, json.data(), json.size());
    fclose(file);
    return wrote;
}

bool WriteAnswersFile(const std::string& path,
                      const MaimemoOfflineState& state,
                      size_t* bytes_written) {
    std::string json = "{\"pending\":[";
    bool first = true;
    for (const auto& answer : state.pending_answers) {
        if (!first) json.push_back(',');
        first = false;
        AppendAnswerJson(&json, answer);
    }
    json.append("],\"conflicts\":[");
    first = true;
    for (const auto& answer : state.conflicts) {
        if (!first) json.push_back(',');
        first = false;
        AppendAnswerJson(&json, answer);
    }
    json.append("],\"review_marks\":[");
    first = true;
    for (const auto& mark : state.review_marks) {
        if (!first) json.push_back(',');
        first = false;
        AppendReviewMarkJson(&json, mark);
    }
    json.append("]}");
    if (json.size() > kMaxStateBytes) return false;
    if (bytes_written != nullptr) *bytes_written = json.size();
    FILE* file = fopen(path.c_str(), "wb");
    if (file == nullptr) return false;
    const bool wrote = WriteAll(file, json.data(), json.size());
    fclose(file);
    return wrote;
}

void LogWriteFailure(const char* phase, const std::string& path, size_t bytes_written) {
    size_t total = 0;
    size_t used = 0;
    (void)esp_spiffs_info("assets", &total, &used);
    ESP_LOGW(kTag,
             "%s failed path=%s bytes=%u errno=%d free=%u/%u",
             phase,
             path.c_str(),
             static_cast<unsigned>(bytes_written),
             errno,
             static_cast<unsigned>(total > used ? total - used : 0),
             static_cast<unsigned>(total));
}

}  // namespace

bool MaimemoOfflineStore::Load(MaimemoOfflineState* state) const {
    if (state == nullptr || !AssetsStore::GetInstance().IsMounted()) {
        return false;
    }

    std::string json;
    const bool has_full_state =
        ReadStateFile(kStateFile, &json) || ReadStateFile(kBackupFile, &json);
    cJSON* root = has_full_state
        ? cJSON_ParseWithLength(json.data(), json.size())
        : nullptr;
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        // The compact answer journal is independently durable. A device that
        // has never downloaded an Open API snapshot may legitimately have no
        // full state file, so continue with an empty root and load the journal
        // instead of discarding pending answers and review marks.
        root = cJSON_CreateObject();
        if (root == nullptr) {
            return false;
        }
    }

    MaimemoOfflineState loaded;
    loaded.snapshot_day_index = JsonIntValue(root, "snapshot_day", 0);
    cJSON* refreshed = cJSON_GetObjectItemCaseSensitive(root, "refreshed_at_ms");
    loaded.refreshed_at_ms = cJSON_IsNumber(refreshed)
        ? static_cast<int64_t>(refreshed->valuedouble)
        : 0;
    loaded.current_index = static_cast<size_t>(std::max(0, JsonIntValue(root, "current_index", 0)));

    cJSON* items = cJSON_GetObjectItemCaseSensitive(root, "today_items");
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, items) {
        if (loaded.today_items.size() >= kMaxTodayItems || !cJSON_IsObject(item)) {
            break;
        }
        MaimemoTodayItem parsed;
        parsed.card = ParseCard(item);
        parsed.order = static_cast<uint16_t>(std::max(0, JsonIntValue(item, "order", 0)));
        parsed.is_new = JsonBoolValue(item, "is_new");
        parsed.is_finished = JsonBoolValue(item, "is_finished");
        parsed.details_cached = JsonBoolValue(item, "details_cached");
        if (!parsed.card.voc_id.empty() && !parsed.card.word.empty()) {
            loaded.today_items.push_back(std::move(parsed));
        }
    }

    auto parse_answers = [](cJSON* array,
                            size_t maximum,
                            std::vector<MaimemoPendingAnswer>* output) {
        cJSON* entry = nullptr;
        cJSON_ArrayForEach(entry, array) {
            if (output->size() >= maximum) {
                break;
            }
            MaimemoPendingAnswer parsed;
            if (ParseAnswer(entry, &parsed)) {
                output->push_back(std::move(parsed));
            }
        }
    };
    parse_answers(cJSON_GetObjectItemCaseSensitive(root, "pending"),
                  kMaxPendingAnswers,
                  &loaded.pending_answers);
    parse_answers(cJSON_GetObjectItemCaseSensitive(root, "conflicts"),
                  kMaxConflicts,
                  &loaded.conflicts);
    auto parse_review_marks = [](cJSON* array, std::vector<MaimemoReviewMark>* output) {
        cJSON* entry = nullptr;
        cJSON_ArrayForEach(entry, array) {
            if (output->size() >= kMaxReviewMarks) {
                break;
            }
            MaimemoReviewMark parsed;
            if (ParseReviewMark(entry, &parsed)) {
                output->push_back(std::move(parsed));
            }
        }
    };
    parse_review_marks(cJSON_GetObjectItemCaseSensitive(root, "review_marks"),
                       &loaded.review_marks);
    cJSON_Delete(root);

    std::string answers_json;
    if (ReadStateFile(kAnswersFile, &answers_json)) {
        cJSON* answers_root = cJSON_ParseWithLength(answers_json.data(), answers_json.size());
        if (cJSON_IsObject(answers_root)) {
            std::vector<MaimemoPendingAnswer> pending;
            std::vector<MaimemoPendingAnswer> conflicts;
            parse_answers(cJSON_GetObjectItemCaseSensitive(answers_root, "pending"),
                          kMaxPendingAnswers,
                          &pending);
            parse_answers(cJSON_GetObjectItemCaseSensitive(answers_root, "conflicts"),
                          kMaxConflicts,
                          &conflicts);
            std::vector<MaimemoReviewMark> review_marks;
            parse_review_marks(cJSON_GetObjectItemCaseSensitive(answers_root, "review_marks"),
                               &review_marks);
            loaded.pending_answers = std::move(pending);
            loaded.conflicts = std::move(conflicts);
            loaded.review_marks = std::move(review_marks);
        }
        cJSON_Delete(answers_root);
    }
    // Upgrade pending answers written by older firmware into visible review
    // marks. The answer may still need server reconciliation, but it accurately
    // represents the choice the user made on the device.
    for (const auto& pending : loaded.pending_answers) {
        auto mark = std::find_if(loaded.review_marks.begin(),
                                 loaded.review_marks.end(),
                                 [&pending](const auto& existing) {
                                     return existing.day_index == pending.day_index &&
                                         ((!pending.voc_id.empty() &&
                                           existing.voc_id == pending.voc_id) ||
                                          existing.word == pending.word);
                                 });
        if (mark == loaded.review_marks.end() &&
            loaded.review_marks.size() < kMaxReviewMarks) {
            MaimemoReviewMark migrated;
            migrated.voc_id = pending.voc_id;
            migrated.word = pending.word;
            migrated.answer = pending.answer;
            migrated.day_index = pending.day_index;
            loaded.review_marks.push_back(std::move(migrated));
        }
    }
    for (auto& item : loaded.today_items) {
        if (std::any_of(loaded.pending_answers.begin(),
                        loaded.pending_answers.end(),
                        [&item](const auto& pending) {
                            return pending.voc_id == item.card.voc_id ||
                                pending.word == item.card.word;
                        })) {
            item.is_finished = true;
        }
    }

    if (!loaded.today_items.empty()) {
        loaded.current_index = std::min(loaded.current_index, loaded.today_items.size() - 1);
    } else {
        loaded.current_index = 0;
    }
    *state = std::move(loaded);
    return true;
}

bool MaimemoOfflineStore::Save(const MaimemoOfflineState& state) const {
    if (!AssetsStore::GetInstance().IsMounted()) {
        return false;
    }

    const auto& assets = AssetsStore::GetInstance();
    const std::string temp_path = assets.PathFor(kTempFile);
    const std::string state_path = assets.PathFor(kStateFile);
    const std::string backup_path = assets.PathFor(kBackupFile);
    remove(temp_path.c_str());
    remove(backup_path.c_str());

    size_t bytes_written = 0;
    if (!WriteStateFile(temp_path, state, &bytes_written)) {
        LogWriteFailure("offline state write", temp_path, bytes_written);
        remove(temp_path.c_str());
        // A valid old snapshot can consume the last available SPIFFS blocks.
        // The complete state is still held in memory, so reclaim it and retry
        // rather than rejecting the user's answer indefinitely.
        const bool removed_old_state = remove(state_path.c_str()) == 0 || errno == ENOENT;
        if (!removed_old_state || !WriteStateFile(temp_path, state, &bytes_written)) {
            LogWriteFailure("offline state retry", temp_path, bytes_written);
            remove(temp_path.c_str());
            return false;
        }
    }

    struct stat existing = {};
    const bool state_exists = stat(state_path.c_str(), &existing) == 0;
    bool moved_state_to_backup = false;
    if (state_exists) {
        remove(backup_path.c_str());
        if (rename(state_path.c_str(), backup_path.c_str()) != 0) {
            remove(temp_path.c_str());
            return false;
        }
        moved_state_to_backup = true;
    }
    if (rename(temp_path.c_str(), state_path.c_str()) != 0) {
        LogWriteFailure("offline state rename", temp_path, bytes_written);
        if (moved_state_to_backup) {
            rename(backup_path.c_str(), state_path.c_str());
        }
        remove(temp_path.c_str());
        return false;
    }
    remove(backup_path.c_str());
    return true;
}

bool MaimemoOfflineStore::SavePendingAnswers(const MaimemoOfflineState& state) const {
    if (!AssetsStore::GetInstance().IsMounted()) return false;
    const auto& assets = AssetsStore::GetInstance();
    const std::string temp_path = assets.PathFor(kAnswersTempFile);
    const std::string state_path = assets.PathFor(kAnswersFile);
    const std::string backup_path = assets.PathFor(kAnswersBackupFile);
    remove(temp_path.c_str());
    remove(backup_path.c_str());

    // Avoid creating and atomically replacing an empty journal. Successful
    // reviews now retain a compact mark here, without rewriting the much larger
    // daily-card snapshot.
    if (state.pending_answers.empty() && state.conflicts.empty() &&
        state.review_marks.empty()) {
        return remove(state_path.c_str()) == 0 || errno == ENOENT;
    }

    size_t bytes_written = 0;
    if (!WriteAnswersFile(temp_path, state, &bytes_written)) {
        LogWriteFailure("pending answers write", temp_path, bytes_written);
        remove(temp_path.c_str());
        const bool removed_old = remove(state_path.c_str()) == 0 || errno == ENOENT;
        if (!removed_old || !WriteAnswersFile(temp_path, state, &bytes_written)) {
            LogWriteFailure("pending answers retry", temp_path, bytes_written);
            remove(temp_path.c_str());
            return false;
        }
    }
    struct stat existing = {};
    const bool state_exists = stat(state_path.c_str(), &existing) == 0;
    if (state_exists) {
        if (rename(state_path.c_str(), backup_path.c_str()) != 0) {
            remove(temp_path.c_str());
            return false;
        }
    }
    if (rename(temp_path.c_str(), state_path.c_str()) != 0) {
        if (state_exists) rename(backup_path.c_str(), state_path.c_str());
        remove(temp_path.c_str());
        return false;
    }
    remove(backup_path.c_str());
    return true;
}

bool MaimemoOfflineStore::Clear() const {
    if (!AssetsStore::GetInstance().IsMounted()) {
        return false;
    }
    const auto& assets = AssetsStore::GetInstance();
    remove(assets.PathFor(kTempFile).c_str());
    remove(assets.PathFor(kBackupFile).c_str());
    remove(assets.PathFor(kAnswersTempFile).c_str());
    remove(assets.PathFor(kAnswersBackupFile).c_str());
    remove(assets.PathFor(kAnswersFile).c_str());
    return remove(assets.PathFor(kStateFile).c_str()) == 0 || !assets.FileExists(kStateFile);
}
