#include "lovetv_mqtt.h"

#include "board.h"
#include "lovetv.h"
#include "settings.h"
#include "system_info.h"

#include <wifi_station.h>

#include <cJSON.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <ctime>

#define TAG "LoveTvMqtt"

namespace {

// 出厂默认指向已部署的服务器；密码故意留空，必须在配置页填，不进源码
const char kDefaultHost[] = "117.72.44.85";
const int kDefaultPort = 1883;
const char kDefaultUser[] = "lovetv";

// EspMqtt::Publish() 判断 esp_mqtt_client_publish() == 0。
// 按 IDF 文档，QoS 0 的 message_id 恒为 0 才算成功，QoS>0 返回正的 message_id，
// 用包装层发 QoS 1 会永远被判成失败。因此这里固定 QoS 0。
const int kQos = 0;

std::string JsonString(cJSON* root, const char* key) {
    auto* item = cJSON_GetObjectItem(root, key);
    if (item != nullptr && cJSON_IsString(item) && item->valuestring != nullptr) {
        return item->valuestring;
    }
    return "";
}

}  // namespace

LoveTvMqtt& LoveTvMqtt::GetInstance() {
    static LoveTvMqtt instance;
    return instance;
}

void LoveTvMqtt::Start() {
    if (started_) {
        return;
    }
    started_ = true;

    mutex_ = xSemaphoreCreateMutex();
    if (mutex_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return;
    }

    LoadConfig();

    // 把发送实现注入界面层。界面只认 "imu"/"check"，不关心 MQTT 细节。
    LoveTv::GetInstance().SetSender([this](const char* type) {
        return Publish(type);
    });

    BaseType_t created = xTaskCreate(&LoveTvMqtt::MqttTask, "lovetv_mqtt",
                                    1024 * 5, this, 4, nullptr);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create mqtt task");
    }
}

void LoveTvMqtt::LoadConfig() {
    Settings settings("lovetv");
    xSemaphoreTake(mutex_, portMAX_DELAY);
    host_ = settings.GetString("mqtt_host", kDefaultHost);
    port_ = settings.GetInt("mqtt_port", kDefaultPort);
    user_ = settings.GetString("mqtt_user", kDefaultUser);
    pass_ = settings.GetString("mqtt_pass", "");
    self_id_ = settings.GetString("self_id", "A");
    peer_id_ = settings.GetString("peer_id", "B");
    sub_topic_ = "lovetv/" + self_id_ + "/text";
    pub_topic_ = "lovetv/" + peer_id_ + "/text";
    xSemaphoreGive(mutex_);

    // 服务器目前是匿名接入，密码为空属正常情况
    ESP_LOGI(TAG, "Config: %s:%d self=%s peer=%s pass=%s",
             host_.c_str(), port_, self_id_.c_str(), peer_id_.c_str(),
             pass_.empty() ? "(anonymous)" : "(set)");
}

void LoveTvMqtt::ReloadConfig() {
    if (!started_) {
        return;
    }
    LoadConfig();
    // 让重连循环下一轮用新参数重建连接
    reconnect_now_ = true;
    ESP_LOGI(TAG, "Config reloaded, will reconnect");
}

bool LoveTvMqtt::Publish(const char* type) {
    if (mqtt_ == nullptr || !mqtt_->IsConnected()) {
        ESP_LOGW(TAG, "Publish %s skipped: not connected", type);
        return false;
    }

    std::string topic;
    std::string from;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    topic = pub_topic_;
    from = self_id_;
    xSemaphoreGive(mutex_);

    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"from\":\"%s\",\"type\":\"%s\",\"ts\":%lld}",
             from.c_str(), type, static_cast<long long>(time(nullptr)));

    // 已连接时 publish 只是写 socket，不会长时间阻塞，可在按键任务里直接调。
    // 未连接则上面已提前返回，避免 esp_mqtt_client_publish 阻塞按键响应。
    bool ok = mqtt_->Publish(topic, payload, kQos);
    ESP_LOGI(TAG, "Publish %s -> %s : %s", type, topic.c_str(), ok ? "ok" : "failed");
    return ok;
}

void LoveTvMqtt::OnMessage(const std::string& topic, const std::string& payload) {
    ESP_LOGI(TAG, "Recv %s : %s", topic.c_str(), payload.c_str());

    cJSON* root = cJSON_Parse(payload.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "Payload is not valid JSON, ignored");
        return;
    }

    std::string from = JsonString(root, "from");
    std::string type = JsonString(root, "type");
    cJSON_Delete(root);

    // 自己发的消息（服务端回环或误配成同一 ID）直接丢弃
    std::string self;
    xSemaphoreTake(mutex_, portMAX_DELAY);
    self = self_id_;
    xSemaphoreGive(mutex_);
    if (!from.empty() && from == self) {
        ESP_LOGI(TAG, "Ignore own message");
        return;
    }

    auto& ui = LoveTv::GetInstance();
    if (type == "imu") {
        ui.OnPeerMissYou();
    } else if (type == "check") {
        ui.OnPeerAck();
    } else {
        ESP_LOGW(TAG, "Unknown type '%s', ignored", type.c_str());
    }
}

void LoveTvMqtt::MqttTask(void* arg) {
    auto* self = static_cast<LoveTvMqtt*>(arg);
    ESP_LOGI(TAG, "MQTT task running");

    // 指数退避：2s 起，翻倍到 60s 上限。原工程重试 5 次即放弃，不适合长期运行。
    int backoff_s = 2;
    const int kMaxBackoffS = 60;

    while (true) {
        if (!WifiStation::GetInstance().IsConnected()) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        std::string host, user, pass, sub_topic;
        int port;
        xSemaphoreTake(self->mutex_, portMAX_DELAY);
        host = self->host_;
        port = self->port_;
        user = self->user_;
        pass = self->pass_;
        sub_topic = self->sub_topic_;
        xSemaphoreGive(self->mutex_);

        // 服务器现为匿名接入（allow_anonymous true），密码可以为空。
        // 只有 host 为空才算未配置。
        if (host.empty()) {
            ESP_LOGW(TAG, "MQTT host empty, fill it on the config page");
            vTaskDelay(pdMS_TO_TICKS(30000));
            continue;
        }

        bool need_connect = self->reconnect_now_ ||
                            self->mqtt_ == nullptr ||
                            !self->mqtt_->IsConnected();
        if (!need_connect) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        self->reconnect_now_ = false;

        // EspMqtt 的 connected_ 只在 Connect() 内部置位，自动重连不会恢复它，
        // 所以每次都重新走一遍 Connect()，顺带把订阅重新建立。
        // 注意：对象只创建一次、永不 delete —— 其析构会无条件调用 Disconnect()，
        // 而 Disconnect() 对 nullptr handle 不做检查。
        if (self->mqtt_ == nullptr) {
            self->mqtt_ = Board::GetInstance().CreateMqtt();
            if (self->mqtt_ == nullptr) {
                ESP_LOGE(TAG, "CreateMqtt returned null");
                vTaskDelay(pdMS_TO_TICKS(10000));
                continue;
            }
            self->mqtt_->OnMessage([self](const std::string& topic,
                                          const std::string& payload) {
                self->OnMessage(topic, payload);
            });
            // 比默认 60s 更快发现掉线
            self->mqtt_->SetKeepAlive(30);
        }

        // client_id 必须全局唯一，否则 broker 会踢掉先连的那个。
        // 两台设备烧同一份固件，用 MAC 后缀区分。
        std::string mac = SystemInfo::GetMacAddress();
        std::string client_id = "lovetv-" + self->self_id_ + "-" +
                                (mac.size() >= 8 ? mac.substr(mac.size() - 8) : mac);

        ESP_LOGI(TAG, "Connecting to %s:%d as %s", host.c_str(), port,
                 client_id.c_str());
        bool ok = self->mqtt_->Connect(host, port, client_id, user, pass);
        if (!ok) {
            ESP_LOGW(TAG, "Connect failed, retry in %ds", backoff_s);
            vTaskDelay(pdMS_TO_TICKS(backoff_s * 1000));
            backoff_s = backoff_s * 2 > kMaxBackoffS ? kMaxBackoffS : backoff_s * 2;
            continue;
        }

        backoff_s = 2;
        if (!self->mqtt_->Subscribe(sub_topic, kQos)) {
            ESP_LOGW(TAG, "Subscribe %s failed", sub_topic.c_str());
        } else {
            ESP_LOGI(TAG, "Connected, subscribed %s, stack high water=%u bytes",
                     sub_topic.c_str(),
                     (unsigned)(uxTaskGetStackHighWaterMark(nullptr) *
                                sizeof(StackType_t)));
        }
    }
}

bool LoveTvMqtt::IsConnected() const {
    return mqtt_ != nullptr && mqtt_->IsConnected();
}
