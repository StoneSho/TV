#ifndef LOVETV_MQTT_H
#define LOVETV_MQTT_H

#include <Arduino.h>

// LoveTV MQTT 互通模块（esp-mqtt，QoS 1）
// 订阅 lovetv/{self}/text，发布 lovetv/{peer}/text
// payload: {"from":"A","type":"imu|check","ts":...}
class MqttLovetv {
public:
    static MqttLovetv& Get();

    void Begin(const String& host, int port,
               const String& selfId, const String& peerId);

    // type: "imu" / "check"，返回是否已交给 MQTT 发送
    bool Publish(const char* type);
    bool IsConnected();

    void SetMessageHandler(void (*fn)(const String& type));

    // 供 esp-mqtt 事件回调调用：只入队，不直接执行 UI 操作
    void QueueMessage(const String& type);
    // 主循环调用：把排队中的消息交给消息处理函数（loop 上下文）
    void HandlePendingMessage();

    // 供事件回调访问（非公开 API）
    void* GetRawClient();
    const char* GetSubTopic();
    const char* GetSelfId();
    void (*GetMessageHandler())(const String& type);
    void MarkConnected(bool connected);

private:
    MqttLovetv() = default;

    void* client_ = nullptr;  // esp_mqtt_client_handle_t
    String host_;
    int port_ = 1883;
    String selfId_;
    String peerId_;
    String subTopic_;
    String pubTopic_;
    String clientId_;
    volatile bool connected_ = false;
    void (*onMessage_)(const String& type) = nullptr;
    portMUX_TYPE mqttMux_ = portMUX_INITIALIZER_UNLOCKED;
    volatile bool messagePending_ = false;
    char pendingType_[8] = {0};
};

#endif
