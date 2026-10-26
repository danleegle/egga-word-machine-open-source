#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <atomic>
#include <functional>
#include <string_view>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <esp_timer.h>

#include "audio_service.h"
#include "device_state.h"
#include "vocabulary/vocabulary_app.h"

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Initialize();
    void Run();

    DeviceState GetDeviceState() const { return state_.load(std::memory_order_acquire); }
    bool SetDeviceState(DeviceState state);

    void Schedule(std::function<void()>&& callback);
    void PlaySound(const std::string_view& sound);
    void PlaySound(const std::string_view& sound, int duration_ms);
    void MuteSound();
    void StopSound();
    bool CanEnterSleepMode() const;
    uint32_t IdleSleepDelayMs() const;
    void SetIdleSleepDelayMs(uint32_t delay_ms);
    void RecordUserActivity();
    void HandleUserWakeFromLightSleep();

    AudioService& GetAudioService() { return audio_service_; }
    VocabularyApp& GetVocabularyApp() { return vocabulary_app_; }

private:
    using ScheduledCallback = std::function<void()>;

    Application();
    ~Application();

    void ConfigurePowerManagement();
    static void LowPowerTimerEntry(void* arg);
    void ArmLowPowerTimer(uint32_t delay_ms);
    void HandleLowPowerTimer();

    std::atomic<DeviceState> state_{kDeviceStateUnknown};
    std::atomic<int64_t> last_user_activity_ms_{0};
    std::atomic<uint32_t> idle_sleep_delay_ms_{3U * 60U * 60U * 1000U};
    QueueHandle_t scheduled_callbacks_ = nullptr;
    esp_timer_handle_t low_power_timer_ = nullptr;
    AudioService audio_service_;
    VocabularyApp vocabulary_app_;
};

#endif  // _APPLICATION_H_
