#ifndef VOCABULARY_SYNC_CLIENT_H
#define VOCABULARY_SYNC_CLIENT_H

#include "vocabulary_types.h"

#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct RemoteDeckEntry {
    std::string id;
    std::string name;
    std::string language;
    std::string version;
    std::string path;
    std::string sha256;
    size_t size = 0;
    size_t entry_count = 0;
};

class SyncClient {
public:
    void MarkPending();
    bool HasPending() const;
    bool TrySync(const DeckManifest& manifest, const VocabularyDailyStats& stats);
    bool FetchDeckManifest(std::vector<RemoteDeckEntry>* entries);
    bool DownloadDeck(const RemoteDeckEntry& entry,
                      std::string* relative_path,
                      std::function<void(size_t, size_t)> progress = nullptr);
    std::string LastStatus() const;

private:
    void SetStatus(const char* status);
    void SetStatus(const std::string& status);

    bool pending_ = false;
    std::string last_status_ = "offline";
    mutable std::mutex mutex_;
};

#endif  // VOCABULARY_SYNC_CLIENT_H
