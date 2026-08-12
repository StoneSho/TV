#ifndef LOVETV_VOICE_H
#define LOVETV_VOICE_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <esp_event.h>

#include <string>
#include <vector>

class AudioCodec;

// 语音录音控制器（实时对讲：按住说话，边录边编码边发）
// 编码：IMA ADPCM 4bit @ 16kHz，30ms/帧（480 samples -> 240 bytes）
// 选择 ADPCM 而不是 Opus：编码器极轻量、固定小栈，无 alloca 大缓冲，
// 不会出现 Opus 编码时任务栈溢出复位的问题。
class VoiceRecorder {
public:
    static VoiceRecorder& GetInstance();

    void StartRecording();
    void StopAndSend();
    void Cancel();

    bool IsRecording() const { return recording_; }
    float GetDuration() const;  // 当前录音时长（秒）

private:
    VoiceRecorder();
    ~VoiceRecorder();

    static void RecordingTask(void* arg);
    void RecordingLoop();

    bool recording_ = false;
    bool should_send_ = false;  // 标记是发送还是取消
    TaskHandle_t task_handle_ = nullptr;

    std::vector<int16_t> pcm_buffer_;  // PCM 累积缓冲区（480 samples = 30ms）
    int64_t start_time_us_ = 0;

    // IMA ADPCM 编码状态
    int16_t adpcm_predictor_ = 0;
    int adpcm_step_index_ = 0;

    SemaphoreHandle_t mutex_ = nullptr;
};

// 语音播放器：单任务 + 队列，按 30ms ADPCM 包逐个解码播放
class VoicePlayer {
public:
    static VoicePlayer& GetInstance();

    void PlayVoice(const std::vector<uint8_t>& adpcm_data);
    bool IsPlaying() const { return playing_; }

private:
    VoicePlayer();
    ~VoicePlayer();

    static void PlaybackTask(void* arg);
    void PlaybackLoop();

    // 16kHz -> 24kHz 线性插值（2:3）
    void Resample16to24(const std::vector<int16_t>& in, std::vector<int16_t>& out);

    bool playing_ = false;
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_handle_ = nullptr;

    // IMA ADPCM 解码状态
    int16_t adpcm_predictor_ = 0;
    int adpcm_step_index_ = 0;

    SemaphoreHandle_t mutex_ = nullptr;
};

// WebSocket 语音流管理器（连接 9002 对讲转发服务器）
class VoiceStreamer {
public:
    static VoiceStreamer& GetInstance();

    bool Connect(const std::string& server_url, const std::string& device_id, const std::string& peer_id);
    void Reconnect();  // 用上次保存的参数重连（心跳超时后调用）
    void Disconnect();
    void SendAudioPacket(const std::vector<uint8_t>& data);
    bool IsConnected() const { return connected_; }

private:
    VoiceStreamer();
    ~VoiceStreamer();

    static void WebSocketEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

    std::string server_url_;
    std::string device_id_;
    std::string peer_id_;
    std::string uri_;
    void* ws_client_ = nullptr;  // esp_websocket_client_handle_t
    bool connected_ = false;
    std::vector<uint8_t> rx_buffer_;  // WebSocket 接收缓冲区（处理分片）

    // 心跳任务
    TaskHandle_t heartbeat_task_ = nullptr;
    static void HeartbeatTask(void* arg);

    SemaphoreHandle_t mutex_ = nullptr;
};

#endif  // LOVETV_VOICE_H
