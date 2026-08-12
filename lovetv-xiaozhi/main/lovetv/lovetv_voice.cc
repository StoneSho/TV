#include "lovetv_voice.h"

#include "audio_codec.h"
#include "board.h"
#include "display.h"
#include "lovetv.h"

#include <esp_websocket_client.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>

#define TAG "LOVETV_VOICE"

// 音频参数：16kHz 单声道，30ms 帧（与 AudioCodec::InputData 的单次读取一致）
#define AUDIO_SAMPLE_RATE 16000
#define FRAME_SIZE (AUDIO_SAMPLE_RATE / 1000 * 30)   // 480 samples
#define ADPCM_FRAME_BYTES (FRAME_SIZE / 2)           // 480 samples -> 240 bytes
#define MAX_RECORDING_DURATION_MS (60 * 1000)        // 最长 60 秒

// ============================================================================
// IMA ADPCM（4bit）编解码：纯查表 + 固定小缓冲，无动态大内存
// ============================================================================

namespace {

const int kIndexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
};

const int kStepTable[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
    3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

inline void ClampPredictor(int32_t& pred) {
    if (pred > 32767) pred = 32767;
    else if (pred < -32768) pred = -32768;
}

inline void UpdateStepIndex(int& step_index, int code) {
    step_index += kIndexTable[code & 0x0F];
    if (step_index < 0) step_index = 0;
    else if (step_index > 88) step_index = 88;
}

// 编码 480 个 int16 样本 -> 240 字节（每字节两个 4bit 编码，先高 4 位）
void AdpcmEncode(const int16_t* pcm, size_t samples,
                 uint8_t* out, int16_t& predictor, int& step_index) {
    size_t bytes = samples / 2;
    for (size_t i = 0; i < bytes; i++) {
        uint8_t byte = 0;
        for (int s = 0; s < 2; s++) {
            int32_t sample = pcm[i * 2 + s];
            int32_t diff = sample - predictor;
            uint8_t code = 0;
            int step = kStepTable[step_index];
            if (diff < 0) {
                code = 8;
                diff = -diff;
            }
            if (diff >= step) {
                code |= 4;
                diff -= step;
            }
            step >>= 1;
            if (diff >= step) {
                code |= 2;
                diff -= step;
            }
            step >>= 1;
            if (diff >= step) {
                code |= 1;
            }

            int32_t pred = predictor;
            int32_t delta = (2 * (code & 7) + 1) * kStepTable[step_index] / 8;
            pred += (code & 8) ? -delta : delta;
            ClampPredictor(pred);
            predictor = static_cast<int16_t>(pred);
            UpdateStepIndex(step_index, code);

            if (s == 0) {
                byte = static_cast<uint8_t>(code << 4);
            } else {
                byte |= code;
            }
        }
        out[i] = byte;
    }
}

// 解码 240 字节 -> 480 个 int16 样本
void AdpcmDecode(const uint8_t* in, size_t bytes,
                 int16_t* pcm, int16_t& predictor, int& step_index) {
    for (size_t i = 0; i < bytes; i++) {
        uint8_t byte = in[i];
        for (int s = 0; s < 2; s++) {
            uint8_t code = (s == 0) ? (byte >> 4) : (byte & 0x0F);
            int step = kStepTable[step_index];
            int32_t diff = step >> 3;
            if (code & 4) diff += step;
            if (code & 2) diff += step >> 1;
            if (code & 1) diff += step >> 2;

            int32_t pred = predictor;
            pred += (code & 8) ? -diff : diff;
            ClampPredictor(pred);
            predictor = static_cast<int16_t>(pred);
            UpdateStepIndex(step_index, code);

            pcm[i * 2 + s] = predictor;
        }
    }
}

}  // namespace

// ============================================================================
// VoiceRecorder 实现
// ============================================================================

VoiceRecorder::VoiceRecorder() {
    mutex_ = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "[VoiceRecorder] initialized (IMA ADPCM, no heavy codec)");
}

VoiceRecorder::~VoiceRecorder() {
    if (mutex_) {
        vSemaphoreDelete(mutex_);
    }
}

VoiceRecorder& VoiceRecorder::GetInstance() {
    static VoiceRecorder instance;
    return instance;
}

void VoiceRecorder::StartRecording() {
    ESP_LOGI(TAG, "[StartRecording] enter, recording=%d free_internal=%u",
             (int)recording_, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    xSemaphoreTake(mutex_, portMAX_DELAY);

    if (recording_) {
        ESP_LOGW(TAG, "Already recording");
        xSemaphoreGive(mutex_);
        return;
    }

    // 半双工：播放时不允许录音
    if (VoicePlayer::GetInstance().IsPlaying()) {
        ESP_LOGW(TAG, "Cannot record while playing");
        Board::GetInstance().GetDisplay()->ShowNotification("对方正在说话");
        xSemaphoreGive(mutex_);
        return;
    }

    recording_ = true;
    should_send_ = false;
    pcm_buffer_.clear();
    adpcm_predictor_ = 0;
    adpcm_step_index_ = 0;
    start_time_us_ = esp_timer_get_time();

    xSemaphoreGive(mutex_);

    // 启用麦克风，关闭扬声器
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    codec->EnableInput(true);
    codec->EnableOutput(false);

    // 创建录音任务（ADPCM 很轻，8192 栈足够）
    BaseType_t ok = xTaskCreate(RecordingTask, "voice_record", 8192, this, 5, &task_handle_);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create recording task");
        xSemaphoreTake(mutex_, portMAX_DELAY);
        recording_ = false;
        xSemaphoreGive(mutex_);
        codec->EnableInput(false);
        codec->EnableOutput(true);
        LoveTv::GetInstance().ShowVoiceMessage("内存不足，无法录音", false);
        return;
    }

    ESP_LOGI(TAG, "Recording started");
    LoveTv::GetInstance().ShowVoiceMessage("正在录音，请说话…", true);
    ESP_LOGI(TAG, "[StartRecording] done");
}

void VoiceRecorder::StopAndSend() {
    xSemaphoreTake(mutex_, portMAX_DELAY);

    if (!recording_) {
        xSemaphoreGive(mutex_);
        return;
    }

    should_send_ = true;
    recording_ = false;

    xSemaphoreGive(mutex_);

    ESP_LOGI(TAG, "Recording stopped, will send");
}

void VoiceRecorder::Cancel() {
    xSemaphoreTake(mutex_, portMAX_DELAY);

    if (!recording_) {
        xSemaphoreGive(mutex_);
        return;
    }

    should_send_ = false;
    recording_ = false;

    xSemaphoreGive(mutex_);

    ESP_LOGI(TAG, "Recording cancelled");
    LoveTv::GetInstance().ShowVoiceMessage("语音已取消", false);
}

float VoiceRecorder::GetDuration() const {
    if (!recording_) {
        return 0.0f;
    }
    return (esp_timer_get_time() - start_time_us_) / 1000000.0f;
}

void VoiceRecorder::RecordingTask(void* arg) {
    auto* self = static_cast<VoiceRecorder*>(arg);
    self->RecordingLoop();
    vTaskDelete(nullptr);
}

void VoiceRecorder::RecordingLoop() {
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();
    bool ws_warned = false;
    ESP_LOGI(TAG, "[RecordingLoop] task started, free_internal=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    while (recording_) {
        float duration = GetDuration();
        if (duration >= 60.0f) {
            ESP_LOGW(TAG, "Recording timeout (60s)");
            LoveTv::GetInstance().ShowVoiceMessage("已达 60 秒上限", false);
            xSemaphoreTake(mutex_, portMAX_DELAY);
            recording_ = false;
            should_send_ = true;
            xSemaphoreGive(mutex_);
            break;
        }

        // 读取 30ms PCM（480 samples @ 16kHz）
        std::vector<int16_t> pcm;
        if (!codec->InputData(pcm) || pcm.empty()) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        pcm_buffer_.insert(pcm_buffer_.end(), pcm.begin(), pcm.end());
        while (pcm_buffer_.size() >= FRAME_SIZE) {
            // 取一帧编码
            uint8_t packet[ADPCM_FRAME_BYTES];
            AdpcmEncode(pcm_buffer_.data(), FRAME_SIZE, packet,
                        adpcm_predictor_, adpcm_step_index_);
            pcm_buffer_.erase(pcm_buffer_.begin(), pcm_buffer_.begin() + FRAME_SIZE);

            std::vector<uint8_t> out(packet, packet + ADPCM_FRAME_BYTES);
            VoiceStreamer::GetInstance().SendAudioPacket(out);
        }

        if (!VoiceStreamer::GetInstance().IsConnected() && !ws_warned) {
            ws_warned = true;
            ESP_LOGW(TAG, "Voice WebSocket not connected, packets dropped");
        }
    }

    // 停止麦克风，恢复扬声器
    codec->EnableInput(false);
    codec->EnableOutput(true);

    xSemaphoreTake(mutex_, portMAX_DELAY);
    bool send = should_send_;
    float duration = (esp_timer_get_time() - start_time_us_) / 1000000.0f;
    xSemaphoreGive(mutex_);

    if (!send) {
        ESP_LOGI(TAG, "Recording cancelled, discarded");
        return;
    }

    if (duration < 1.0f) {
        ESP_LOGW(TAG, "Recording too short: %.1fs", duration);
        LoveTv::GetInstance().ShowVoiceMessage("说话时间太短", false);
        return;
    }

    ESP_LOGI(TAG, "Recording completed: %.1fs (streamed in real-time)", duration);
    LoveTv::GetInstance().ShowVoiceMessage("语音已发送", true);
}

// ============================================================================
// VoicePlayer 实现：单任务 + 队列，逐个 30ms ADPCM 包解码播放
// ============================================================================

VoicePlayer::VoicePlayer() {
    mutex_ = xSemaphoreCreateMutex();
    queue_ = xQueueCreate(64, sizeof(std::vector<uint8_t>*));  // 64 帧 = 1.92 秒缓冲

    // 播放任务优先级 6（高于录音的 5），保证听感流畅
    xTaskCreate(PlaybackTask, "voice_play", 8192, this, 6, &task_handle_);
    ESP_LOGI(TAG, "[VoicePlayer] initialized (IMA ADPCM)");
}

VoicePlayer::~VoicePlayer() {
    if (queue_) {
        vQueueDelete(queue_);
    }
    if (mutex_) {
        vSemaphoreDelete(mutex_);
    }
}

VoicePlayer& VoicePlayer::GetInstance() {
    static VoicePlayer instance;
    return instance;
}

void VoicePlayer::PlayVoice(const std::vector<uint8_t>& adpcm_data) {
    // 半双工：录音时收到语音直接丢弃
    if (VoiceRecorder::GetInstance().IsRecording()) {
        ESP_LOGW(TAG, "Cannot play while recording");
        return;
    }

    auto* packet = new std::vector<uint8_t>(adpcm_data);
    BaseType_t sent = xQueueSend(queue_, &packet, pdMS_TO_TICKS(20));
    if (sent != pdTRUE) {
        delete packet;
        ESP_LOGW(TAG, "Playback queue full, packet dropped");
        return;
    }

    xSemaphoreTake(mutex_, portMAX_DELAY);
    bool was_playing = playing_;

    // 新一轮播放开始时重置解码器状态
    if (!was_playing) {
        adpcm_predictor_ = 0;
        adpcm_step_index_ = 0;
    }

    playing_ = true;
    xSemaphoreGive(mutex_);

    if (!was_playing) {
        ESP_LOGI(TAG, "Playback started: %d bytes", (int)adpcm_data.size());
        LoveTv::GetInstance().ShowVoiceMessage("正在播放语音…", true);
    }
}

void VoicePlayer::PlaybackTask(void* arg) {
    auto* self = static_cast<VoicePlayer*>(arg);
    self->PlaybackLoop();
    vTaskDelete(nullptr);
}

void VoicePlayer::PlaybackLoop() {
    AudioCodec* codec = Board::GetInstance().GetAudioCodec();

    while (true) {
        std::vector<uint8_t>* packet = nullptr;
        if (xQueueReceive(queue_, &packet, portMAX_DELAY) != pdTRUE || packet == nullptr) {
            continue;
        }

        // 播放期间保持麦克风关闭、扬声器开启
        codec->EnableInput(false);
        codec->EnableOutput(true);

        // ADPCM 解码（240 字节 -> 480 samples；容错处理任意偶数长度）
        size_t bytes = packet->size();
        if (bytes == 0 || (bytes % 2) != 0) {
            ESP_LOGW(TAG, "Bad ADPCM packet: %d bytes", (int)bytes);
            delete packet;
            continue;
        }
        size_t samples = bytes * 2;
        std::vector<int16_t> pcm_16k(samples);
        AdpcmDecode(packet->data(), bytes, pcm_16k.data(),
                    adpcm_predictor_, adpcm_step_index_);

        // 重采样 16kHz -> 24kHz（2:3）
        std::vector<int16_t> pcm_24k;
        Resample16to24(pcm_16k, pcm_24k);
        codec->OutputData(pcm_24k);

        delete packet;

        // 队列清空 = 本次讲话播放完毕
        if (uxQueueMessagesWaiting(queue_) == 0) {
            xSemaphoreTake(mutex_, portMAX_DELAY);
            playing_ = false;
            xSemaphoreGive(mutex_);
            ESP_LOGI(TAG, "Playback completed");
            LoveTv::GetInstance().ShowVoiceMessage("播放完毕", true);
        }
    }
}

void VoicePlayer::Resample16to24(const std::vector<int16_t>& in, std::vector<int16_t>& out) {
    // 16kHz -> 24kHz = 2:3：每 2 个输入样本生成 3 个输出样本（线性插值）
    out.clear();
    out.reserve(in.size() * 3 / 2 + 1);

    size_t i = 0;
    for (; i + 1 < in.size(); i += 2) {
        int16_t s0 = in[i];
        int16_t s1 = in[i + 1];
        out.push_back(s0);                       // 0/3
        out.push_back(static_cast<int16_t>((s0 * 2 + s1) / 3));  // 1/3
        out.push_back(static_cast<int16_t>((s0 + s1 * 2) / 3));  // 2/3
    }
    if (i < in.size()) {
        out.push_back(in[i]);
    }
}

// ============================================================================
// VoiceStreamer 实现
// ============================================================================

VoiceStreamer::VoiceStreamer() {
    mutex_ = xSemaphoreCreateMutex();
}

VoiceStreamer::~VoiceStreamer() {
    Disconnect();
    if (mutex_) {
        vSemaphoreDelete(mutex_);
    }
}

VoiceStreamer& VoiceStreamer::GetInstance() {
    static VoiceStreamer instance;
    return instance;
}

bool VoiceStreamer::Connect(const std::string& server_url, const std::string& device_id, const std::string& peer_id) {
    xSemaphoreTake(mutex_, portMAX_DELAY);

    if (ws_client_ != nullptr) {
        ESP_LOGW(TAG, "Already connected/connecting");
        xSemaphoreGive(mutex_);
        return true;
    }

    // 保存参数供心跳超时后无参重连使用
    server_url_ = server_url;
    device_id_ = device_id;
    peer_id_ = peer_id;

    // 构造 WebSocket URL，并保存到成员变量防止指针悬垂
    uri_ = server_url + "/voice?id=" + device_id + "&peer=" + peer_id;
    ESP_LOGI(TAG, "Connecting to %s", uri_.c_str());

    esp_websocket_client_config_t ws_cfg = {};
    ws_cfg.uri = uri_.c_str();
    ws_cfg.buffer_size = 4096;  // 加大接收缓冲，减少 TCP 流控抖动
    // WiFi 信号弱时默认 10s 重连太慢；语音对讲需要快速恢复
    ws_cfg.reconnect_timeout_ms = 2000;
    ws_cfg.network_timeout_ms = 5000;

    ws_client_ = esp_websocket_client_init(&ws_cfg);
    if (ws_client_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize WebSocket client");
        xSemaphoreGive(mutex_);
        return false;
    }

    esp_websocket_register_events(static_cast<esp_websocket_client_handle_t>(ws_client_),
                                  WEBSOCKET_EVENT_ANY,
                                  WebSocketEventHandler,
                                  this);

    if (esp_websocket_client_start(static_cast<esp_websocket_client_handle_t>(ws_client_)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WebSocket client");
        esp_websocket_client_destroy(static_cast<esp_websocket_client_handle_t>(ws_client_));
        ws_client_ = nullptr;
        xSemaphoreGive(mutex_);
        return false;
    }

    xSemaphoreGive(mutex_);

    // 启动心跳任务（每 10 秒 ping 一次，3 次失败就重连）
    xTaskCreate(HeartbeatTask, "ws_heartbeat", 2048, this, 3, &heartbeat_task_);

    ESP_LOGI(TAG, "WebSocket connecting...");
    return true;
}

void VoiceStreamer::Reconnect() {
    if (server_url_.empty() || device_id_.empty() || peer_id_.empty()) {
        ESP_LOGW(TAG, "Reconnect skipped: no saved connection params");
        return;
    }
    Connect(server_url_, device_id_, peer_id_);
}

void VoiceStreamer::Disconnect() {
    xSemaphoreTake(mutex_, portMAX_DELAY);

    // 停止心跳任务
    if (heartbeat_task_ != nullptr) {
        vTaskDelete(heartbeat_task_);
        heartbeat_task_ = nullptr;
    }

    if (ws_client_ != nullptr) {
        esp_websocket_client_stop(static_cast<esp_websocket_client_handle_t>(ws_client_));
        esp_websocket_client_destroy(static_cast<esp_websocket_client_handle_t>(ws_client_));
        ws_client_ = nullptr;
    }
    connected_ = false;

    xSemaphoreGive(mutex_);

    ESP_LOGI(TAG, "WebSocket disconnected");
}

void VoiceStreamer::SendAudioPacket(const std::vector<uint8_t>& data) {
    xSemaphoreTake(mutex_, portMAX_DELAY);

    if (!connected_ || ws_client_ == nullptr) {
        xSemaphoreGive(mutex_);
        return;
    }

    void* client = ws_client_;
    xSemaphoreGive(mutex_);

    // 20ms 超时：快速失败而非阻塞录音任务（对讲允许丢帧）
    int sent = esp_websocket_client_send_bin(static_cast<esp_websocket_client_handle_t>(client),
                                             reinterpret_cast<const char*>(data.data()),
                                             data.size(),
                                             pdMS_TO_TICKS(20));

    if (sent < 0) {
        ESP_LOGE(TAG, "Failed to send audio packet: %d", sent);
    } else if (sent != static_cast<int>(data.size())) {
        ESP_LOGW(TAG, "Partial send: %d/%d bytes", sent, (int)data.size());
    }
}

void VoiceStreamer::WebSocketEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* self = static_cast<VoiceStreamer*>(arg);
    auto* data = static_cast<esp_websocket_event_data_t*>(event_data);

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        Board::GetInstance().GetDisplay()->ShowNotification("语音已连接");
        xSemaphoreTake(self->mutex_, portMAX_DELAY);
        self->connected_ = true;
        xSemaphoreGive(self->mutex_);
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected");
        Board::GetInstance().GetDisplay()->ShowNotification("语音断开，重连中");
        xSemaphoreTake(self->mutex_, portMAX_DELAY);
        self->connected_ = false;
        self->rx_buffer_.clear();  // 断线时清空接收缓冲区
        xSemaphoreGive(self->mutex_);
        break;

    case WEBSOCKET_EVENT_DATA:
        if (data->op_code == 0x02) {  // Binary frame：对方语音包
            ESP_LOGI(TAG, "Recv: %d bytes (payload_len=%d, payload_offset=%d)",
                     data->data_len, data->payload_len, data->payload_offset);

            // WebSocket 可能分片接收，累积到完整的 ADPCM 帧（240 字节的倍数）再播放
            self->rx_buffer_.insert(self->rx_buffer_.end(),
                                    reinterpret_cast<const uint8_t*>(data->data_ptr),
                                    reinterpret_cast<const uint8_t*>(data->data_ptr) + data->data_len);

            // 每 240 字节为一帧，逐个交给播放器
            while (self->rx_buffer_.size() >= ADPCM_FRAME_BYTES) {
                std::vector<uint8_t> frame(self->rx_buffer_.begin(),
                                           self->rx_buffer_.begin() + ADPCM_FRAME_BYTES);
                self->rx_buffer_.erase(self->rx_buffer_.begin(),
                                       self->rx_buffer_.begin() + ADPCM_FRAME_BYTES);
                VoicePlayer::GetInstance().PlayVoice(frame);
            }
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WebSocket error");
        break;

    default:
        break;
    }
}

void VoiceStreamer::HeartbeatTask(void* arg) {
    auto* self = static_cast<VoiceStreamer*>(arg);
    int fail_count = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));  // 每 10 秒一次

        xSemaphoreTake(self->mutex_, portMAX_DELAY);
        bool is_connected = self->connected_;
        void* client = self->ws_client_;
        xSemaphoreGive(self->mutex_);

        if (!is_connected || client == nullptr) {
            fail_count = 0;
            continue;
        }

        // 发一个最小文本包作为心跳（WebSocket ping 帧在某些实现上不可靠）
        int sent = esp_websocket_client_send_text(
            static_cast<esp_websocket_client_handle_t>(client),
            "ping", 4, pdMS_TO_TICKS(1000));

        if (sent < 0) {
            fail_count++;
            ESP_LOGW(TAG, "Heartbeat failed (%d/3)", fail_count);
            if (fail_count >= 3) {
                ESP_LOGE(TAG, "Heartbeat timeout, reconnecting...");
                self->Disconnect();
                vTaskDelay(pdMS_TO_TICKS(1000));
                self->Reconnect();
                fail_count = 0;
            }
        } else {
            fail_count = 0;
        }
    }
}
