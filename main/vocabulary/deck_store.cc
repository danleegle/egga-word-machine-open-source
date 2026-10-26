#include "deck_store.h"

#include "assets_store.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace {

constexpr char kTag[] = "DeckStore";
constexpr const char* kDeckId = "egga-demo-en-zh";
constexpr const char* kStaticDeckName = "Egga Demo";
constexpr const char* kStaticDeckLanguage = "en-zh";
constexpr uint16_t kStaticDeckVersion = 0;
constexpr const char* kDecksDir = "/decks";
constexpr const char* kDeckIndexPath = "/decks/index.tsv";
constexpr const char* kAssetDeckCandidatePaths[] = {
    "/decks/current.tsv",
    "/decks/current.txt",
};
constexpr size_t kMaxDeckLineBytes = 4096;
constexpr size_t kInvalidTextOffset = static_cast<size_t>(-1);
constexpr size_t kDeckLoadYieldInterval = 16;
constexpr bool kEnableAssetDeckCacheWrite = false;
constexpr size_t kMaxDeckCacheTextBytes = 6 * 1024 * 1024;
constexpr uint32_t kAssetDeckCacheMagic = 0x43444745;  // EGDC
constexpr uint16_t kAssetDeckCacheVersion = 1;

struct AssetDeckCacheHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t word_count;
    uint32_t text_bytes;
    uint32_t source_size;
};

struct CachedAssetWordCard {
    uint16_t id;
    uint16_t reserved;
    uint32_t word_offset;
    uint32_t phonetic_offset;
    uint32_t meaning_offset;
    uint32_t example_offset;
    uint32_t example_translation_offset;
};

const WordCard kKaoyanFullDeck[] = {
#include "demo_deck.inc"
};

std::string Trim(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }

    size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }

    return value.substr(begin, end - begin);
}

std::string Lower(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::string FileStem(const std::string& path) {
    size_t begin = path.find_last_of("/\\");
    begin = begin == std::string::npos ? 0 : begin + 1;
    size_t end = path.find_last_of('.');
    if (end == std::string::npos || end < begin) {
        end = path.size();
    }
    return path.substr(begin, end - begin);
}

std::string ReplaceExtension(const std::string& path, const char* extension) {
    const size_t slash = path.find_last_of("/\\");
    const size_t dot = path.find_last_of('.');
    const size_t end = (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        ? path.size()
        : dot;
    return path.substr(0, end) + extension;
}

bool HasDeckExtension(const std::string& path) {
    const std::string lower = Lower(path);
    return lower.size() >= 4 &&
        (lower.rfind(".tsv") == lower.size() - 4 ||
         lower.rfind(".txt") == lower.size() - 4);
}

bool LooksLikeDeckRow(const std::vector<std::string>& fields) {
    if (fields.size() < 2 || fields[0].empty()) {
        return false;
    }
    if (Lower(fields[0]) == "word") {
        return false;
    }
    // Two-column decks store the meaning in column 2. Extended decks store
    // pronunciation in column 2 and meaning in column 3; pronunciation is
    // optional, so it must not make an otherwise valid card disappear from
    // deck discovery.
    return fields.size() == 2 ? !fields[1].empty() : !fields[2].empty();
}

bool ContainsDeck(const std::vector<DeckInfo>& decks, const std::string& id) {
    return std::find_if(decks.begin(), decks.end(), [&id](const DeckInfo& deck) {
        return deck.id == id;
    }) != decks.end();
}

bool ContainsString(const std::vector<std::string>& values, const std::string& target) {
    return std::find(values.begin(), values.end(), target) != values.end();
}

uint16_t ParseVersion(const std::string& value, uint16_t fallback) {
    if (value.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long parsed = strtoul(value.c_str(), &end, 10);
    if (end == value.c_str() || parsed > UINT16_MAX) {
        return fallback;
    }
    return static_cast<uint16_t>(parsed);
}

size_t ParseSize(const std::string& value, size_t fallback) {
    if (value.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const unsigned long parsed = strtoul(value.c_str(), &end, 10);
    if (end == value.c_str()) {
        return fallback;
    }
    return static_cast<size_t>(parsed);
}

void StripUtf8Bom(std::string* line) {
    if (line != nullptr && line->size() >= 3 &&
        static_cast<unsigned char>((*line)[0]) == 0xef &&
        static_cast<unsigned char>((*line)[1]) == 0xbb &&
        static_cast<unsigned char>((*line)[2]) == 0xbf) {
        line->erase(0, 3);
    }
}

bool ReadLine(FILE* file, std::string* line) {
    if (file == nullptr || line == nullptr) {
        return false;
    }

    line->clear();
    bool overflow = false;
    char buffer[kMaxDeckLineBytes + 3];
    while (fgets(buffer, sizeof(buffer), file) != nullptr) {
        size_t len = strlen(buffer);
        const bool has_newline = len > 0 && buffer[len - 1] == '\n';
        while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r')) {
            buffer[--len] = '\0';
        }

        if (len > 0) {
            const size_t remaining = kMaxDeckLineBytes > line->size()
                ? kMaxDeckLineBytes - line->size()
                : 0;
            if (remaining > 0) {
                line->append(buffer, std::min(len, remaining));
            }
            if (len > remaining && !overflow) {
                ESP_LOGW(kTag, "Asset deck line exceeds %u bytes, truncating",
                         static_cast<unsigned>(kMaxDeckLineBytes));
                overflow = true;
            }
        }
        if (has_newline) {
            return true;
        }
    }
    return !line->empty() || overflow;
}

std::vector<std::string> SplitTabs(const std::string& line) {
    std::vector<std::string> fields;
    size_t start = 0;
    while (start <= line.size()) {
        const size_t tab = line.find('\t', start);
        if (tab == std::string::npos) {
            fields.push_back(Trim(line.substr(start)));
            break;
        }
        fields.push_back(Trim(line.substr(start, tab - start)));
        start = tab + 1;
    }
    return fields;
}

void ApplyMetadata(const std::string& line,
                   std::string* deck_id,
                   std::string* name,
                   std::string* language,
                   uint16_t* version) {
    const std::string text = Trim(line.substr(1));
    const size_t equals = text.find('=');
    if (equals == std::string::npos) {
        return;
    }

    const std::string key = Lower(Trim(text.substr(0, equals)));
    const std::string value = Trim(text.substr(equals + 1));
    if (value.empty()) {
        return;
    }

    if (key == "deck_id" || key == "deck-id" || key == "id") {
        *deck_id = value;
    } else if (key == "name") {
        *name = value;
    } else if (key == "language" || key == "lang") {
        *language = value;
    } else if (key == "version") {
        const long parsed = strtol(value.c_str(), nullptr, 10);
        if (parsed > 0 && parsed <= UINT16_MAX) {
            *version = static_cast<uint16_t>(parsed);
        }
    }
}

struct PendingAssetWordCard {
    size_t word_offset = kInvalidTextOffset;
    size_t phonetic_offset = kInvalidTextOffset;
    size_t meaning_offset = kInvalidTextOffset;
    size_t example_offset = kInvalidTextOffset;
    size_t example_translation_offset = kInvalidTextOffset;
};

size_t AppendText(std::string* pool, const std::string& value) {
    if (pool == nullptr || value.empty()) {
        return kInvalidTextOffset;
    }

    const size_t offset = pool->size();
    pool->append(value);
    pool->push_back('\0');
    return offset;
}

const char* TextAt(const std::string& pool, size_t offset) {
    if (offset == kInvalidTextOffset || offset >= pool.size()) {
        return nullptr;
    }
    return pool.c_str() + offset;
}

void YieldDuringDeckLoad(size_t* lines_read) {
    if (lines_read == nullptr) {
        return;
    }
    ++(*lines_read);
    if ((*lines_read % kDeckLoadYieldInterval) == 0) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

int64_t NowMs() {
    return esp_timer_get_time() / 1000;
}

}  // namespace

DeckStore::DeckStore() {
    UseStaticManifest();
}

void DeckStore::Initialize(const std::string& active_deck_id) {
    if (initialized_) {
        return;
    }
    initialized_ = true;

    RefreshDeckList();
    SetActiveDeckId(active_deck_id);
}

bool DeckStore::SelectDeck(const std::string& deck_id) {
    if (!initialized_) {
        Initialize(deck_id);
        return manifest_.deck_id != nullptr && deck_id == manifest_.deck_id;
    }

    const std::string wanted = deck_id.empty() ? std::string(kDeckId) : deck_id;
    if (decks_.empty() || !ContainsDeck(decks_, wanted)) {
        RefreshDeckList();
    }
    const std::string previous_id = manifest_.deck_id != nullptr ? manifest_.deck_id : "";
    SetActiveDeckId(deck_id);
    return manifest_.deck_id != nullptr && manifest_.deck_id != previous_id;
}

void DeckStore::AdoptPrepared(DeckStore&& prepared) {
    struct CardOffsets {
        size_t word = kInvalidTextOffset;
        size_t phonetic = kInvalidTextOffset;
        size_t meaning = kInvalidTextOffset;
        size_t example = kInvalidTextOffset;
        size_t translation = kInvalidTextOffset;
    };

    const char* source_begin = prepared.asset_text_pool_.data();
    const uintptr_t source_address = reinterpret_cast<uintptr_t>(source_begin);
    const uintptr_t source_end = source_address + prepared.asset_text_pool_.size();
    auto capture_offset = [source_address, source_end](const char* value) {
        const uintptr_t address = reinterpret_cast<uintptr_t>(value);
        return value != nullptr && address >= source_address && address <= source_end
            ? static_cast<size_t>(address - source_address)
            : kInvalidTextOffset;
    };
    std::vector<CardOffsets> offsets;
    offsets.reserve(prepared.asset_word_cards_.size());
    for (const WordCard& card : prepared.asset_word_cards_) {
        offsets.push_back({
            .word = capture_offset(card.word),
            .phonetic = capture_offset(card.phonetic),
            .meaning = capture_offset(card.meaning),
            .example = capture_offset(card.example),
            .translation = capture_offset(card.example_translation),
        });
    }

    initialized_ = prepared.initialized_;
    decks_ = std::move(prepared.decks_);
    active_deck_index_ = prepared.active_deck_index_;
    asset_deck_id_ = std::move(prepared.asset_deck_id_);
    asset_name_ = std::move(prepared.asset_name_);
    asset_language_ = std::move(prepared.asset_language_);
    asset_version_ = prepared.asset_version_;
    asset_text_pool_ = std::move(prepared.asset_text_pool_);
    asset_word_cards_ = std::move(prepared.asset_word_cards_);
    last_refresh_ms_ = prepared.last_refresh_ms_;
    last_load_ms_ = prepared.last_load_ms_;
    last_refresh_index_entries_ = prepared.last_refresh_index_entries_;
    last_refresh_used_index_ = prepared.last_refresh_used_index_;
    last_load_used_cache_ = prepared.last_load_used_cache_;
    last_loaded_deck_id_ = std::move(prepared.last_loaded_deck_id_);

    if (asset_word_cards_.empty()) {
        UseStaticManifest();
        return;
    }

    const char* deck_id = asset_deck_id_.c_str();
    for (size_t i = 0; i < asset_word_cards_.size() && i < offsets.size(); ++i) {
        WordCard& card = asset_word_cards_[i];
        const CardOffsets& src = offsets[i];
        card.deck_id = deck_id;
        card.word = TextAt(asset_text_pool_, src.word);
        card.phonetic = TextAt(asset_text_pool_, src.phonetic);
        card.meaning = TextAt(asset_text_pool_, src.meaning);
        card.example = TextAt(asset_text_pool_, src.example);
        card.example_translation = TextAt(asset_text_pool_, src.translation);
    }
    manifest_.deck_id = asset_deck_id_.c_str();
    manifest_.name = asset_name_.c_str();
    manifest_.language = asset_language_.c_str();
    manifest_.version = asset_version_;
    manifest_.word_count = asset_word_cards_.size();
}

bool DeckStore::RefreshDeckList() {
    const int64_t start_ms = NowMs();
    last_refresh_ms_ = 0;
    last_refresh_index_entries_ = 0;
    last_refresh_used_index_ = false;
    decks_.clear();
    DeckInfo builtin;
    builtin.id = kDeckId;
    builtin.name = kStaticDeckName;
    builtin.language = kStaticDeckLanguage;
    builtin.version = kStaticDeckVersion;
    builtin.word_count = sizeof(kKaoyanFullDeck) / sizeof(kKaoyanFullDeck[0]);
    builtin.builtin = true;
    builtin.installed = true;
    decks_.push_back(builtin);
    AddInstalledDecks();
    const int64_t elapsed_ms = NowMs() - start_ms;
    last_refresh_ms_ = elapsed_ms;
    if (elapsed_ms >= 200) {
        ESP_LOGW(kTag,
                 "RefreshDeckList took %lld ms decks=%u index_entries=%u used_index=%d free_internal=%u min_internal=%u",
                 static_cast<long long>(elapsed_ms),
                 static_cast<unsigned>(decks_.size()),
                 static_cast<unsigned>(last_refresh_index_entries_),
                 last_refresh_used_index_ ? 1 : 0,
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
    }
    return !decks_.empty();
}

const DeckManifest& DeckStore::Manifest() const {
    return manifest_;
}

const std::vector<DeckInfo>& DeckStore::Decks() const {
    return decks_;
}

size_t DeckStore::ActiveDeckIndex() const {
    return active_deck_index_;
}

bool DeckStore::HasDeck(const std::string& deck_id) const {
    if (deck_id.empty()) {
        return false;
    }
    return ContainsDeck(decks_, deck_id);
}

bool DeckStore::DeleteDeck(const std::string& deck_id) {
    if (deck_id.empty()) {
        return false;
    }

    DeckInfo target;
    bool found = false;
    for (const DeckInfo& deck : decks_) {
        if (deck.id == deck_id && !deck.builtin && !deck.path.empty()) {
            target = deck;
            found = true;
            break;
        }
    }
    if (!found) {
        return false;
    }

    AssetsStore& assets = AssetsStore::GetInstance();
    if (!assets.IsMounted()) {
        return false;
    }
    const bool removed_deck_file = assets.DeleteFile(target.path);
    const bool removed_cache_file = assets.DeleteFile(ReplaceExtension(target.path, ".vdc"));

    std::string existing;
    bool removed_index_entry = false;
    if (assets.ReadFile(kDeckIndexPath, &existing, 64 * 1024)) {
        std::string next;
        size_t start = 0;
        while (start <= existing.size()) {
            const size_t end = existing.find('\n', start);
            std::string line = existing.substr(start, end == std::string::npos ? std::string::npos : end - start);
            line = Trim(line);
            if (!line.empty() && line[0] != '#') {
                const std::vector<std::string> fields = SplitTabs(line);
                if (!fields.empty() && fields[0] == deck_id) {
                    removed_index_entry = true;
                    if (end == std::string::npos) {
                        break;
                    }
                    start = end + 1;
                    continue;
                }
            }
            if (!line.empty()) {
                next += line;
                next.push_back('\n');
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }

        const std::string index_path = assets.PathFor(kDeckIndexPath);
        const std::string temp_path = index_path + ".tmp";
        FILE* index = fopen(temp_path.c_str(), "wb");
        if (index != nullptr) {
            const bool ok = fwrite(next.data(), 1, next.size(), index) == next.size() &&
                fflush(index) == 0;
            fclose(index);
            if (ok) {
                remove(index_path.c_str());
                if (rename(temp_path.c_str(), index_path.c_str()) != 0) {
                    remove(temp_path.c_str());
                }
            } else {
                remove(temp_path.c_str());
            }
        }
    }

    if (!removed_deck_file && !removed_cache_file && !removed_index_entry) {
        return false;
    }

    const std::string active_id = manifest_.deck_id != nullptr ? manifest_.deck_id : "";
    RefreshDeckList();
    if (active_id == deck_id) {
        UseStaticManifest();
    } else {
        SetActiveDeckId(active_id);
    }
    return true;
}

size_t DeckStore::Count() const {
    if (!asset_word_cards_.empty()) {
        return asset_word_cards_.size();
    }
    return sizeof(kKaoyanFullDeck) / sizeof(kKaoyanFullDeck[0]);
}

const WordCard* DeckStore::Get(size_t index) const {
    if (index >= Count()) {
        return nullptr;
    }
    if (!asset_word_cards_.empty()) {
        return &asset_word_cards_[index];
    }
    return &kKaoyanFullDeck[index];
}

void DeckStore::UseStaticManifest() {
    asset_deck_id_.clear();
    asset_name_.clear();
    asset_language_.clear();
    asset_text_pool_.clear();
    asset_word_cards_.clear();
    manifest_.deck_id = kDeckId;
    manifest_.name = kStaticDeckName;
    manifest_.language = kStaticDeckLanguage;
    manifest_.version = kStaticDeckVersion;
    manifest_.word_count = Count();
    active_deck_index_ = 0;
}

void DeckStore::SetActiveDeckId(const std::string& deck_id) {
    if (decks_.empty()) {
        RefreshDeckList();
    }
    const std::string wanted = deck_id.empty() ? std::string(kDeckId) : deck_id;
    for (size_t i = 0; i < decks_.size(); ++i) {
        const DeckInfo& deck = decks_[i];
        if (deck.id != wanted) {
            continue;
        }
        if (deck.builtin) {
            UseStaticManifest();
            active_deck_index_ = i;
            return;
        }
        if (LoadAssetDeck(deck)) {
            active_deck_index_ = i;
            return;
        }
        ESP_LOGW(kTag, "Failed to load selected deck %s", wanted.c_str());
        break;
    }
    UseStaticManifest();
}

bool DeckStore::ReadAssetDeckInfo(const std::string& relative_path, DeckInfo* info) const {
    if (info == nullptr || !HasDeckExtension(relative_path)) {
        return false;
    }
    AssetsStore& assets = AssetsStore::GetInstance();
    if (!assets.IsMounted()) {
        return false;
    }

    if (!assets.FileExists(relative_path)) {
        return false;
    }

    const std::string path = assets.PathFor(relative_path);
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }

    const int64_t start_ms = NowMs();
    std::string deck_id = FileStem(relative_path);
    std::string name = deck_id;
    std::string language = "en-zh";
    uint16_t version = 1;
    size_t count = 0;

    std::string line;
    bool first_line = true;
    size_t lines_read = 0;
    while (ReadLine(file, &line)) {
        YieldDuringDeckLoad(&lines_read);
        if (first_line) {
            StripUtf8Bom(&line);
            first_line = false;
        }
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        if (line[0] == '#') {
            ApplyMetadata(line, &deck_id, &name, &language, &version);
            continue;
        }
        const std::vector<std::string> fields = SplitTabs(line);
        if (LooksLikeDeckRow(fields)) {
            ++count;
        }
    }
    fclose(file);

    if (deck_id.empty() || count == 0) {
        return false;
    }

    info->id = deck_id;
    info->name = name.empty() ? deck_id : name;
    info->language = language.empty() ? "en-zh" : language;
    info->path = relative_path;
    info->version = version;
    info->word_count = count;
    struct stat st = {};
    if (stat(path.c_str(), &st) == 0 && st.st_size > 0) {
        info->file_size = static_cast<size_t>(st.st_size);
    }
    info->builtin = false;
    info->installed = true;
    const int64_t elapsed_ms = NowMs() - start_ms;
    if (elapsed_ms >= 200) {
        ESP_LOGW(kTag,
                 "ReadAssetDeckInfo %s took %lld ms words=%u",
                 relative_path.c_str(),
                 static_cast<long long>(elapsed_ms),
                 static_cast<unsigned>(count));
    }
    return true;
}

void DeckStore::AddInstalledDecks() {
    AssetsStore& assets = AssetsStore::GetInstance();
    if (!assets.IsMounted()) {
        return;
    }

    std::vector<std::string> indexed_paths;
    bool loaded_index = false;

    std::string index_data;
    if (assets.ReadFile(kDeckIndexPath, &index_data, 64 * 1024)) {
        loaded_index = true;
        size_t lines_read = 0;
        size_t start = 0;
        while (start <= index_data.size()) {
            const size_t end = index_data.find('\n', start);
            std::string line = index_data.substr(
                start,
                end == std::string::npos ? std::string::npos : end - start);
            if (end == std::string::npos) {
                start = index_data.size() + 1;
            } else {
                start = end + 1;
            }
            YieldDuringDeckLoad(&lines_read);
            line = Trim(line);
            if (line.empty() || line[0] == '#') {
                continue;
            }
            std::vector<std::string> fields = SplitTabs(line);
            if (fields.size() < 5 || fields[0].empty() || fields[4].empty()) {
                continue;
            }
            if (ContainsDeck(decks_, fields[0])) {
                continue;
            }
            indexed_paths.push_back(fields[4]);
            DeckInfo info;
            info.id = fields[0];
            info.name = fields.size() > 1 && !fields[1].empty() ? fields[1] : fields[0];
            info.language = fields.size() > 2 && !fields[2].empty() ? fields[2] : "en-zh";
            info.version = fields.size() > 3 ? ParseVersion(fields[3], 1) : 1;
            info.path = fields[4];
            info.file_size = fields.size() > 5 ? ParseSize(fields[5], 0) : 0;
            info.word_count = fields.size() > 6 ? ParseSize(fields[6], 0) : 0;
            info.builtin = false;
            info.installed = true;
            decks_.push_back(std::move(info));
            ++last_refresh_index_entries_;
        }
    }

    // Remote decks are installed through index.tsv with hash-based file names.
    // Keep those indexed files on the fast path, but still scan the directory
    // for unindexed decks written through the browser management protocol.
    // The indexed-path check below prevents reparsing the remote deck files.
    if (loaded_index && !indexed_paths.empty()) {
        last_refresh_used_index_ = true;
    }

    for (const char* candidate : kAssetDeckCandidatePaths) {
        DeckInfo info;
        if (candidate != nullptr &&
            ReadAssetDeckInfo(candidate, &info) &&
            !ContainsDeck(decks_, info.id)) {
            decks_.push_back(std::move(info));
        }
    }

    const std::string dir_path = assets.PathFor(kDecksDir);
    DIR* dir = opendir(dir_path.c_str());
    if (dir == nullptr) {
        return;
    }

    while (dirent* entry = readdir(dir)) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        const std::string name = entry->d_name;
        if (!HasDeckExtension(name) ||
            name == "current.tsv" ||
            name == "current.txt" ||
            name == "index.tsv") {
            continue;
        }
        DeckInfo info;
        const std::string relative_path = std::string(kDecksDir) + "/" + name;
        if (ContainsString(indexed_paths, relative_path)) {
            continue;
        }
        if (ReadAssetDeckInfo(relative_path, &info) &&
            !ContainsDeck(decks_, info.id)) {
            decks_.push_back(std::move(info));
        }
    }
    closedir(dir);
}

bool DeckStore::LoadAssetDeck(const DeckInfo& deck) {
    const int64_t start_ms = NowMs();
    last_load_ms_ = 0;
    last_load_used_cache_ = false;
    AssetsStore& assets = AssetsStore::GetInstance();
    if (!assets.IsMounted() || deck.path.empty()) {
        return false;
    }

    const std::string path = assets.PathFor(deck.path);
    struct stat st = {};
    const size_t source_size = (stat(path.c_str(), &st) == 0 && st.st_size > 0)
        ? static_cast<size_t>(st.st_size)
        : deck.file_size;
    if (LoadAssetDeckCache(deck, path, source_size)) {
        return true;
    }

    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        ESP_LOGW(kTag, "Failed to open asset deck %s", path.c_str());
        return false;
    }

    asset_deck_id_ = deck.id.empty() ? FileStem(deck.path) : deck.id;
    asset_name_ = deck.name.empty() ? asset_deck_id_ : deck.name;
    asset_language_ = deck.language.empty() ? "en-zh" : deck.language;
    asset_version_ = deck.version == 0 ? 1 : deck.version;
    asset_text_pool_.clear();
    asset_word_cards_.clear();

    if (source_size > 0) {
        asset_text_pool_.reserve(source_size + 1);
    }

    std::vector<PendingAssetWordCard> pending_cards;
    pending_cards.reserve(deck.word_count > 0
        ? std::min<size_t>(deck.word_count, UINT16_MAX)
        : 256);

    std::string line;
    bool first_line = true;
    size_t lines_read = 0;
    while (ReadLine(file, &line)) {
        YieldDuringDeckLoad(&lines_read);
        if (first_line) {
            StripUtf8Bom(&line);
            first_line = false;
        }

        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        if (line[0] == '#') {
            ApplyMetadata(line, &asset_deck_id_, &asset_name_, &asset_language_, &asset_version_);
            continue;
        }

        std::vector<std::string> fields = SplitTabs(line);
        if (fields.empty()) {
            continue;
        }
        if (Lower(fields[0]) == "word") {
            continue;
        }

        PendingAssetWordCard card;
        if (fields.size() == 2) {
            card.word_offset = AppendText(&asset_text_pool_, fields[0]);
            card.meaning_offset = AppendText(&asset_text_pool_, fields[1]);
        } else if (fields.size() >= 3) {
            card.word_offset = AppendText(&asset_text_pool_, fields[0]);
            card.phonetic_offset = AppendText(&asset_text_pool_, fields[1]);
            card.meaning_offset = AppendText(&asset_text_pool_, fields[2]);
            if (fields.size() >= 4) {
                card.example_offset = AppendText(&asset_text_pool_, fields[3]);
            }
            if (fields.size() >= 5) {
                card.example_translation_offset = AppendText(&asset_text_pool_, fields[4]);
            }
        }

        if (card.word_offset == kInvalidTextOffset ||
            card.meaning_offset == kInvalidTextOffset) {
            continue;
        }
        if (pending_cards.size() >= UINT16_MAX) {
            ESP_LOGW(kTag, "Asset deck reached max word count");
            break;
        }
        pending_cards.push_back(card);
    }

    fclose(file);

    if (pending_cards.empty()) {
        ESP_LOGW(kTag, "Asset deck is empty: %s", path.c_str());
        asset_text_pool_.clear();
        return false;
    }

    asset_word_cards_.reserve(pending_cards.size());
    const char* deck_id = asset_deck_id_.c_str();
    size_t built_cards = 0;
    for (size_t i = 0; i < pending_cards.size(); ++i) {
        YieldDuringDeckLoad(&built_cards);
        const PendingAssetWordCard& src = pending_cards[i];
        WordCard card;
        card.id = static_cast<uint16_t>(i + 1);
        card.deck_id = deck_id;
        card.word = TextAt(asset_text_pool_, src.word_offset);
        card.phonetic = TextAt(asset_text_pool_, src.phonetic_offset);
        card.meaning = TextAt(asset_text_pool_, src.meaning_offset);
        card.example = TextAt(asset_text_pool_, src.example_offset);
        card.example_translation = TextAt(asset_text_pool_, src.example_translation_offset);
        asset_word_cards_.push_back(card);
    }

    manifest_.deck_id = asset_deck_id_.c_str();
    manifest_.name = asset_name_.c_str();
    manifest_.language = asset_language_.c_str();
    manifest_.version = asset_version_;
    manifest_.word_count = asset_word_cards_.size();

    ESP_LOGI(kTag, "Loaded asset deck %s: %u words, text bytes=%u",
             path.c_str(),
             static_cast<unsigned>(asset_word_cards_.size()),
             static_cast<unsigned>(asset_text_pool_.size()));
    if (kEnableAssetDeckCacheWrite) {
        WriteAssetDeckCache(path, source_size);
    }
    const int64_t elapsed_ms = NowMs() - start_ms;
    last_load_ms_ = elapsed_ms;
    last_loaded_deck_id_ = asset_deck_id_;
    last_load_used_cache_ = false;
    if (elapsed_ms >= 200) {
        ESP_LOGW(kTag,
                 "LoadAssetDeck %s took %lld ms free_internal=%u min_internal=%u",
                 deck.id.c_str(),
                 static_cast<long long>(elapsed_ms),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
    }
    return true;
}

bool DeckStore::LoadAssetDeckCache(const DeckInfo& deck, const std::string& path, size_t source_size) {
    if (source_size == 0) {
        return false;
    }

    const int64_t start_ms = NowMs();
    const std::string cache_path = ReplaceExtension(path, ".vdc");
    FILE* file = fopen(cache_path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }

    AssetDeckCacheHeader header = {};
    bool ok = fread(&header, 1, sizeof(header), file) == sizeof(header);
    ok = ok &&
        header.magic == kAssetDeckCacheMagic &&
        header.version == kAssetDeckCacheVersion &&
        header.word_count > 0 &&
        header.word_count <= UINT16_MAX &&
        header.text_bytes > 0 &&
        header.text_bytes <= kMaxDeckCacheTextBytes &&
        header.source_size == source_size;

    std::vector<CachedAssetWordCard> cached_cards;
    std::string text_pool;
    if (ok) {
        cached_cards.resize(header.word_count);
        ok = fread(cached_cards.data(),
                   sizeof(CachedAssetWordCard),
                   cached_cards.size(),
                   file) == cached_cards.size();
    }
    if (ok) {
        text_pool.assign(header.text_bytes, '\0');
        ok = fread(text_pool.data(), 1, text_pool.size(), file) == text_pool.size();
    }
    fclose(file);

    if (!ok) {
        ESP_LOGW(kTag, "Ignore invalid asset deck cache %s", cache_path.c_str());
        return false;
    }

    asset_deck_id_ = deck.id.empty() ? FileStem(deck.path) : deck.id;
    asset_name_ = deck.name.empty() ? asset_deck_id_ : deck.name;
    asset_language_ = deck.language.empty() ? "en-zh" : deck.language;
    asset_version_ = deck.version == 0 ? 1 : deck.version;
    asset_text_pool_ = std::move(text_pool);
    asset_word_cards_.clear();
    asset_word_cards_.reserve(cached_cards.size());

    const char* deck_id = asset_deck_id_.c_str();
    size_t built_cards = 0;
    for (const CachedAssetWordCard& src : cached_cards) {
        YieldDuringDeckLoad(&built_cards);
        WordCard card;
        card.id = src.id;
        card.deck_id = deck_id;
        card.word = TextAt(asset_text_pool_, src.word_offset);
        card.phonetic = TextAt(asset_text_pool_, src.phonetic_offset);
        card.meaning = TextAt(asset_text_pool_, src.meaning_offset);
        card.example = TextAt(asset_text_pool_, src.example_offset);
        card.example_translation = TextAt(asset_text_pool_, src.example_translation_offset);
        if (card.word == nullptr || card.meaning == nullptr) {
            asset_word_cards_.clear();
            asset_text_pool_.clear();
            ESP_LOGW(kTag, "Ignore asset deck cache with bad offsets %s", cache_path.c_str());
            return false;
        }
        asset_word_cards_.push_back(card);
    }

    manifest_.deck_id = asset_deck_id_.c_str();
    manifest_.name = asset_name_.c_str();
    manifest_.language = asset_language_.c_str();
    manifest_.version = asset_version_;
    manifest_.word_count = asset_word_cards_.size();
    last_load_ms_ = NowMs() - start_ms;
    last_loaded_deck_id_ = asset_deck_id_;
    last_load_used_cache_ = true;

    ESP_LOGI(kTag,
             "Loaded asset deck cache %s: %u words, text bytes=%u, took %lld ms",
             cache_path.c_str(),
             static_cast<unsigned>(asset_word_cards_.size()),
             static_cast<unsigned>(asset_text_pool_.size()),
             static_cast<long long>(last_load_ms_));
    return true;
}

void DeckStore::WriteAssetDeckCache(const std::string& path, size_t source_size) const {
    if (source_size == 0 || asset_word_cards_.empty() || asset_text_pool_.empty()) {
        return;
    }
    if (asset_word_cards_.size() > UINT16_MAX ||
        asset_text_pool_.size() > UINT32_MAX ||
        source_size > UINT32_MAX) {
        return;
    }

    const std::string cache_path = ReplaceExtension(path, ".vdc");
    const std::string temp_path = ReplaceExtension(path, ".tmp");
    FILE* file = fopen(temp_path.c_str(), "wb");
    if (file == nullptr) {
        ESP_LOGW(kTag, "Open asset deck cache temp failed %s", temp_path.c_str());
        return;
    }

    AssetDeckCacheHeader header = {};
    header.magic = kAssetDeckCacheMagic;
    header.version = kAssetDeckCacheVersion;
    header.word_count = static_cast<uint32_t>(asset_word_cards_.size());
    header.text_bytes = static_cast<uint32_t>(asset_text_pool_.size());
    header.source_size = static_cast<uint32_t>(source_size);

    const char* base = asset_text_pool_.c_str();
    auto offset_of = [this, base](const char* text) -> uint32_t {
        if (text == nullptr) {
            return static_cast<uint32_t>(kInvalidTextOffset);
        }
        const ptrdiff_t offset = text - base;
        if (offset < 0 || static_cast<size_t>(offset) >= asset_text_pool_.size()) {
            return static_cast<uint32_t>(kInvalidTextOffset);
        }
        return static_cast<uint32_t>(offset);
    };

    bool ok = fwrite(&header, 1, sizeof(header), file) == sizeof(header);
    for (const WordCard& card : asset_word_cards_) {
        if (!ok) {
            break;
        }
        CachedAssetWordCard cached = {};
        cached.id = card.id;
        cached.word_offset = offset_of(card.word);
        cached.phonetic_offset = offset_of(card.phonetic);
        cached.meaning_offset = offset_of(card.meaning);
        cached.example_offset = offset_of(card.example);
        cached.example_translation_offset = offset_of(card.example_translation);
        ok = cached.word_offset != static_cast<uint32_t>(kInvalidTextOffset) &&
            cached.meaning_offset != static_cast<uint32_t>(kInvalidTextOffset) &&
            fwrite(&cached, 1, sizeof(cached), file) == sizeof(cached);
    }
    if (ok) {
        ok = fwrite(asset_text_pool_.data(), 1, asset_text_pool_.size(), file) == asset_text_pool_.size() &&
            fflush(file) == 0;
    }
    fclose(file);

    if (!ok) {
        ESP_LOGW(kTag, "Write asset deck cache failed %s", temp_path.c_str());
        remove(temp_path.c_str());
        return;
    }
    remove(cache_path.c_str());
    if (rename(temp_path.c_str(), cache_path.c_str()) != 0) {
        ESP_LOGW(kTag,
                 "Rename asset deck cache failed %s -> %s errno=%d",
                 temp_path.c_str(),
                 cache_path.c_str(),
                 errno);
        remove(temp_path.c_str());
        return;
    }
    ESP_LOGI(kTag, "Wrote asset deck cache %s", cache_path.c_str());
}
