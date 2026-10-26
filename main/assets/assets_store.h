#ifndef ASSETS_STORE_H
#define ASSETS_STORE_H

#include <cstddef>
#include <string>

class AssetsStore {
public:
    static AssetsStore& GetInstance();

    bool Initialize();
    bool IsMounted() const { return mounted_; }
    const char* BasePath() const;

    std::string PathFor(const std::string& relative_path) const;
    bool FileExists(const std::string& relative_path) const;
    bool ReadFile(const std::string& relative_path, std::string* out, size_t max_size) const;
    bool DeleteFile(const std::string& relative_path);
    int FreePercent() const;

private:
    AssetsStore() = default;
    void CleanupTemporaryFiles();

    bool mounted_ = false;
};

#endif  // ASSETS_STORE_H
