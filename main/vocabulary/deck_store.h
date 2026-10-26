#ifndef VOCABULARY_DECK_STORE_H
#define VOCABULARY_DECK_STORE_H

#include "vocabulary_types.h"

#include <cstddef>
#include <string>
#include <vector>

struct DeckInfo {
    std::string id;
    std::string name;
    std::string language;
    std::string path;
    uint16_t version = 1;
    size_t word_count = 0;
    size_t file_size = 0;
    bool builtin = false;
    bool installed = false;
};

class DeckStore {
public:
    DeckStore();

    void Initialize(const std::string& active_deck_id = std::string());
    bool SelectDeck(const std::string& deck_id);
    // Replaces the active in-memory store with a fully prepared store. This is
    // intentionally cheap so parsing/indexing can happen on a worker task
    // without blocking vocabulary input.
    void AdoptPrepared(DeckStore&& prepared);
    bool RefreshDeckList();
    const DeckManifest& Manifest() const;
    const std::vector<DeckInfo>& Decks() const;
    size_t ActiveDeckIndex() const;
    bool HasDeck(const std::string& deck_id) const;
    int64_t LastRefreshMs() const { return last_refresh_ms_; }
    int64_t LastLoadMs() const { return last_load_ms_; }
    size_t LastRefreshIndexEntries() const { return last_refresh_index_entries_; }
    bool LastRefreshUsedIndex() const { return last_refresh_used_index_; }
    bool LastLoadUsedCache() const { return last_load_used_cache_; }
    const std::string& LastLoadedDeckId() const { return last_loaded_deck_id_; }
    bool DeleteDeck(const std::string& deck_id);
    size_t Count() const;
    const WordCard* Get(size_t index) const;

private:
    void UseStaticManifest();
    bool LoadAssetDeck(const DeckInfo& deck);
    bool LoadAssetDeckCache(const DeckInfo& deck, const std::string& path, size_t source_size);
    void WriteAssetDeckCache(const std::string& path, size_t source_size) const;
    bool ReadAssetDeckInfo(const std::string& relative_path, DeckInfo* info) const;
    void AddInstalledDecks();
    void SetActiveDeckId(const std::string& deck_id);

    DeckManifest manifest_;
    bool initialized_ = false;
    std::vector<DeckInfo> decks_;
    size_t active_deck_index_ = 0;
    std::string asset_deck_id_;
    std::string asset_name_;
    std::string asset_language_;
    uint16_t asset_version_ = 1;
    std::string asset_text_pool_;
    std::vector<WordCard> asset_word_cards_;
    int64_t last_refresh_ms_ = 0;
    int64_t last_load_ms_ = 0;
    size_t last_refresh_index_entries_ = 0;
    bool last_refresh_used_index_ = false;
    bool last_load_used_cache_ = false;
    std::string last_loaded_deck_id_;
};

#endif  // VOCABULARY_DECK_STORE_H
