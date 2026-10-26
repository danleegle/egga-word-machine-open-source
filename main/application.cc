#include "application.h"

#include "ai/ai_dialog_service.h"
#include "assets_store.h"
#include "board.h"
#include "client_mgmt_service.h"
#include "common/sleep_manager.h"
#include "display.h"

#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_pm.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <cstdint>
#include <new>

namespace {

constexpr char kTag[] = "Application";
constexpr size_t kScheduledCallbackQueueDepth = 16;
constexpr uint32_t kDeepSleepIdleMs = 60 * 60 * 1000;
constexpr uint32_t kLowPowerRetryMs = 60 * 1000;
constexpr int64_t kSlowScheduledCallbackMs = 200;
constexpr uint32_t kSleepScreenRefreshTimeoutMs = 10 * 1000;
constexpr bool kEnableIdleLowPowerSleep = true;
constexpr bool kEnableAutoDeepSleep = false;

int64_t NowMs() {
    return esp_timer_get_time() / 1000;
}

const char* WakeActionName(LightSleepWakeAction action) {
    switch (action) {
        case LightSleepWakeAction::kConfirmClick:
            return "confirm_click";
        case LightSleepWakeAction::kUpClick:
            return "up_click";
        case LightSleepWakeAction::kDownClick:
            return "down_click";
        case LightSleepWakeAction::kSettingsCombo:
            return "settings_combo";
        case LightSleepWakeAction::kNone:
        default:
            return "none";
    }
}

bool WakeActionToVocabularyButton(LightSleepWakeAction action, VocabularyButton* button) {
    if (button == nullptr) {
        return false;
    }
    switch (action) {
        case LightSleepWakeAction::kConfirmClick:
            *button = VocabularyButton::kConfirmClick;
            return true;
        case LightSleepWakeAction::kUpClick:
            *button = VocabularyButton::kUpClick;
            return true;
        case LightSleepWakeAction::kDownClick:
            *button = VocabularyButton::kDownClick;
            return true;
        case LightSleepWakeAction::kSettingsCombo:
            *button = VocabularyButton::kSettingsCombo;
            return true;
        case LightSleepWakeAction::kNone:
        default:
            return false;
    }
}

}  // namespace

Application::Application() {
    scheduled_callbacks_ = xQueueCreate(
        kScheduledCallbackQueueDepth,
        sizeof(ScheduledCallback*));
    if (scheduled_callbacks_ == nullptr) {
        ESP_LOGE(kTag, "Failed to create application callback queue");
    }
}

Application::~Application() {
    if (scheduled_callbacks_ == nullptr) {
        return;
    }

    ScheduledCallback* callback = nullptr;
    while (xQueueReceive(scheduled_callbacks_, &callback, 0) == pdTRUE) {
        delete callback;
    }
    vQueueDelete(scheduled_callbacks_);
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);
    ConfigurePowerManagement();

    if (low_power_timer_ == nullptr) {
        esp_timer_create_args_t timer_args = {};
        timer_args.callback = &Application::LowPowerTimerEntry;
        timer_args.arg = this;
        timer_args.dispatch_method = ESP_TIMER_TASK;
        timer_args.name = "ultra_low_power";
        const esp_err_t ret = esp_timer_create(&timer_args, &low_power_timer_);
        if (ret != ESP_OK) {
            ESP_LOGW(kTag, "Failed to create low power timer: %s", esp_err_to_name(ret));
        }
    }

    AudioCodec* codec = board.GetAudioCodec();
    if (codec == nullptr) {
        ESP_LOGE(kTag, "Audio codec is null");
        SetDeviceState(kDeviceStateFatalError);
        return;
    }

    audio_service_.Initialize(codec);
    audio_service_.Start();
    AiDialogService::GetInstance().Initialize(&audio_service_, [this]() {
        Schedule([this]() { vocabulary_app_.RefreshSnapshot(); });
    });
    AssetsStore::GetInstance().Initialize();
    // Publish the management protocol only after the assets mount attempt has
    // settled. Otherwise a browser connecting during USB-reset startup can
    // briefly see mounted=false and misdiagnose a compatible partition table.
    ClientMgmtService::GetInstance().Start();
    ClientMgmtService::GetInstance().ImportWifiConfigFromAssetsOnce();

    Display* display = board.GetDisplay();
    if (display != nullptr) {
        display->ShowVocabularyPage();
        display->UpdateStatusBar(true);
    }

    vocabulary_app_.Initialize(display);
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string&) {
        if (event == NetworkEvent::Connected ||
            event == NetworkEvent::Disconnected ||
            event == NetworkEvent::WifiConfigModeEnter ||
            event == NetworkEvent::WifiConfigModeExit) {
            Schedule([this, event]() {
                if (event == NetworkEvent::Connected) {
                    vocabulary_app_.NotifyNetworkConnected();
                } else {
                    vocabulary_app_.RefreshSnapshot();
                }
            });
        }
    });
    SetDeviceState(kDeviceStateIdle);
    RecordUserActivity();
}

void Application::Run() {
    ScheduledCallback* callback = nullptr;
    while (true) {
        if (scheduled_callbacks_ == nullptr) {
            vTaskDelay(portMAX_DELAY);
            continue;
        }

        if (xQueueReceive(scheduled_callbacks_, &callback, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (callback != nullptr) {
            const int64_t begin_ms = NowMs();
            (*callback)();
            const int64_t elapsed_ms = NowMs() - begin_ms;
            if (elapsed_ms >= kSlowScheduledCallbackMs) {
                ESP_LOGW(kTag,
                         "Slow scheduled callback: %lld ms free_internal=%u min_internal=%u queued=%u",
                         static_cast<long long>(elapsed_ms),
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                         static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
                         static_cast<unsigned>(uxQueueMessagesWaiting(scheduled_callbacks_)));
            }
            delete callback;
            callback = nullptr;
        }
    }
}

bool Application::SetDeviceState(DeviceState state) {
    const DeviceState old_state = state_.exchange(state, std::memory_order_acq_rel);
    ESP_LOGI(kTag, "State %d -> %d", old_state, state);
    return true;
}

void Application::Schedule(std::function<void()>&& callback) {
    if (!callback) {
        return;
    }

    if (scheduled_callbacks_ == nullptr) {
        callback();
        return;
    }

    auto* scheduled = new (std::nothrow) ScheduledCallback(std::move(callback));
    if (scheduled == nullptr) {
        ESP_LOGW(kTag, "Dropping scheduled callback due to allocation failure");
        return;
    }

    if (xQueueSend(scheduled_callbacks_, &scheduled, 0) != pdTRUE) {
        ESP_LOGW(kTag,
                 "Dropping scheduled callback because queue is full queued=%u",
                 static_cast<unsigned>(uxQueueMessagesWaiting(scheduled_callbacks_)));
        delete scheduled;
    }
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::PlaySound(const std::string_view& sound, int duration_ms) {
    audio_service_.PlaySound(sound, duration_ms);
}

void Application::MuteSound() {
    audio_service_.MuteOutput();
}

void Application::StopSound() {
    audio_service_.ResetDecoder();
}

bool Application::CanEnterSleepMode() const {
    return state_.load(std::memory_order_acquire) == kDeviceStateIdle;
}

uint32_t Application::IdleSleepDelayMs() const {
    return idle_sleep_delay_ms_.load(std::memory_order_acquire);
}

void Application::SetIdleSleepDelayMs(uint32_t delay_ms) {
    idle_sleep_delay_ms_.store(delay_ms, std::memory_order_release);
    ESP_LOGI(kTag, "Auto sleep delay updated: %u ms%s",
             static_cast<unsigned>(delay_ms),
             delay_ms == 0 ? " (disabled)" : "");
    RecordUserActivity();
}

void Application::ConfigurePowerManagement() {
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm_config = {};
    pm_config.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    pm_config.min_freq_mhz = 80;
    pm_config.light_sleep_enable = false;
    esp_err_t ret = esp_pm_configure(&pm_config);
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "PM config 80-%d MHz failed: %s",
                 pm_config.max_freq_mhz, esp_err_to_name(ret));
        pm_config.min_freq_mhz = 40;
        ret = esp_pm_configure(&pm_config);
    }
    if (ret == ESP_OK) {
        ESP_LOGI(kTag, "PM DFS enabled: min=%d MHz max=%d MHz auto_light_sleep=%d",
                 pm_config.min_freq_mhz,
                 pm_config.max_freq_mhz,
                 pm_config.light_sleep_enable ? 1 : 0);
    } else {
        ESP_LOGW(kTag, "PM DFS disabled: %s", esp_err_to_name(ret));
    }
#endif
}

void Application::RecordUserActivity() {
    last_user_activity_ms_.store(NowMs(), std::memory_order_release);
    const uint32_t delay_ms = IdleSleepDelayMs();
    if (delay_ms == 0) {
        ArmLowPowerTimer(0);
        return;
    }
    sm_kick(delay_ms, "user_activity");
    ArmLowPowerTimer(delay_ms);
}

void Application::HandleUserWakeFromLightSleep() {
    const int64_t resume_start_ms = NowMs();
    const LightSleepWakeAction wake_action =
        Board::GetInstance().ConsumeLightSleepWakeAction();
    ESP_LOGI(kTag, "Light sleep user wake resume begin action=%s",
             WakeActionName(wake_action));

    RecordUserActivity();
    VocabularyButton replay_button = VocabularyButton::kConfirmClick;
    const bool replay_wake_button =
        WakeActionToVocabularyButton(wake_action, &replay_button);
    vocabulary_app_.HandleUserWakeFromLowPowerMode(!replay_wake_button);
    if (replay_wake_button) {
        ESP_LOGI(kTag, "Replaying light sleep wake button action=%s",
                 WakeActionName(wake_action));
        vocabulary_app_.HandleButton(replay_button);
    }
    // Queue the user-visible e-paper update before bringing WiFi back.  Starting
    // the station can take long enough to make the wake button feel ignored.
    vocabulary_app_.RestoreNetworkAfterUserWake();

    ESP_LOGI(kTag, "Light sleep user wake resume done in %lld ms replayed=%d",
             static_cast<long long>(NowMs() - resume_start_ms),
             replay_wake_button ? 1 : 0);
}

void Application::LowPowerTimerEntry(void* arg) {
    auto* app = static_cast<Application*>(arg);
    if (app == nullptr) {
        return;
    }
    app->Schedule([app]() {
        app->HandleLowPowerTimer();
    });
}

void Application::ArmLowPowerTimer(uint32_t delay_ms) {
    if (low_power_timer_ == nullptr) {
        return;
    }
    esp_timer_stop(low_power_timer_);
    if (delay_ms == 0) {
        return;
    }
    const uint64_t delay_us = static_cast<uint64_t>(delay_ms) * 1000ULL;
    const esp_err_t ret = esp_timer_start_once(low_power_timer_, delay_us);
    if (ret != ESP_OK) {
        ESP_LOGW(kTag, "Failed to arm low power timer: %s", esp_err_to_name(ret));
    }
}

void Application::HandleLowPowerTimer() {
    const uint32_t idle_sleep_delay_ms = IdleSleepDelayMs();
    if (!kEnableIdleLowPowerSleep || idle_sleep_delay_ms == 0) {
        ArmLowPowerTimer(0);
        return;
    }

    const int64_t last_activity_ms =
        last_user_activity_ms_.load(std::memory_order_acquire);
    if (last_activity_ms <= 0) {
        RecordUserActivity();
        return;
    }

    const int64_t elapsed_ms = NowMs() - last_activity_ms;
    if (elapsed_ms < static_cast<int64_t>(idle_sleep_delay_ms)) {
        ArmLowPowerTimer(static_cast<uint32_t>(
            static_cast<int64_t>(idle_sleep_delay_ms) - elapsed_ms));
        return;
    }

    if (!audio_service_.IsIdle() ||
        !vocabulary_app_.CanEnterLowPowerMode() ||
        !SleepManager::GetInstance().CanSleepNow()) {
        ArmLowPowerTimer(kLowPowerRetryMs);
        return;
    }

    StopSound();
    MuteSound();
    vocabulary_app_.PrepareForLowPowerMode();

    Display* display = Board::GetInstance().GetDisplay();
    if (display != nullptr) {
        VocabularySnapshot sleep_snapshot = vocabulary_app_.GetSnapshot();
        sleep_snapshot.sleeping = true;
        display->UpdateVocabularySnapshot(sleep_snapshot);
        // Render the LVGL tree before requesting the physical e-paper update;
        // otherwise the refresh worker can race ahead with the previous frame.
        display->RenderNow();
        display->RequestUrgentRefresh();

        const int64_t refresh_deadline_ms = NowMs() + kSleepScreenRefreshTimeoutMs;
        while (display->IsRefreshPending() && NowMs() < refresh_deadline_ms) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (display->IsRefreshPending()) {
            ESP_LOGW(kTag, "Sleep screen refresh timed out; postponing sleep");
            vocabulary_app_.RefreshSnapshot();
            ArmLowPowerTimer(kLowPowerRetryMs);
            return;
        }
    }

    if (kEnableAutoDeepSleep &&
        elapsed_ms >= static_cast<int64_t>(kDeepSleepIdleMs)) {
        ESP_LOGI(kTag, "Idle for %lld ms, entering deep sleep",
                 static_cast<long long>(elapsed_ms));
        Board::GetInstance().EnterUltraLowPowerMode();
        RecordUserActivity();
        vocabulary_app_.RefreshSnapshot();
        return;
    }

    const uint32_t light_sleep_timeout_ms = kEnableAutoDeepSleep
        ? static_cast<uint32_t>(static_cast<int64_t>(kDeepSleepIdleMs) - elapsed_ms)
        : 0;
    ESP_LOGI(kTag, "Idle for %lld ms, entering light sleep timeout=%u",
             static_cast<long long>(elapsed_ms),
             static_cast<unsigned>(light_sleep_timeout_ms));
    const bool woke_by_user = Board::GetInstance().EnterLightSleepMode(light_sleep_timeout_ms);
    if (woke_by_user) {
        HandleUserWakeFromLightSleep();
        return;
    }

    const int64_t after_sleep_elapsed_ms = NowMs() - last_activity_ms;
    if (kEnableAutoDeepSleep &&
        after_sleep_elapsed_ms >= static_cast<int64_t>(kDeepSleepIdleMs)) {
        HandleLowPowerTimer();
        return;
    }

    ArmLowPowerTimer(kEnableAutoDeepSleep
        ? static_cast<uint32_t>(static_cast<int64_t>(kDeepSleepIdleMs) - after_sleep_elapsed_ms)
        : kLowPowerRetryMs);
}
