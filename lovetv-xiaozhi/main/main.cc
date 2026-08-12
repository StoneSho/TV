#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <wifi_station.h>

#include "board.h"
#include "display.h"
#include "audio_codecs/audio_codec.h"
#include "lovetv.h"
#include "lovetv_config_server.h"
#include "lovetv_mqtt.h"

#define TAG "main"

// 纯 LoveTV 固件入口
// 启动顺序：事件循环 -> NVS -> 板级(屏幕/按键) -> 网络(含配网) -> LoveTV 界面
extern "C" void app_main(void)
{
    // 默认事件循环，WiFi 事件依赖它
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // NVS：保存 WiFi 配置和 LoveTV 参数
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 板级初始化：构造函数里完成 SPI / 屏幕 / 按键
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    display->SetStatus("正在启动...");

    // I2S 音频通道使能（录音/播放前提），只需一次
    board.GetAudioCodec()->Start();

    ESP_LOGI(TAG, "LoveTV starting, board: %s", board.GetBoardType().c_str());

    // 联网。未配网时会在此进入配网模式并阻塞，
    // 此时屏幕显示的是板级自带的状态栏提示（热点名 + 网址）
    board.StartNetwork();

    // 网络就绪后再接管屏幕，避免盖住配网提示
    LoveTv::GetInstance().Start(display);

    // 局域网配置页：浏览器打开设备 IP 即可改城市 / 相识日期 / MQTT 参数
    LoveTvConfigServer::GetInstance().Start();

    // MQTT 互通。独占一个任务，因为 EspMqtt::Connect() 会阻塞最多 10 秒
    LoveTvMqtt::GetInstance().Start();

    // 语音对讲：网络就绪后连接 WebSocket 服务器
    LoveTv::GetInstance().StartVoice();

    ESP_LOGI(TAG, "LoveTV started, config page: http://%s",
             WifiStation::GetInstance().GetIpAddress().c_str());
    // app_main 返回后主任务退出并释放栈；
    // LoveTV 的 lv_timer 和天气任务会继续运行
}
