#include "lovetv.h"

#include "board.h"
#include "lovetv_voice.h"
#include "settings.h"
#include "font_awesome_symbols.h"
#include "wifi_station.h"
#include <esp_wifi.h>

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <esp_sntp.h>
#include <esp_system.h>
#include <sys/time.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>

#define TAG "LoveTv"

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_awesome_16_4);
LV_FONT_DECLARE(font_awesome_30_4);
LV_FONT_DECLARE(lv_font_montserrat_24);
LV_FONT_DECLARE(lv_font_montserrat_36);
LV_FONT_DECLARE(lv_font_montserrat_48);
// 32px 中文子集，只含"芜湖起飞！"，用于消息页大标题（main/lovetv/font_lovetv_title_32.c）
LV_FONT_DECLARE(font_lovetv_title_32);

namespace {

// 粉白主题：浅粉渐变底 + 白色卡片 + 深粉文字，统一设计语言
const lv_color_t kColorBgTop = lv_color_hex(0xFFC9DA);     // 底色上：浅粉白
const lv_color_t kColorBgBottom = lv_color_hex(0xF79CBB);  // 底色下：粉
const lv_color_t kColorCard = lv_color_hex(0xFFFFFF);      // 卡片：白
const lv_color_t kColorCardEdge = lv_color_hex(0xFFD8E4);  // 卡片描边：浅粉
const lv_color_t kColorSub = lv_color_hex(0xE88FB0);       // 卡片内次要文字：浅粉
const lv_color_t kColorTrack = lv_color_hex(0xF5B8CC);     // 进度条底槽：浅粉
const lv_color_t kColorValue = lv_color_hex(0xD6457A);     // 卡片主文字/数值：深粉
const lv_color_t kColorRed = lv_color_hex(0x8A0025);       // 失败提示
const lv_color_t kColorOnBg = lv_color_hex(0xC24B77);      // 直接压在粉底上的文字：深粉

// banner 轮播条数（最后一条是本机 IP，用于访问配置页）
constexpr int kBannerCount = 8;

// 页面底色：浅粉到粉的上下渐变
void ApplyPageBg(lv_obj_t* page) {
    lv_obj_set_style_bg_color(page, kColorBgTop, 0);
    lv_obj_set_style_bg_grad_color(page, kColorBgBottom, 0);
    lv_obj_set_style_bg_grad_dir(page, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
}

// 半透明白色卡片：圆角 + 浅粉描边，营造分层感
lv_obj_t* CreateCard(lv_obj_t* parent, int x, int y, int w, int h, int radius) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, kColorCard, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_80, 0);
    lv_obj_set_style_radius(card, radius, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, kColorCardEdge, 0);
    lv_obj_set_style_border_opa(card, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

// 卡片内的小标题（左上角）
lv_obj_t* CardTitle(lv_obj_t* card, int x, int y, const char* text,
                    const lv_font_t* font) {
    lv_obj_t* label = lv_label_create(card);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, kColorSub, 0);
    lv_label_set_text(label, text);
    return label;
}

// 卡片底部的白色进度条
lv_obj_t* CreateMiniBar(lv_obj_t* card, int x, int y, int w, int h) {
    lv_obj_t* bar = lv_bar_create(card);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, x, y);
    lv_obj_set_size(bar, w, h);
    lv_obj_set_style_radius(bar, h / 2, 0);
    lv_obj_set_style_bg_color(bar, kColorTrack, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar, h / 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, kColorValue, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    return bar;
}

std::string ExtractBetween(const std::string& s, const std::string& start,
                           const std::string& end) {
    auto p = s.find(start);
    if (p == std::string::npos) {
        return "";
    }
    p += start.size();
    auto q = s.find(end, p);
    if (q == std::string::npos) {
        q = s.size();
    }
    return s.substr(p, q - p);
}

std::string JsonString(cJSON* obj, const char* key) {
    auto* item = obj != nullptr ? cJSON_GetObjectItem(obj, key) : nullptr;
    return (item != nullptr && cJSON_IsString(item)) ? item->valuestring : "";
}

int JsonInt(cJSON* obj, const char* key, int default_value) {
    auto* item = obj != nullptr ? cJSON_GetObjectItem(obj, key) : nullptr;
    return (item != nullptr && cJSON_IsNumber(item)) ? item->valueint : default_value;
}

// 把 UTC 的 tm 转成 Unix 时间戳。
// newlib 未导出 timegm，且 mktime 会按本机时区(CST-8)解释，因此自己算。
// 用 Howard Hinnant 的 days_from_civil 算法，纯整数运算、无时区依赖。
int64_t TimegmPortable(const struct tm& t) {
    int y = t.tm_year + 1900;
    int m = t.tm_mon + 1;
    int d = t.tm_mday;

    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);          // [0, 399]
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;         // [0, 146096]
    const int64_t days =
        static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;

    return days * 86400 + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
}

// 部分接口返回的 JSON 片段末尾可能残留多余的 "}"，这里做容错解析
cJSON* ParseLenient(const std::string& s) {
    std::string text = s;
    for (int i = 0; i < 8; ++i) {
        cJSON* root = cJSON_Parse(text.c_str());
        if (root != nullptr) {
            return root;
        }
        if (!text.empty() && (text.back() == '}' || text.back() == ']')) {
            text.pop_back();
        } else {
            break;
        }
    }
    return nullptr;
}

}  // namespace

LoveTv& LoveTv::GetInstance() {
    static LoveTv instance;
    return instance;
}

void LoveTv::LoadConfig() {
    Settings settings("lovetv");
    anniv_year_ = settings.GetInt("year", 2019);
    anniv_month_ = settings.GetInt("month", 9);
    anniv_day_ = settings.GetInt("day", 1);
    bottom_text_ = settings.GetString("bottom", "known since 2019.9.1");
    sweet_words_ = settings.GetString("words", "Happy Every Day");
    city_code_ = settings.GetString("city", "101120501");  // 默认烟台
}

void LoveTv::Start(Display* display) {
    if (started_) {
        return;
    }
    started_ = true;
    display_ = display;

    ESP_LOGI(TAG, "Starting LoveTV interface");

    LoadConfig();
    ESP_LOGI(TAG, "Config: city=%s anniv=%04d-%02d-%02d",
             city_code_.c_str(), anniv_year_, anniv_month_, anniv_day_);

    setenv("TZ", "CST-8", 1);
    tzset();

    {
        DisplayLockGuard lock(display_);

        screen_ = lv_obj_create(nullptr);
        lv_obj_remove_style_all(screen_);
        lv_obj_set_size(screen_, display_->width(), display_->height());
        ApplyPageBg(screen_);
        lv_obj_set_style_pad_all(screen_, 0, 0);

        CreateClockPage();
        CreateLovePage();
        CreateMissuPage();

        ShowPage(Page::kClock);
        lv_screen_load(screen_);
        active_ = true;
    }

    lv_timer_t* timer = lv_timer_create(&LoveTv::OnTimer, 1000, this);
    lv_timer_ready(timer);

    // 授时独立于天气任务启动：即使天气接口不可用，时间也要能出来
    StartSntp();

    // 天气任务需要跑 HTTP + JSON 解析，栈给足（8KB 曾导致溢出重启）
    BaseType_t created = xTaskCreate(&LoveTv::WeatherTask, "lovetv_weather",
                                     1024 * 14, this, 3, nullptr);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create weather task! free internal heap=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    } else {
        ESP_LOGI(TAG, "Weather task created, free internal heap=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
}

void LoveTv::ReloadConfig() {
    if (!started_) {
        return;
    }
    LoadConfig();
    ESP_LOGI(TAG, "Config reloaded: city=%s anniv=%04d-%02d-%02d",
             city_code_.c_str(), anniv_year_, anniv_month_, anniv_day_);

    {
        DisplayLockGuard lock(display_);
        if (words_label_ != nullptr) {
            lv_label_set_text(words_label_, sweet_words_.c_str());
        }
        if (bottom_label_ != nullptr) {
            lv_label_set_text(bottom_label_, bottom_text_.c_str());
        }
    }
    love_days_ = -1;          // 强制重算天数
    weather_refresh_ = true;  // 让天气任务立刻重新拉取
}

bool LoveTv::IsTimeValid() {
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    // 1970 起算，tm_year >= 120 即 2020 年之后，视为已授时
    return timeinfo.tm_year >= 120;
}

void LoveTv::StartSntp() {
    if (sntp_started_) {
        return;
    }
    // 多个服务器：单一服务器被墙/被路由器拦截时仍有机会
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        3, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "cn.pool.ntp.org", "pool.ntp.org"));
    config.start = true;
    config.server_from_dhcp = false;
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_sntp_init failed: %s", esp_err_to_name(err));
        return;
    }
    sntp_started_ = true;
    ESP_LOGI(TAG, "SNTP started (ntp.aliyun.com / cn.pool.ntp.org / pool.ntp.org)");

    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
    if (err == ESP_OK) {
        time_t now = time(nullptr);
        struct tm t;
        localtime_r(&now, &t);
        ESP_LOGI(TAG, "SNTP synced: %04d-%02d-%02d %02d:%02d:%02d",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
    } else {
        ESP_LOGW(TAG, "SNTP not synced within 10s (%s); will fall back to HTTP Date",
                 esp_err_to_name(err));
    }
}

bool LoveTv::SyncTimeFromHttpDate(Http* http) {
    if (http == nullptr) {
        return false;
    }
    std::string date = http->GetResponseHeader("Date");
    if (date.empty()) {
        return false;
    }
    // RFC1123，例如 "Sat, 09 Aug 2025 08:13:42 GMT"，始终是 GMT
    struct tm tm_gmt = {};
    const char* rest = strptime(date.c_str(), "%a, %d %b %Y %H:%M:%S", &tm_gmt);
    if (rest == nullptr) {
        ESP_LOGW(TAG, "Unrecognized Date header: %s", date.c_str());
        return false;
    }
    int64_t utc = TimegmPortable(tm_gmt);
    if (utc <= 0) {
        return false;
    }
    struct timeval tv = { .tv_sec = static_cast<time_t>(utc), .tv_usec = 0 };
    settimeofday(&tv, nullptr);

    time_t t = static_cast<time_t>(utc);
    struct tm local;
    localtime_r(&t, &local);
    ESP_LOGI(TAG, "Time set from HTTP Date: %04d-%02d-%02d %02d:%02d:%02d (local)",
             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
             local.tm_hour, local.tm_min, local.tm_sec);
    return true;
}

void LoveTv::Show() {
    if (!started_) {
        return;
    }
    DisplayLockGuard lock(display_);
    ShowPage(page_);
    lv_screen_load(screen_);
    active_ = true;
    ESP_LOGI(TAG, "LoveTV screen shown");
}

void LoveTv::GoToPage(Page page) {
    DisplayLockGuard lock(display_);
    page_ = page;
    ShowPage(page_);
}

// BOOT 单击：主界面 <-> 相识页；在消息页时直接回主界面
void LoveTv::OnBootClick() {
    if (!started_ || !active_) {
        return;
    }
    Page target;
    switch (page_) {
        case Page::kClock:
            target = Page::kLove;
            break;
        case Page::kLove:
            target = Page::kClock;
            break;
        default:  // 消息页
            role_ = Role::kNone;
            target = Page::kClock;
            break;
    }
    GoToPage(target);
    ESP_LOGI(TAG, "BOOT click -> page %d", static_cast<int>(target));
}

// VOL+ 单击：不在消息页时发送；已在消息页时作为确认并返回
void LoveTv::OnSendClick() {
    if (!started_ || !active_) {
        return;
    }

    // 已在消息页：本次单击是"收到/退出"确认
    if (page_ == Page::kMissu) {
        if (role_ == Role::kReceiver) {
            // 接收方确认，回一条 check 让对方看到"起飞！"
            bool ok = sender_ ? sender_("check") : false;
            ESP_LOGI(TAG, "Ack send: %s", ok ? "ok" : "failed");
        }
        role_ = Role::kNone;
        GoToPage(pre_page_);
        ESP_LOGI(TAG, "Leave message page, back to %d", static_cast<int>(pre_page_));
        return;
    }

    // 不在消息页：发送一条消息
    pre_page_ = page_;
    role_ = Role::kSender;

    bool ok = sender_ ? sender_("imu") : false;
    {
        DisplayLockGuard lock(display_);
        if (missu_icon_ != nullptr) {
            // 发出去了用对话气泡，发失败用警告
            lv_label_set_text(missu_icon_,
                              ok ? FONT_AWESOME_COMMENT : FONT_AWESOME_WARNING);
        }
        if (missu_title_ != nullptr) {
            lv_obj_set_style_text_font(missu_title_, &font_lovetv_title_32, 0);
            lv_label_set_text(missu_title_, "芜湖！");
        }
        if (missu_hint_ != nullptr) {
            lv_label_set_text(missu_hint_, "（单击 VOL+ 返回）");
        }
    }
    ShowMessage(ok ? "已发送：芜湖！" : "发送失败，检查网络", ok);
    ESP_LOGI(TAG, "Send 芜湖: %s", ok ? "ok" : "failed");
}

void LoveTv::ShowMessage(const std::string& message, bool success) {
    if (!started_) {
        return;
    }
    DisplayLockGuard lock(display_);
    if (missu_message_ != nullptr) {
        lv_obj_set_style_text_color(missu_message_,
                                    success ? kColorValue : kColorRed, 0);
        lv_label_set_text(missu_message_, message.c_str());
    }
    page_ = Page::kMissu;
    ShowPage(page_);
}

// 语音对讲页：标题固定“语音对讲”，提示区分于文字消息
void LoveTv::ShowVoiceMessage(const std::string& message, bool success) {
    if (!started_) {
        return;
    }
    DisplayLockGuard lock(display_);
    if (missu_message_ != nullptr) {
        lv_obj_set_style_text_color(missu_message_,
                                    success ? kColorValue : kColorRed, 0);
        lv_label_set_text(missu_message_, message.c_str());
    }
    if (missu_title_ != nullptr) {
        // 语音标题用通用 16px 字体（32px 子集字体只有“芜湖起飞！”）
        lv_obj_set_style_text_font(missu_title_, &font_puhui_16_4, 0);
        lv_label_set_text(missu_title_, "语音对讲");
    }
    if (missu_hint_ != nullptr) {
        lv_label_set_text(missu_hint_, "（再按 VOL- 结束 / VOL+ 返回）");
    }
    page_ = Page::kMissu;
    ShowPage(page_);
}

// 收到对方的“芜湖！”
void LoveTv::OnPeerMissYou() {
    if (!started_) {
        return;
    }
    if (page_ != Page::kMissu) {
        pre_page_ = page_;
    }
    role_ = Role::kReceiver;
    {
        DisplayLockGuard lock(display_);
        if (missu_icon_ != nullptr) {
            // 对方喊你，用铃铛表示"有人叫你"
            lv_label_set_text(missu_icon_, FONT_AWESOME_BELL);
        }
        if (missu_title_ != nullptr) {
            lv_obj_set_style_text_font(missu_title_, &font_lovetv_title_32, 0);
            lv_label_set_text(missu_title_, "芜湖！");
        }
        if (missu_hint_ != nullptr) {
            lv_label_set_text(missu_hint_, "（单击 VOL+ 回复起飞）");
        }
    }
    ShowMessage("对方喊你：芜湖！", true);
    ESP_LOGI(TAG, "Peer 芜湖 received");
}

// 对方确认收到，本机显示“起飞！”
void LoveTv::OnPeerAck() {
    if (!started_) {
        return;
    }
    if (page_ != Page::kMissu) {
        pre_page_ = page_;
    }
    role_ = Role::kNone;
    {
        DisplayLockGuard lock(display_);
        if (missu_icon_ != nullptr) {
            // 对方确认收到，用对勾
            lv_label_set_text(missu_icon_, FONT_AWESOME_CHECK);
        }
        if (missu_title_ != nullptr) {
            lv_obj_set_style_text_font(missu_title_, &font_lovetv_title_32, 0);
            lv_label_set_text(missu_title_, "起飞！");
        }
        if (missu_hint_ != nullptr) {
            lv_label_set_text(missu_hint_, "（单击 VOL+ 返回）");
        }
    }
    ShowMessage("对方已收到", true);
    ESP_LOGI(TAG, "Peer ack received -> 起飞");
}

void LoveTv::CreateClockPage() {
    lv_obj_t* page = lv_obj_create(screen_);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, display_->width(), display_->height());
    lv_obj_set_pos(page, 0, 0);
    ApplyPageBg(page);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    pages_[0] = page;

    // 状态条卡片：日期 + 电量
    lv_obj_t* status_card = CreateCard(page, 6, 4, 228, 28, 14);
    date_label_ = lv_label_create(status_card);
    lv_obj_set_pos(date_label_, 12, 4);
    lv_obj_set_width(date_label_, 178);
    lv_obj_set_style_text_font(date_label_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(date_label_, kColorValue, 0);
    lv_label_set_text(date_label_, "正在同步时间...");

    battery_label_ = lv_label_create(status_card);
    lv_obj_set_pos(battery_label_, 198, 4);
    lv_obj_set_style_text_font(battery_label_, &font_awesome_16_4, 0);
    lv_obj_set_style_text_color(battery_label_, kColorValue, 0);
    lv_label_set_text(battery_label_, "");

    // 时间大卡：时分居中，秒数缩在右下角
    lv_obj_t* time_card = CreateCard(page, 6, 38, 228, 72, 16);
    time_label_ = lv_label_create(time_card);
    lv_obj_set_pos(time_label_, 0, 10);
    lv_obj_set_width(time_label_, 228);
    lv_obj_set_style_text_font(time_label_, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(time_label_, kColorValue, 0);
    lv_obj_set_style_text_align(time_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(time_label_, "--:--");

    seconds_label_ = lv_label_create(time_card);
    lv_obj_set_pos(seconds_label_, 188, 38);
    lv_obj_set_style_text_font(seconds_label_, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(seconds_label_, kColorSub, 0);
    lv_label_set_text(seconds_label_, "");

    // 温度卡 / 湿度卡：单位写进小标题，数值只放数字，
    // 这样数值宽度变化（-5 / 26 / 100）不会挤到单位符号
    lv_obj_t* temp_card = CreateCard(page, 6, 116, 110, 70, 14);
    CardTitle(temp_card, 10, 5, "温度 ℃", &font_puhui_16_4);
    temp_value_ = lv_label_create(temp_card);
    lv_obj_set_pos(temp_value_, 8, 30);
    lv_obj_set_style_text_font(temp_value_, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(temp_value_, kColorValue, 0);
    lv_label_set_text(temp_value_, "--");
    temp_bar_ = CreateMiniBar(temp_card, 8, 61, 92, 6);

    // 天气状况卡：显示“晴/多云/雨”等文字
    lv_obj_t* weather_card = CreateCard(page, 124, 116, 110, 70, 14);
    CardTitle(weather_card, 10, 5, "天气", &font_puhui_16_4);
    weather_value_ = lv_label_create(weather_card);
    lv_obj_set_pos(weather_value_, 8, 24);
    lv_obj_set_width(weather_value_, 94);
    // 天气状况是中文（晴/多云/雨），必须用中文字体，否则显示为方框
    lv_obj_set_style_text_font(weather_value_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(weather_value_, kColorValue, 0);
    lv_obj_set_style_text_align(weather_value_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(weather_value_, LV_LABEL_LONG_DOT);
    lv_label_set_text(weather_value_, "--");

    lv_obj_t* banner_card = CreateCard(page, 6, 192, 228, 36, 14);
    banner_label_ = lv_label_create(banner_card);
    lv_obj_set_pos(banner_label_, 10, 8);
    lv_obj_set_width(banner_label_, 208);
    lv_obj_set_style_text_font(banner_label_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(banner_label_, kColorValue, 0);
    lv_label_set_long_mode(banner_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(banner_label_, "正在获取天气...");

    banner_texts_[0] = "实时天气 --";
    banner_texts_[1] = "空气质量 --";
    banner_texts_[2] = "风向 --";
    banner_texts_[3] = "今日 --";
    banner_texts_[4] = "最低温度 --℃";
    banner_texts_[5] = "最高温度 --℃";
    banner_texts_[6] = "湿度 --";
    banner_texts_[7] = "配置页 http://--";
}

void LoveTv::CreateLovePage() {
    lv_obj_t* page = lv_obj_create(screen_);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, display_->width(), display_->height());
    lv_obj_set_pos(page, 0, 0);
    ApplyPageBg(page);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    pages_[1] = page;

    // 天数大卡：「相识」在左上，天数居中大字，「天」在右下
    lv_obj_t* day_card = CreateCard(page, 6, 44, 228, 104, 18);
    CardTitle(day_card, 16, 10, "相识", &font_puhui_16_4);

    day_label_ = lv_label_create(day_card);
    lv_obj_set_pos(day_label_, 0, 32);
    lv_obj_set_width(day_label_, 228);
    lv_obj_set_style_text_font(day_label_, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(day_label_, kColorValue, 0);
    lv_obj_set_style_text_align(day_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(day_label_, "--");

    CardTitle(day_card, 196, 78, "天", &font_puhui_16_4);

    // 一句话单独一张卡
    lv_obj_t* words_card = CreateCard(page, 6, 156, 228, 42, 14);
    words_label_ = lv_label_create(words_card);
    lv_obj_set_pos(words_label_, 8, 11);
    lv_obj_set_width(words_label_, 212);
    lv_obj_set_style_text_font(words_label_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(words_label_, kColorValue, 0);
    lv_obj_set_style_text_align(words_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(words_label_, LV_LABEL_LONG_DOT);
    lv_label_set_text(words_label_, sweet_words_.c_str());

    // 底部文字直接压在底色上，用深色保证可读
    bottom_label_ = lv_label_create(page);
    lv_obj_set_pos(bottom_label_, 0, 208);
    lv_obj_set_width(bottom_label_, display_->width());
    lv_obj_set_style_text_font(bottom_label_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(bottom_label_, kColorOnBg, 0);
    lv_obj_set_style_text_align(bottom_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(bottom_label_, bottom_text_.c_str());
}

void LoveTv::CreateMissuPage() {
    lv_obj_t* page = lv_obj_create(screen_);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, display_->width(), display_->height());
    lv_obj_set_pos(page, 0, 0);
    ApplyPageBg(page);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    pages_[2] = page;

    // 顶部圆形图标：发送 COMMENT / 收到 BELL / 确认 CHECK
    lv_obj_t* icon_circle = CreateCard(page, 88, 26, 64, 64, 32);
    missu_icon_ = lv_label_create(icon_circle);
    lv_obj_set_style_text_font(missu_icon_, &font_awesome_30_4, 0);
    lv_obj_set_style_text_color(missu_icon_, kColorValue, 0);
    lv_label_set_text(missu_icon_, FONT_AWESOME_COMMENT);
    lv_obj_center(missu_icon_);

    // 32px 中文标题用专门生成的字体子集（只含 芜湖起飞！六个码点）
    missu_title_ = lv_label_create(page);
    lv_obj_set_pos(missu_title_, 0, 104);
    lv_obj_set_width(missu_title_, display_->width());
    lv_obj_set_style_text_font(missu_title_, &font_lovetv_title_32, 0);
    lv_obj_set_style_text_color(missu_title_, kColorOnBg, 0);
    lv_obj_set_style_text_align(missu_title_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(missu_title_, "芜湖！");

    // 正文压在卡片上，比直接放粉底更清晰
    lv_obj_t* msg_card = CreateCard(page, 16, 144, 208, 52, 14);
    missu_message_ = lv_label_create(msg_card);
    lv_obj_set_pos(missu_message_, 8, 6);
    lv_obj_set_width(missu_message_, 192);
    lv_obj_set_style_text_font(missu_message_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(missu_message_, kColorValue, 0);
    lv_obj_set_style_text_align(missu_message_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(missu_message_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(missu_message_, "——");

    missu_hint_ = lv_label_create(page);
    lv_obj_set_pos(missu_hint_, 0, 206);
    lv_obj_set_width(missu_hint_, display_->width());
    lv_obj_set_style_text_font(missu_hint_, &font_puhui_16_4, 0);
    lv_obj_set_style_text_color(missu_hint_, kColorOnBg, 0);
    lv_obj_set_style_text_align(missu_hint_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(missu_hint_, "（单击 VOL+ 返回）");
}

void LoveTv::ShowPage(Page page) {
    for (int i = 0; i < 3; ++i) {
        if (pages_[i] != nullptr) {
            lv_obj_add_flag(pages_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (pages_[static_cast<int>(page)] != nullptr) {
        lv_obj_remove_flag(pages_[static_cast<int>(page)], LV_OBJ_FLAG_HIDDEN);
    }
    switch (page) {
        case Page::kClock:
            UpdateClock();
            break;
        case Page::kLove:
            UpdateLovePage();
            break;
        case Page::kMissu:
            break;
    }
}

void LoveTv::OnTimer(lv_timer_t* timer) {
    auto* self = static_cast<LoveTv*>(lv_timer_get_user_data(timer));
    if (self != nullptr) {
        self->OnTick();
    }
}

void LoveTv::OnTick() {
    if (!active_) {
        return;
    }
    switch (page_) {
        case Page::kClock:
            UpdateClock();
            break;
        case Page::kLove:
            UpdateLovePage();
            break;
        case Page::kMissu:
            break;
    }
}

void LoveTv::UpdateClock() {
    if (time_label_ == nullptr) {
        return;
    }

    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    bool synced = esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED ||
                  timeinfo.tm_year >= 120;
    if (!synced) {
        lv_label_set_text(time_label_, "--:--");
        lv_label_set_text(seconds_label_, "");
        if (WifiStation::GetInstance().IsConnected()) {
            lv_label_set_text(date_label_, "正在同步时间...");
        } else {
            lv_label_set_text(date_label_, "WiFi 未连接");
        }
        return;
    }

    static const char* weeks[] = {"日", "一", "二", "三", "四", "五", "六"};
    char buf[48];
    snprintf(buf, sizeof(buf), "%d年%d月%d日 周%s", timeinfo.tm_year + 1900,
             timeinfo.tm_mon + 1, timeinfo.tm_mday, weeks[timeinfo.tm_wday]);
    lv_label_set_text(date_label_, buf);

    snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    lv_label_set_text(time_label_, buf);

    snprintf(buf, sizeof(buf), "%02d", timeinfo.tm_sec);
    lv_label_set_text(seconds_label_, buf);

    UpdateBattery();

    if (++banner_tick_ % 3 == 0) {
        UpdateBanner();
    }
}

void LoveTv::UpdateBattery() {
    if (battery_label_ == nullptr) {
        return;
    }
    int level = 0;
    bool charging = false;
    if (!Board::GetInstance().GetBatteryLevel(level, charging)) {
        lv_label_set_text(battery_label_, "");
        return;
    }
    const char* icon = FONT_AWESOME_BATTERY_EMPTY;
    if (charging) {
        icon = FONT_AWESOME_BATTERY_CHARGING;
    } else if (level >= 4) {
        icon = FONT_AWESOME_BATTERY_FULL;
    } else if (level == 3) {
        icon = FONT_AWESOME_BATTERY_3;
    } else if (level == 2) {
        icon = FONT_AWESOME_BATTERY_2;
    } else if (level == 1) {
        icon = FONT_AWESOME_BATTERY_1;
    }
    lv_label_set_text(battery_label_, icon);
}

void LoveTv::UpdateBanner() {
    if (banner_label_ == nullptr) {
        return;
    }
    // 最后一条实时刷新为本机 IP，方便手机访问配置页
    if (banner_index_ == kBannerCount - 1) {
        auto& wifi = WifiStation::GetInstance();
        if (wifi.IsConnected()) {
            banner_texts_[kBannerCount - 1] = "配置页 http://" + wifi.GetIpAddress();
        } else {
            banner_texts_[kBannerCount - 1] = "WiFi 未连接";
        }
    }
    lv_label_set_text(banner_label_, banner_texts_[banner_index_].c_str());
    banner_index_ = (banner_index_ + 1) % kBannerCount;
}

void LoveTv::UpdateLovePage() {
    if (day_label_ == nullptr) {
        return;
    }
    struct tm start_tm = {};
    start_tm.tm_year = anniv_year_ - 1900;
    start_tm.tm_mon = anniv_month_ - 1;
    start_tm.tm_mday = anniv_day_;
    time_t start_time = mktime(&start_tm);

    int days = static_cast<int>((time(nullptr) - start_time) / 86400) + 1;
    if (days < 1) {
        days = 1;
    }
    if (days != love_days_) {
        love_days_ = days;
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", days);
        lv_label_set_text(day_label_, buf);
    }
}

void LoveTv::WeatherTask(void* arg) {
    auto* self = static_cast<LoveTv*>(arg);
    ESP_LOGI(TAG, "Weather task running");
    while (true) {
        // 等 WiFi 连上再碰网络：网络协议栈未就绪时发起 HTTP 会触发断言崩溃
        if (!WifiStation::GetInstance().IsConnected()) {
            vTaskDelay(pdMS_TO_TICKS(5 * 1000));
            continue;
        }

        // SNTP 若未成功（被拦截/DNS 失败），这里兜底重试一次
        if (!self->sntp_started_) {
            self->StartSntp();
        }

        self->weather_refresh_ = false;
        bool ok = self->FetchWeather();
        ESP_LOGI(TAG, "Weather fetch %s, task stack high water=%u bytes",
                 ok ? "ok" : "failed",
                 (unsigned)(uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)));

        // 成功 5 分钟一次，失败 30 秒重试；配置页改了城市则立即重拉
        int waited_ms = 0;
        const int total_ms = ok ? 5 * 60 * 1000 : 30 * 1000;
        while (waited_ms < total_ms && !self->weather_refresh_) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            waited_ms += 1000;
        }
    }
}

bool LoveTv::FetchWeather() {
    if (city_code_.empty()) {
        ESP_LOGW(TAG, "City code empty, skip weather");
        return false;
    }

    char url[160];
    snprintf(url, sizeof(url),
             "http://d1.weather.com.cn/weather_index/%s.html?_=%lld",
             city_code_.c_str(), static_cast<long long>(time(nullptr)));

    Http* http = Board::GetInstance().CreateHttp();
    if (http == nullptr) {
        return false;
    }
    http->SetHeader("User-Agent",
                    "Mozilla/5.0 (iPhone; CPU iPhone OS 11_0 like Mac OS X) "
                    "AppleWebKit/604.1.38 (KHTML, like Gecko) Version/11.0 "
                    "Mobile/15A372 Safari/604.1");
    http->SetHeader("Referer", "http://www.weather.com.cn/");

    bool opened = http->Open("GET", url);
    if (!opened) {
        ESP_LOGW(TAG, "Weather request failed (city=%s)", city_code_.c_str());
        http->Close();
        delete http;
        return false;
    }

    // SNTP 不通时用响应头的 Date 兜底授时（出厂固件也是靠 HTTP 授时的）
    if (!IsTimeValid()) {
        SyncTimeFromHttpDate(http);
    }

    std::string body = http->GetBody();
    http->Close();
    delete http;

    // 注意：不能用 GetStatusCode() 判断成功——EspHttp 里 status_code_
    // 从未被赋值，恒为 0。Open() 已经校验过 header 和 content-length，
    // 这里只要 body 非空就继续解析。
    ESP_LOGI(TAG, "Weather body %u bytes", (unsigned)body.size());
    if (body.empty()) {
        return false;
    }

    std::string sk_json = ExtractBetween(body, "dataSK =", ";var dataZS");
    std::string dz_json = ExtractBetween(body, "\"weatherinfo\":", "};var alarmDZ");
    std::string fc_json = ExtractBetween(body, "\"f\":[", ",{\"fa");
    if (sk_json.empty()) {
        ESP_LOGW(TAG, "Weather data not found in response (first 80: %.80s)",
                 body.c_str());
        return false;
    }

    cJSON* sk = ParseLenient(sk_json);
    cJSON* dz = dz_json.empty() ? nullptr : ParseLenient(dz_json);
    cJSON* fc = fc_json.empty() ? nullptr : ParseLenient(fc_json);
    if (sk == nullptr) {
        ESP_LOGW(TAG, "Failed to parse dataSK");
        if (dz != nullptr) cJSON_Delete(dz);
        if (fc != nullptr) cJSON_Delete(fc);
        return false;
    }

    std::string temp = JsonString(sk, "temp");
    std::string humidity = JsonString(sk, "SD");
    std::string weather = JsonString(sk, "weather");
    std::string wd = JsonString(sk, "WD");
    std::string ws = JsonString(sk, "WS");
    std::string today_weather = JsonString(dz, "weather");
    std::string low = JsonString(fc, "fd");
    std::string high = JsonString(fc, "fc");

    int aqi = JsonInt(sk, "aqi", 0);
    std::string aqi_text = "优";
    if (aqi > 200) {
        aqi_text = "重度";
    } else if (aqi > 150) {
        aqi_text = "中度";
    } else if (aqi > 100) {
        aqi_text = "轻度";
    } else if (aqi > 50) {
        aqi_text = "良";
    }

    ESP_LOGI(TAG, "Weather: %s %s℃ humidity %s aqi %d",
             weather.c_str(), temp.c_str(), humidity.c_str(), aqi);

    ApplyWeather(temp, humidity, weather, wd, ws, aqi_text,
                 today_weather.empty() ? weather : today_weather,
                 low.empty() ? temp : low, high.empty() ? temp : high);

    cJSON_Delete(sk);
    if (dz != nullptr) cJSON_Delete(dz);
    if (fc != nullptr) cJSON_Delete(fc);
    return true;
}

void LoveTv::ApplyWeather(const std::string& temp, const std::string& humidity,
                          const std::string& weather, const std::string& wd,
                          const std::string& ws, const std::string& aqi_text,
                          const std::string& today_weather,
                          const std::string& low, const std::string& high) {
    DisplayLockGuard lock(display_);

    // 接口返回的温度可能为负，用 strtol 容错解析
    long t = strtol(temp.c_str(), nullptr, 10);

    if (temp_value_ != nullptr) {
        lv_label_set_text(temp_value_, temp.empty() ? "--" : std::to_string(t).c_str());
    }
    if (temp_bar_ != nullptr) {
        // -10~40℃ 映射到 0~100，超界钳位
        long pct = (t + 10) * 2;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        lv_bar_set_value(temp_bar_, static_cast<int32_t>(pct), LV_ANIM_OFF);
    }
    if (weather_value_ != nullptr) {
        lv_label_set_text(weather_value_, weather.empty() ? "--" : weather.c_str());
    }

    banner_texts_[0] = "实时天气 " + weather;
    banner_texts_[1] = "空气质量 " + aqi_text;
    banner_texts_[2] = "风向 " + wd + ws;
    banner_texts_[3] = "今日" + today_weather;
    banner_texts_[4] = "最低温度 " + low + "℃";
    banner_texts_[5] = "最高温度 " + high + "℃";
    std::string hum = humidity;
    if (!hum.empty() && hum.find('%') == std::string::npos) {
        hum += "%";
    }
    banner_texts_[6] = "湿度 " + hum;
    banner_index_ = 0;
    UpdateBanner();
}
void LoveTv::StartVoice() {
    // 弱 WiFi（RSSI -80 左右）下省电模式会让连续语音流丢包/断连，直接关掉
    esp_wifi_set_ps(WIFI_PS_NONE);

    // Opus 编解码器必须在这里（主任务，栈充足）构造完成。
    // 放在所有 return 之前：即使没配 self_id/peer_id 也要预热，
    // 否则首次按 VOL- 会在 esp_timer 任务（栈仅 3584）里创建编解码器而复位。
    VoiceRecorder::GetInstance();
    VoicePlayer::GetInstance();

    // 从 NVS 读取配置：与 MQTT 共用同一命名空间，设备 ID 复用
    Settings settings("lovetv");
    // 默认值与 MQTT 模块一致（A/B）：即使配置页从未保存，语音也能直接工作
    std::string self_id = settings.GetString("self_id", "A");
    std::string peer_id = settings.GetString("peer_id", "B");
    std::string host = settings.GetString("mqtt_host", "117.72.44.85");
    int ws_port = settings.GetInt("ws_port", 9002);

    if (self_id.empty() || peer_id.empty()) {
        ESP_LOGW(TAG, "Voice disabled: self_id/peer_id not configured");
        return;
    }
    if (host.empty()) {
        ESP_LOGW(TAG, "Voice disabled: mqtt_host empty");
        return;
    }

    std::string voice_server = "ws://" + host + ":" + std::to_string(ws_port);

    if (VoiceStreamer::GetInstance().Connect(voice_server, self_id, peer_id)) {
        ESP_LOGI(TAG, "Voice streaming connecting to %s (id=%s peer=%s)",
                 voice_server.c_str(), self_id.c_str(), peer_id.c_str());
    } else {
        ESP_LOGE(TAG, "Failed to connect voice server");
        ShowMessage("语音服务连接失败", false);
    }
}
