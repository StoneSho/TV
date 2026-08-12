#ifndef LOVETV_CONFIG_SERVER_H
#define LOVETV_CONFIG_SERVER_H

#include <esp_http_server.h>

// 局域网配置页
// 设备联网后在 80 端口提供一个网页，浏览器访问设备 IP 即可修改：
//   城市代码 / 纪念日 / 底部文字 / 情话 / 设备 ID / MQTT 参数
// 不需要重新进配网模式，保存后立即生效。
class LoveTvConfigServer {
public:
    static LoveTvConfigServer& GetInstance();

    void Start();
    void Stop();

private:
    LoveTvConfigServer() = default;
    ~LoveTvConfigServer() = default;

    httpd_handle_t server_ = nullptr;

    static esp_err_t HandleIndex(httpd_req_t* req);
    static esp_err_t HandleConfigGet(httpd_req_t* req);
    static esp_err_t HandleConfigPost(httpd_req_t* req);
};

#endif // LOVETV_CONFIG_SERVER_H
