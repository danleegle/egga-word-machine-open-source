#include "assets_store.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_spiffs.h>

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

namespace {

constexpr char kTag[] = "AssetsStore";
constexpr char kAssetsBasePath[] = "/assets";
constexpr char kAssetsPartitionLabel[] = "assets";

bool HasSuffix(const char* text, const char* suffix) {
    if (text == nullptr || suffix == nullptr) {
        return false;
    }
    const size_t text_len = strlen(text);
    const size_t suffix_len = strlen(suffix);
    return text_len >= suffix_len &&
        memcmp(text + text_len - suffix_len, suffix, suffix_len) == 0;
}

bool IsTemporaryAssetFile(const char* name) {
    return HasSuffix(name, ".tmp") ||
        HasSuffix(name, ".part") ||
        HasSuffix(name, ".vdc.tmp");
}

}  // namespace

AssetsStore& AssetsStore::GetInstance() {
    static AssetsStore store;
    return store;
}

bool AssetsStore::Initialize() {
    if (mounted_) {
        return true;
    }

    esp_vfs_spiffs_conf_t conf = {};
    conf.base_path = kAssetsBasePath;
    conf.partition_label = kAssetsPartitionLabel;
    conf.max_files = 8;
    conf.format_if_mount_failed = true;

    const esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "Failed to mount assets partition: %s", esp_err_to_name(ret));
        return false;
    }

    mounted_ = true;

    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info(kAssetsPartitionLabel, &total, &used) == ESP_OK) {
        ESP_LOGI(kTag, "Mounted %s: total=%u used=%u",
                 kAssetsBasePath, static_cast<unsigned>(total), static_cast<unsigned>(used));
    } else {
        ESP_LOGI(kTag, "Mounted %s", kAssetsBasePath);
    }

    CleanupTemporaryFiles();

    return true;
}

const char* AssetsStore::BasePath() const {
    return kAssetsBasePath;
}

std::string AssetsStore::PathFor(const std::string& relative_path) const {
    if (relative_path.empty()) {
        return kAssetsBasePath;
    }
    if (relative_path[0] == '/') {
        return std::string(kAssetsBasePath) + relative_path;
    }
    return std::string(kAssetsBasePath) + "/" + relative_path;
}

bool AssetsStore::FileExists(const std::string& relative_path) const {
    if (!mounted_) {
        return false;
    }

    const std::string path = PathFor(relative_path);
    struct stat st = {};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool AssetsStore::ReadFile(const std::string& relative_path, std::string* out, size_t max_size) const {
    if (!mounted_ || out == nullptr) {
        return false;
    }

    const std::string path = PathFor(relative_path);
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    const long size = ftell(file);
    if (size < 0 || static_cast<size_t>(size) > max_size) {
        ESP_LOGW(kTag, "Skip %s, size=%ld max=%u",
                 path.c_str(), size, static_cast<unsigned>(max_size));
        fclose(file);
        return false;
    }
    rewind(file);

    out->assign(static_cast<size_t>(size), '\0');
    if (!out->empty()) {
        const size_t read = fread(out->data(), 1, out->size(), file);
        if (read != out->size()) {
            out->clear();
            fclose(file);
            return false;
        }
    }

    fclose(file);
    return true;
}

bool AssetsStore::DeleteFile(const std::string& relative_path) {
    if (!mounted_) {
        return false;
    }
    const std::string path = PathFor(relative_path);
    return remove(path.c_str()) == 0;
}

void AssetsStore::CleanupTemporaryFiles() {
    if (!mounted_) {
        return;
    }

    size_t removed = 0;
    const char* dirs[] = {
        kAssetsBasePath,
        "/assets/decks",
    };
    for (const char* dir_path : dirs) {
        DIR* dir = opendir(dir_path);
        if (dir == nullptr) {
            continue;
        }

        while (dirent* entry = readdir(dir)) {
            if (entry->d_name[0] == '.') {
                continue;
            }
            if (!IsTemporaryAssetFile(entry->d_name)) {
                continue;
            }

            const std::string path = std::string(dir_path) + "/" + entry->d_name;
            if (remove(path.c_str()) == 0) {
                ++removed;
            }
        }
        closedir(dir);
    }

    if (removed > 0) {
        ESP_LOGI(kTag, "Removed %u stale temporary asset files", static_cast<unsigned>(removed));
    }
}

int AssetsStore::FreePercent() const {
    if (!mounted_) {
        return -1;
    }

    size_t total = 0;
    size_t used = 0;
    if (esp_spiffs_info(kAssetsPartitionLabel, &total, &used) != ESP_OK || total == 0) {
        return -1;
    }
    const size_t free_bytes = total > used ? total - used : 0;
    return static_cast<int>((free_bytes * 100 + total / 2) / total);
}
