#include "progress_store.h"

#include "assets_store.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <nvs.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace {

constexpr char kTag[] = "ProgressStore";
constexpr char kNamespace[] = "vocab";
constexpr char kBlobKey[] = "progress";
constexpr char kSettingsOrderKey[] = "order";
constexpr char kSettingsDailyTargetKey[] = "daily_target";
constexpr char kSettingsVoiceTypeKey[] = "voice_type";
constexpr char kSettingsStudyModeKey[] = "study_mode";
constexpr char kSettingsFontModeKey[] = "font_mode";
constexpr char kSettingsAutoPronunciationKey[] = "auto_pron";
constexpr char kSettingsOnboardingKey[] = "onboarded";
constexpr char kSettingsAutoSleepKey[] = "auto_sleep";
constexpr char kSettingsDeckKey[] = "deck";
constexpr char kProgressPartitionLabel[] = "progress";
constexpr char kFileProgressPrefix[] = "/progress-";
constexpr char kFileProgressSuffix[] = ".bin";
constexpr char kFileProgressTempSuffix[] = ".part";
constexpr char kFileProgressBackupSuffix[] = ".bak";
constexpr uint32_t kMagic = 0x56434152;  // VCAR
constexpr uint16_t kVersion = 5;
constexpr uint16_t kLegacyVersion = 4;
constexpr uint32_t kRawSlotMagic = 0x56505247;  // VPRG
constexpr uint16_t kRawSlotVersion = 2;
constexpr uint16_t kLegacyRawSlotVersion = 1;
constexpr size_t kFlashSectorSize = 4096;
constexpr size_t kRawProgressSlotSize = 128 * 1024;
constexpr uint16_t kDailyTargetStep = 5;
constexpr uint16_t kMinDailyTarget = 5;
constexpr uint16_t kMaxDailyTarget = 200;

struct ProgressHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    int32_t stats_day;
    uint16_t reviewed_today;
    uint16_t correct_today;
    uint16_t streak_days;
    uint16_t reserved;
};

struct RawProgressSlotHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t sequence;
    uint32_t length;
    uint32_t checksum;
    uint32_t deck_hash;
};

struct LegacyRawProgressSlotHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t sequence;
    uint32_t length;
    uint32_t checksum;
};

struct PackedReviewState {
    int32_t due_day;
    int32_t last_review_day;
    uint16_t interval_days;
    uint8_t status;
    uint8_t correct_streak;
    uint16_t lapses;
    uint16_t reserved;
};

struct PackedReviewRecord {
    uint64_t word_key;
    PackedReviewState state;
};

bool IsSupportedProgressVersion(uint16_t version) {
    return version == kVersion || version == kLegacyVersion;
}

bool IsNvsCapacityError(esp_err_t error) {
    return error == ESP_ERR_NVS_VALUE_TOO_LONG ||
        error == ESP_ERR_NVS_NOT_ENOUGH_SPACE ||
        error == ESP_ERR_NVS_PAGE_FULL;
}

ReviewStatus ClampStatus(uint8_t raw) {
    switch (static_cast<ReviewStatus>(raw)) {
        case ReviewStatus::kNew:
        case ReviewStatus::kLearning:
        case ReviewStatus::kReview:
        case ReviewStatus::kMastered:
            return static_cast<ReviewStatus>(raw);
        default:
            return ReviewStatus::kNew;
    }
}

ReviewState DefaultState() {
    ReviewState state;
    state.status = ReviewStatus::kNew;
    state.due_day = 0;
    return state;
}

VocabularyOrderMode ClampOrderMode(uint8_t raw) {
    switch (static_cast<VocabularyOrderMode>(raw)) {
        case VocabularyOrderMode::kSequential:
        case VocabularyOrderMode::kRandom:
            return static_cast<VocabularyOrderMode>(raw);
        default:
            return VocabularyOrderMode::kSequential;
    }
}

VocabularyVoiceType ClampVoiceType(uint8_t raw) {
    switch (static_cast<VocabularyVoiceType>(raw)) {
        case VocabularyVoiceType::kAmerican:
        case VocabularyVoiceType::kBritish:
            return static_cast<VocabularyVoiceType>(raw);
        default:
            return VocabularyVoiceType::kAmerican;
    }
}

VocabularyStudyMode ClampStudyMode(uint8_t raw) {
    switch (static_cast<VocabularyStudyMode>(raw)) {
        case VocabularyStudyMode::kOffline:
        case VocabularyStudyMode::kBrowse:
        case VocabularyStudyMode::kMaimemo:
            return static_cast<VocabularyStudyMode>(raw);
        default:
            return VocabularyStudyMode::kOffline;
    }
}

VocabularyFontMode ClampFontMode(uint8_t raw) {
    switch (static_cast<VocabularyFontMode>(raw)) {
        case VocabularyFontMode::kClear:
        case VocabularyFontMode::kCompact:
            return static_cast<VocabularyFontMode>(raw);
        default:
            return VocabularyFontMode::kClear;
    }
}

VocabularyAutoSleepTimeout ClampAutoSleepTimeout(uint8_t raw) {
    switch (static_cast<VocabularyAutoSleepTimeout>(raw)) {
        case VocabularyAutoSleepTimeout::k30Minutes:
        case VocabularyAutoSleepTimeout::k1Hour:
        case VocabularyAutoSleepTimeout::k3Hours:
        case VocabularyAutoSleepTimeout::k6Hours:
        case VocabularyAutoSleepTimeout::kNever:
            return static_cast<VocabularyAutoSleepTimeout>(raw);
        default:
            return VocabularyAutoSleepTimeout::k3Hours;
    }
}

uint16_t ClampDailyTarget(uint16_t target) {
    if (target < kMinDailyTarget) {
        return kMinDailyTarget;
    }
    if (target > kMaxDailyTarget) {
        return kMaxDailyTarget;
    }
    return static_cast<uint16_t>(((target + kDailyTargetStep / 2) / kDailyTargetStep) * kDailyTargetStep);
}

uint32_t Fnv1a32(const std::string& value) {
    uint32_t hash = 2166136261U;
    for (unsigned char ch : value) {
        hash ^= ch;
        hash *= 16777619U;
    }
    return hash;
}

std::string BlobKeyForDeck(const std::string& deck_id) {
    if (deck_id.empty()) {
        return kBlobKey;
    }
    char key[12];
    snprintf(key, sizeof(key), "p%08x", static_cast<unsigned>(Fnv1a32(deck_id)));
    return key;
}

std::string FileKeyForDeck(const std::string& deck_id) {
    return BlobKeyForDeck(deck_id);
}

std::string FilePathForDeck(const std::string& deck_id) {
    return std::string(kFileProgressPrefix) + FileKeyForDeck(deck_id) + kFileProgressSuffix;
}

std::string LegacyAssetsProgressPathForDeck(const std::string& deck_id) {
    return AssetsStore::GetInstance().PathFor(FilePathForDeck(deck_id));
}

std::string IndexKeyForDeck(const std::string& deck_id) {
    if (deck_id.empty()) {
        return "index";
    }
    char key[12];
    snprintf(key, sizeof(key), "i%08x", static_cast<unsigned>(Fnv1a32(deck_id)));
    return key;
}

uint32_t BlobChecksum(const std::vector<uint8_t>& blob) {
    uint32_t hash = 2166136261U;
    for (const uint8_t value : blob) {
        hash ^= value;
        hash *= 16777619U;
    }
    return hash;
}

const esp_partition_t* ProgressPartition() {
    static const esp_partition_t* partition = nullptr;
    static bool looked_up = false;
    if (!looked_up) {
        partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA,
            ESP_PARTITION_SUBTYPE_ANY,
            kProgressPartitionLabel);
        looked_up = true;
        if (partition != nullptr) {
            ESP_LOGI(kTag,
                     "Using progress partition offset=0x%lx size=%lu",
                     static_cast<unsigned long>(partition->address),
                     static_cast<unsigned long>(partition->size));
        } else {
            ESP_LOGW(kTag, "progress partition not found");
        }
    }
    return partition;
}

bool RawProgressSlotOffset(const esp_partition_t* partition,
                           uint8_t slot,
                           size_t* offset,
                           size_t* slot_size) {
    if (partition == nullptr || offset == nullptr || slot_size == nullptr || slot > 1) {
        return false;
    }
    *slot_size = kRawProgressSlotSize;
    *offset = static_cast<size_t>(slot) * *slot_size;
    if (*slot_size <= sizeof(RawProgressSlotHeader) ||
        (*slot_size % kFlashSectorSize) != 0 ||
        *offset + *slot_size > partition->size) {
        ESP_LOGW(kTag, "invalid progress slot layout size=%u",
                 static_cast<unsigned>(partition->size));
        return false;
    }
    return true;
}

bool ReadRawProgressSlot(const esp_partition_t* partition,
                         uint8_t slot,
                         uint32_t deck_hash,
                         bool allow_legacy,
                         std::vector<uint8_t>* blob,
                         uint32_t* sequence) {
    size_t offset = 0;
    size_t slot_size = 0;
    if (blob == nullptr ||
        !RawProgressSlotOffset(partition, slot, &offset, &slot_size)) {
        return false;
    }

    LegacyRawProgressSlotHeader prefix = {};
    esp_err_t ret = esp_partition_read(partition, offset, &prefix, sizeof(prefix));
    if (ret != ESP_OK || prefix.magic != kRawSlotMagic ||
        (prefix.version != kRawSlotVersion &&
         !(allow_legacy && prefix.version == kLegacyRawSlotVersion))) {
        return false;
    }

    size_t header_size = sizeof(LegacyRawProgressSlotHeader);
    if (prefix.version == kRawSlotVersion) {
        RawProgressSlotHeader header = {};
        ret = esp_partition_read(partition, offset, &header, sizeof(header));
        if (ret != ESP_OK || header.deck_hash != deck_hash) {
            return false;
        }
        header_size = sizeof(RawProgressSlotHeader);
    }
    if (prefix.length < sizeof(ProgressHeader) ||
        prefix.length > slot_size - header_size ||
        prefix.length > 256 * 1024) {
        return false;
    }

    blob->assign(prefix.length, 0);
    ret = esp_partition_read(
        partition,
        offset + header_size,
        blob->data(),
        blob->size());
    if (ret != ESP_OK || BlobChecksum(*blob) != prefix.checksum) {
        blob->clear();
        return false;
    }

    ProgressHeader progress_header = {};
    memcpy(&progress_header, blob->data(), sizeof(progress_header));
    if (progress_header.magic != kMagic ||
        !IsSupportedProgressVersion(progress_header.version)) {
        blob->clear();
        return false;
    }
    if (sequence != nullptr) {
        *sequence = prefix.sequence;
    }
    return true;
}

bool LoadRawProgressFile(const std::string& deck_id,
                         bool allow_legacy,
                         std::vector<uint8_t>* blob) {
    const esp_partition_t* partition = ProgressPartition();
    if (partition == nullptr || blob == nullptr) {
        return false;
    }

    std::vector<uint8_t> slot0;
    uint32_t seq0 = 0;
    const uint32_t deck_hash = Fnv1a32(deck_id);
    const bool slot0_valid = ReadRawProgressSlot(
        partition, 0, deck_hash, allow_legacy, &slot0, &seq0);
    std::vector<uint8_t> slot1;
    uint32_t seq1 = 0;
    const bool slot1_valid = ReadRawProgressSlot(
        partition, 1, deck_hash, allow_legacy, &slot1, &seq1);

    if (slot0_valid && slot1_valid) {
        if (static_cast<int32_t>(seq0 - seq1) >= 0) {
            *blob = std::move(slot0);
        } else {
            *blob = std::move(slot1);
        }
        return true;
    }
    if (slot0_valid) {
        *blob = std::move(slot0);
        return true;
    }
    if (slot1_valid) {
        *blob = std::move(slot1);
        return true;
    }
    return false;
}

bool SaveRawProgressFile(const std::string& deck_id, std::vector<uint8_t> blob) {
    const esp_partition_t* partition = ProgressPartition();
    if (partition == nullptr || blob.size() < sizeof(ProgressHeader)) {
        return false;
    }

    std::vector<uint8_t> existing0;
    uint32_t seq0 = 0;
    const uint32_t deck_hash = Fnv1a32(deck_id);
    const bool slot0_valid = ReadRawProgressSlot(
        partition, 0, deck_hash, false, &existing0, &seq0);
    std::vector<uint8_t> existing1;
    uint32_t seq1 = 0;
    const bool slot1_valid = ReadRawProgressSlot(
        partition, 1, deck_hash, false, &existing1, &seq1);

    uint8_t target_slot = 0;
    uint32_t next_sequence = 1;
    if (slot0_valid && slot1_valid) {
        const bool slot0_newer = static_cast<int32_t>(seq0 - seq1) >= 0;
        target_slot = slot0_newer ? 1 : 0;
        next_sequence = (slot0_newer ? seq0 : seq1) + 1;
    } else if (slot0_valid) {
        target_slot = 1;
        next_sequence = seq0 + 1;
    } else if (slot1_valid) {
        target_slot = 0;
        next_sequence = seq1 + 1;
    }

    ProgressHeader progress_header = {};
    memcpy(&progress_header, blob.data(), sizeof(progress_header));
    progress_header.reserved = static_cast<uint16_t>(next_sequence);
    memcpy(blob.data(), &progress_header, sizeof(progress_header));

    size_t offset = 0;
    size_t slot_size = 0;
    if (!RawProgressSlotOffset(partition, target_slot, &offset, &slot_size) ||
        blob.size() > slot_size - sizeof(RawProgressSlotHeader)) {
        return false;
    }

    RawProgressSlotHeader raw_header = {};
    raw_header.magic = kRawSlotMagic;
    raw_header.version = kRawSlotVersion;
    raw_header.sequence = next_sequence;
    raw_header.length = static_cast<uint32_t>(blob.size());
    raw_header.checksum = BlobChecksum(blob);
    raw_header.deck_hash = deck_hash;

    esp_err_t ret = esp_partition_erase_range(partition, offset, slot_size);
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "erase progress slot failed slot=%u err=%s",
                 static_cast<unsigned>(target_slot), esp_err_to_name(ret));
        return false;
    }
    ret = esp_partition_write(partition, offset, &raw_header, sizeof(raw_header));
    if (ret == ESP_OK) {
        ret = esp_partition_write(
            partition,
            offset + sizeof(RawProgressSlotHeader),
            blob.data(),
            blob.size());
    }
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "write progress slot failed slot=%u err=%s",
                 static_cast<unsigned>(target_slot), esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI(kTag,
             "saved progress raw deck=%s slot=%u sequence=%lu bytes=%u",
             deck_id.c_str(),
             static_cast<unsigned>(target_slot),
             static_cast<unsigned long>(next_sequence),
             static_cast<unsigned>(blob.size()));
    return true;
}

bool ReadProgressFile(const std::string& path, std::vector<uint8_t>* blob) {
    if (blob == nullptr) {
        return false;
    }
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    const long size = ftell(file);
    if (size < static_cast<long>(sizeof(ProgressHeader)) || size > 256 * 1024) {
        fclose(file);
        return false;
    }
    rewind(file);
    blob->assign(static_cast<size_t>(size), 0);
    const bool ok = fread(blob->data(), 1, blob->size(), file) == blob->size();
    fclose(file);
    if (!ok) {
        return false;
    }
    ProgressHeader header = {};
    memcpy(&header, blob->data(), sizeof(header));
    return header.magic == kMagic && IsSupportedProgressVersion(header.version);
}

bool ReadProgressHeader(const std::string& path, ProgressHeader* header) {
    if (header == nullptr) {
        return false;
    }
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }
    const bool ok = fread(header, 1, sizeof(*header), file) == sizeof(*header);
    fclose(file);
    return ok && header->magic == kMagic && IsSupportedProgressVersion(header->version);
}

bool SequenceNewerOrEqual(uint16_t a, uint16_t b) {
    return static_cast<int16_t>(a - b) >= 0;
}

bool LoadProgressFile(const std::string& deck_id, std::vector<uint8_t>* blob) {
    if (blob == nullptr) {
        return false;
    }

    if (AssetsStore::GetInstance().IsMounted()) {
        const std::string legacy_path = LegacyAssetsProgressPathForDeck(deck_id);
        const std::string legacy_backup_path = legacy_path + kFileProgressBackupSuffix;
        ProgressHeader legacy_main_header = {};
        const bool legacy_main_valid = ReadProgressHeader(legacy_path, &legacy_main_header);
        ProgressHeader legacy_backup_header = {};
        const bool legacy_backup_valid = ReadProgressHeader(legacy_backup_path, &legacy_backup_header);
        if (legacy_main_valid && legacy_backup_valid) {
            if (SequenceNewerOrEqual(legacy_main_header.reserved, legacy_backup_header.reserved)) {
                if (ReadProgressFile(legacy_path, blob)) {
                    ESP_LOGI(kTag, "loaded legacy assets progress %s", legacy_path.c_str());
                    return true;
                }
                return ReadProgressFile(legacy_backup_path, blob);
            }
            if (ReadProgressFile(legacy_backup_path, blob)) {
                ESP_LOGI(kTag, "loaded legacy assets progress %s", legacy_backup_path.c_str());
                return true;
            }
            return ReadProgressFile(legacy_path, blob);
        }
        if (legacy_main_valid && ReadProgressFile(legacy_path, blob)) {
            ESP_LOGI(kTag, "loaded legacy assets progress %s", legacy_path.c_str());
            return true;
        }
        if (legacy_backup_valid && ReadProgressFile(legacy_backup_path, blob)) {
            ESP_LOGI(kTag, "loaded legacy assets progress %s", legacy_backup_path.c_str());
            return true;
        }
    }

    // Version 1 raw slots did not carry a deck identifier. They are accepted
    // only as a final migration fallback for the active deck. Every subsequent
    // save writes a deck-bound version 2 slot and a per-deck assets copy.
    if (LoadRawProgressFile(deck_id, true, blob)) {
        return true;
    }

    ESP_LOGW(kTag, "no valid progress data found for deck=%s", deck_id.c_str());
    return false;
}

bool SaveProgressFile(const std::string& deck_id, const std::vector<uint8_t>& blob) {
    bool file_saved = false;
    if (AssetsStore::GetInstance().IsMounted()) {
        const std::string path = LegacyAssetsProgressPathForDeck(deck_id);
        const std::string temp_path = path + kFileProgressTempSuffix;
        const std::string backup_path = path + kFileProgressBackupSuffix;
        FILE* file = fopen(temp_path.c_str(), "wb");
        if (file != nullptr) {
            const bool wrote = fwrite(blob.data(), 1, blob.size(), file) == blob.size();
            const bool flushed = fflush(file) == 0;
            const bool synced = flushed && fsync(fileno(file)) == 0;
            fclose(file);
            if (wrote && synced) {
                remove(backup_path.c_str());
                const bool had_previous = rename(path.c_str(), backup_path.c_str()) == 0;
                if (rename(temp_path.c_str(), path.c_str()) == 0) {
                    file_saved = true;
                } else {
                    if (had_previous) {
                        (void)rename(backup_path.c_str(), path.c_str());
                    }
                    remove(temp_path.c_str());
                }
            } else {
                remove(temp_path.c_str());
            }
        }
    }

    const bool raw_saved = SaveRawProgressFile(deck_id, blob);
    return file_saved || raw_saved;
}

void DeleteProgressFile(const std::string& deck_id) {
    const esp_partition_t* partition = ProgressPartition();
    if (partition != nullptr) {
        const uint32_t deck_hash = Fnv1a32(deck_id);
        for (uint8_t slot = 0; slot < 2; ++slot) {
            size_t offset = 0;
            size_t slot_size = 0;
            if (!RawProgressSlotOffset(partition, slot, &offset, &slot_size)) {
                continue;
            }
            LegacyRawProgressSlotHeader prefix = {};
            if (esp_partition_read(partition, offset, &prefix, sizeof(prefix)) != ESP_OK ||
                prefix.magic != kRawSlotMagic) {
                continue;
            }
            bool matches = prefix.version == kLegacyRawSlotVersion;
            if (prefix.version == kRawSlotVersion) {
                RawProgressSlotHeader header = {};
                matches = esp_partition_read(partition, offset, &header, sizeof(header)) == ESP_OK &&
                    header.deck_hash == deck_hash;
            }
            if (!matches) {
                continue;
            }
            const esp_err_t ret = esp_partition_erase_range(partition, offset, slot_size);
            if (ret != ESP_OK) {
                ESP_LOGW(kTag, "erase progress slot failed slot=%u: %s",
                         static_cast<unsigned>(slot), esp_err_to_name(ret));
            }
        }
    }
    if (AssetsStore::GetInstance().IsMounted()) {
        const std::string path = LegacyAssetsProgressPathForDeck(deck_id);
        remove(path.c_str());
        remove((path + kFileProgressTempSuffix).c_str());
        remove((path + kFileProgressBackupSuffix).c_str());
    }
}

bool ApplyProgressBlob(const std::vector<uint8_t>& blob,
                       const std::vector<uint64_t>& word_keys,
                       std::vector<ReviewState>* states,
                       VocabularyDailyStats* stats) {
    if (blob.size() < sizeof(ProgressHeader) || states == nullptr || stats == nullptr) {
        return false;
    }

    ProgressHeader header = {};
    memcpy(&header, blob.data(), sizeof(header));
    if (header.magic != kMagic || !IsSupportedProgressVersion(header.version)) {
        ESP_LOGW(kTag, "ignore incompatible progress blob");
        return false;
    }

    stats->day = header.stats_day;
    stats->reviewed = header.reviewed_today;
    stats->correct = header.correct_today;
    stats->streak_days = header.streak_days;

    auto unpack = [](const PackedReviewState& packed) {
        ReviewState state;
        state.status = ClampStatus(packed.status);
        state.due_day = packed.due_day;
        state.last_review_day = packed.last_review_day;
        state.interval_days = packed.interval_days;
        state.correct_streak = packed.correct_streak;
        state.lapses = packed.lapses;
        return state;
    };

    if (header.version == kLegacyVersion) {
        const size_t available_count =
            (blob.size() - sizeof(ProgressHeader)) / sizeof(PackedReviewState);
        const size_t copy_count = std::min({
            word_keys.size(), static_cast<size_t>(header.count), available_count});
        for (size_t i = 0; i < copy_count; ++i) {
            PackedReviewState packed = {};
            memcpy(&packed,
                   blob.data() + sizeof(ProgressHeader) + i * sizeof(PackedReviewState),
                   sizeof(packed));
            (*states)[i] = unpack(packed);
        }
        ESP_LOGI(kTag, "migrated legacy index-based progress entries=%u",
                 static_cast<unsigned>(copy_count));
        return true;
    }

    const size_t available_count =
        (blob.size() - sizeof(ProgressHeader)) / sizeof(PackedReviewRecord);
    const size_t record_count =
        std::min(static_cast<size_t>(header.count), available_count);
    std::vector<PackedReviewRecord> saved(record_count);
    for (size_t i = 0; i < record_count; ++i) {
        memcpy(&saved[i],
               blob.data() + sizeof(ProgressHeader) + i * sizeof(PackedReviewRecord),
               sizeof(PackedReviewRecord));
    }
    std::sort(saved.begin(), saved.end(), [](const auto& left, const auto& right) {
        return left.word_key < right.word_key;
    });
    size_t matched = 0;
    for (size_t i = 0; i < word_keys.size(); ++i) {
        const auto it = std::lower_bound(
            saved.begin(), saved.end(), word_keys[i],
            [](const PackedReviewRecord& record, uint64_t key) {
                return record.word_key < key;
            });
        if (it != saved.end() && it->word_key == word_keys[i]) {
            (*states)[i] = unpack(it->state);
            ++matched;
        }
    }
    ESP_LOGI(kTag, "restored stable-key progress matched=%u saved=%u current=%u",
             static_cast<unsigned>(matched),
             static_cast<unsigned>(record_count),
             static_cast<unsigned>(word_keys.size()));
    return true;
}

}  // namespace

bool ProgressStore::Load(const std::string& deck_id,
                         const std::vector<uint64_t>& word_keys,
                         std::vector<ReviewState>* states,
                         VocabularyDailyStats* stats) {
    if (states == nullptr || stats == nullptr) {
        return false;
    }

    states->assign(word_keys.size(), DefaultState());
    *stats = VocabularyDailyStats{};

    std::vector<uint8_t> file_blob;
    if (LoadProgressFile(deck_id, &file_blob)) {
        ProgressHeader header = {};
        memcpy(&header, file_blob.data(), sizeof(header));
        const bool loaded = ApplyProgressBlob(file_blob, word_keys, states, stats);
        if (loaded && header.version == kLegacyVersion &&
            !Save(deck_id, word_keys, *states, *stats)) {
            ESP_LOGW(kTag, "legacy progress loaded but immediate migration failed");
        }
        return loaded;
    }

    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "open failed: %s", esp_err_to_name(ret));
        return false;
    }

    size_t blob_size = 0;
    const std::string blob_key = BlobKeyForDeck(deck_id);
    ret = nvs_get_blob(handle, blob_key.c_str(), nullptr, &blob_size);
    if (ret != ESP_OK || blob_size < sizeof(ProgressHeader)) {
        nvs_close(handle);
        return true;
    }

    std::vector<uint8_t> blob(blob_size);
    ret = nvs_get_blob(handle, blob_key.c_str(), blob.data(), &blob_size);
    nvs_close(handle);
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "read failed: %s", esp_err_to_name(ret));
        return false;
    }

    ProgressHeader header = {};
    memcpy(&header, blob.data(), sizeof(header));
    const bool loaded = ApplyProgressBlob(blob, word_keys, states, stats);
    if (loaded && header.version == kLegacyVersion &&
        !Save(deck_id, word_keys, *states, *stats)) {
        ESP_LOGW(kTag, "legacy NVS progress loaded but immediate migration failed");
    }
    return loaded;
}

bool ProgressStore::Load(const std::string& deck_id,
                         size_t word_count,
                         std::vector<ReviewState>* states,
                         VocabularyDailyStats* stats) {
    std::vector<uint64_t> positional_keys(word_count);
    for (size_t i = 0; i < word_count; ++i) {
        positional_keys[i] = static_cast<uint64_t>(i) + 1;
    }
    return Load(deck_id, positional_keys, states, stats);
}

bool ProgressStore::Load(size_t word_count,
                         std::vector<ReviewState>* states,
                         VocabularyDailyStats* stats) {
    return Load(std::string(), word_count, states, stats);
}

bool ProgressStore::Save(const std::string& deck_id,
                         const std::vector<uint64_t>& word_keys,
                         const std::vector<ReviewState>& states,
                         const VocabularyDailyStats& stats) {
    ProgressHeader header = {};
    header.magic = kMagic;
    header.version = kVersion;
    header.count = static_cast<uint16_t>(
        std::min({word_keys.size(), states.size(), static_cast<size_t>(UINT16_MAX)}));
    header.stats_day = stats.day;
    header.reviewed_today = stats.reviewed;
    header.correct_today = stats.correct;
    header.streak_days = stats.streak_days;

    const size_t packed_count = header.count;
    std::vector<uint8_t> blob(sizeof(ProgressHeader) + packed_count * sizeof(PackedReviewRecord));
    memcpy(blob.data(), &header, sizeof(header));
    for (size_t i = 0; i < packed_count; ++i) {
        PackedReviewRecord record = {};
        record.word_key = word_keys[i];
        record.state.due_day = states[i].due_day;
        record.state.last_review_day = states[i].last_review_day;
        record.state.interval_days = states[i].interval_days;
        record.state.status = static_cast<uint8_t>(states[i].status);
        record.state.correct_streak = states[i].correct_streak;
        record.state.lapses = states[i].lapses;
        memcpy(blob.data() + sizeof(ProgressHeader) + i * sizeof(PackedReviewRecord),
               &record,
               sizeof(record));
    }

    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "open write failed: %s", esp_err_to_name(ret));
        return SaveProgressFile(deck_id, blob);
    }

    const std::string blob_key = BlobKeyForDeck(deck_id);
    ret = nvs_set_blob(handle, blob_key.c_str(), blob.data(), blob.size());
    if (IsNvsCapacityError(ret)) {
        nvs_close(handle);
        if (!SaveProgressFile(deck_id, blob)) {
            ESP_LOGW(kTag, "save file progress failed");
            return false;
        }
        ESP_LOGI(kTag, "saved progress file %s (%u bytes)",
                 FilePathForDeck(deck_id).c_str(),
                 static_cast<unsigned>(blob.size()));
        return true;
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "save failed: %s", esp_err_to_name(ret));
        return SaveProgressFile(deck_id, blob);
    }
    DeleteProgressFile(deck_id);
    return true;
}

bool ProgressStore::Save(const std::string& deck_id,
                         const std::vector<ReviewState>& states,
                         const VocabularyDailyStats& stats) {
    std::vector<uint64_t> positional_keys(states.size());
    for (size_t i = 0; i < states.size(); ++i) {
        positional_keys[i] = static_cast<uint64_t>(i) + 1;
    }
    return Save(deck_id, positional_keys, states, stats);
}

bool ProgressStore::Save(const std::vector<ReviewState>& states,
                         const VocabularyDailyStats& stats) {
    return Save(std::string(), states, stats);
}

bool ProgressStore::LoadSettings(VocabularySettings* settings) {
    if (settings == nullptr) {
        return false;
    }

    *settings = VocabularySettings{};

    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "open settings failed: %s", esp_err_to_name(ret));
        return false;
    }

    // This namespace predates onboarding. Existing vocabulary data means this
    // is an upgraded device, not a first boot. A genuinely erased device has
    // no namespace and keeps the default false value above.
    settings->onboarding_completed = true;

    uint8_t order = static_cast<uint8_t>(settings->order_mode);
    ret = nvs_get_u8(handle, kSettingsOrderKey, &order);
    if (ret == ESP_OK) {
        settings->order_mode = ClampOrderMode(order);
    } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(kTag, "read order failed: %s", esp_err_to_name(ret));
    }

    uint16_t daily_target = settings->daily_target;
    ret = nvs_get_u16(handle, kSettingsDailyTargetKey, &daily_target);
    if (ret == ESP_OK) {
        settings->daily_target = ClampDailyTarget(daily_target);
    } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(kTag, "read daily target failed: %s", esp_err_to_name(ret));
    }

    uint8_t voice_type = static_cast<uint8_t>(settings->voice_type);
    ret = nvs_get_u8(handle, kSettingsVoiceTypeKey, &voice_type);
    if (ret == ESP_OK) {
        settings->voice_type = ClampVoiceType(voice_type);
    } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(kTag, "read voice type failed: %s", esp_err_to_name(ret));
    }

    uint8_t study_mode = static_cast<uint8_t>(settings->study_mode);
    ret = nvs_get_u8(handle, kSettingsStudyModeKey, &study_mode);
    if (ret == ESP_OK) {
        settings->study_mode = ClampStudyMode(study_mode);
    } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(kTag, "read study mode failed: %s", esp_err_to_name(ret));
    }

    uint8_t font_mode = static_cast<uint8_t>(settings->font_mode);
    ret = nvs_get_u8(handle, kSettingsFontModeKey, &font_mode);
    if (ret == ESP_OK) {
        settings->font_mode = ClampFontMode(font_mode);
    } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(kTag, "read font mode failed: %s", esp_err_to_name(ret));
    }

    uint8_t auto_pronunciation = settings->auto_pronunciation ? 1 : 0;
    ret = nvs_get_u8(handle, kSettingsAutoPronunciationKey, &auto_pronunciation);
    if (ret == ESP_OK) {
        settings->auto_pronunciation = auto_pronunciation != 0;
    } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(kTag, "read auto pronunciation failed: %s", esp_err_to_name(ret));
    }

    uint8_t onboarding_completed = settings->onboarding_completed ? 1 : 0;
    ret = nvs_get_u8(handle, kSettingsOnboardingKey, &onboarding_completed);
    if (ret == ESP_OK) {
        settings->onboarding_completed = onboarding_completed != 0;
    } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(kTag, "read onboarding state failed: %s", esp_err_to_name(ret));
    }

    uint8_t auto_sleep_timeout = static_cast<uint8_t>(settings->auto_sleep_timeout);
    ret = nvs_get_u8(handle, kSettingsAutoSleepKey, &auto_sleep_timeout);
    if (ret == ESP_OK) {
        settings->auto_sleep_timeout = ClampAutoSleepTimeout(auto_sleep_timeout);
    } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(kTag, "read auto sleep timeout failed: %s", esp_err_to_name(ret));
    }

    size_t deck_len = 0;
    ret = nvs_get_str(handle, kSettingsDeckKey, nullptr, &deck_len);
    if (ret == ESP_OK && deck_len > 1) {
        settings->active_deck_id.assign(deck_len, '\0');
        ret = nvs_get_str(handle, kSettingsDeckKey, settings->active_deck_id.data(), &deck_len);
        if (ret == ESP_OK) {
            while (!settings->active_deck_id.empty() && settings->active_deck_id.back() == '\0') {
                settings->active_deck_id.pop_back();
            }
        } else {
            settings->active_deck_id.clear();
            ESP_LOGW(kTag, "read active deck failed: %s", esp_err_to_name(ret));
        }
    } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(kTag, "read active deck length failed: %s", esp_err_to_name(ret));
    }

    nvs_close(handle);
    return true;
}

bool ProgressStore::SaveSettings(const VocabularySettings& settings) {
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "open settings write failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = nvs_set_u8(handle, kSettingsOrderKey, static_cast<uint8_t>(settings.order_mode));
    if (ret == ESP_OK) {
        ret = nvs_set_u16(handle, kSettingsDailyTargetKey, ClampDailyTarget(settings.daily_target));
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(handle, kSettingsVoiceTypeKey, static_cast<uint8_t>(settings.voice_type));
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(handle, kSettingsStudyModeKey, static_cast<uint8_t>(settings.study_mode));
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(handle, kSettingsFontModeKey, static_cast<uint8_t>(settings.font_mode));
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(handle,
                         kSettingsAutoPronunciationKey,
                         settings.auto_pronunciation ? 1 : 0);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(handle,
                         kSettingsOnboardingKey,
                         settings.onboarding_completed ? 1 : 0);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_u8(handle,
                         kSettingsAutoSleepKey,
                         static_cast<uint8_t>(ClampAutoSleepTimeout(
                             static_cast<uint8_t>(settings.auto_sleep_timeout))));
    }
    if (ret == ESP_OK) {
        if (settings.active_deck_id.empty()) {
            ret = nvs_erase_key(handle, kSettingsDeckKey);
            if (ret == ESP_ERR_NVS_NOT_FOUND) {
                ret = ESP_OK;
            }
        } else {
            ret = nvs_set_str(handle, kSettingsDeckKey, settings.active_deck_id.c_str());
        }
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);

    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "save settings failed: %s", esp_err_to_name(ret));
        return false;
    }
    return true;
}

bool ProgressStore::LoadCurrentIndex(const std::string& deck_id, size_t* index) {
    if (index == nullptr) {
        return false;
    }
    *index = 0;

    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "open index read failed: %s", esp_err_to_name(ret));
        return false;
    }

    uint32_t value = 0;
    const std::string key = IndexKeyForDeck(deck_id);
    ret = nvs_get_u32(handle, key.c_str(), &value);
    nvs_close(handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "read current index failed: %s", esp_err_to_name(ret));
        return false;
    }

    *index = static_cast<size_t>(value);
    return true;
}

bool ProgressStore::SaveCurrentIndex(const std::string& deck_id, size_t index) {
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "open index write failed: %s", esp_err_to_name(ret));
        return false;
    }

    const std::string key = IndexKeyForDeck(deck_id);
    const uint32_t value = static_cast<uint32_t>(std::min<size_t>(index, UINT32_MAX));
    ret = nvs_set_u32(handle, key.c_str(), value);
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);

    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "save current index failed: %s", esp_err_to_name(ret));
        return false;
    }
    return true;
}

bool ProgressStore::ClearProgress(const std::string& deck_id) {
    nvs_handle_t handle = 0;
    esp_err_t ret = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "open clear failed: %s", esp_err_to_name(ret));
        return false;
    }

    const std::string blob_key = BlobKeyForDeck(deck_id);
    ret = nvs_erase_key(handle, blob_key.c_str());
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);

    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "clear failed: %s", esp_err_to_name(ret));
        return false;
    }
    DeleteProgressFile(deck_id);
    return true;
}

bool ProgressStore::ClearProgress() {
    return ClearProgress(std::string());
}

void ProgressStore::ResetDailyStatsIfNeeded(int32_t today,
                                            std::vector<ReviewState>* states,
                                            VocabularyDailyStats* stats) {
    if (states == nullptr || stats == nullptr) {
        return;
    }
    if (stats->day == today) {
        return;
    }

    if (stats->day != 0 && stats->reviewed > 0) {
        stats->streak_days = static_cast<uint16_t>(
            std::min<int>(static_cast<int>(stats->streak_days) + 1, UINT16_MAX));
    }
    stats->day = today;
    stats->reviewed = 0;
    stats->correct = 0;

    for (auto& state : *states) {
        if (state.due_day < 0) {
            state.due_day = today;
        }
    }
}
