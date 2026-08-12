#include "MqttLovetv.h"

#include <WiFi.h>
#include <mqtt_client.h>
#include <ArduinoJson.h>
#include <time.h>

namespace {

const int kQos = 1;

int OnMqttEvent(esp_mqtt_event_handle_t event) {
    auto* self = static_cast<MqttLovetv*>(event->user_context);
    auto* client = event->client;

    switch (event->event_id) {
    case MQTT_EVENT_CONNECTED: {
        self->MarkConnected(true);
        Serial.println("[MQTT] connected");
        int msg_id = esp_mqtt_client_subscribe(client, self->GetSubTopic(), kQos);
        Serial.printf("[MQTT] subscribed %s (id=%d)\n", self->GetSubTopic(), msg_id);
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        self->MarkConnected(false);
        Serial.println("[MQTT] disconnected, auto reconnect");
        break;
    case MQTT_EVENT_DATA: {
        auto* data = event;
        if (data->data_len <= 0) {
            break;
        }
        String payload;
        payload.concat(reinterpret_cast<const char*>(data->data), data->data_len);
        String type = "";

        DynamicJsonDocument doc(256);
        DeserializationError err = deserializeJson(doc, payload);
        if (!err) {
            const char* from = doc["from"];
            const char* t = doc["type"];
            if (from != nullptr && t != nullptr) {
                if (String(from) != self->GetSelfId()) {
                    type = String(t);
                }
            }
        } else {
            // 兼容原版裸字符串 imu / check
            payload.trim();
            if (payload == "imu" || payload == "check") {
                type = payload;
            }
        }

        if (type.length() > 0 && self->GetMessageHandler() != nullptr) {
            self->GetMessageHandler()(type);
        }
        break;
    }
    default:
        break;
    }
    return ESP_OK;
}

}  // namespace

MqttLovetv& MqttLovetv::Get() {
    static MqttLovetv instance;
    return instance;
}

void MqttLovetv::Begin(const String& host, int port,
                       const String& selfId, const String& peerId) {
    host_ = host;
    port_ = port;
    selfId_ = selfId;
    peerId_ = peerId;
    subTopic_ = "lovetv/" + selfId_ + "/text";
    pubTopic_ = "lovetv/" + peerId_ + "/text";

    String mac = WiFi.macAddress();
    mac.replace(":", "");
    String suffix = mac.length() >= 8 ? mac.substring(mac.length() - 8) : mac;
    clientId_ = "lovetv-" + selfId_ + "-" + suffix;

    esp_mqtt_client_config_t cfg = {};
    cfg.host = host_.c_str();
    cfg.port = port_;
    cfg.client_id = clientId_.c_str();
    cfg.keepalive = 30;
    cfg.reconnect_timeout_ms = 2000;
    cfg.event_handle = OnMqttEvent;
    cfg.user_context = this;

    client_ = esp_mqtt_client_init(&cfg);
    if (client_ == nullptr) {
        Serial.println("[MQTT] init failed");
        return;
    }
    esp_mqtt_client_start(static_cast<esp_mqtt_client_handle_t>(client_));
    Serial.printf("[MQTT] connecting to %s:%d (client=%s)\n",
                  host_.c_str(), port_, clientId_.c_str());
}

bool MqttLovetv::Publish(const char* type) {
    if (client_ == nullptr || !connected_) {
        Serial.println("[MQTT] publish skipped: not connected");
        return false;
    }

    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"from\":\"%s\",\"type\":\"%s\",\"ts\":%ld}",
             selfId_.c_str(), type, static_cast<long>(time(nullptr)));

    int msg_id = esp_mqtt_client_publish(static_cast<esp_mqtt_client_handle_t>(client_),
                                         pubTopic_.c_str(), payload, 0, kQos, 0);
    Serial.printf("[MQTT] publish %s -> %s (id=%d)\n", type, pubTopic_.c_str(), msg_id);
    return msg_id > 0;
}

bool MqttLovetv::IsConnected() {
    return connected_;
}

void MqttLovetv::SetMessageHandler(void (*fn)(const String& type)) {
    onMessage_ = fn;
}

void* MqttLovetv::GetRawClient() {
    return client_;
}

const char* MqttLovetv::GetSubTopic() {
    return subTopic_.c_str();
}

const char* MqttLovetv::GetSelfId() {
    return selfId_.c_str();
}

void (*MqttLovetv::GetMessageHandler())(const String&) {
    return onMessage_;
}

void MqttLovetv::MarkConnected(bool connected) {
    connected_ = connected;
}
