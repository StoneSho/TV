#include "voice.h"
#include "config.h"

#include <WebSocketsClient.h>
#include <driver/i2s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <limits.h>

namespace {

constexpr size_t FRAME_SAMPLES = 480;       // 30 ms at 16 kHz
constexpr size_t FRAME_BYTES = 240;
constexpr size_t PREBUFFER_FRAMES = 6;      // 180 ms: 短预缓冲，兼顾抗抖与低延迟
constexpr size_t SEND_QUEUE_FRAMES = 24;    // 720 ms 发送缓冲上限
constexpr size_t PLAY_QUEUE_FRAMES = 16;    // 480 ms 播放缓冲上限
constexpr uint32_t MAX_RECORD_MS = 60000;
constexpr uint32_t PLAY_DRAIN_MS = 120;

struct AudioFrame {
  uint32_t generation;
  uint8_t data[FRAME_BYTES];
};

struct StatusMailbox {
  char text[64];
  bool showPage;
  bool dirty;
  uint32_t generation;
};

const int kIndexTable[16] = {
  -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8,
};
const int kStepTable[89] = {
  7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
  34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
  157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
  598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
  1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
  5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635,
  13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

void Clamp(int32_t &value) {
  if (value > 32767) value = 32767;
  if (value < -32768) value = -32768;
}

void UpdateIndex(int &index, int code) {
  index += kIndexTable[code & 0x0f];
  if (index < 0) index = 0;
  if (index > 88) index = 88;
}

void Encode(const int16_t *pcm, uint8_t *out, int16_t &predictor, int &index) {
  for (size_t i = 0; i < FRAME_BYTES; ++i) {
    uint8_t packed = 0;
    for (int half = 0; half < 2; ++half) {
      int32_t difference = pcm[i * 2 + half] - predictor;
      uint8_t code = 0;
      int step = kStepTable[index];
      if (difference < 0) { code = 8; difference = -difference; }
      if (difference >= step) { code |= 4; difference -= step; }
      step >>= 1;
      if (difference >= step) { code |= 2; difference -= step; }
      step >>= 1;
      if (difference >= step) code |= 1;

      int32_t next = predictor;
      next += (code & 8 ? -1 : 1) *
              ((2 * (code & 7) + 1) * kStepTable[index] / 8);
      Clamp(next);
      predictor = static_cast<int16_t>(next);
      UpdateIndex(index, code);
      if (half == 0) packed = code << 4; else packed |= code;
    }
    out[i] = packed;
  }
}

void Decode(const uint8_t *in, int16_t *pcm, int16_t &predictor, int &index) {
  for (size_t i = 0; i < FRAME_BYTES; ++i) {
    for (int half = 0; half < 2; ++half) {
      const uint8_t code = half == 0 ? in[i] >> 4 : in[i] & 0x0f;
      const int step = kStepTable[index];
      int32_t difference = step >> 3;
      if (code & 4) difference += step;
      if (code & 2) difference += step >> 1;
      if (code & 1) difference += step >> 2;
      int32_t next = predictor + ((code & 8) ? -difference : difference);
      Clamp(next);
      predictor = static_cast<int16_t>(next);
      UpdateIndex(index, code);
      pcm[i * 2 + half] = predictor;
    }
  }
}

size_t Resample(const int16_t *in, int16_t *out) {
  size_t output = 0;
  for (size_t i = 0; i + 1 < FRAME_SAMPLES; i += 2) {
    const int16_t a = in[i], b = in[i + 1];
    out[output++] = a;
    out[output++] = static_cast<int16_t>((a * 2 + b) / 3);
    out[output++] = static_cast<int16_t>((a + b * 2) / 3);
  }
  return output;
}

WebSocketsClient websocket;
Voice::StatusFn statusCallback = nullptr;
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
portMUX_TYPE statusMux = portMUX_INITIALIZER_UNLOCKED;
StatusMailbox mailbox = {{0}, false, false, 0};

bool ready = false;
bool websocketStarted = false;
bool connected = false;
bool recording = false;
bool stopRequested = false;
bool shouldSend = false;
bool autoStopped = false;
bool playing = false;
uint32_t recordStartMs = 0;
uint32_t uiGeneration = 1;
uint32_t recordGeneration = 1;
uint32_t playGeneration = 1;
int volume = 40;
TaskHandle_t recordTask = nullptr;
TaskHandle_t playTask = nullptr;
QueueHandle_t sendQueue = nullptr;
QueueHandle_t playQueue = nullptr;
SemaphoreHandle_t audioMutex = nullptr;
bool speakerInstalled = false;
bool microphoneInstalled = false;

AudioFrame prebuffer[PREBUFFER_FRAMES];
uint8_t receiveBuffer[FRAME_BYTES];
size_t receiveLength = 0;
uint32_t lastHeartbeatMs = 0;
uint32_t lastDiagnosticsMs = 0;
int heartbeatFailures = 0;
uint32_t sendDropped = 0;
uint32_t sendFailed = 0;
uint32_t playDropped = 0;

uint32_t GetUiGeneration() {
  portENTER_CRITICAL(&stateMux);
  const uint32_t value = uiGeneration;
  portEXIT_CRITICAL(&stateMux);
  return value;
}

void PostStatus(const char *text, bool showPage, uint32_t generation) {
  portENTER_CRITICAL(&statusMux);
  strlcpy(mailbox.text, text, sizeof(mailbox.text));
  mailbox.showPage = showPage;
  mailbox.generation = generation;
  mailbox.dirty = true;
  portEXIT_CRITICAL(&statusMux);
}

void PostCurrentStatus(const char *text, bool showPage) {
  PostStatus(text, showPage, GetUiGeneration());
}

void ClearStatus() {
  portENTER_CRITICAL(&statusMux);
  mailbox.dirty = false;
  mailbox.text[0] = '\0';
  portEXIT_CRITICAL(&statusMux);
}

void Increment(uint32_t &counter) {
  portENTER_CRITICAL(&stateMux);
  ++counter;
  portEXIT_CRITICAL(&stateMux);
}

void UninstallI2s() {
  if (microphoneInstalled) {
    i2s_stop(I2S_NUM_1);
    i2s_driver_uninstall(I2S_NUM_1);
    microphoneInstalled = false;
  }
  if (speakerInstalled) {
    i2s_stop(I2S_NUM_0);
    i2s_driver_uninstall(I2S_NUM_0);
    speakerInstalled = false;
  }
}

bool InitI2s() {
  i2s_config_t cfg = {};
  cfg.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = 24000;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 240;
  cfg.tx_desc_auto_clear = true;
  i2s_pin_config_t pins = {};
  pins.bck_io_num = SPK_BCLK;
  pins.ws_io_num = SPK_LRCK;
  pins.data_out_num = SPK_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

  esp_err_t error = i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr);
  if (error != ESP_OK) {
    Serial.printf("[Voice] speaker install failed: %s\n", esp_err_to_name(error));
    return false;
  }
  speakerInstalled = true;
  error = i2s_set_pin(I2S_NUM_0, &pins);
  if (error != ESP_OK) {
    Serial.printf("[Voice] speaker pins failed: %s\n", esp_err_to_name(error));
    UninstallI2s();
    return false;
  }

  cfg = {};
  cfg.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = 16000;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 240;
  pins = {};
  pins.bck_io_num = MIC_SCK;
  pins.ws_io_num = MIC_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = MIC_DIN;

  error = i2s_driver_install(I2S_NUM_1, &cfg, 0, nullptr);
  if (error != ESP_OK) {
    Serial.printf("[Voice] microphone install failed: %s\n", esp_err_to_name(error));
    UninstallI2s();
    return false;
  }
  microphoneInstalled = true;
  error = i2s_set_pin(I2S_NUM_1, &pins);
  if (error != ESP_OK) {
    Serial.printf("[Voice] microphone pins failed: %s\n", esp_err_to_name(error));
    UninstallI2s();
    return false;
  }
  error = i2s_stop(I2S_NUM_1);
  if (error != ESP_OK) {
    Serial.printf("[Voice] microphone standby failed: %s\n", esp_err_to_name(error));
    UninstallI2s();
    return false;
  }
  i2s_zero_dma_buffer(I2S_NUM_0);
  return true;
}

bool RecordAudioMode(TickType_t wait) {
  if (!audioMutex || xSemaphoreTake(audioMutex, wait) != pdTRUE) return false;
  const esp_err_t tx = i2s_stop(I2S_NUM_0);
  i2s_zero_dma_buffer(I2S_NUM_0);
  const esp_err_t rx = i2s_start(I2S_NUM_1);
  xSemaphoreGive(audioMutex);
  if (tx != ESP_OK || rx != ESP_OK) {
    Serial.printf("[Voice] record mode failed: tx=%s rx=%s\n",
                  esp_err_to_name(tx), esp_err_to_name(rx));
    return false;
  }
  return true;
}

bool PlaybackAudioMode(TickType_t wait) {
  if (!audioMutex || xSemaphoreTake(audioMutex, wait) != pdTRUE) return false;
  const esp_err_t rx = i2s_stop(I2S_NUM_1);
  i2s_zero_dma_buffer(I2S_NUM_1);
  // Stop and restart TX as well. This makes CancelAndExit cut the current DMA
  // buffer instead of merely clearing frames that have not started yet.
  const esp_err_t stopTx = i2s_stop(I2S_NUM_0);
  i2s_zero_dma_buffer(I2S_NUM_0);
  const esp_err_t tx = i2s_start(I2S_NUM_0);
  i2s_zero_dma_buffer(I2S_NUM_0);
  xSemaphoreGive(audioMutex);
  if (rx != ESP_OK || stopTx != ESP_OK || tx != ESP_OK) {
    Serial.printf("[Voice] playback mode failed: rx=%s stop_tx=%s tx=%s\n",
                  esp_err_to_name(rx), esp_err_to_name(stopTx), esp_err_to_name(tx));
    return false;
  }
  return true;
}

void ResetQueues() {
  if (sendQueue) xQueueReset(sendQueue);
  if (playQueue) xQueueReset(playQueue);
}

void CleanupInit() {
  if (playQueue) { vQueueDelete(playQueue); playQueue = nullptr; }
  if (sendQueue) { vQueueDelete(sendQueue); sendQueue = nullptr; }
  if (audioMutex) { vSemaphoreDelete(audioMutex); audioMutex = nullptr; }
  UninstallI2s();
}

bool RecordingActive(uint32_t generation) {
  portENTER_CRITICAL(&stateMux);
  const bool active = recording && !stopRequested && recordGeneration == generation;
  portEXIT_CRITICAL(&stateMux);
  return active;
}

bool RecordingMaySend(uint32_t generation) {
  portENTER_CRITICAL(&stateMux);
  const bool maySend = recordGeneration == generation && (recording || shouldSend);
  portEXIT_CRITICAL(&stateMux);
  return maySend;
}

void RecordingTask(void *) {
  int32_t raw[FRAME_SAMPLES];
  int16_t pcm[FRAME_SAMPLES];
  AudioFrame frame = {};
  int16_t predictor = 0;
  int stepIndex = 0;
  size_t buffered = 0;
  uint32_t capturedFrames = 0;
  bool streaming = false;
  bool warned = false;

  portENTER_CRITICAL(&stateMux);
  const uint32_t generation = recordGeneration;
  const uint32_t taskUiGeneration = uiGeneration;
  portEXIT_CRITICAL(&stateMux);
  frame.generation = generation;

  while (RecordingActive(generation)) {
    portENTER_CRITICAL(&stateMux);
    const uint32_t elapsed = millis() - recordStartMs;
    portEXIT_CRITICAL(&stateMux);
    if (elapsed >= MAX_RECORD_MS) {
      portENTER_CRITICAL(&stateMux);
      if (recordGeneration == generation) {
        recording = false;
        stopRequested = true;
        shouldSend = true;
        autoStopped = true;
      }
      portEXIT_CRITICAL(&stateMux);
      break;
    }

    size_t total = 0;
    while (total < sizeof(raw) && RecordingActive(generation)) {
      size_t got = 0;
      const esp_err_t error = i2s_read(I2S_NUM_1,
          reinterpret_cast<uint8_t *>(raw) + total, sizeof(raw) - total,
          &got, pdMS_TO_TICKS(100));
      if (error == ESP_OK) total += got;
      else if (error != ESP_ERR_TIMEOUT && RecordingActive(generation)) {
        Serial.printf("[Voice] microphone read failed: %s\n", esp_err_to_name(error));
        vTaskDelay(pdMS_TO_TICKS(10));
      }
    }
    if (total != sizeof(raw) || !RecordingActive(generation)) break;

    for (size_t i = 0; i < FRAME_SAMPLES; ++i) {
      const int32_t sample = raw[i] >> 12;
      pcm[i] = sample > 32767 ? 32767 : sample < -32768 ? -32768 : sample;
    }
    Encode(pcm, frame.data, predictor, stepIndex);
    ++capturedFrames;

    if (!streaming) {
      prebuffer[buffered++] = frame;
      if (buffered == PREBUFFER_FRAMES) {
        streaming = true;
        for (size_t i = 0; i < buffered; ++i) {
          if (!RecordingMaySend(generation) ||
              xQueueSend(sendQueue, &prebuffer[i], pdMS_TO_TICKS(5)) != pdTRUE) {
            Increment(sendDropped);
          }
        }
        buffered = 0;
      }
    } else if (xQueueSend(sendQueue, &frame, 0) != pdTRUE) {
      Increment(sendDropped);
    }

    portENTER_CRITICAL(&stateMux);
    const bool online = connected;
    portEXIT_CRITICAL(&stateMux);
    if (!online && !warned) {
      warned = true;
      Serial.println("[Voice] disconnected while recording");
    }
  }

  PlaybackAudioMode(pdMS_TO_TICKS(20));
  portENTER_CRITICAL(&stateMux);
  const bool current = recordGeneration == generation;
  const bool send = current && shouldSend;
  const bool hitLimit = current && autoStopped;
  if (current) {
    recording = false;
    stopRequested = false;
    shouldSend = false;
    autoStopped = false;
  }
  // A new recorder cannot start while recordTask is non-null, so this task
  // always owns the handle even when CancelAndExit changed the generation.
  recordTask = nullptr;
  portEXIT_CRITICAL(&stateMux);

  if (current && send) {
    if (!streaming) {
      if (sendQueue) xQueueReset(sendQueue);
      PostStatus("说话时间太短", true, taskUiGeneration);
    } else if (hitLimit) {
      PostStatus("已达 60 秒上限", true, taskUiGeneration);
    } else {
      PostStatus("语音已发送", true, taskUiGeneration);
    }
  }
  Serial.printf("[Voice] record end: %lu ms stack_hwm=%u heap=%u\n",
      static_cast<unsigned long>(capturedFrames * 30),
      static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
      static_cast<unsigned>(ESP.getFreeHeap()));
  vTaskDelete(nullptr);
}

void PlaybackTask(void *) {
  AudioFrame frame = {};
  int16_t pcm16[FRAME_SAMPLES];
  int16_t pcm24[FRAME_SAMPLES * 3 / 2];
  int32_t output[FRAME_SAMPLES * 3 / 2];
  int16_t predictor = 0;
  int stepIndex = 0;
  bool stream = false;
  uint32_t streamGeneration = 0;
  uint32_t taskUiGeneration = 0;

  for (;;) {
    const TickType_t wait = stream ? pdMS_TO_TICKS(PLAY_DRAIN_MS) : portMAX_DELAY;
    if (xQueueReceive(playQueue, &frame, wait) != pdTRUE) {
      if (stream) {
        portENTER_CRITICAL(&stateMux);
        const bool current = streamGeneration == playGeneration;
        if (current) playing = false;
        portEXIT_CRITICAL(&stateMux);
        if (current) PostStatus("播放完毕", false, taskUiGeneration);
        stream = false;
      }
      continue;
    }

    portENTER_CRITICAL(&stateMux);
    const bool valid = !recording && frame.generation == playGeneration;
    const bool first = valid && !playing;
    if (valid) {
      playing = true;
      taskUiGeneration = uiGeneration;
    }
    portEXIT_CRITICAL(&stateMux);
    if (!valid) continue;
    if (first) PostStatus("正在播放语音…", true, taskUiGeneration);

    if (!stream || streamGeneration != frame.generation) {
      stream = true;
      streamGeneration = frame.generation;
      predictor = 0;
      stepIndex = 0;
      if (!PlaybackAudioMode(pdMS_TO_TICKS(20))) {
        PostStatus("音频输出不可用", true, taskUiGeneration);
        stream = false;
        continue;
      }
    }

    Decode(frame.data, pcm16, predictor, stepIndex);
    const size_t samples = Resample(pcm16, pcm24);
    portENTER_CRITICAL(&stateMux);
    const int currentVolume = volume;
    const bool stillValid = frame.generation == playGeneration && !recording;
    portEXIT_CRITICAL(&stateMux);
    if (!stillValid) { stream = false; continue; }

    const int64_t gain = static_cast<int64_t>(currentVolume) * currentVolume *
                         65536 / (100LL * 100LL);
    for (size_t i = 0; i < samples; ++i) {
      const int64_t value = static_cast<int64_t>(pcm24[i]) * gain;
      output[i] = value > INT32_MAX ? INT32_MAX : value < INT32_MIN ? INT32_MIN : value;
    }
    size_t written = 0;
    const esp_err_t error = i2s_write(I2S_NUM_0, output,
        samples * sizeof(output[0]), &written, pdMS_TO_TICKS(100));
    if (error != ESP_OK) {
      portENTER_CRITICAL(&stateMux);
      const bool canceled = frame.generation != playGeneration || recording;
      portEXIT_CRITICAL(&stateMux);
      if (!canceled) Serial.printf("[Voice] speaker write failed: %s\n", esp_err_to_name(error));
    }
  }
}

void WebSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_CONNECTED) {
    portENTER_CRITICAL(&stateMux);
    connected = true;
    heartbeatFailures = 0;
    portEXIT_CRITICAL(&stateMux);
    Serial.println("[Voice] WebSocket connected");
    return;
  }
  if (type == WStype_DISCONNECTED) {
    portENTER_CRITICAL(&stateMux);
    connected = false;
    portEXIT_CRITICAL(&stateMux);
    receiveLength = 0;
    Serial.println("[Voice] WebSocket disconnected, reconnecting in 2 s");
    return;
  }
  if (type != WStype_BIN || !playQueue) return;

  for (size_t i = 0; i < length; ++i) {
    portENTER_CRITICAL(&stateMux);
    const bool isRecording = recording;
    const uint32_t generation = playGeneration;
    portEXIT_CRITICAL(&stateMux);
    if (isRecording) { receiveLength = 0; break; }
    receiveBuffer[receiveLength++] = payload[i];
    if (receiveLength == FRAME_BYTES) {
      AudioFrame frame = {};
      frame.generation = generation;
      memcpy(frame.data, receiveBuffer, FRAME_BYTES);
      if (xQueueSend(playQueue, &frame, 0) != pdTRUE) {
        // 队列满时丢最旧的一帧，而不是丢新帧，避免播放延迟持续累积
        AudioFrame discarded = {};
        if (xQueueReceive(playQueue, &discarded, 0) == pdTRUE) {
          Increment(playDropped);
          xQueueSend(playQueue, &frame, 0);
        } else {
          Increment(playDropped);
        }
      }
      receiveLength = 0;
    }
  }
}

void Diagnostics() {
  if (millis() - lastDiagnosticsMs < 30000) return;
  lastDiagnosticsMs = millis();
  portENTER_CRITICAL(&stateMux);
  const uint32_t txDrop = sendDropped, txFail = sendFailed, rxDrop = playDropped;
  TaskHandle_t player = playTask;
  portEXIT_CRITICAL(&stateMux);
  Serial.printf("[Voice] diag heap=%u play_stack_hwm=%u send_q=%u play_q=%u "
                "drop_tx=%lu fail_tx=%lu drop_rx=%lu\n",
      static_cast<unsigned>(ESP.getFreeHeap()),
      player ? static_cast<unsigned>(uxTaskGetStackHighWaterMark(player)) : 0,
      sendQueue ? static_cast<unsigned>(uxQueueMessagesWaiting(sendQueue)) : 0,
      playQueue ? static_cast<unsigned>(uxQueueMessagesWaiting(playQueue)) : 0,
      static_cast<unsigned long>(txDrop), static_cast<unsigned long>(txFail),
      static_cast<unsigned long>(rxDrop));
}

}  // namespace

namespace Voice {

void Init(const String &host, int port, const String &selfId, const String &peerId) {
  if (ready) return;
  audioMutex = xSemaphoreCreateMutex();
  if (!audioMutex) {
    PostCurrentStatus("语音模块内存不足", true);
    return;
  }
  if (!InitI2s()) {
    CleanupInit();
    PostCurrentStatus("音频硬件初始化失败", true);
    return;
  }
  playQueue = xQueueCreate(PLAY_QUEUE_FRAMES, sizeof(AudioFrame));
  if (!playQueue) {
    CleanupInit();
    PostCurrentStatus("语音模块内存不足", true);
    return;
  }
  sendQueue = xQueueCreate(SEND_QUEUE_FRAMES, sizeof(AudioFrame));
  if (!sendQueue) {
    CleanupInit();
    PostCurrentStatus("语音模块内存不足", true);
    return;
  }
  if (xTaskCreate(PlaybackTask, "voice_play", 8192, nullptr, 6, &playTask) != pdPASS || !playTask) {
    playTask = nullptr;
    CleanupInit();
    PostCurrentStatus("语音播放任务创建失败", true);
    return;
  }
  portENTER_CRITICAL(&stateMux);
  ready = true;
  portEXIT_CRITICAL(&stateMux);

  const String path = "/voice?id=" + selfId + "&peer=" + peerId;
  Serial.println("[Voice] ws://" + host + ":" + String(port) + path);
  websocket.begin(host, port, path);
  websocket.setReconnectInterval(2000);
  websocket.onEvent(WebSocketEvent);
  websocketStarted = true;
}

void Loop() {
  if (websocketStarted) websocket.loop();

  AudioFrame frame = {};
  bool sendFailedOnce = false;
  portENTER_CRITICAL(&stateMux);
  const bool online = connected;
  portEXIT_CRITICAL(&stateMux);
  while (sendQueue && xQueueReceive(sendQueue, &frame, 0) == pdTRUE) {
    portENTER_CRITICAL(&stateMux);
    const bool current = frame.generation == recordGeneration;
    portEXIT_CRITICAL(&stateMux);
    if (!current) {
      continue;
    }
    if (!online) {
      Increment(sendFailed);
      sendFailedOnce = true;
    } else if (!websocket.sendBIN(frame.data, FRAME_BYTES)) {
      Increment(sendFailed);
      sendFailedOnce = true;
    }
  }
  if (sendFailedOnce) {
    PostCurrentStatus("语音发送失败", true);
  }

  portENTER_CRITICAL(&stateMux);
  const bool onlineHeartbeat = connected;
  portEXIT_CRITICAL(&stateMux);
  if (onlineHeartbeat && millis() - lastHeartbeatMs >= 10000) {
    lastHeartbeatMs = millis();
    if (!websocket.sendTXT("ping")) {
      ++heartbeatFailures;
      Serial.printf("[Voice] heartbeat failed (%d/3)\n", heartbeatFailures);
      if (heartbeatFailures >= 3) {
        websocket.disconnect();
        heartbeatFailures = 0;
      }
    } else {
      heartbeatFailures = 0;
    }
  }

  StatusMailbox pending = {};
  portENTER_CRITICAL(&statusMux);
  if (mailbox.dirty) {
    pending = mailbox;
    mailbox.dirty = false;
  }
  portEXIT_CRITICAL(&statusMux);
  if (pending.dirty && pending.generation == GetUiGeneration() && statusCallback) {
    statusCallback(String(pending.text), pending.showPage);
  }
  Diagnostics();
}

void StartRecording() {
  portENTER_CRITICAL(&stateMux);
  const bool moduleReady = ready;
  const bool online = connected;
  const bool busy = recording || recordTask;
  const bool isPlaying = playing;
  if (moduleReady && online && !busy && !isPlaying) {
    ++uiGeneration;
    ++recordGeneration;
    ++playGeneration;
    recording = true;
    stopRequested = false;
    shouldSend = false;
    autoStopped = false;
    recordStartMs = millis();
  }
  const uint32_t generation = uiGeneration;
  portEXIT_CRITICAL(&stateMux);

  if (!moduleReady) { PostStatus("语音模块未就绪", true, generation); return; }
  if (!online) { PostStatus("语音通道未连接", true, generation); return; }
  if (isPlaying) { PostStatus("正在播放语音…", true, generation); return; }
  if (busy) return;

  ClearStatus();
  ResetQueues();
  receiveLength = 0;
  if (!RecordAudioMode(pdMS_TO_TICKS(20))) {
    PlaybackAudioMode(pdMS_TO_TICKS(20));
    portENTER_CRITICAL(&stateMux);
    recording = false;
    portEXIT_CRITICAL(&stateMux);
    PostStatus("麦克风启动失败", true, generation);
    return;
  }
  if (xTaskCreate(RecordingTask, "voice_rec", 8192, nullptr, 5, &recordTask) != pdPASS || !recordTask) {
    recordTask = nullptr;
    portENTER_CRITICAL(&stateMux);
    recording = false;
    portEXIT_CRITICAL(&stateMux);
    PlaybackAudioMode(pdMS_TO_TICKS(20));
    PostStatus("内存不足，无法录音", true, generation);
    return;
  }
  PostStatus("正在录音，请说话…", true, generation);
  Serial.printf("[Voice] recording started heap=%u\n", static_cast<unsigned>(ESP.getFreeHeap()));
}

void StopAndSend() {
  portENTER_CRITICAL(&stateMux);
  if (recording) {
    shouldSend = true;
    stopRequested = true;
  }
  portEXIT_CRITICAL(&stateMux);
}

void CancelAndExit() {
  portENTER_CRITICAL(&stateMux);
  ++uiGeneration;
  ++recordGeneration;
  ++playGeneration;
  recording = false;
  stopRequested = true;
  shouldSend = false;
  autoStopped = false;
  playing = false;
  portEXIT_CRITICAL(&stateMux);
  receiveLength = 0;
  ResetQueues();
  ClearStatus();
  if (ready && !PlaybackAudioMode(pdMS_TO_TICKS(20))) {
    Serial.println("[Voice] I2S restore failed while exiting");
  }
  Serial.println("[Voice] canceled and exited");
}

bool IsRecording() {
  portENTER_CRITICAL(&stateMux); const bool value = recording; portEXIT_CRITICAL(&stateMux); return value;
}
bool IsPlaying() {
  portENTER_CRITICAL(&stateMux); const bool value = playing; portEXIT_CRITICAL(&stateMux); return value;
}
bool IsConnected() {
  portENTER_CRITICAL(&stateMux); const bool value = connected; portEXIT_CRITICAL(&stateMux); return value;
}
bool IsReady() {
  portENTER_CRITICAL(&stateMux); const bool value = ready; portEXIT_CRITICAL(&stateMux); return value;
}
uint32_t GetRecordingDurationMs() {
  portENTER_CRITICAL(&stateMux);
  const uint32_t value = recording ? millis() - recordStartMs : 0;
  portEXIT_CRITICAL(&stateMux);
  return value;
}
void SetVolume(int value) {
  if (value < 0) value = 0;
  if (value > 100) value = 100;
  portENTER_CRITICAL(&stateMux); volume = value; portEXIT_CRITICAL(&stateMux);
}
int GetVolume() {
  portENTER_CRITICAL(&stateMux); const int value = volume; portEXIT_CRITICAL(&stateMux); return value;
}
void SetStatusCallback(StatusFn callback) { statusCallback = callback; }

}  // namespace Voice
