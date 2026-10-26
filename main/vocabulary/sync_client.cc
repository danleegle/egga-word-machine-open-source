#include "sync_client.h"

#include "assets_store.h"
#include "board.h"
#include "system_info.h"
#include "wifi_manager.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_err.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_spiffs.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>
#include <sdkconfig.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr char kTag[] = "DeckOnline";
constexpr char kBaseUrl[] = CONFIG_EGGA_OTA_BASE_URL;
constexpr char kManifestPath[] = "/api/decks/manifest";
constexpr char kDeckDir[] = "/decks";
constexpr char kDeckIndexPath[] = "/decks/index.tsv";
constexpr int kHttpTimeoutMs = 20000;
constexpr size_t kHttpBufferBytes = 2048;
constexpr size_t kMaxManifestBytes = 96 * 1024;
constexpr size_t kMaxDeckBytes = 6 * 1024 * 1024;
constexpr size_t kMinReservedAssetBytes = 64 * 1024;

std::string TrimBaseUrl() {
    std::string url = kBaseUrl;
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return url.empty() ? "http://localhost:3000" : url;
}

bool IsUnreserved(char ch) {
    return (ch >= 'a' && ch <= 'z') ||
        (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') ||
        ch == '-' || ch == '_' || ch == '.' || ch == '~';
}

std::string UrlEncode(const std::string& value) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char ch : value) {
        if (IsUnreserved(static_cast<char>(ch))) {
            out.push_back(static_cast<char>(ch));
        } else {
            out.push_back('%');
            out.push_back(kHex[ch >> 4]);
            out.push_back(kHex[ch & 0x0f]);
        }
    }
    return out;
}

std::string DeviceMacQuery() {
    const std::string mac = SystemInfo::GetMacAddress();
    return mac.empty() ? std::string() : "?mac=" + UrlEncode(mac);
}

std::string AbsoluteUrl(const std::string& path) {
    if (path.rfind("https://", 0) == 0 || path.rfind("http://", 0) == 0) {
        return path;
    }
    if (path.empty()) {
        return std::string();
    }
    return TrimBaseUrl() + (path.front() == '/' ? path : "/" + path);
}

std::string StringField(cJSON* object, const char* key) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsString(item) || item->valuestring == nullptr) {
        return std::string();
    }
    return item->valuestring;
}

size_t SizeField(cJSON* object, const char* key) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item) || item->valuedouble <= 0) {
        return 0;
    }
    return static_cast<size_t>(item->valuedouble);
}

std::string SafeId(const std::string& value) {
    std::string out;
    bool separator = false;
    for (unsigned char ch : value) {
        if (std::isalnum(ch) != 0) {
            out.push_back(static_cast<char>(std::tolower(ch)));
            separator = false;
        } else if (ch == '-' || ch == '_' || ch == '.') {
            if (!out.empty()) {
                out.push_back(ch == '_' ? '-' : static_cast<char>(ch));
                separator = ch != '.';
            }
        } else if (!out.empty() && !separator) {
            out.push_back('-');
            separator = true;
        }
    }
    while (!out.empty() && (out.back() == '-' || out.back() == '.')) {
        out.pop_back();
    }
    return out;
}

uint32_t Fnv1a32(const std::string& value) {
    uint32_t hash = 2166136261U;
    for (unsigned char ch : value) {
        hash ^= ch;
        hash *= 16777619U;
    }
    return hash;
}

std::string ShortDeckFileStem(const RemoteDeckEntry& entry) {
    if (entry.sha256.size() >= 12) {
        return std::string("d") + entry.sha256.substr(0, 12);
    }
    char out[16];
    snprintf(out, sizeof(out), "d%08x", static_cast<unsigned>(Fnv1a32(entry.id)));
    return out;
}

std::string IndexSafe(std::string value) {
    for (char& ch : value) {
        if (ch == '\t' || ch == '\r' || ch == '\n') {
            ch = ' ';
        }
    }
    return value;
}

std::string FirstField(const std::string& line) {
    const size_t tab = line.find('\t');
    return tab == std::string::npos ? line : line.substr(0, tab);
}

bool EnsureAssetDirectory(const std::string& relative_dir) {
    AssetsStore& assets = AssetsStore::GetInstance();
    if (!assets.IsMounted()) {
        return false;
    }
    (void)relative_dir;
    // SPIFFS has a flat namespace. Paths such as /decks/foo.tsv are valid file
    // names, but mkdir("/assets/decks") fails and must not block downloads.
    return true;
}

bool RegularFileSize(const std::string& path, size_t* size) {
    struct stat st = {};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    if (size != nullptr) {
        *size = st.st_size > 0 ? static_cast<size_t>(st.st_size) : 0;
    }
    return true;
}

bool WaitForWifi(std::string* error) {
    WifiManager& wifi = WifiManager::GetInstance();
    if (wifi.IsConnected()) {
        return true;
    }

    Board::GetInstance().RequestNetwork();
    for (int i = 0; i < 60; ++i) {
        if (wifi.IsConnected()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    if (error != nullptr) {
        *error = "WiFi not connected";
    }
    return false;
}

class HttpReader {
public:
    explicit HttpReader(const std::string& url) : url_(url) {}
    ~HttpReader() { Close(); }

    bool Open(std::string* error) {
        esp_http_client_config_t config = {};
        config.url = url_.c_str();
        config.method = HTTP_METHOD_GET;
        config.timeout_ms = kHttpTimeoutMs;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.buffer_size = kHttpBufferBytes;
        config.buffer_size_tx = 512;
        config.keep_alive_enable = true;

        client_ = esp_http_client_init(&config);
        if (client_ == nullptr) {
            SetError(error, "HTTP init failed");
            return false;
        }
        esp_http_client_set_header(client_, "Accept", "*/*");
        const std::string mac = SystemInfo::GetMacAddress();
        if (!mac.empty()) {
            esp_http_client_set_header(client_, "X-Egga-Device-Mac", mac.c_str());
        }

        const esp_err_t open_ret = esp_http_client_open(client_, 0);
        if (open_ret != ESP_OK) {
            SetError(error, std::string("HTTP open failed: ") + esp_err_to_name(open_ret));
            return false;
        }
        opened_ = true;

        content_length_ = esp_http_client_fetch_headers(client_);
        status_code_ = esp_http_client_get_status_code(client_);
        if (status_code_ < 200 || status_code_ >= 300) {
            SetError(error, "HTTP status " + std::to_string(status_code_));
            return false;
        }
        return true;
    }

    int Read(char* data, size_t size) {
        if (client_ == nullptr || data == nullptr || size == 0) {
            return -1;
        }
        return esp_http_client_read(client_, data, size);
    }

    int64_t ContentLength() const { return content_length_; }

private:
    void Close() {
        if (client_ == nullptr) {
            return;
        }
        if (opened_) {
            esp_http_client_close(client_);
            opened_ = false;
        }
        esp_http_client_cleanup(client_);
        client_ = nullptr;
    }

    static void SetError(std::string* error, const std::string& value) {
        if (error != nullptr) {
            *error = value;
        }
    }

    std::string url_;
    esp_http_client_handle_t client_ = nullptr;
    bool opened_ = false;
    int status_code_ = 0;
    int64_t content_length_ = -1;
};

bool ReadHttpText(const std::string& url, size_t max_bytes, std::string* body, std::string* error) {
    if (body == nullptr) {
        return false;
    }
    body->clear();

    HttpReader reader(url);
    if (!reader.Open(error)) {
        return false;
    }
    if (reader.ContentLength() > static_cast<int64_t>(max_bytes)) {
        if (error != nullptr) {
            *error = "response too large";
        }
        return false;
    }

    std::vector<char> buffer(kHttpBufferBytes);
    while (true) {
        const int read = reader.Read(buffer.data(), buffer.size());
        if (read < 0) {
            if (error != nullptr) {
                *error = "HTTP read failed";
            }
            return false;
        }
        if (read == 0) {
            break;
        }
        body->append(buffer.data(), static_cast<size_t>(read));
        if (body->size() > max_bytes) {
            if (error != nullptr) {
                *error = "response too large";
            }
            return false;
        }
    }
    return !body->empty();
}

bool AssetHasSpace(size_t expected_size, std::string* error) {
    if (expected_size > kMaxDeckBytes) {
        if (error != nullptr) {
            *error = "deck too large";
        }
        return false;
    }

    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info("assets", &total, &used) != ESP_OK || total <= used) {
        return true;
    }
    const size_t free_bytes = total - used;
    if (expected_size > 0 && expected_size + kMinReservedAssetBytes > free_bytes) {
        if (error != nullptr) {
            *error = "not enough storage";
        }
        return false;
    }
    return true;
}

std::string Sha256ToHex(const unsigned char digest[32]) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.resize(64);
    for (size_t i = 0; i < 32; ++i) {
        out[i * 2] = kHex[digest[i] >> 4];
        out[i * 2 + 1] = kHex[digest[i] & 0x0f];
    }
    return out;
}

bool DownloadFile(const std::string& url,
                  const std::string& target_path,
                  size_t expected_size,
                  const std::string& expected_sha256,
                  size_t* written,
                  std::string* error,
                  const std::function<void(size_t, size_t)>& progress) {
    if (!AssetHasSpace(expected_size, error)) {
        return false;
    }

    HttpReader reader(url);
    if (!reader.Open(error)) {
        return false;
    }
    if (reader.ContentLength() > static_cast<int64_t>(kMaxDeckBytes)) {
        if (error != nullptr) {
            *error = "deck too large";
        }
        return false;
    }
    if (expected_size > 0 && reader.ContentLength() > 0 &&
        static_cast<size_t>(reader.ContentLength()) != expected_size) {
        if (error != nullptr) {
            *error = "deck size changed";
        }
        return false;
    }
    const size_t target_size = expected_size > 0
        ? expected_size
        : (reader.ContentLength() > 0 ? static_cast<size_t>(reader.ContentLength()) : 0);

    const std::string temp_path = target_path + ".tmp";
    const std::string backup_path = target_path + ".bak";
    // Recover an interrupted activation before starting another download.
    struct stat target_stat = {};
    struct stat backup_stat = {};
    if (stat(backup_path.c_str(), &backup_stat) == 0) {
        // A backup is kept until the deck index commit succeeds. Its presence
        // therefore means the previous activation was interrupted.
        remove(target_path.c_str());
        (void)rename(backup_path.c_str(), target_path.c_str());
    }
    remove(temp_path.c_str());
    FILE* file = fopen(temp_path.c_str(), "wb");
    if (file == nullptr) {
        if (error != nullptr) {
            *error = std::string("open file failed: ") + std::strerror(errno);
        }
        return false;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    std::vector<char> buffer(kHttpBufferBytes);
    size_t total_written = 0;
    size_t next_progress = 64 * 1024;
    size_t next_callback = 128 * 1024;
    bool ok = true;
    while (true) {
        if (target_size > 0 && total_written >= target_size) {
            break;
        }
        const int read = reader.Read(buffer.data(), buffer.size());
        if (read < 0) {
            if (error != nullptr) {
                *error = "HTTP read failed";
            }
            ok = false;
            break;
        }
        if (read == 0) {
            break;
        }
        const size_t chunk = static_cast<size_t>(read);
        if (total_written + chunk > kMaxDeckBytes) {
            if (error != nullptr) {
                *error = "deck too large";
            }
            ok = false;
            break;
        }
        if (fwrite(buffer.data(), 1, chunk, file) != chunk) {
            if (error != nullptr) {
                *error = "write file failed";
            }
            ok = false;
            break;
        }
        mbedtls_sha256_update(&sha, reinterpret_cast<const unsigned char*>(buffer.data()), chunk);
        total_written += chunk;
        if (progress &&
            (total_written >= next_callback || (target_size > 0 && total_written >= target_size))) {
            progress(total_written, target_size);
            next_callback = total_written + 128 * 1024;
        }
        if (total_written >= next_progress || (target_size > 0 && total_written >= target_size)) {
            ESP_LOGI(kTag,
                     "Downloaded %u/%u bytes",
                     static_cast<unsigned>(total_written),
                     static_cast<unsigned>(target_size));
            next_progress += 64 * 1024;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    const bool flush_ok = fflush(file) == 0 && fsync(fileno(file)) == 0;
    fclose(file);

    unsigned char digest[32] = {};
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);

    if (written != nullptr) {
        *written = total_written;
    }
    if (!ok || !flush_ok || total_written == 0) {
        remove(temp_path.c_str());
        return false;
    }
    if (expected_size > 0 && total_written != expected_size) {
        if (error != nullptr) {
            *error = "deck size mismatch";
        }
        remove(temp_path.c_str());
        return false;
    }
    if (!expected_sha256.empty() && Sha256ToHex(digest) != expected_sha256) {
        if (error != nullptr) {
            *error = "deck checksum mismatch";
        }
        remove(temp_path.c_str());
        return false;
    }

    remove(backup_path.c_str());
    const bool had_previous = rename(target_path.c_str(), backup_path.c_str()) == 0;
    if (rename(temp_path.c_str(), target_path.c_str()) != 0) {
        if (error != nullptr) {
            *error = std::string("install file failed: ") + std::strerror(errno);
        }
        if (had_previous) {
            (void)rename(backup_path.c_str(), target_path.c_str());
        }
        remove(temp_path.c_str());
        return false;
    }
    return true;
}

bool ParseManifest(const std::string& body, std::vector<RemoteDeckEntry>* entries, std::string* error) {
    cJSON* root = cJSON_ParseWithLength(body.data(), body.size());
    if (root == nullptr) {
        if (error != nullptr) {
            *error = "manifest JSON invalid";
        }
        return false;
    }

    cJSON* array = cJSON_GetObjectItemCaseSensitive(root, "entries");
    if (!cJSON_IsArray(array)) {
        cJSON* decks = cJSON_GetObjectItemCaseSensitive(root, "decks");
        array = cJSON_GetObjectItemCaseSensitive(decks, "entries");
    }
    if (!cJSON_IsArray(array)) {
        cJSON_Delete(root);
        if (error != nullptr) {
            *error = "manifest has no entries";
        }
        return false;
    }

    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, array) {
        if (!cJSON_IsObject(item)) {
            continue;
        }

        RemoteDeckEntry entry;
        entry.id = SafeId(StringField(item, "id"));
        entry.name = StringField(item, "name");
        entry.language = StringField(item, "language");
        entry.version = StringField(item, "version");
        entry.path = StringField(item, "path");
        entry.sha256 = StringField(item, "sha256");
        entry.size = SizeField(item, "size");
        entry.entry_count = SizeField(item, "entryCount");

        const std::string format = StringField(item, "format");
        if (!format.empty() && format != "tsv") {
            continue;
        }
        if (entry.id.empty() || entry.path.empty() || entry.size == 0) {
            continue;
        }
        if (entry.name.empty()) {
            entry.name = entry.id;
        }
        if (entry.language.empty()) {
            entry.language = "en-zh";
        }
        if (entry.version.empty()) {
            entry.version = "1";
        }
        entries->push_back(std::move(entry));
    }

    cJSON_Delete(root);
    if (entries->empty()) {
        if (error != nullptr) {
            *error = "manifest entries empty";
        }
        return false;
    }
    return true;
}

bool WriteDeckIndexEntry(const RemoteDeckEntry& entry, const std::string& relative_path) {
    AssetsStore& assets = AssetsStore::GetInstance();
    if (!EnsureAssetDirectory(kDeckDir)) {
        return false;
    }

    std::string existing;
    (void)assets.ReadFile(kDeckIndexPath, &existing, 96 * 1024);

    std::string next;
    size_t cursor = 0;
    while (cursor <= existing.size()) {
        const size_t end = existing.find('\n', cursor);
        std::string line = existing.substr(cursor, end == std::string::npos ? std::string::npos : end - cursor);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (!line.empty() && FirstField(line) != entry.id) {
            next += line;
            next.push_back('\n');
        }
        if (end == std::string::npos) {
            break;
        }
        cursor = end + 1;
    }

    next += entry.id;
    next.push_back('\t');
    next += IndexSafe(entry.name);
    next.push_back('\t');
    next += IndexSafe(entry.language);
    next.push_back('\t');
    next += IndexSafe(entry.version);
    next.push_back('\t');
    next += IndexSafe(relative_path);
    next.push_back('\t');
    next += std::to_string(entry.size);
    next.push_back('\t');
    next += std::to_string(entry.entry_count);
    next.push_back('\n');

    const std::string full_path = assets.PathFor(kDeckIndexPath);
    const std::string temp_path = full_path + ".tmp";
    FILE* file = fopen(temp_path.c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    const bool ok = fwrite(next.data(), 1, next.size(), file) == next.size() &&
        fflush(file) == 0 && fsync(fileno(file)) == 0;
    fclose(file);
    if (!ok) {
        remove(temp_path.c_str());
        return false;
    }
    const std::string backup_path = full_path + ".bak";
    remove(backup_path.c_str());
    const bool had_previous = rename(full_path.c_str(), backup_path.c_str()) == 0;
    if (rename(temp_path.c_str(), full_path.c_str()) != 0) {
        if (had_previous) {
            (void)rename(backup_path.c_str(), full_path.c_str());
        }
        remove(temp_path.c_str());
        return false;
    }
    remove(backup_path.c_str());
    return true;
}

}  // namespace

void SyncClient::SetStatus(const char* status) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_status_ = status != nullptr ? status : "";
}

void SyncClient::SetStatus(const std::string& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_status_ = status;
}

void SyncClient::MarkPending() {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_ = true;
    last_status_ = "pending";
}

bool SyncClient::HasPending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_;
}

bool SyncClient::TrySync(const DeckManifest& manifest, const VocabularyDailyStats& stats) {
    (void)manifest;
    (void)stats;
    std::lock_guard<std::mutex> lock(mutex_);
    last_status_ = pending_ ? "pending local progress" : "ready";
    return !pending_;
}

bool SyncClient::FetchDeckManifest(std::vector<RemoteDeckEntry>* entries) {
    if (entries == nullptr) {
        SetStatus("internal error");
        return false;
    }
    entries->clear();

    std::string error;
    if (!WaitForWifi(&error)) {
        SetStatus(error);
        return false;
    }

    const std::string url = TrimBaseUrl() + kManifestPath + DeviceMacQuery();
    ESP_LOGI(kTag, "Fetching deck manifest: %s", url.c_str());

    std::string body;
    if (!ReadHttpText(url, kMaxManifestBytes, &body, &error)) {
        SetStatus(error);
        return false;
    }
    if (!ParseManifest(body, entries, &error)) {
        SetStatus(error);
        return false;
    }

    std::sort(entries->begin(), entries->end(), [](const RemoteDeckEntry& a, const RemoteDeckEntry& b) {
        return a.name < b.name;
    });
    SetStatus("catalog ok: " + std::to_string(entries->size()));
    return true;
}

bool SyncClient::DownloadDeck(const RemoteDeckEntry& entry,
                              std::string* relative_path,
                              std::function<void(size_t, size_t)> progress) {
    if (entry.id.empty() || entry.path.empty()) {
        SetStatus("deck entry invalid");
        return false;
    }

    AssetsStore& assets = AssetsStore::GetInstance();
    if (!assets.IsMounted()) {
        SetStatus("assets not mounted");
        return false;
    }
    if (!EnsureAssetDirectory(kDeckDir)) {
        SetStatus("deck directory unavailable");
        return false;
    }

    std::string error;
    if (!WaitForWifi(&error)) {
        SetStatus(error);
        return false;
    }

    const std::string id = SafeId(entry.id);
    if (id.empty()) {
        SetStatus("deck id invalid");
        return false;
    }
    RemoteDeckEntry installed = entry;
    installed.id = id;

    const std::string local_relative_path = std::string(kDeckDir) + "/" + ShortDeckFileStem(installed) + ".tsv";
    const std::string local_full_path = assets.PathFor(local_relative_path);
    const std::string url = AbsoluteUrl(entry.path);

    size_t existing_size = 0;
    if (!installed.sha256.empty() &&
        installed.size > 0 &&
        RegularFileSize(local_full_path, &existing_size) &&
        existing_size == installed.size) {
        if (!WriteDeckIndexEntry(installed, local_relative_path)) {
            SetStatus("deck index write failed");
            return false;
        }
        if (relative_path != nullptr) {
            *relative_path = local_relative_path;
        }
        SetStatus("already downloaded " + id + " " + std::to_string(existing_size / 1024) + "KB");
        ESP_LOGI(kTag, "Deck %s already installed at %s", id.c_str(), local_full_path.c_str());
        return true;
    }

    SetStatus("downloading " + id);
    ESP_LOGI(kTag, "Downloading deck %s: %s", id.c_str(), url.c_str());

    size_t written = 0;
    if (!DownloadFile(url, local_full_path, entry.size, entry.sha256, &written, &error, progress)) {
        SetStatus(error);
        return false;
    }

    if (!WriteDeckIndexEntry(installed, local_relative_path)) {
        remove(local_full_path.c_str());
        (void)rename((local_full_path + ".bak").c_str(), local_full_path.c_str());
        SetStatus("deck index write failed");
        return false;
    }
    remove((local_full_path + ".bak").c_str());

    if (relative_path != nullptr) {
        *relative_path = local_relative_path;
    }
    SetStatus("downloaded " + id + " " + std::to_string(written / 1024) + "KB");
    return true;
}

std::string SyncClient::LastStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_status_;
}
