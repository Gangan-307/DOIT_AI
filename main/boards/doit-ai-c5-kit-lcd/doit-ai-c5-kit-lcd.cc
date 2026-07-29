#include "dual_network_board.h"
#include "audio/codecs/vb6824_audio_codec.h"
#include "display/lcd_display.h"
#include "application.h"
#include "button.h"
#include "adc_battery_monitor.h"
#include "config.h"

#include <wifi_station.h>
#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/spi_common.h>

#include "settings.h"

#include <wifi_station.h>
#include <wifi_configuration_ap.h>
#include <ssid_manager.h>
#include "assets/lang_config.h"

#include "led/circular_strip.h"
#include "power_save_timer.h" 
#include "backlight.h"
#include "esp_lcd_panel_st7789_fix.h"

#define TAG "CustomBoard"

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_awesome_16_4);

class C5LcdDisplay final : public SpiLcdDisplay
{
public:
  C5LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
               int width, int height, int offset_x, int offset_y,
               bool mirror_x, bool mirror_y, bool swap_xy, DisplayFonts fonts)
      : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y,
                      mirror_x, mirror_y, swap_xy, fonts)
  {
    // Keep network and battery indicators independent from the status text's flex layout.
    auto screen = lv_screen_active();
    lv_obj_set_parent(network_label_, screen);
    lv_obj_align(network_label_, LV_ALIGN_TOP_LEFT, 65, 3);
    lv_obj_move_foreground(network_label_);

    lv_obj_set_parent(battery_label_, screen);
    lv_obj_align(battery_label_, LV_ALIGN_TOP_LEFT, 220, 3);
    lv_obj_move_foreground(battery_label_);

  }
};

class CustomBoard : public DualNetworkBoard
{
private:
  int64_t boot_time_us_ = 0;
  Button boot_button_;
  VbAduioCodec audio_codec_;
  LcdDisplay *display_ = nullptr;
  PwmBacklight *backlight_ = nullptr;
  PowerSaveTimer *power_save_timer_ = nullptr;
  AdcBatteryMonitor *battery_monitor_ = nullptr;
  CircularStrip *led_ = nullptr;
  // PullUp4GPin pullup = PullUp4GPin();

  void InitializeButtons()
  {
    boot_button_.OnClick([this]()
                         {
      auto& app = Application::GetInstance();
      ESP_LOGI(TAG, "BOOT button single click, network=%s, state=%d",
               GetNetworkType() == NetworkType::ML307 ? "ML307" : "WIFI",
               static_cast<int>(app.GetDeviceState()));
      if (GetNetworkType() == NetworkType::WIFI) {
        if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
          // cast to WifiBoard
          auto& wifi_board = static_cast<WifiBoard&>(GetCurrentBoard());
          wifi_board.ResetWifiConfiguration();
        }
      }
      // app.ToggleChatState(); 
      if(Application::GetInstance().GetDeviceState() != kDeviceStateListening){
            Application::GetInstance().WakeWordInvoke("你好小智");
        }
    });

    boot_button_.OnDoubleClick([this]()
    {
      auto& app = Application::GetInstance();
      const int64_t elapsed_us = esp_timer_get_time() - boot_time_us_;
      ESP_LOGI(TAG, "BOOT button double click, network=%s, elapsed=%lld ms",
               GetNetworkType() == NetworkType::ML307 ? "ML307" : "WIFI",
               static_cast<long long>(elapsed_us / 1000));
      const bool is_wifi_configuring =
          GetNetworkType() == NetworkType::WIFI &&
          app.GetDeviceState() == kDeviceStateWifiConfiguring;
      if (elapsed_us <= 30LL * 1000LL * 1000LL &&
          (GetNetworkType() == NetworkType::ML307 || is_wifi_configuring))
      {
        ESP_LOGI(TAG, "Double click switches network to %s",
                 GetNetworkType() == NetworkType::ML307 ? "WIFI" : "ML307");
        SwitchNetworkType();
        return;
      }
#if (defined(CONFIG_VB6824_OTA_SUPPORT) && CONFIG_VB6824_OTA_SUPPORT == 1)
      if (esp_timer_get_time() > 20 * 1000 * 1000)
      {
        ESP_LOGI(TAG, "double click, do not enter OTA mode %ld", (uint32_t)esp_timer_get_time());
        return;
      }
      else if (WifiStation::GetInstance().IsConnected())
      {
        audio_codec_.OtaStart(0);
      }

#endif
    });

    boot_button_.OnPressRepeaDone([this](uint16_t count)
    {
      ESP_LOGI(TAG, "BOOT button press sequence done, count=%u, network=%s",
               static_cast<unsigned int>(count),
               GetNetworkType() == NetworkType::ML307 ? "ML307" : "WIFI");
      if (count >= 3 && GetNetworkType() == NetworkType::WIFI &&
          WifiStation::GetInstance().IsConnected())
      {
        ESP_LOGI(TAG, "Three or more presses reset WiFi configuration");
        auto& wifi_board = static_cast<WifiBoard&>(GetCurrentBoard());
        wifi_board.ResetWifiConfiguration();
      } 
    });
  }

  void InitializeSpi()
  {
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
    buscfg.miso_io_num = GPIO_NUM_NC;
    buscfg.sclk_io_num = DISPLAY_CLK_PIN;
    buscfg.quadwp_io_num = GPIO_NUM_NC;
    buscfg.quadhd_io_num = GPIO_NUM_NC;
    buscfg.max_transfer_sz = 64;
    buscfg.flags = SPICOMMON_BUSFLAG_SLAVE;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
  }

  void InitializeLcdDisplay()
  {
    esp_lcd_panel_io_handle_t panel_io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;
    esp_lcd_panel_io_spi_config_t io_config = {};

    io_config.cs_gpio_num = DISPLAY_CS_PIN;
    io_config.dc_gpio_num = DISPLAY_DC_PIN;
    io_config.spi_mode = 3;
    io_config.pclk_hz = 80 * 1000 * 1000;
    io_config.trans_queue_depth = 5;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = DISPLAY_RST_PIN;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel));

    esp_lcd_panel_reset(panel);
    vTaskDelay(100 / portTICK_PERIOD_MS);
    esp_lcd_panel_reset(panel);

    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR);
    esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
    esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    // esp_lcd_panel_set_gap(panel, 24, 0);//横屏需要设置x方向偏移24

    // int angle = (int)GetBoardCfg()->GetFunValue(kFunValueDisplayAngle);
    // AngleMap angle_map_ = {(angle) % 360, (angle + 90) % 360,
    //                        (angle + 180) % 360, (angle + 270) % 360};

    auto *display = new C5LcdDisplay(panel_io, panel,
                                     DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
                                     {
                                         .text_font = &font_puhui_16_4,
                                         .icon_font = &font_awesome_16_4,
                                         .emoji_font = font_emoji_64_init(),
                                     });
    display_ = display;
  }

  void InitializeBacklight()
  {
    backlight_ = new PwmBacklight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
  }

  void InitializeBatteryMonitor()
  {
    battery_monitor_ = new AdcBatteryMonitor(
        ADC_UNIT_1, BATTERY_ADC_CHANNEL,
        BATTERY_DIVIDER_UPPER_RESISTOR_OHMS,
        BATTERY_DIVIDER_LOWER_RESISTOR_OHMS,
        BATTERY_CHANG_GPIO);
  }
  void InitializeLed()
  {
    xTaskCreate(
        [](void *arg)
        {
          auto led = (CircularStrip **)arg;
          gpio_set_pull_mode(RBG_DI_GPIO, GPIO_PULLDOWN_ONLY);
          gpio_set_direction(RBG_DI_GPIO, GPIO_MODE_OUTPUT);
          (*led) = new CircularStrip(RBG_DI_GPIO, 1);
          vTaskDelete(NULL);
        },
        "led_init", 1024 * 8, &led_, 1, NULL);
  }

public:
  CustomBoard() : DualNetworkBoard(ML307_TX_GPIO,
                                   ML307_RX_GPIO, GPIO_NUM_NC),
                  boot_button_(BOOT_BUTTON_GPIO),
                  audio_codec_(CODEC_TX_GPIO,CODEC_RX_GPIO)
  {
    boot_time_us_ = esp_timer_get_time();
    gpio_set_level(POWER_KRRP_GPIO, 1); // 保持高电平
    InitializeLed();

    InitializeButtons();

    InitializeSpi();

    InitializeLcdDisplay();

    InitializeBacklight();
    InitializeBatteryMonitor();
    GetBacklight()->RestoreBrightness();

    // McpTools::GetInstance()->McpToolsInit();

    audio_codec_.OnWakeUp([this](const std::string &command)
                           {
            if (command == std::string(vb6824_get_wakeup_word())){
                if(Application::GetInstance().GetDeviceState() != kDeviceStateListening){
                    Application::GetInstance().WakeWordInvoke("你好小智");
                }
            }else if (command == "开始配网"){
                       if (GetNetworkType() == NetworkType::WIFI) {
          auto &wifi_board = static_cast<WifiBoard &>(GetCurrentBoard());
          wifi_board.ResetWifiConfiguration();
        }
            } });
  }

  virtual AudioCodec *GetAudioCodec() override
  {
    return &audio_codec_;
  }

  virtual Display *GetDisplay() override
  {
    return display_;
  }

  virtual Backlight *GetBacklight() override
  {
    return backlight_;
  }

  virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override
  {
    if (battery_monitor_ == nullptr)
    {
      return false;
    }

    charging = battery_monitor_->IsCharging();
    discharging = battery_monitor_->IsDischarging();
    level = battery_monitor_->GetBatteryLevel();
    return true;
  }

  // virtual void SetPowerSaveMode(bool enabled) override
  // {
  //     if (!enabled && power_save_timer_)
  //     {
  //         power_save_timer_->WakeUp();
  //     }
  //     DualNetworkBoard::SetPowerSaveMode(enabled);
  // }

  virtual Led *GetLed() override
  {
    return (Led *)led_;
  }
};
DECLARE_BOARD(CustomBoard);
