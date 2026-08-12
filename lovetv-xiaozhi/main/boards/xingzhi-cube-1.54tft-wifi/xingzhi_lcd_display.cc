#include "xingzhi_lcd_display.h"

#include <font_awesome_symbols.h>
#include <esp_log.h>
#include <esp_err.h>
#include <driver/ledc.h>
#include <vector>
#include <esp_lvgl_port.h>
#include <esp_timer.h>
#include "assets/lang_config.h"

#include "board.h"

#include "esp_adc/adc_oneshot.h"
#include "button.h"
#include <inttypes.h> 
#include "config.h"  
#include "settings.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include <driver/gpio.h>

#define TAG "XINGZHI_1_54_TFT_LcdDisplay"
#define LCD_LEDC_CH LEDC_CHANNEL_0
#define PIN_NUMBER GPIO_NUM_21  

LV_FONT_DECLARE(font_awesome_30_4);

XINGZHI_1_54_TFT_LcdDisplay::XINGZHI_1_54_TFT_LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           gpio_num_t backlight_pin, bool backlight_output_invert,
                           int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy,
                           DisplayFonts fonts)
    : panel_io_(panel_io), panel_(panel), backlight_pin_(backlight_pin), backlight_output_invert_(backlight_output_invert),
      fonts_(fonts),last_interaction_time_(esp_timer_get_time()),boot_button_(BOOT_BUTTON_GPIO),volume_up_button_(VOLUME_UP_BUTTON_GPIO), volume_down_button_(VOLUME_DOWN_BUTTON_GPIO){
    width_ = width;
    height_ = height;

    // 创建背光渐变定时器
    const esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            XINGZHI_1_54_TFT_LcdDisplay* display = static_cast<XINGZHI_1_54_TFT_LcdDisplay*>(arg);
            display->OnBacklightTimer();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "backlight_timer",
        .skip_unhandled_events = true,
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &backlight_timer_));
    InitializeBacklight(backlight_pin);

    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // 创建充电检测定时器
    esp_timer_create_args_t charging_timer_args = {
        .callback = &XINGZHI_1_54_TFT_LcdDisplay::ChargingTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "charging_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&charging_timer_args, &charging_timer_));

    // 创建电量检测定时器
    esp_timer_create_args_t battery_timer_args = {
        .callback = &XINGZHI_1_54_TFT_LcdDisplay::BatteryTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "battery_timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&battery_timer_args, &battery_timer_));

    // 初始化充电引脚
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << charging_pin_);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE; 
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;     
    gpio_config(&io_conf);

    rtc_gpio_init(GPIO_NUM_21);
    rtc_gpio_set_direction(GPIO_NUM_21, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(GPIO_NUM_21, 1);
    
    boot_button_.OnPressDown([this]() {
        this->UpdateInteractionTime();
    });

    volume_up_button_.OnPressDown([this]() {
        this->UpdateInteractionTime();
    });

    volume_down_button_.OnPressDown([this]() {
        this->UpdateInteractionTime();
    });

    // Set the display to on
    ESP_LOGI(TAG, "Turning display on");
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD screen");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 10),
        .double_buffer = false,
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
            .swap_bytes = 1,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }

    // 读取初始亮度值
    Settings settings("display", true);
    brightness_ = settings.GetInt("brightness", 75);

    SetBacklight(brightness_);

    SetupUI();
    StartChargingTimer();
    StartBatteryTimer();
}

XINGZHI_1_54_TFT_LcdDisplay::~XINGZHI_1_54_TFT_LcdDisplay() {
    if (backlight_timer_ != nullptr) {
        esp_timer_stop(backlight_timer_);
        esp_timer_delete(backlight_timer_);
    }
    // 然后再清理 LVGL 对象
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }
    if (status_bar_ != nullptr) {
        lv_obj_del(status_bar_);
    }
    if (side_bar_ != nullptr) {
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }
    if (display_ != nullptr) {
        lv_display_delete(display_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
}

void XINGZHI_1_54_TFT_LcdDisplay::SetBacklight(uint8_t brightness) {
    if (backlight_pin_ == GPIO_NUM_NC) {
        return;
    }

    if (brightness > 100) {
        brightness = 100;
    }

    ESP_LOGI(TAG, "Setting LCD backlight: %d%%", brightness);
    // 停止现有的定时器（如果正在运行）
    esp_timer_stop(backlight_timer_);

    Settings settings("display", true);
    if (is_light_sleep_) {
        // 处于浅睡眠状态，保存浅睡眠亮度
        settings.SetInt("sleep_bright", brightness);
        brightness_ = brightness;
    } else {
        // 正常状态，保存睡眠前亮度
        settings.SetInt("brightness", brightness);
        brightness_ = brightness;
    }

    // 启动定时器，每 5ms 更新一次
    ESP_ERROR_CHECK(esp_timer_start_periodic(backlight_timer_, 5 * 1000));
}

void XINGZHI_1_54_TFT_LcdDisplay::InitializeBacklight(gpio_num_t backlight_pin) {
    if (backlight_pin == GPIO_NUM_NC) {
        return;
    }

    // Setup LEDC peripheral for PWM backlight control
    const ledc_channel_config_t backlight_channel = {
        .gpio_num = backlight_pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
        .flags = {
            .output_invert = backlight_output_invert_,
        }
    };
    const ledc_timer_config_t backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 20000, //背光pwm频率需要高一点，防止电感啸叫
        .clk_cfg = LEDC_AUTO_CLK,
        .deconfigure = false
    };

    ESP_ERROR_CHECK(ledc_timer_config(&backlight_timer));
    ESP_ERROR_CHECK(ledc_channel_config(&backlight_channel));
}

void XINGZHI_1_54_TFT_LcdDisplay::OnBacklightTimer() {
    if (current_brightness_ < brightness_) {
        current_brightness_++;
    } else if (current_brightness_ > brightness_) {
        current_brightness_--;
    }
    
    // LEDC resolution set to 10bits, thus: 100% = 1023
    uint32_t duty_cycle = (1023 * current_brightness_) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH, duty_cycle);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH);
    
    if (current_brightness_ == brightness_) {
        esp_timer_stop(backlight_timer_);
    }
}

void XINGZHI_1_54_TFT_LcdDisplay::UpdateInteractionTime() {
    last_interaction_time_ = esp_timer_get_time();
    if (is_light_sleep_) {
        // 从浅睡眠中唤醒，恢复亮度
        Settings settings("display", true);
        uint8_t norm_bright = settings.GetInt("brightness", 75);
        SetBacklight(norm_bright);
        is_light_sleep_ = false;
    }
}

void XINGZHI_1_54_TFT_LcdDisplay::CheckSleepState() {
    int64_t current_time = esp_timer_get_time();
    int64_t elapsed_time = (current_time - last_interaction_time_) / 1000000; // 转换为秒

    int charging_level = gpio_get_level(charging_pin_);
    bool is_charging = (charging_level == 1);

    if (is_charging) {
        // 正在充电，不进入睡眠
        return;
    }

    if (elapsed_time >= 60 && !is_light_sleep_ && !is_deep_sleep_) {
        is_light_sleep_ = true;
        SetBacklight(1);
    } else if (elapsed_time >= 300 && is_light_sleep_)
    {
        is_deep_sleep_ = true;
        is_light_sleep_ = false;
        rtc_gpio_set_level(GPIO_NUM_21, 0);
        // 启用保持功能，确保睡眠期间电平不变
        rtc_gpio_hold_en(GPIO_NUM_21);
        esp_lcd_panel_disp_on_off(panel_, false); //关闭显示

        // 必须在进入深睡前设置唤醒源，否则只能断电重启才能恢复。
        // BOOT 键(GPIO0)是 RTC 引脚且低电平有效，按一下即唤醒（唤醒后为重新启动）。
        esp_sleep_enable_ext0_wakeup(BOOT_BUTTON_GPIO, 0);
        ESP_LOGI(TAG, "Entering deep sleep, press BOOT to wake");
        esp_deep_sleep_start();
    }
}

bool XINGZHI_1_54_TFT_LcdDisplay::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

void XINGZHI_1_54_TFT_LcdDisplay::Unlock() {
    lvgl_port_unlock();
}

void XINGZHI_1_54_TFT_LcdDisplay::SetupUI() {
    DisplayLockGuard lock(this);

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, fonts_.text_font, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);

    /* Status bar */
    status_bar_ = lv_obj_create(container_);
    lv_obj_set_size(status_bar_, LV_HOR_RES, fonts_.text_font->line_height);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    
    /* Content */
    content_ = lv_obj_create(container_);
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);

    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN); // 垂直布局（从上到下）
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_SPACE_EVENLY); // 子对象居中对齐，等距分布

    emotion_label_ = lv_label_create(content_);
    lv_obj_set_style_text_font(emotion_label_, &font_awesome_30_4, 0);
    lv_label_set_text(emotion_label_, FONT_AWESOME_AI_CHIP);

    chat_message_label_ = lv_label_create(content_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES * 0.9); // 限制宽度为屏幕宽度的 90%
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP); // 设置为自动换行模式
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0); // 设置文本居中对齐

    /* Status bar */
    lv_obj_set_flex_flow(status_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_column(status_bar_, 0, 0);
    lv_obj_set_style_pad_left(status_bar_, 2, 0);
    lv_obj_set_style_pad_right(status_bar_, 2, 0);

    network_label_ = lv_label_create(status_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, fonts_.icon_font, 0);

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(notification_label_, 1);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(notification_label_, "通知");
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_flex_grow(status_label_, 1);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(status_label_, "正在初始化");
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);

    mute_label_ = lv_label_create(status_bar_);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, fonts_.icon_font, 0);

    battery_label_ = lv_label_create(status_bar_);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, fonts_.icon_font, 0);

    // 充电状态标签
    charging_label_ = lv_label_create(status_bar_);
    lv_obj_set_style_text_font(charging_label_, fonts_.icon_font, 0);
    lv_obj_set_style_text_align(charging_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_flex_grow(charging_label_, 0);
    lv_label_set_text(charging_label_, FONT_AWESOME_BATTERY_CHARGING);
    
    // 检查充电状态，如果未充电，设置充电图标为空
    int charging_level = gpio_get_level(charging_pin_);
    if (charging_level == 0) {
        lv_label_set_text(charging_label_, "");
    }
}

void XINGZHI_1_54_TFT_LcdDisplay::UpdateBatteryAndChargingDisplay(uint16_t average_adc) {
    DisplayLockGuard lock(this);

    // 未充电时，显示电池图标
    if (charging_label_ != nullptr) {
        lv_label_set_text(charging_label_, "");
    }

    uint8_t battery_level = 0;
    if (average_adc < 1970) {
        battery_level = 0;
        // 显示电量过低提示窗口
        ShowLowBatteryPopup();
    } else if (average_adc >= 1970 && average_adc < 2100) {
        battery_level = 1;
        // 如果电量回升，隐藏提示窗口
        if (low_battery_popup_ != nullptr) {
            lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (average_adc >= 2100 && average_adc < 2200) {
        battery_level = 2;
        if (low_battery_popup_ != nullptr) {
            lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (average_adc >= 2200 && average_adc < 2300) {
        battery_level = 3;
        if (low_battery_popup_ != nullptr) {
            lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        battery_level = 4;
        if (low_battery_popup_ != nullptr) {
            lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    const char* battery_icon;
    switch (battery_level) {
        case 0:
            battery_icon = FONT_AWESOME_BATTERY_EMPTY;
            break;
        case 1:
            battery_icon = FONT_AWESOME_BATTERY_1;
            break;
        case 2:
            battery_icon = FONT_AWESOME_BATTERY_2;
            break;
        case 3:
            battery_icon = FONT_AWESOME_BATTERY_3;
            break;
        case 4:
            battery_icon = FONT_AWESOME_BATTERY_FULL;
            break;
        default:
            battery_icon = FONT_AWESOME_BATTERY_SLASH;
            break;
    }

    if (battery_label_ != nullptr) {
        lv_obj_set_style_text_font(battery_label_, fonts_.icon_font, 0);
        lv_label_set_text(battery_label_, battery_icon);
    }
}

// lcd_display.c
void XINGZHI_1_54_TFT_LcdDisplay::ShowLowBatteryPopup() {
    DisplayLockGuard lock(this);
    if (low_battery_popup_ == nullptr) {
        low_battery_popup_ = lv_obj_create(lv_scr_act());
        lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, LV_VER_RES * 0.5);
        lv_obj_center(low_battery_popup_);
        lv_obj_set_style_bg_color(low_battery_popup_, lv_color_black(), 0);
        lv_obj_set_style_radius(low_battery_popup_, 10, 0);

        lv_obj_t* label = lv_label_create(low_battery_popup_);
        lv_label_set_text(label, "电量过低，请充电");
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_center(label);
    }
    lv_obj_clear_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
}

uint16_t XINGZHI_1_54_TFT_LcdDisplay::ReadBatteryLevel() {
    adc_oneshot_unit_handle_t adc_handle;
    adc_oneshot_unit_init_cfg_t init_config = {
       .unit_id = ADC_UNIT_2,
       .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    // 初始化 ADC 单元
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));
    adc_oneshot_chan_cfg_t chan_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    // 配置 ADC 通道
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, ADC_CHANNEL_6, &chan_config));
    int adc_value;
    // 读取 ADC 值
    ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, ADC_CHANNEL_6, &adc_value));
    adc_oneshot_del_unit(adc_handle);
    return adc_value;
}

void XINGZHI_1_54_TFT_LcdDisplay::ChargingTimerCallback(void* arg) {
    XINGZHI_1_54_TFT_LcdDisplay* display = static_cast<XINGZHI_1_54_TFT_LcdDisplay*>(arg);
    DisplayLockGuard lock(display);

    // 检查充电状态
    int charging_level = gpio_get_level(display->charging_pin_);
    bool is_charging = (charging_level == 1);
    display->OnStateChanged(); // 检测当前对对话状态
    // 检查电池是否充满，adc值超过2430，判定为充满
    bool is_battery_full = 0;
    if (display->average_adc > 2430)
    {
        is_battery_full = 1;
    } 
    if (is_charging) {
        // 正在充电，更新交互时间，防止进入睡眠
        display->UpdateInteractionTime();
        if (is_battery_full) {
            if (display->charging_label_ != nullptr) {
                lv_label_set_text(display->charging_label_, "");
            }
            if (display->battery_label_ != nullptr) {
                lv_obj_set_style_text_font(display->battery_label_, display->fonts_.icon_font, 0);
                lv_label_set_text(display->battery_label_, FONT_AWESOME_BATTERY_FULL);
            }
        } else {
            if (display->charging_label_ != nullptr) {
                lv_obj_set_style_text_font(display->charging_label_, display->fonts_.icon_font, 0);
                lv_label_set_text(display->charging_label_, FONT_AWESOME_BATTERY_CHARGING);
            }
            if (display->battery_label_ != nullptr) {
                lv_label_set_text(display->battery_label_, "");
            }
        }
        // 如果正在充电，隐藏电量过低提示窗口
        if (display->low_battery_popup_ != nullptr) {
            lv_obj_add_flag(display->low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
        }
        display->was_charging = true; // 更新上一次的充电状态为正在充电
    } else {
        if (display->was_charging) {
            // 充电状态从充电变为未充电，立即读取并更新电池电量
            display->average_adc = display->ReadBatteryLevel();
        } else {
            // 一直处于未充电状态，正常显示电池图标
            if (display->charging_label_ != nullptr) {
                if (!display->first_battery_invert_) {
                    display->average_adc = display->ReadBatteryLevel();
                }
                display->UpdateBatteryAndChargingDisplay(display->average_adc);
                display->adc_values.clear();
                display->adc_count = 0;
            }
        }
        display->was_charging = false; // 更新上一次的充电状态为未充电
    }
    // 检查睡眠状态
    display->CheckSleepState();
}

void XINGZHI_1_54_TFT_LcdDisplay::BatteryTimerCallback(void* arg) {
    XINGZHI_1_54_TFT_LcdDisplay* display = static_cast<XINGZHI_1_54_TFT_LcdDisplay*>(arg);
    uint16_t adc_value = display->ReadBatteryLevel();
    if (display->first_battery_invert_) {
        display->adc_samp_interval = 180000000;  // adc值采样的时间间隔
        // 停止当前定时器
        esp_timer_stop(display->battery_timer_);
        // 重新启动定时器，使用新的时间间隔
        ESP_ERROR_CHECK(esp_timer_start_periodic(display->battery_timer_, display->adc_samp_interval));
    }
    ESP_LOGI(TAG, "adc_samp_interval: %" PRId32 "", display->adc_samp_interval);
    ESP_LOGI(TAG, "Value of first_battery_invert_ before condition: %d", display->first_battery_invert_);
    display->adc_values.push_back(adc_value);
    display->adc_count++;

    if (display->adc_count >= 1) {
        uint32_t sum = 0;
        for (uint16_t value : display->adc_values) {
            sum += value;
        }
        display->average_adc = sum / display->adc_values.size();
        display->first_battery_invert_ = true; 
    }
}

void XINGZHI_1_54_TFT_LcdDisplay::StartChargingTimer() {
    ESP_ERROR_CHECK(esp_timer_start_periodic(charging_timer_, adc_samp_interval));
}

void XINGZHI_1_54_TFT_LcdDisplay::StartBatteryTimer() {
    ESP_ERROR_CHECK(esp_timer_start_periodic(battery_timer_, adc_samp_interval));
}

void XINGZHI_1_54_TFT_LcdDisplay::SetEmotion(const char* emotion) {
    struct Emotion {
        const char* icon;
        const char* text;
    };

    static const std::vector<Emotion> emotions = {
        {"😶", "neutral"},
        {"🙂", "happy"},
        {"😆", "laughing"},
        {"😂", "funny"},
        {"😔", "sad"},
        {"😠", "angry"},
        {"😭", "crying"},
        {"😍", "loving"},
        {"😳", "embarrassed"},
        {"😯", "surprised"},
        {"😱", "shocked"},
        {"🤔", "thinking"},
        {"😉", "winking"},
        {"😎", "cool"},
        {"😌", "relaxed"},
        {"🤤", "delicious"},
        {"😘", "kissy"},
        {"😏", "confident"},
        {"😴", "sleepy"},
        {"😜", "silly"},
        {"🙄", "confused"}
    };
    
    // 查找匹配的表情
    std::string_view emotion_view(emotion);
    auto it = std::find_if(emotions.begin(), emotions.end(),
        [&emotion_view](const Emotion& e) { return e.text == emotion_view; });

    DisplayLockGuard lock(this);
    if (emotion_label_ == nullptr) {
        return;
    }

    // 如果找到匹配的表情就显示对应图标，否则显示默认的neutral表情
    lv_obj_set_style_text_font(emotion_label_, fonts_.emoji_font, 0);
    if (it != emotions.end()) {
        lv_label_set_text(emotion_label_, it->icon);
    } else {
        lv_label_set_text(emotion_label_, "😶");
    }
}

void XINGZHI_1_54_TFT_LcdDisplay::SetIcon(const char* icon) {
    DisplayLockGuard lock(this);
    if (emotion_label_ == nullptr) {
        return;
    }
    lv_obj_set_style_text_font(emotion_label_, &font_awesome_30_4, 0);
    lv_label_set_text(emotion_label_, icon);
}

void XINGZHI_1_54_TFT_LcdDisplay::OnStateChanged() {
    // 原小智逻辑：非 Idle 状态（对话中）时刷新交互时间以阻止休眠。
    // 纯 LoveTV 无对话状态机，休眠仅由按键交互和充电状态驱动，此处保持空实现。
}

uint8_t XINGZHI_1_54_TFT_LcdDisplay::GetBatteryLevel() const {
    if (average_adc < 1970) {
        return 0;
    }
    if (average_adc < 2100) {
        return 1;
    }
    if (average_adc < 2200) {
        return 2;
    }
    if (average_adc < 2300) {
        return 3;
    }
    return 4;
}

bool XINGZHI_1_54_TFT_LcdDisplay::IsCharging() const {
    return gpio_get_level(charging_pin_) == 1;
}
