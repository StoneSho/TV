#ifndef LOVETV_VOICE_H
#define LOVETV_VOICE_H

#include <Arduino.h>

// LoveTV 语音对讲模块
// IMA ADPCM 4bit @16kHz，30ms/帧（480 样本 -> 240 字节）
// 半双工：VOL- 短按开始/结束录音；播放队列自动播放收到的语音
namespace Voice {

    void Init(const String& host, int port, const String& selfId, const String& peerId);

    // 在 Arduino loop() 中轮询：WebSocket 事件、心跳、发送队列、UI 状态
    void Loop();

    void StartRecording();
    void StopAndSend();
    // 非阻塞退出语音页：取消录音、停止播放并丢弃所有待处理事件。
    void CancelAndExit();
    bool IsRecording();
    bool IsPlaying();
    bool IsConnected();
    bool IsReady();
    uint32_t GetRecordingDurationMs();

    // 语音信箱：设备端只保留少量元数据，音频由服务器流式下发
    int GetMailUnreadCount();
    bool IsMailPlaying();
    void PlayNextMail();

    void SetVolume(int v);   // 0-100
    int  GetVolume();

    // 状态回调（showPage=true 表示需要切到语音页显示该状态）
    typedef void (*StatusFn)(const String& status, bool showPage);
    void SetStatusCallback(StatusFn fn);
}

#endif
