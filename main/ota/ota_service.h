#ifndef OTA_SERVICE_H
#define OTA_SERVICE_H

#include <string>

class OtaService {
public:
    struct Snapshot {
        bool running = false;
        std::string state;
        std::string current_version;
        std::string latest_version;
        std::string message;
        std::string url;
        int progress = -1;
    };

    static OtaService& GetInstance();

    Snapshot GetSnapshot();
    std::string StatusJson();
    bool Check(const std::string& base_url, std::string* error);
    bool Start(const std::string& base_url, std::string* error);
    void RunTask(std::string url, std::string version, int expected_size);

private:
    OtaService();

    struct UpdateInfo {
        bool available = false;
        std::string version;
        std::string url;
        std::string sha256;
        int size = 0;
        std::string message;
    };

    bool FetchUpdateInfo(const std::string& base_url, UpdateInfo* info, std::string* error);
    void SetIdle();
    void SetChecking();
    void SetStarting(const UpdateInfo& info);
    void SetAvailable(const UpdateInfo& info);
    void SetNotAvailable(const std::string& message);
    void SetDownloading(int progress, const std::string& message);
    void SetError(const std::string& message);
    void SetRebooting(const std::string& version);

    void* mutex_ = nullptr;
    bool running_ = false;
    std::string state_ = "idle";
    std::string latest_version_;
    std::string message_;
    std::string url_;
    int progress_ = -1;
};

#endif  // OTA_SERVICE_H
