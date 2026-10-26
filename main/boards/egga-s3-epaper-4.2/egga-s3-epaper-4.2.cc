#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <iot_button.h>

#include <memory>
#include <utility>

#include "FT/factory_test_service.h"
#include "application.h"
#include "ai/ai_dialog_service.h"
#include "board.h"
#include "board_power_bsp.h"
#include "boards/common/i2c_bus_lock.h"
#include "boards/egga/egga_nfc.h"
#include "button.h"
#include "charge_status.h"
#include "codecs/es8311_audio_codec.h"
#include "config.h"
#include "custom_lcd_display.h"
#include "display/pages/factory_test_page_adapter.h"
#include "esp_network.h"
#include "ssid_manager.h"
#include "rtc_pcf8563.h"
#include "wifi_manager.h"

namespace {

constexpr char kTag[] = "EggaFtBoard";
constexpr uint16_t kNavLongPressMs = 1000;
constexpr uint16_t kConfirmLongPressMs = 1600;
// A settings entry must be an actual two-button hold.  Do not infer it from
// two separate long-press callbacks: those callbacks can be delivered in a
// different order when the GPIO driver is busy.
constexpr int64_t kComboClickSuppressMs = 800;
constexpr int64_t kWakeButtonSuppressMs = 3000;
constexpr int64_t kWakeButtonReleaseSuppressMs = 900;
constexpr uint32_t kNfcWakeRestoreDelayMs = 150;

class NullNetworkInterface : public NetworkInterface {
public:
    std::unique_ptr<Http> CreateHttp(int connect_id = -1) override {
        (void)connect_id;
        return nullptr;
    }

    std::unique_ptr<Tcp> CreateTcp(int connect_id = -1) override {
        (void)connect_id;
        return nullptr;
    }

    std::unique_ptr<Tcp> CreateSsl(int connect_id = -1) override {
        (void)connect_id;
        return nullptr;
    }

    std::unique_ptr<Udp> CreateUdp(int connect_id = -1) override {
        (void)connect_id;
        return nullptr;
    }

    std::unique_ptr<Mqtt> CreateMqtt(int connect_id = -1) override {
        (void)connect_id;
        return nullptr;
    }

    std::unique_ptr<WebSocket> CreateWebSocket(int connect_id = -1) override {
        (void)connect_id;
        return nullptr;
    }
};

class CustomBoard : public Board {
public:
    CustomBoard()
        : up_button_(TODO_UP_BUTTON_GPIO, false, kNavLongPressMs, 0, false),
          down_button_(TODO_DOWN_BUTTON_GPIO, false, kNavLongPressMs, 0, false),
          confirm_button_(BOOT_BUTTON_GPIO, false, kConfirmLongPressMs, 0, false) {
        const esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
        gpio_deep_sleep_hold_dis();
        if (wake_cause != ESP_SLEEP_WAKEUP_UNDEFINED) {
            ESP_LOGI(kTag, "Boot after sleep wakeup, cause=%d", static_cast<int>(wake_cause));
        }
        InitializePower();
        InitializeI2c();
        InitializeRtc();
        InitializeNfc();
        InitializeChargeStatus();
        InitializeLcdDisplay();
        InitializeButtons();
        BindFactoryTestCallbacks();
    }

    std::string GetBoardType() override {
        return "egga-s3-epaper-4.2";
    }

    std::string GetDisplayName() override {
        return "Egga Vocabulary Trainer 4.2";
    }

    AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec codec(i2c_bus_,
                                      I2C_NUM_0,
                                      AUDIO_INPUT_SAMPLE_RATE,
                                      AUDIO_OUTPUT_SAMPLE_RATE,
                                      AUDIO_I2S_GPIO_MCLK,
                                      AUDIO_I2S_GPIO_BCLK,
                                      AUDIO_I2S_GPIO_WS,
                                      AUDIO_I2S_GPIO_DOUT,
                                      AUDIO_I2S_GPIO_DIN,
                                      AUDIO_CODEC_PA_PIN,
                                      AUDIO_CODEC_ES8311_ADDR);
        return &codec;
    }

    Display* GetDisplay() override {
        return display_;
    }

    NetworkInterface* GetNetwork() override {
        return &network_;
    }

    void StartNetwork() override {
        auto& wifi = WifiManager::GetInstance();
        ConfigureWifiEventCallback();
        if (!wifi.IsInitialized()) {
            WifiManagerConfig config;
            config.ssid_prefix = "Egga";
            config.station_scan_min_interval_seconds = 3;
            config.station_scan_max_interval_seconds = 30;
            if (!wifi.Initialize(config)) {
                ESP_LOGW(kTag, "WiFi init failed");
                return;
            }
        }
        if (SsidManager::GetInstance().GetSsidList().empty()) {
            wifi.StartConfigAp();
            return;
        }
        wifi.StartStation();
        wifi.SetPowerSaveLevel(WifiPowerSaveLevel::LOW_POWER);
    }

    void RequestNetwork() override {
        StartNetwork();
    }

    void SetNetworkEventCallback(NetworkEventCallback callback) override {
        network_event_callback_ = std::move(callback);
        ConfigureWifiEventCallback();
    }

    bool IsFactoryTestMode() const override {
        return false;
    }

    void EnterFactoryTestFlow() override {
        if (display_ == nullptr) {
            return;
        }
        display_->ShowFactoryTestPage();
        display_->RequestUrgentFullRefresh();
        FactoryTestService::Instance().StartFlow();
    }

    const char* GetNetworkStateIcon() override {
        return nullptr;
    }

    bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        ChargeStatus::Snapshot snapshot = charge_status_.Get();
        charging = snapshot.charging;
        discharging = !snapshot.power_present;

        uint16_t voltage_mv = 0;
        uint8_t percent = 0;
        const bool ok = ReadBatteryStatus(voltage_mv, percent);
        level = static_cast<int>(percent);
        return ok;
    }

    void SetPowerSaveLevel(PowerSaveLevel level) override {
        (void)level;
    }

    bool EnterLightSleepMode(uint32_t max_sleep_ms) override {
        ESP_LOGI(kTag, "Entering ultra-low-power light sleep");
        last_light_sleep_wake_action_ = LightSleepWakeAction::kNone;
        last_light_sleep_wake_still_pressed_ = false;
        WifiManager::GetInstance().StopStation();

        if (nfc_ != nullptr) {
            nfc_->PowerOff();
        }

        AudioCodec* codec = GetAudioCodec();
        if (codec != nullptr) {
            codec->EnableInput(false);
            codec->EnableOutput(false);
        }

        if (power_ != nullptr) {
            power_->PowerAmpOff();
            power_->PowerAudioOff();
            power_->PowerEpdOff();
        }

        static constexpr uint64_t kWakeMask =
            (1ULL << static_cast<int>(BOOT_BUTTON_GPIO)) |
            (1ULL << static_cast<int>(TODO_UP_BUTTON_GPIO)) |
            (1ULL << static_cast<int>(TODO_DOWN_BUTTON_GPIO));

        gpio_config_t wake_cfg = {};
        wake_cfg.pin_bit_mask = kWakeMask;
        wake_cfg.mode = GPIO_MODE_INPUT;
        wake_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
        wake_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        wake_cfg.intr_type = GPIO_INTR_DISABLE;
        ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&wake_cfg));

        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL));
        ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_wakeup_enable(BOOT_BUTTON_GPIO, GPIO_INTR_LOW_LEVEL));
        ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_wakeup_enable(TODO_UP_BUTTON_GPIO, GPIO_INTR_LOW_LEVEL));
        ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_wakeup_enable(TODO_DOWN_BUTTON_GPIO, GPIO_INTR_LOW_LEVEL));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_sleep_enable_gpio_wakeup());
        if (max_sleep_ms > 0) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(
                esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(max_sleep_ms) * 1000ULL));
        }

        const int64_t sleep_start_us = esp_timer_get_time();
        const esp_err_t sleep_ret = esp_light_sleep_start();
        const int64_t slept_ms = (esp_timer_get_time() - sleep_start_us) / 1000;
        const esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
        const esp_err_t button_resume_ret = iot_button_resume();
        const bool woke_by_user = sleep_ret == ESP_OK && wake_cause == ESP_SLEEP_WAKEUP_GPIO;
        const bool woke_by_timer = sleep_ret == ESP_OK && wake_cause == ESP_SLEEP_WAKEUP_TIMER;
        uint64_t wake_gpio_mask = 0;
        if (woke_by_user) {
            wake_gpio_mask = CurrentHeldWakeButtonMask();
            last_light_sleep_wake_action_ = DetectLightSleepWakeAction(wake_gpio_mask);
            last_light_sleep_wake_still_pressed_ = AnyWakeButtonHeld();
        }

        ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_wakeup_disable(BOOT_BUTTON_GPIO));
        ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_wakeup_disable(TODO_UP_BUTTON_GPIO));
        ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_wakeup_disable(TODO_DOWN_BUTTON_GPIO));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER));
        if (button_resume_ret != ESP_OK) {
            ESP_LOGW(kTag, "Failed to resume button scanner after light sleep: %s",
                     esp_err_to_name(button_resume_ret));
        }

        if (!woke_by_timer && power_ != nullptr) {
            power_->PowerAudioOn();
            power_->PowerEpdOn();
        }
        if (!woke_by_timer && nfc_ != nullptr) {
            RestoreNfcAfterWake();
        }

        ESP_LOGI(kTag,
                 "Woke from ultra-low-power light sleep: %s, cause=%d slept=%lld ms "
                 "gpio_mask=0x%llx action=%u",
                 esp_err_to_name(sleep_ret),
                 static_cast<int>(wake_cause),
                 static_cast<long long>(slept_ms),
                 static_cast<unsigned long long>(wake_gpio_mask),
                 static_cast<unsigned>(last_light_sleep_wake_action_));
        return woke_by_user;
    }

    LightSleepWakeAction ConsumeLightSleepWakeAction() override {
        const LightSleepWakeAction action = last_light_sleep_wake_action_;
        if (action != LightSleepWakeAction::kNone && last_light_sleep_wake_still_pressed_) {
            suppress_wake_button_events_ = true;
            suppress_wake_button_events_started_ms_ = GetNowMs();
            suppress_wake_button_events_until_ms_ = GetNowMs() + kWakeButtonSuppressMs;
            ESP_LOGI(kTag, "Suppressing native wake button events action=%u",
                     static_cast<unsigned>(action));
        }
        last_light_sleep_wake_action_ = LightSleepWakeAction::kNone;
        last_light_sleep_wake_still_pressed_ = false;
        return action;
    }

    void EnterUltraLowPowerMode() override {
        ESP_LOGI(kTag, "Entering ultra-low-power deep sleep");
        WifiManager::GetInstance().StopStation();

        if (nfc_ != nullptr) {
            nfc_->PowerOff();
        }

        AudioCodec* codec = GetAudioCodec();
        if (codec != nullptr) {
            codec->EnableInput(false);
            codec->EnableOutput(false);
        }

        if (power_ != nullptr) {
            power_->PowerAmpOff();
            power_->PowerAudioOff();
            power_->PowerEpdOff();
        }

        static constexpr uint64_t kDeepWakeMask =
            (1ULL << static_cast<int>(BOOT_BUTTON_GPIO)) |
            (1ULL << static_cast<int>(TODO_UP_BUTTON_GPIO)) |
            (1ULL << static_cast<int>(TODO_DOWN_BUTTON_GPIO));

        gpio_config_t wake_cfg = {};
        wake_cfg.pin_bit_mask = kDeepWakeMask;
        wake_cfg.mode = GPIO_MODE_INPUT;
        wake_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
        wake_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        wake_cfg.intr_type = GPIO_INTR_DISABLE;
        ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&wake_cfg));

        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL));
        const esp_err_t wake_ret =
            esp_sleep_enable_ext1_wakeup_io(kDeepWakeMask, ESP_EXT1_WAKEUP_ANY_LOW);
        if (wake_ret != ESP_OK) {
            ESP_LOGE(kTag, "Failed to configure deep sleep wakeup: %s",
                     esp_err_to_name(wake_ret));
            if (power_ != nullptr) {
                power_->PowerAudioOn();
                power_->PowerEpdOn();
            }
            if (display_ != nullptr) {
                display_->RequestUrgentFullRefresh();
            }
            return;
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON));
        gpio_deep_sleep_hold_en();
        esp_deep_sleep_start();
    }

    std::string GetBoardJson() override {
        return R"({"type":"egga-s3-epaper-4.2","mode":"vocabulary_card"})";
    }

    std::string GetDeviceStatusJson() override {
        return R"({"mode":"vocabulary_card"})";
    }

    RtcPcf8563* GetRtc() {
        return rtc_.get();
    }

    EggaNfc* GetNfc() {
        return nfc_.get();
    }

    ChargeStatus::Snapshot GetChargeSnapshot() const {
        return charge_status_.Get();
    }

    ChargeStatus::Snapshot RefreshChargeSnapshotForFactoryTest() {
        charge_status_.Tick(GetNowMs());
        return charge_status_.Get();
    }

    bool ReadBatteryPercentForFactoryTest(int* level) {
        if (level == nullptr) {
            return false;
        }

        uint16_t voltage_mv = 0;
        uint8_t percent = 0;
        const bool ok = ReadBatteryStatus(voltage_mv, percent);
        *level = static_cast<int>(percent);
        return ok;
    }

    void SetFactoryLedOverride(bool enabled, bool blink) {
        if (power_ != nullptr) {
            power_->SetFactoryLedOverride(enabled, blink);
        }
    }

private:
    static int64_t GetNowMs() {
        return esp_timer_get_time() / 1000;
    }

    static uint64_t GpioBit(gpio_num_t gpio) {
        return 1ULL << static_cast<int>(gpio);
    }

    static bool WakeMaskOrLevelActive(uint64_t wake_gpio_mask, gpio_num_t gpio) {
        return (wake_gpio_mask & GpioBit(gpio)) != 0 || gpio_get_level(gpio) == 0;
    }

    uint64_t CurrentHeldWakeButtonMask() const {
        uint64_t mask = 0;
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            mask |= GpioBit(BOOT_BUTTON_GPIO);
        }
        if (gpio_get_level(TODO_UP_BUTTON_GPIO) == 0) {
            mask |= GpioBit(TODO_UP_BUTTON_GPIO);
        }
        if (gpio_get_level(TODO_DOWN_BUTTON_GPIO) == 0) {
            mask |= GpioBit(TODO_DOWN_BUTTON_GPIO);
        }
        return mask;
    }

    bool AnyWakeButtonHeld() const {
        return CurrentHeldWakeButtonMask() != 0;
    }

    void RestoreNfcAfterWake() {
        const BaseType_t created = xTaskCreate(
            [](void* arg) {
                auto* self = static_cast<CustomBoard*>(arg);
                vTaskDelay(pdMS_TO_TICKS(kNfcWakeRestoreDelayMs));
                const int64_t started_ms = GetNowMs();
                const bool restored = self->nfc_ != nullptr && self->nfc_->PowerOn();
                ESP_LOGI(kTag, "NFC wake restore done ok=%d elapsed=%lld ms",
                         restored ? 1 : 0,
                         static_cast<long long>(GetNowMs() - started_ms));
                vTaskDelete(nullptr);
            },
            "nfc_wake",
            3 * 1024,
            this,
            1,
            nullptr);
        if (created != pdPASS) {
            ESP_LOGW(kTag, "Failed to defer NFC wake restore; restoring inline");
            nfc_->PowerOn();
        }
    }

    LightSleepWakeAction DetectLightSleepWakeAction(uint64_t wake_gpio_mask) const {
        const bool up_active = WakeMaskOrLevelActive(wake_gpio_mask, TODO_UP_BUTTON_GPIO);
        const bool down_active = WakeMaskOrLevelActive(wake_gpio_mask, TODO_DOWN_BUTTON_GPIO);
        const bool confirm_active = WakeMaskOrLevelActive(wake_gpio_mask, BOOT_BUTTON_GPIO);

        if (up_active && down_active) {
            return LightSleepWakeAction::kSettingsCombo;
        }
        if (confirm_active) {
            return LightSleepWakeAction::kConfirmClick;
        }
        if (up_active) {
            return LightSleepWakeAction::kUpClick;
        }
        if (down_active) {
            return LightSleepWakeAction::kDownClick;
        }
        return LightSleepWakeAction::kNone;
    }

    void ClearWakeButtonSuppression() {
        suppress_wake_button_events_ = false;
        suppress_wake_button_events_started_ms_ = 0;
        suppress_wake_button_events_until_ms_ = 0;
    }

    bool ShouldSuppressWakeButtonEvent(const char* event_name) {
        if (!suppress_wake_button_events_) {
            return false;
        }

        const int64_t now_ms = GetNowMs();
        if (now_ms > suppress_wake_button_events_until_ms_) {
            ClearWakeButtonSuppression();
            return false;
        }

        const bool wake_button_held = AnyWakeButtonHeld();
        if (!wake_button_held &&
            now_ms - suppress_wake_button_events_started_ms_ > kWakeButtonReleaseSuppressMs) {
            ClearWakeButtonSuppression();
            return false;
        }

        ESP_LOGI(kTag, "Suppressed native wake button event: %s",
                 event_name != nullptr ? event_name : "unknown");
        if (!wake_button_held) {
            ClearWakeButtonSuppression();
        }
        return true;
    }

    void InitializePower() {
        power_ = std::make_unique<BoardPowerBsp>(EPD_PWR_PIN,
                                                 Audio_PWR_PIN,
                                                 Audio_AMP_PIN,
                                                 VBAT_PWR_PIN,
                                                 &charge_status_);
        power_->VbatPowerOn();
        power_->PowerAudioOn();
        power_->PowerEpdOn();
        while (!gpio_get_level(VBAT_PWR_GPIO)) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    void InitializeI2c() {
        ScopedI2cBusLock bus_lock("CustomBoard::InitializeI2c");
        ESP_ERROR_CHECK(bus_lock.status());

        i2c_master_bus_config_t i2c_bus_cfg = {};
        i2c_bus_cfg.i2c_port = static_cast<i2c_port_t>(0);
        i2c_bus_cfg.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
        i2c_bus_cfg.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
        i2c_bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        i2c_bus_cfg.glitch_ignore_cnt = 7;
        i2c_bus_cfg.intr_priority = 0;
        i2c_bus_cfg.trans_queue_depth = 0;
        i2c_bus_cfg.flags.enable_internal_pullup = 1;
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeRtc() {
        rtc_ = std::make_unique<RtcPcf8563>(i2c_bus_, RTC_I2C_ADDR);
        if (!rtc_->Init(RTC_INT_GPIO)) {
            ESP_LOGW(kTag, "RTC init failed");
        }
    }

    void InitializeNfc() {
        nfc_ = std::make_unique<EggaNfc>(i2c_bus_,
                                            NFC_I2C_ADDR,
                                            NFC_PWR_GPIO,
                                            NFC_FD_GPIO,
                                            NFC_FD_ACTIVE_LEVEL);
        if (!nfc_->Init()) {
            ESP_LOGW(kTag, "NFC init failed");
            nfc_.reset();
        }
    }

    void InitializeChargeStatus() {
        charge_status_.Init(CHARGE_DETECT_GPIO, CHARGE_FULL_GPIO, GetNowMs());
        charge_status_.OnStateChanged([this](const ChargeStatus::Snapshot&) {
            if (display_ == nullptr || display_->IsFactoryTestPageActive()) {
                return;
            }
            Application::GetInstance().Schedule([]() {
                Application::GetInstance().GetVocabularyApp().RefreshSnapshot();
            });
        });
    }

    void InitializeLcdDisplay() {
        custom_lcd_spi_t lcd_spi_data = {};
        lcd_spi_data.cs = EPD_CS_PIN;
        lcd_spi_data.dc = EPD_DC_PIN;
        lcd_spi_data.rst = EPD_RST_PIN;
        lcd_spi_data.busy = EPD_BUSY_PIN;
        lcd_spi_data.mosi = EPD_MOSI_PIN;
        lcd_spi_data.scl = EPD_SCK_PIN;
        lcd_spi_data.power = EPD_PWR_PIN;
        lcd_spi_data.spi_host = EPD_SPI_NUM;
        lcd_spi_data.buffer_len = ((EXAMPLE_LCD_WIDTH + 7) / 8) * EXAMPLE_LCD_HEIGHT;
        display_ = new CustomLcdDisplay(nullptr,
                                        nullptr,
                                        EXAMPLE_LCD_WIDTH,
                                        EXAMPLE_LCD_HEIGHT,
                                        DISPLAY_OFFSET_X,
                                        DISPLAY_OFFSET_Y,
                                        DISPLAY_MIRROR_X,
                                        DISPLAY_MIRROR_Y,
                                        DISPLAY_SWAP_XY,
                                        lcd_spi_data);
    }

    void InitializeButtons() {
        up_button_.OnPressDown([this]() {
            up_pressed_ = true;
        });

        up_button_.OnPressUp([this]() {
            up_pressed_ = false;
            ResetNavigationComboState();
        });

        down_button_.OnPressDown([this]() {
            down_pressed_ = true;
        });

        down_button_.OnPressUp([this]() {
            down_pressed_ = false;
            ResetNavigationComboState();
        });

        confirm_button_.OnPressDown([this]() {
            confirm_pressed_ = true;
            confirm_long_press_active_ = false;
        });

        confirm_button_.OnPressUp([this]() {
            confirm_pressed_ = false;
            if (confirm_long_press_active_) {
                confirm_long_press_active_ = false;
                AiDialogService::GetInstance().SetPttButtonHeld(false);
                DispatchVocabularyOnlyButton(VocabularyButton::kConfirmLongRelease);
            }
        });

        up_button_.OnClick([this]() {
            if (ShouldSuppressNavigationComboClick()) {
                return;
            }
            DispatchButton(FactoryTestButton::kUpClick, VocabularyButton::kUpClick);
        });

        up_button_.OnLongPress([this]() {
            DispatchNavigationLongButton(true);
        });

        down_button_.OnClick([this]() {
            if (ShouldSuppressNavigationComboClick()) {
                return;
            }
            DispatchButton(FactoryTestButton::kDownClick, VocabularyButton::kDownClick);
        });

        down_button_.OnLongPress([this]() {
            DispatchNavigationLongButton(false);
        });

        confirm_button_.OnClick([this]() {
            DispatchButton(FactoryTestButton::kConfirmClick, VocabularyButton::kConfirmClick);
        });

        confirm_button_.OnDoubleClick([this]() {
            DispatchVocabularyOnlyButton(VocabularyButton::kConfirmDoubleClick);
        });

        confirm_button_.OnLongPress([this]() {
            if (display_ == nullptr || !display_->IsFactoryTestPageActive()) {
                confirm_long_press_active_ = true;
                AiDialogService::GetInstance().SetPttButtonHeld(true);
            }
            DispatchButton(FactoryTestButton::kConfirmLongPress, VocabularyButton::kConfirmLongPress);
        });
    }

    void ResetNavigationComboState() {
        if (!up_pressed_ && !down_pressed_) {
            navigation_combo_dispatched_ = false;
        }
    }

    bool ShouldSuppressNavigationComboClick() const {
        return GetNowMs() < navigation_combo_suppress_clicks_until_ms_;
    }

    void DispatchNavigationLongButton(bool is_up_button) {
        if (ShouldSuppressWakeButtonEvent(is_up_button ? "up_long" : "down_long")) {
            return;
        }
        Application::GetInstance().RecordUserActivity();
        if (display_ != nullptr && display_->IsFactoryTestPageActive()) {
            return;
        }
        if (up_pressed_ && down_pressed_) {
            if (!navigation_combo_dispatched_) {
                navigation_combo_dispatched_ = true;
                navigation_combo_suppress_clicks_until_ms_ = GetNowMs() + kComboClickSuppressMs;
                ESP_LOGI(kTag, "Navigation combo recognized; opening settings");
                Application::GetInstance().Schedule([]() {
                    ESP_LOGI(kTag, "Navigation combo delivered to vocabulary app");
                    Application::GetInstance().GetVocabularyApp().HandleButton(VocabularyButton::kSettingsCombo);
                });
            }
            return;
        }
        if (navigation_combo_dispatched_) {
            return;
        }
        ESP_LOGI(kTag, "Navigation long press button=%s", is_up_button ? "up" : "down");
        const VocabularyButton button = is_up_button
            ? VocabularyButton::kUpLongPress
            : VocabularyButton::kDownLongPress;
        Application::GetInstance().Schedule([button]() {
            Application::GetInstance().GetVocabularyApp().HandleButton(button);
        });
    }

    void DispatchButton(FactoryTestButton factory_button, VocabularyButton vocabulary_button) {
        if (ShouldSuppressWakeButtonEvent("button")) {
            return;
        }
        Application::GetInstance().RecordUserActivity();
        Application::GetInstance().Schedule([this, factory_button, vocabulary_button]() {
            if (display_ != nullptr && display_->IsFactoryTestPageActive()) {
                FactoryTestService::Instance().HandleButton(factory_button);
                return;
            }
            Application::GetInstance().GetVocabularyApp().HandleButton(vocabulary_button);
        });
    }

    void DispatchVocabularyOnlyButton(VocabularyButton vocabulary_button) {
        if (ShouldSuppressWakeButtonEvent("vocabulary_button")) {
            return;
        }
        Application::GetInstance().RecordUserActivity();
        Application::GetInstance().Schedule([this, vocabulary_button]() {
            if (display_ != nullptr && display_->IsFactoryTestPageActive()) {
                return;
            }
            Application::GetInstance().GetVocabularyApp().HandleButton(vocabulary_button);
        });
    }

    void BindFactoryTestCallbacks() {
        auto& factory_test = FactoryTestService::Instance();
        factory_test.SetSnapshotCallback([this](const FactoryTestSnapshot& snapshot) {
            if (display_ == nullptr) {
                return;
            }

            auto* page = display_->GetFactoryTestPageAdapter();
            if (page == nullptr) {
                return;
            }

            DisplayLockGuard lock(display_);
            page->UpdateSnapshot(snapshot);
            display_->RequestUrgentRefresh();
        });

        factory_test.SetShutdownCallback([this]() {
            if (power_ != nullptr) {
                power_->VbatPowerOff();
            }
        });
    }

    void ConfigureWifiEventCallback() {
        WifiManager::GetInstance().SetEventCallback([this](WifiEvent event) {
            NetworkEvent mapped = NetworkEvent::Disconnected;
            switch (event) {
                case WifiEvent::Scanning:
                    mapped = NetworkEvent::Scanning;
                    break;
                case WifiEvent::Connecting:
                    mapped = NetworkEvent::Connecting;
                    break;
                case WifiEvent::Connected:
                    mapped = NetworkEvent::Connected;
                    break;
                case WifiEvent::Disconnected:
                    mapped = NetworkEvent::Disconnected;
                    break;
                case WifiEvent::ConfigModeEnter:
                    mapped = NetworkEvent::WifiConfigModeEnter;
                    break;
                case WifiEvent::ConfigModeExit:
                    mapped = NetworkEvent::WifiConfigModeExit;
                    break;
            }

            std::string data;
            if (event == WifiEvent::Connecting || event == WifiEvent::Connected) {
                data = WifiManager::GetInstance().GetSsid();
            }
            if (network_event_callback_) {
                network_event_callback_(mapped, data);
            }
        });
    }

    uint16_t ReadBatteryVoltage() {
        static bool initialized = false;
        static adc_oneshot_unit_handle_t adc_handle = nullptr;
        static adc_cali_handle_t cali_handle = nullptr;

        if (!initialized) {
            adc_oneshot_unit_init_cfg_t init_config = {
                .unit_id = ADC_UNIT_1,
                .ulp_mode = ADC_ULP_MODE_DISABLE,
            };
            ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));

            adc_oneshot_chan_cfg_t ch_config = {
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_12,
            };
            ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_3, &ch_config));

            adc_cali_curve_fitting_config_t cali_config = {
                .unit_id = ADC_UNIT_1,
                .chan = ADC_CHANNEL_3,
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_12,
            };
            if (adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle) == ESP_OK) {
                initialized = true;
            }
        }

        if (!initialized) {
            return 0;
        }

        int raw_value = 0;
        int raw_voltage = 0;
        ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, ADC_CHANNEL_3, &raw_value));
        ESP_ERROR_CHECK(adc_cali_raw_to_voltage(cali_handle, raw_value, &raw_voltage));
        return static_cast<uint16_t>(raw_voltage * 2);
    }

    bool ReadBatteryStatus(uint16_t& voltage_mv, uint8_t& percent) {
        int voltage_sum = 0;
        for (int i = 0; i < 10; ++i) {
            voltage_sum += ReadBatteryVoltage();
        }

        const int average_voltage = voltage_sum / 10;
        if (average_voltage <= 0) {
            voltage_mv = 0;
            percent = 0;
            return false;
        }

        int computed_percent =
            (-1 * average_voltage * average_voltage + 9016 * average_voltage - 19189000) / 10000;
        computed_percent = computed_percent > 100 ? 100 : (computed_percent < 0 ? 0 : computed_percent);

        voltage_mv = static_cast<uint16_t>(average_voltage);
        percent = static_cast<uint8_t>(computed_percent);
        return true;
    }

    EspNetwork network_;
    CustomLcdDisplay* display_ = nullptr;
    NetworkEventCallback network_event_callback_;
    std::unique_ptr<BoardPowerBsp> power_;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    std::unique_ptr<RtcPcf8563> rtc_;
    std::unique_ptr<EggaNfc> nfc_;
    ChargeStatus charge_status_;
    Button up_button_;
    Button down_button_;
    Button confirm_button_;
    bool up_pressed_ = false;
    bool down_pressed_ = false;
    bool navigation_combo_dispatched_ = false;
    int64_t navigation_combo_suppress_clicks_until_ms_ = 0;
    bool confirm_pressed_ = false;
    bool confirm_long_press_active_ = false;
    LightSleepWakeAction last_light_sleep_wake_action_ = LightSleepWakeAction::kNone;
    bool last_light_sleep_wake_still_pressed_ = false;
    bool suppress_wake_button_events_ = false;
    int64_t suppress_wake_button_events_started_ms_ = 0;
    int64_t suppress_wake_button_events_until_ms_ = 0;
};

}  // namespace

DECLARE_BOARD(CustomBoard);

extern "C" void BoardOnNetworkConnected() {
}

extern "C" void BoardOnNetworkDisconnected() {
}

extern "C" RtcPcf8563* EggaGetRtc() {
    auto& board = static_cast<CustomBoard&>(Board::GetInstance());
    return board.GetRtc();
}

extern "C" ChargeStatus::Snapshot EggaGetChargeSnapshot() {
    auto& board = static_cast<CustomBoard&>(Board::GetInstance());
    return board.GetChargeSnapshot();
}

extern "C" ChargeStatus::Snapshot EggaRefreshChargeSnapshotForFactoryTest() {
    auto& board = static_cast<CustomBoard&>(Board::GetInstance());
    return board.RefreshChargeSnapshotForFactoryTest();
}

extern "C" bool EggaReadBatteryPercentForFactoryTest(int* level) {
    auto& board = static_cast<CustomBoard&>(Board::GetInstance());
    return board.ReadBatteryPercentForFactoryTest(level);
}

extern "C" void EggaSetFactoryLedOverride(bool enabled, bool blink) {
    auto& board = static_cast<CustomBoard&>(Board::GetInstance());
    board.SetFactoryLedOverride(enabled, blink);
}

extern "C" EggaNfc* EggaGetNfc() {
    auto& board = static_cast<CustomBoard&>(Board::GetInstance());
    return board.GetNfc();
}
