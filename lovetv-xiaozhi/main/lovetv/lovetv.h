#ifndef LOVETV_H
#define LOVETV_H

#include <functional>
#include <string>

#include "display.h"

class Http;

// LoveTV 界面模块
//   主界面 = 时钟页（粉底白字）
//   相识页 = 认识天数，BOOT 单击进入 / 再单击返回主界面
//   消息页 = MQTT 收发提示，由 VOL+ 或对方消息触发
class LoveTv {
public:
    enum class Page : uint8_t {
        kClock = 0,   // 主界面
        kLove = 1,    // 相识天数
        kMissu = 2,   // 消息页
    };

    // 消息页的角色：谁发起的这次互动
    enum class Role : uint8_t {
        kNone = 0,
        kSender = 1,    // 本机发出的消息
        kReceiver = 2,  // 收到对方的消息
    };

    static LoveTv& GetInstance();

    void Start(Display* display);

    void Show();
    bool IsActive() const { return active_; }

    // BOOT 单击：主界面 <-> 相识页 来回切换；在消息页时回主界面
    void OnBootClick();

    // VOL+ 单击：不在消息页时发送消息；已在消息页时作为"收到/退出"确认
    //（接收方确认会回一条 check 回执给对方）
    void OnSendClick();

    // 在消息页显示一行文本
    void ShowMessage(const std::string& message, bool success = true);

    // 在消息页显示语音对讲状态（标题/提示与文字消息区分开）
    void ShowVoiceMessage(const std::string& message, bool success = true);

    // 收到对方消息时由 MQTT 模块调用
    void OnPeerMissYou();
    void OnPeerAck();

    // MQTT 模块注入发送实现；返回 false 表示发送失败（未连接等）
    // type 取值："imu"（发消息）/ "check"（回执）
    void SetSender(std::function<bool(const char* type)> sender) {
        sender_ = std::move(sender);
    }

    // 系统时间是否已经有效（SNTP 或 HTTP Date 任一成功）
    static bool IsTimeValid();

    // 配置页保存后调用：重读 NVS 并立即生效（城市变更会触发天气重新拉取）
    void ReloadConfig();

    // 启动语音对讲：读取 NVS 配置并连接 WebSocket 服务器（须网络就绪后调用）
    void StartVoice();

private:
    LoveTv() = default;
    ~LoveTv() = default;

    void CreateClockPage();
    void CreateLovePage();
    void CreateMissuPage();
    void ShowPage(Page page);
    void GoToPage(Page page);
    void LoadConfig();

    void OnTick();
    void UpdateClock();
    void UpdateLovePage();
    void UpdateBattery();
    void UpdateBanner();

    void StartSntp();
    bool SyncTimeFromHttpDate(Http* http);

    bool FetchWeather();
    void ApplyWeather(const std::string& temp, const std::string& humidity,
                      const std::string& weather, const std::string& wd, const std::string& ws,
                      const std::string& aqi_text, const std::string& today_weather,
                      const std::string& low, const std::string& high);

    static void OnTimer(lv_timer_t* timer);
    static void WeatherTask(void* arg);

    Display* display_ = nullptr;
    lv_obj_t* screen_ = nullptr;
    lv_obj_t* pages_[3] = {nullptr, nullptr, nullptr};
    Page page_ = Page::kClock;
    Page pre_page_ = Page::kClock;   // 进入消息页之前所在的页面
    Role role_ = Role::kNone;
    bool active_ = false;
    bool started_ = false;
    bool sntp_started_ = false;

    // 配置页改了城市后，让天气任务尽快重新拉取
    volatile bool weather_refresh_ = false;

    // 由 MQTT 模块注入的发送实现
    std::function<bool(const char* type)> sender_;

    // 时钟页控件
    lv_obj_t* date_label_ = nullptr;
    lv_obj_t* time_label_ = nullptr;
    lv_obj_t* seconds_label_ = nullptr;
    lv_obj_t* battery_label_ = nullptr;
    lv_obj_t* banner_label_ = nullptr;
    // 温湿度卡片：数值 + 进度条
    lv_obj_t* temp_value_ = nullptr;
    lv_obj_t* temp_bar_ = nullptr;
    // 天气卡片：天气状况文字
    lv_obj_t* weather_value_ = nullptr;
    int banner_index_ = 0;
    uint32_t banner_tick_ = 0;
    std::string banner_texts_[8];

    // 相识页控件
    lv_obj_t* day_label_ = nullptr;
    lv_obj_t* words_label_ = nullptr;
    lv_obj_t* bottom_label_ = nullptr;
    int love_days_ = -1;

    // 消息页控件
    lv_obj_t* missu_icon_ = nullptr;   // 圆形图标：发/收用 COMMENT，回执用 CHECK
    lv_obj_t* missu_title_ = nullptr;
    lv_obj_t* missu_message_ = nullptr;
    lv_obj_t* missu_hint_ = nullptr;

    // 配置（NVS 覆盖）
    int anniv_year_ = 2019;
    int anniv_month_ = 9;
    int anniv_day_ = 1;
    std::string bottom_text_ = "known since 2019.9.1";
    std::string sweet_words_ = "Happy Every Day";
    std::string city_code_ = "101120501";  // 默认烟台
};

#endif // LOVETV_H
