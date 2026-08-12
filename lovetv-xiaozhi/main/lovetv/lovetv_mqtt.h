#ifndef LOVETV_MQTT_H
#define LOVETV_MQTT_H

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <string>

class Mqtt;

// LoveTV 的 MQTT 互通模块
//
// 订阅 lovetv/{self}/text，发布到 lovetv/{peer}/text
// payload: {"from":"A","type":"imu|check","ts":1775000000}
//   imu   = 发一条"芜湖！"
//   check = 接收方确认，让对方看到"起飞！"
//
// 连接参数全部来自 NVS（配置页写入），密码不进源码。
class LoveTvMqtt {
public:
    static LoveTvMqtt& GetInstance();

    // 启动后台任务：读配置 -> 连接 -> 订阅 -> 掉线自动重连
    // 必须在网络就绪后调用（Connect 会阻塞最多 10 秒，因此独占一个任务）
    void Start();

    // 配置页保存后调用：重读 NVS 并用新参数重连
    void ReloadConfig();

    bool IsConnected() const;

private:
    LoveTvMqtt() = default;
    ~LoveTvMqtt() = default;

    static void MqttTask(void* arg);

    void LoadConfig();
    void OnMessage(const std::string& topic, const std::string& payload);

    // 供 LoveTv::SetSender 注入。type 取 "imu" / "check"
    bool Publish(const char* type);

    Mqtt* mqtt_ = nullptr;
    bool started_ = false;

    // 配置页保存后置位，让重连循环下一轮用新参数重建连接
    volatile bool reconnect_now_ = false;

    // 保护下面的配置字段：按键任务会读，MQTT 任务会在重载时写
    SemaphoreHandle_t mutex_ = nullptr;

    std::string host_;
    int port_ = 1883;
    std::string user_;
    std::string pass_;
    std::string self_id_;
    std::string peer_id_;
    std::string sub_topic_;
    std::string pub_topic_;
};

#endif // LOVETV_MQTT_H
