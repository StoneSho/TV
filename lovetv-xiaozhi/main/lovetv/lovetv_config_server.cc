#include "lovetv_config_server.h"

#include "lovetv.h"
#include "lovetv_mqtt.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_log.h>

#include <string>

#define TAG "LoveTvConfig"

namespace {

// 配置页。粉底白字，手机浏览器直接打开设备 IP 即可。
const char kIndexHtml[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>LoveTV 配置</title>
<style>
body{background:#ffaec9;color:#fff;font-family:-apple-system,BlinkMacSystemFont,"Microsoft YaHei",sans-serif;
margin:0;padding:16px}
h1{font-size:20px;margin:8px 0 16px}
.card{background:rgba(255,255,255,.18);border-radius:12px;padding:14px;margin-bottom:14px}
h2{font-size:15px;margin:0 0 10px;font-weight:600}
label{display:block;font-size:13px;margin:10px 0 4px}
input{width:100%;box-sizing:border-box;padding:9px;border:0;border-radius:7px;font-size:15px;
background:#fff;color:#333}
.row{display:flex;gap:8px}
.row>div{flex:1}
button{width:100%;padding:13px;border:0;border-radius:9px;background:#fd638b;color:#fff;
font-size:16px;font-weight:600;margin-top:6px}
.hint{font-size:12px;opacity:.85;margin-top:5px;line-height:1.5}
#msg{text-align:center;padding:10px;font-size:14px;min-height:20px}
a{color:#fff}
</style></head><body>
<h1>LoveTV 配置</h1>

<div class="card">
<h2>天气</h2>
<label>城市代码</label>
<input id="city" placeholder="101120501">
<div class="hint">中国天气网城市代码。默认烟台 101120501；北京 101010100，上海 101020100，广州 101280101，
深圳 101280601，杭州 101210101，成都 101270101，武汉 101200101，西安 101110101，
南京 101190101，石家庄 101090101。其他城市可在 weather.com.cn 搜索后看网址里的 9 位数字。</div>
</div>

<div class="card">
<h2>相识页</h2>
<div class="row">
<div><label>年</label><input id="year" type="number" placeholder="2019"></div>
<div><label>月</label><input id="month" type="number" placeholder="9"></div>
<div><label>日</label><input id="day" type="number" placeholder="1"></div>
</div>
<label>底部文字</label>
<input id="bottom" placeholder="known since 2019.9.1">
<div class="hint">底部文字用的是英文字体，建议只填英文和数字。</div>
<label>一句话</label>
<input id="words" placeholder="Happy Every Day">
</div>

<div class="card">
<h2>消息互通（MQTT）</h2>
<div class="row">
<div><label>本机 ID</label><input id="self" placeholder="A"></div>
<div><label>对方 ID</label><input id="peer" placeholder="B"></div>
</div>
<div class="hint">两台设备互为对方。一台填 A/B，另一台填 B/A。</div>
<label>服务器</label>
<input id="host" placeholder="117.72.44.85">
<label>端口</label>
<input id="port" type="number" placeholder="1883">
<label>语音端口</label>
<input id="wsport" type="number" placeholder="9002">
<div class="hint">对讲机 WebSocket 端口，默认 9002。</div>
<label>用户名</label>
<input id="user" placeholder="lovetv">
<label>密码</label>
<input id="pass" type="password" placeholder="不修改就留空">
<div class="hint">密码只存在设备里，页面不会回显。</div>
</div>

<button onclick="save()">保存</button>
<div id="msg"></div>

<script>
async function load(){
  const r = await fetch('/config');
  const c = await r.json();
  for (const k of ['city','year','month','day','bottom','words','self','peer','host','port','wsport','user'])
    if (c[k] !== undefined && c[k] !== null) document.getElementById(k).value = c[k];
}
async function save(){
  const p = {};
  for (const k of ['city','bottom','words','self','peer','host','user','pass'])
    p[k] = document.getElementById(k).value;
  for (const k of ['year','month','day','port','wsport'])
    p[k] = parseInt(document.getElementById(k).value || '0', 10);
  const m = document.getElementById('msg');
  m.textContent = '保存中...';
  try {
    const r = await fetch('/config', {method:'POST', headers:{'Content-Type':'application/json'},
                                      body: JSON.stringify(p)});
    const t = await r.json();
    m.textContent = t.ok ? '已保存，设备已生效' : ('保存失败: ' + (t.error || ''));
    if (t.ok) document.getElementById('pass').value = '';
  } catch(e) { m.textContent = '保存失败: ' + e; }
}
load();
</script>
</body></html>)HTML";

std::string JsonEscape(const std::string& in) {
    std::string out;
    for (char c : in) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c >= 0 && c < 0x20) {
            // 控制字符直接丢弃，避免产生非法 JSON
        } else {
            out += c;
        }
    }
    return out;
}

std::string JsonStr(cJSON* root, const char* key, const std::string& fallback) {
    auto* item = cJSON_GetObjectItem(root, key);
    if (item != nullptr && cJSON_IsString(item) && item->valuestring != nullptr) {
        return item->valuestring;
    }
    return fallback;
}

int JsonNum(cJSON* root, const char* key, int fallback) {
    auto* item = cJSON_GetObjectItem(root, key);
    if (item != nullptr && cJSON_IsNumber(item) && item->valueint > 0) {
        return item->valueint;
    }
    return fallback;
}

}  // namespace

LoveTvConfigServer& LoveTvConfigServer::GetInstance() {
    static LoveTvConfigServer instance;
    return instance;
}

void LoveTvConfigServer::Start() {
    if (server_ != nullptr) {
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    config.stack_size = 6144;

    esp_err_t err = httpd_start(&server_, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        server_ = nullptr;
        return;
    }

    httpd_uri_t index_uri = {
        .uri = "/", .method = HTTP_GET, .handler = HandleIndex, .user_ctx = nullptr };
    httpd_uri_t get_uri = {
        .uri = "/config", .method = HTTP_GET, .handler = HandleConfigGet, .user_ctx = nullptr };
    httpd_uri_t post_uri = {
        .uri = "/config", .method = HTTP_POST, .handler = HandleConfigPost, .user_ctx = nullptr };

    httpd_register_uri_handler(server_, &index_uri);
    httpd_register_uri_handler(server_, &get_uri);
    httpd_register_uri_handler(server_, &post_uri);

    ESP_LOGI(TAG, "Config server started on port 80");
}

void LoveTvConfigServer::Stop() {
    if (server_ != nullptr) {
        httpd_stop(server_);
        server_ = nullptr;
    }
}

esp_err_t LoveTvConfigServer::HandleIndex(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, kIndexHtml, HTTPD_RESP_USE_STRLEN);
}

esp_err_t LoveTvConfigServer::HandleConfigGet(httpd_req_t* req) {
    Settings settings("lovetv");
    std::string json = "{";
    json += "\"city\":\"" + JsonEscape(settings.GetString("city", "101120501")) + "\",";
    json += "\"year\":" + std::to_string(settings.GetInt("year", 2019)) + ",";
    json += "\"month\":" + std::to_string(settings.GetInt("month", 9)) + ",";
    json += "\"day\":" + std::to_string(settings.GetInt("day", 1)) + ",";
    json += "\"bottom\":\"" + JsonEscape(settings.GetString("bottom", "known since 2019.9.1")) + "\",";
    json += "\"words\":\"" + JsonEscape(settings.GetString("words", "Happy Every Day")) + "\",";
    json += "\"self\":\"" + JsonEscape(settings.GetString("self_id", "A")) + "\",";
    json += "\"peer\":\"" + JsonEscape(settings.GetString("peer_id", "B")) + "\",";
    json += "\"host\":\"" + JsonEscape(settings.GetString("mqtt_host", "117.72.44.85")) + "\",";
    json += "\"port\":" + std::to_string(settings.GetInt("mqtt_port", 1883)) + ",";
    json += "\"wsport\":" + std::to_string(settings.GetInt("ws_port", 9002)) + ",";
    json += "\"user\":\"" + JsonEscape(settings.GetString("mqtt_user", "lovetv")) + "\"";
    // 密码不回显
    json += "}";

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json.c_str(), HTTPD_RESP_USE_STRLEN);
}

esp_err_t LoveTvConfigServer::HandleConfigPost(httpd_req_t* req) {
    if (req->content_len == 0 || req->content_len > 2048) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"bad length\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    std::string body;
    body.resize(req->content_len);
    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, &body[received], req->content_len - received);
        if (ret <= 0) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_send(req, "{\"ok\":false,\"error\":\"recv\"}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        }
        received += ret;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":false,\"error\":\"json\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    {
        Settings settings("lovetv", true);
        settings.SetString("city", JsonStr(root, "city", "101120501"));
        settings.SetInt("year", JsonNum(root, "year", 2019));
        settings.SetInt("month", JsonNum(root, "month", 9));
        settings.SetInt("day", JsonNum(root, "day", 1));
        settings.SetString("bottom", JsonStr(root, "bottom", "known since 2019.9.1"));
        settings.SetString("words", JsonStr(root, "words", "Happy Every Day"));
        settings.SetString("self_id", JsonStr(root, "self", "A"));
        settings.SetString("peer_id", JsonStr(root, "peer", "B"));
        settings.SetString("mqtt_host", JsonStr(root, "host", "117.72.44.85"));
        settings.SetInt("mqtt_port", JsonNum(root, "port", 1883));
        settings.SetInt("ws_port", JsonNum(root, "wsport", 9002));
        settings.SetString("mqtt_user", JsonStr(root, "user", "lovetv"));

        // 密码留空表示不修改
        std::string pass = JsonStr(root, "pass", "");
        if (!pass.empty()) {
            settings.SetString("mqtt_pass", pass);
        }
    }
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Config saved from web page");
    LoveTv::GetInstance().ReloadConfig();
    // MQTT 的服务器/账号/设备 ID 可能变了，让 MQTT 任务重连
    LoveTvMqtt::GetInstance().ReloadConfig();

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}
