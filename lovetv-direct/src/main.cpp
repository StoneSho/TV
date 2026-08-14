/* *****************************************************************
 *  库文件、头文件
 * *****************************************************************/
#include <ArduinoJson.h>
#include <Button2.h> //按钮库
#include <HTTPClient.h>
#include <SPI.h>
#include <StaticThreadController.h> //协程控制
#include <TFT_eSPI.h>
#include <TJpg_Decoder.h>
//使用Tjpg_Decoder库创建C数组图片的网址: http://tomeko.net/online_tools/file_to_hex.php?lang=en 
#include <Thread.h> //协程
#include <TimeLib.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <SetWiFi.h>

#include <MqttLovetv.h>
#include <voice.h>

#include "Animate/Animate.h" //动画模块
#include "config.h"          //配置文件
#include "loveimg/Anilove.h"
#include "weatherNum/weatherNum.h" //天气图库
#include "misspage/missu.h"

#define Version "MINI TV UI 2.0"
#define CLOCK_PAGE 0
#define LOVE_PAGE 1
#define MISSU_PAGE 2
#define VOICE_PAGE 3
/* *****************************************************************
 *  配置使能位
 * *****************************************************************/

// 按钮定义（指定 INPUT_PULLUP 模式，因为按键是低电平有效）
// BOOT 硬件异常已放弃，不再创建 Button2 对象，避免 GPIO0 卡低导致误触。
Button2 Button_vol_up = Button2(BTN_VOL_UP, INPUT_PULLUP);     // VOL+ 键（低电平有效）
Button2 Button_vol_down = Button2(BTN_VOL_DOWN, INPUT_PULLUP); // VOL- 键（低电平有效）
/* *****************************************************************
 *  字库、图片库
 * *****************************************************************/
#include "font/Chancery_L_20.h" //字体库
#include "font/KT_20.h"
#include "font/LoveTV_UI_18.h"
#include "font/ZdyLwFont_20.h"  //字体库
#include "font/timeClockFont.h" //字体库
#include "img/humidity.h"       //湿度图标
#include "img/temperature.h"    //温度图标

#include "img/battery.h"
#include "img/love.h"
#include "img/love1.h"
#include "img/setWiFi_img.h"

/* *****************************************************************
 *  函数声明
 * *****************************************************************/
void sendNTPpacket(IPAddress &address); // 向NTP服务器发送请求
time_t getNtpTime();                    // 从NTP获取时间
void digitalClockDisplay(int reflash_en);
void printDigits(int digits);
String num2str(int digits);
void LCD_reflash();
void clear_screen();
void getCityWeather();          // 获取城市天气
void saveParamCallback();
void esp_reset(Button2 &btn);
void scrollBanner();
void weaterData(String *dataSK, String *dataFC); // 天气信息写到屏幕上
void refresh_AnimatedImage();    // 更新右下角
void goto_clock_page();
void Battery();
// 纪念日页面函数
void show_mg_love_day();
void show_love_day(int reflash_en);
unsigned int get_love_day(time_t start_love);
void show_mg_love_page();
void refresh_loveImage();
void show_default_words();
void goto_love_page();
// 想你页面
void handle_serial_debug();
void on_boot_click();
void on_boot_long();
void apply_backlight_brightness();
void cycle_brightness();
void save_brightness();
void save_volume(int volume);
void on_vol_up_click(Button2 &btn);
void on_vol_up_double(Button2 &btn);
void on_vol_up_long(Button2 &btn);
void on_vol_down_click(Button2 &btn);
void on_vol_down_double(Button2 &btn);
void on_vol_down_long(Button2 &btn);
void send_imu();
void on_mqtt_message(const String &type);
void on_voice_status(const String &status, bool showPage);
void goto_missu_page(String word, uint16_t word_color);
void goto_voice_page(String status, bool showPage);
void draw_voice_page();
void show_notify(String text);
void goto_page(byte page);
//*********************时钟线程****************************
// 创建时间更新函数线程
Thread reflash_time = Thread();
// 创建副标题切换线程
Thread reflash_Banner = Thread();
// 创建恢复WIFI链接
Thread reflash_openWifi = Thread();
// 创建动画绘制线程
Thread reflash_Animate = Thread();
// 电池电压检测
Thread reflash_Battery = Thread();
// 联网后所有需要更新的数据
Thread WIFI_reflash = Thread();
// 创建协程池
StaticThreadController<5> controller(&reflash_time, &reflash_Banner, &reflash_openWifi, &reflash_Animate, &reflash_Battery);

//********************love线程******************************
// 创建四毛线程
Thread reflash_Love_Animate = Thread();
// 创建
Thread reflash_Love_openWifi = Thread();
StaticThreadController<2> controller_love(&reflash_Love_Animate, &reflash_Love_openWifi);

/* *****************************************************************
 *  参数设置
 * *****************************************************************/
// 天气更新时间  X 分钟（默认1分钟）
unsigned int updateweater_time = WEATHER_UPDATE_TIME;
// 天气信息
struct Weather_Msg {
  String dataSK;
  String dataFC;
};

Weather_Msg weather_msg = {{""}, {""}};
//----------------------------------------------------

// LCD屏幕相关设置
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite clk = TFT_eSprite(&tft);
uint16_t bgColor = 0x0000;
uint16_t pinkColor = tft.color565(255, 174, 201);
uint16_t zongseColor = tft.color565(128, 64, 64);
uint16_t whiteColor = tft.color565(245, 246, 247);
uint16_t fhColor = tft.color565(253, 99, 139);
uint16_t blueColor = tft.color565(175, 221, 224);
// 其余状态标志位
int LCD_Rotation = 0; // LCD屏幕方向
uint8_t brightness = 60;
constexpr uint8_t BACKLIGHT_LEDC_CHANNEL = 0;
constexpr uint32_t BACKLIGHT_LEDC_FREQUENCY = 20000;
constexpr uint8_t BACKLIGHT_LEDC_RESOLUTION = 10;
constexpr uint16_t BACKLIGHT_LEDC_MAX_DUTY = (1U << BACKLIGHT_LEDC_RESOLUTION) - 1;

uint8_t Wifi_en = 1;         // WIFI模块启动  1：打开    0：关闭
bool wifi_disconnect_shown = false; // 断网提示只显示一次，避免反复劫持页面
uint8_t UpdateWeater_en = 0; // 更新时间标志位
int prevTime = 0;            // 滚动显示更新标志位
time_t love_start_time = 0;
int Mode = 2;
time_t prevDisplay = 0;       // 显示时间显示记录
int Amimate_reflash_Time = 0; // 更新时间记录
uint32_t mode_switch_ms = 0;  // 胡桃/宇航员上次切换时间
int Amilove_reflash_Time = 0; // 更新时间记录
int connectTimes = 0;

/*** Component objects ***/
WeatherNum wrat;

uint32_t targetTime = 0;
int tempnum = 0;               // 温度百分比
int huminum = 0;               // 湿度百分比
int tempcol = 0xffff;          // 温度显示颜色
int humicol = 0xffff;          // 湿度显示颜色

// NTP服务器参数
static const char ntpServerName[] = "ntp6.aliyun.com";
const int timeZone = 8; // 东八区

// wifi连接UDP设置参数
WiFiUDP Udp;
WiFiClient wificlient;
unsigned int localPort = 8000;
float duty = 0;

// 显示页面
byte page = CLOCK_PAGE; // 默认时钟页面
byte pre_page = CLOCK_PAGE; // 切换前一个页面,后面单击退出时能够回到之前的页面
byte role = RECEIVER;
enum MessageState : uint8_t {
  MESSAGE_NONE,
  MESSAGE_WAITING_ACK,
  MESSAGE_RECEIVED_IMU,
  MESSAGE_ACKED
};
MessageState messageState = MESSAGE_NONE;
uint32_t missu_auto_close_ms = 0; // 消息页自动关闭时间点（0=不自动关闭）

String words = DEFAULT_WORDS;

// 可配置参数（网页配置 -> NVS，重启生效）
String selfId = DEFAULT_SELF_ID;
String peerId = DEFAULT_PEER_ID;
String mqttHost = DEFAULT_MQTT_HOST;
int mqttPort = DEFAULT_MQTT_PORT;
String voiceHost = DEFAULT_VOICE_HOST;
int voicePort = DEFAULT_VOICE_PORT;
int cfgYear = YEAR;
int cfgMonth = MONTH;
int cfgDay = DAY;
String bottomWord = BUTTOM_WORD;
String voiceStatus = "语音对讲";
uint32_t lastVoiceDraw = 0;

// 长按调音量后 500ms 内的"单击"视为松手误触，忽略掉
volatile long vol_up_long_ms = -100000;
volatile long vol_down_long_ms = -100000;

// 星期
String week() {
  String wk[7] = {"日", "一", "二", "三", "四", "五", "六"};
  String s = "周" + wk[weekday() - 1];
  return s;
}

// 月日
String monthDay() {
  String s = String(month());
  s = s + "月" + day() + "日";
  return s;
}

/* *****************************************************************
 *  函数
 * *****************************************************************/

// TFT屏幕输出函数
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  if (y >= tft.height())
    return 0;
  tft.pushImage(x, y, w, h, bitmap);
  return 1;
}

// 进度条函数
byte loadNum = 6;
void loading(byte delayTime){
  clk.setColorDepth(8);

  clk.createSprite(200, 100); // 创建窗口
  clk.fillSprite(0x0000);     // 填充率

  clk.drawRoundRect(0, 0, 200, 16, 8, 0xFFFF);     // 空心圆角矩形
  clk.fillRoundRect(3, 3, loadNum, 10, 5, 0xFFFF); // 实心圆角矩形
  clk.setTextDatum(CC_DATUM);                      // 设置文本数据
  clk.setTextColor(TFT_GREEN, 0x0000);
  clk.drawString("Connecting to WiFi......", 100, 40, 2);
  clk.setTextColor(TFT_WHITE, 0x0000);
  clk.drawRightString(Version, 180, 60, 2);
  clk.pushSprite(20, 120); // 窗口位置

  clk.deleteSprite();
  loadNum += 1;
  delay(delayTime);
}

//强制门户Web配网
bool setWiFi_Flag = false;
void setWiFi() {  //设置WiFi
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(tft_output);
  TJpgDec.drawJpg(0,0,setWiFi_img, sizeof(setWiFi_img));

  initBasic();
  initSoftAP();
  initWebServer();
  initDNS();
  while(setWiFi_Flag == false) {
    server.handleClient();
    dnsServer.processNextRequest();
    if(WiFi.status() == WL_CONNECTED) {
      server.stop();
      setWiFi_Flag = true;
    }
  }
}


// 湿度图标显示函数
void humidityWin() {
  clk.setColorDepth(8);

  huminum = huminum / 2;
  clk.createSprite(52, 6); // 创建窗口
  clk.fillSprite(0x0000);  // 填充率
  clk.drawRoundRect(
      0, 0, 52, 6, 3,
      0xFFFF); // 空心圆角矩形  起始位x,y,长度，宽度，圆弧半径，颜色
  clk.fillRoundRect(1, 1, huminum, 4, 2, humicol); // 实心圆角矩形
  clk.pushSprite(45, 222);                         // 窗口位置
  clk.deleteSprite();
}

// 温度图标显示函数
void tempWin() {
  clk.setColorDepth(8);

  clk.createSprite(52, 6); // 创建窗口
  clk.fillSprite(0x0000);  // 填充率
  clk.drawRoundRect(
      0, 0, 52, 6, 3,
      0xFFFF); // 空心圆角矩形  起始位x,y,长度，宽度，圆弧半径，颜色
  clk.fillRoundRect(1, 1, tempnum, 4, 2, tempcol); // 实心圆角矩形
  clk.pushSprite(45, 192);                         // 窗口位置
  clk.deleteSprite();
}

// WEB配网LCD显示函数
void Web_win() {
  clk.setColorDepth(8);

  clk.createSprite(200, 60); // 创建窗口
  clk.fillSprite(0x0000);    // 填充率

  clk.setTextDatum(CC_DATUM); // 设置文本数据
  clk.setTextColor(TFT_GREEN, 0x0000);
  clk.drawString("WiFi Connect Fail!", 100, 10, 2);
  clk.drawString("SSID:", 45, 40, 2);
  clk.setTextColor(TFT_WHITE, 0x0000);
  clk.loadFont(LoveTV_UI_18);
  clk.drawString("zwz的小电视", 125, 40);
  clk.unloadFont();
  clk.pushSprite(20, 50); // 窗口位置

  //clk.deleteSprite();
}

// 获取城市天气
void getCityWeather() {
  // String URL = "http://d1.weather.com.cn/dingzhi/" + cityCode +
  // ".html?_="+String(now());//新
  String URL = "http://d1.weather.com.cn/weather_index/" + cityCode + ".html?_=" + String(now()); // 原来
  // 创建 HTTPClient 对象
  HTTPClient httpClient;

  // httpClient.begin(URL);
  httpClient.begin(wificlient, URL); // 使用新方法
  httpClient.setTimeout(1500);       // 避免天气请求长时间阻塞语音处理

  // 设置请求头中的User-Agent
  httpClient.setUserAgent("Mozilla/5.0 (iPhone; CPU iPhone OS 11_0 like Mac "
                          "OS X) AppleWebKit/604.1.38 (KHTML, like Gecko) "
                          "Version/11.0 Mobile/15A372 Safari/604.1");
  httpClient.addHeader("Referer", "http://www.weather.com.cn/");

  // 启动连接并发送HTTP请求
  int httpCode = httpClient.GET();
  Serial.println("正在获取天气数据");
  Serial.println(URL);

  // 如果服务器响应OK则从服务器获取响应体信息并通过串口输出
  if (httpCode == HTTP_CODE_OK) {

    String str = httpClient.getString();
    // Serial.println("httpdata=" + str);
    int indexStart = str.indexOf("dataSK =");
    int indexEnd = str.indexOf(";var dataZS");
    weather_msg.dataSK = str.substring(indexStart + 8, indexEnd);
    // Serial.println(jsonDataSK);

    indexStart = str.indexOf("\"f\":[");
    indexEnd = str.indexOf(",{\"fa");
    weather_msg.dataFC = str.substring(indexStart + 5, indexEnd);
    // Serial.println(jsonFC);

    weaterData(&(weather_msg.dataSK), &(weather_msg.dataFC));
    Serial.println("获取成功");
  } else {
    Serial.println("请求城市天气错误：");
    Serial.print(httpCode);
  }

  // 关闭ESP32与服务器连接
  httpClient.end();
}

constexpr size_t BANNER_ITEM_COUNT = 6;
String scrollText[BANNER_ITEM_COUNT] = {
    "实时天气 --", "空气质量 --", "风向 --",
    "最低温度 --℃", "最高温度 --℃", "IP --"};
// int scrollTextWidth = 0;

String jsonTextOr(JsonVariantConst value, const String &fallback) {
  if (value.isNull()) {
    return fallback;
  }
  String text = value.as<String>();
  text.trim();
  if (text.length() == 0 || text == "null") {
    return fallback;
  }
  return text;
}

// 天气信息写到屏幕上
void weaterData(String *dataSK, String *dataFC) {
  // 解析第一段JSON
  DynamicJsonDocument doc(1024);
  DeserializationError skError = deserializeJson(doc, *dataSK);
  if (skError) {
    Serial.printf("天气实况 JSON 解析失败: %s\n", skError.c_str());
    return;
  }
  JsonObject sk = doc.as<JsonObject>();

  const String tempText = jsonTextOr(sk["temp"], "--");
  const String humidityText = jsonTextOr(sk["SD"], "--");
  const String cityName = jsonTextOr(sk["cityname"], "--");
  const String weatherText = jsonTextOr(sk["weather"], "--");
  const String windDirection = jsonTextOr(sk["WD"], "--");
  const String windLevel = jsonTextOr(sk["WS"], "");
  const String weatherCode = jsonTextOr(sk["weathercode"], "");

  /***绘制相关文字***/
  clk.setColorDepth(8);
  clk.loadFont(ZdyLwFont_20);

  // 温度
  clk.createSprite(58, 24);
  clk.fillSprite(bgColor);
  clk.setTextDatum(CC_DATUM);
  clk.setTextColor(TFT_WHITE, bgColor);
  clk.drawString(tempText + "℃", 28, 13);
  clk.pushSprite(100, 184);
  clk.deleteSprite();
  tempnum = (tempText == "--") ? 0 : tempText.toInt();
  tempnum = tempnum + 10;
  if (tempnum < 10)
    tempcol = 0x00FF;
  else if (tempnum < 28)
    tempcol = 0x0AFF;
  else if (tempnum < 34)
    tempcol = 0x0F0F;
  else if (tempnum < 41)
    tempcol = 0xFF0F;
  else if (tempnum < 49)
    tempcol = 0xF00F;
  else {
    tempcol = 0xF00F;
    tempnum = 50;
  }
  tempWin();

  // 湿度
  clk.createSprite(58, 24);
  clk.fillSprite(bgColor);
  clk.setTextDatum(CC_DATUM);
  clk.setTextColor(TFT_WHITE, bgColor);
  clk.drawString(humidityText, 28, 13);
  // clk.drawString("100%",28,13);
  clk.pushSprite(100, 214);
  clk.deleteSprite();
  // String A = sk["SD"].as<String>();
  huminum = (humidityText == "--") ? 0 : humidityText.toInt();

  if (huminum > 90)
    humicol = 0x00FF;
  else if (huminum > 70)
    humicol = 0x0AFF;
  else if (huminum > 40)
    humicol = 0x0F0F;
  else if (huminum > 20)
    humicol = 0xFF0F;
  else
    humicol = 0xF00F;
  humidityWin();

  // 城市名称
  clk.createSprite(94, 30);
  clk.fillSprite(bgColor);
  clk.setTextDatum(CC_DATUM);
  clk.setTextColor(TFT_WHITE, bgColor);
  clk.drawString(cityName, 44, 16);
  clk.pushSprite(15, 15);
  clk.deleteSprite();

  // PM2.5空气指数
  uint16_t pm25BgColor = tft.color565(156, 202, 127); // 优
  String aqiTxt = "优";
  int pm25V = sk["aqi"] | 0;
  if (pm25V > 200) {
    pm25BgColor = tft.color565(136, 11, 32); // 重度
    aqiTxt = "重度";
  } else if (pm25V > 150) {
    pm25BgColor = tft.color565(186, 55, 121); // 中度
    aqiTxt = "中度";
  } else if (pm25V > 100) {
    pm25BgColor = tft.color565(242, 159, 57); // 轻
    aqiTxt = "轻度";
  } else if (pm25V > 50) {
    pm25BgColor = tft.color565(247, 219, 100); // 良
    aqiTxt = "良";
  }
  clk.createSprite(56, 24);
  clk.fillSprite(bgColor);
  clk.fillRoundRect(0, 0, 50, 24, 4, pm25BgColor);
  clk.setTextDatum(CC_DATUM);
  clk.setTextColor(0x0000);
  clk.drawString(aqiTxt, 25, 13);
  clk.pushSprite(104, 18);
  clk.deleteSprite();

  scrollText[0] = "实时天气 " + weatherText;
  scrollText[1] = "空气质量 " + aqiTxt;
  scrollText[2] = "风向 " + windDirection + windLevel;

  // 天气图标代码来自实时天气数据。
  // atoi((sk["weathercode"].as<String>()).substring(1,3).c_str()) ;

  // 天气图标
  if (weatherCode.length() >= 3) {
    wrat.printfweather(170, 15, weatherCode.substring(1, 3).toInt());
  }

  // 左上角滚动字幕中的最低/最高温度来自预报数据。
  DynamicJsonDocument forecastDoc(1024);
  DeserializationError forecastError = deserializeJson(forecastDoc, *dataFC);
  if (!forecastError) {
    JsonObject fc = forecastDoc.as<JsonObject>();
    scrollText[3] = "最低温度" + jsonTextOr(fc["fd"], "--") + "℃";
    scrollText[4] = "最高温度" + jsonTextOr(fc["fc"], "--") + "℃";
  } else {
    Serial.printf("温度预报 JSON 解析失败: %s\n", forecastError.c_str());
  }

  clk.unloadFont();
}

size_t currentIndex = 0;
TFT_eSprite clkb = TFT_eSprite(&tft);

void scrollBanner() {
  for (size_t checked = 0; checked < BANNER_ITEM_COUNT; checked++) {
    scrollText[currentIndex].trim();
    if (scrollText[currentIndex].length() > 0 && scrollText[currentIndex] != "null") {
      break;
    }
    currentIndex = (currentIndex + 1) % BANNER_ITEM_COUNT;
  }
  if (scrollText[currentIndex].length() > 0 && scrollText[currentIndex] != "null") {
    clkb.setColorDepth(8);
    clkb.loadFont(ZdyLwFont_20);
    clkb.createSprite(150, 30);
    clkb.fillSprite(bgColor);
    clkb.setTextWrap(false);
    clkb.setTextDatum(CC_DATUM);
    clkb.setTextColor(TFT_WHITE, bgColor);
    clkb.drawString(scrollText[currentIndex], 74, 16);
    clkb.pushSprite(10, 45);

    clkb.deleteSprite();
    clkb.unloadFont();

    if (currentIndex >= BANNER_ITEM_COUNT - 1)
      currentIndex = 0; // 回第一个
    else
      currentIndex += 1; // 准备切换到下一个
  }
  prevTime = 1;
}
// 用快速线方法绘制数字
void drawLineFont(uint32_t _x, uint32_t _y, uint32_t _num, uint32_t _size, uint32_t _color) {
  uint32_t fontSize;
  const LineAtom *fontOne;
  // 小号(9*14)
  if (_size == 1) {
    fontOne = smallLineFont[_num];
    fontSize = smallLineFont_size[_num];
    // 绘制前清理字体绘制区域
    tft.fillRect(_x, _y, 9, 14, TFT_BLACK);
  }
  // 中号(18*30)
  else if (_size == 2) {
    fontOne = middleLineFont[_num];
    fontSize = middleLineFont_size[_num];
    // 绘制前清理字体绘制区域
    tft.fillRect(_x, _y, 18, 30, TFT_BLACK);
  }
  // 大号(36*90)
  else if (_size == 3) {
    fontOne = largeLineFont[_num];
    fontSize = largeLineFont_size[_num];
    // 绘制前清理字体绘制区域
    tft.fillRect(_x, _y, 36, 90, TFT_BLACK);
  } else
    return;

  for (uint32_t i = 0; i < fontSize; i++) {
    tft.drawFastHLine(fontOne[i].xValue + _x, fontOne[i].yValue + _y, fontOne[i].lValue, _color);
  }
}

int Hour_sign = 60;
int Minute_sign = 60;
int Second_sign = 60;
// 日期刷新
void digitalClockDisplay(int reflash_en = 0) {
  // 时钟刷新,输入1强制刷新
  int now_hour = hour();     // 获取小时
  int now_minute = minute(); // 获取分钟
  int now_second = second(); // 获取秒针
  // 小时刷新
  if ((now_hour != Hour_sign) || (reflash_en == 1)) {
    drawLineFont(20, timeY, now_hour / 10, 3, SD_FONT_WHITE);
    drawLineFont(60, timeY, now_hour % 10, 3, SD_FONT_WHITE);
    Hour_sign = now_hour;
  }
  // 分钟刷新
  if ((now_minute != Minute_sign) || (reflash_en == 1)) {
    drawLineFont(101, timeY, now_minute / 10, 3, SD_FONT_YELLOW);
    drawLineFont(141, timeY, now_minute % 10, 3, SD_FONT_YELLOW);
    Minute_sign = now_minute;
  }
  // 秒针刷新
  if ((now_second != Second_sign) || (reflash_en == 1)) // 分钟刷新
  {
    drawLineFont(182, timeY + 30, now_second / 10, 2, SD_FONT_WHITE);
    drawLineFont(202, timeY + 30, now_second % 10, 2, SD_FONT_WHITE);
    Second_sign = now_second;
  }

  if (reflash_en == 1)
    reflash_en = 0;
  /***日期****/
  clk.setColorDepth(8);
  clk.loadFont(ZdyLwFont_20);

  // 星期
  clk.createSprite(58, 30);
  clk.fillSprite(bgColor);
  clk.setTextDatum(CC_DATUM);
  clk.setTextColor(TFT_WHITE, bgColor);
  clk.drawString(week(), 29, 16);
  clk.pushSprite(102, 150);
  clk.deleteSprite();

  // 月日
  clk.createSprite(95, 30);
  clk.fillSprite(bgColor);
  clk.setTextDatum(CC_DATUM);
  clk.setTextColor(TFT_WHITE, bgColor);
  clk.drawString(monthDay(), 49, 16);
  clk.pushSprite(5, 150);
  clk.deleteSprite();

  clk.unloadFont();
  /***日期****/
}

/*-------- NTP code ----------*/

const int NTP_PACKET_SIZE = 48; // NTP时间在消息的前48字节中
byte
    packetBuffer[NTP_PACKET_SIZE]; // buffer to hold incoming & outgoing packets

time_t getNtpTime() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("NTP skipped: WiFi disconnected");
    return 0;
  }

  IPAddress ntpServerIP;
  if (WiFi.hostByName(ntpServerName, ntpServerIP) != 1) {
    Serial.println("NTP DNS lookup failed");
    return 0;
  }

  for (uint8_t attempt = 0; attempt < 3; ++attempt) {
    while (Udp.parsePacket() > 0) {
      Udp.flush();
    }
    sendNTPpacket(ntpServerIP);
    uint32_t beginWait = millis();
    while (millis() - beginWait < 1500) {
      int size = Udp.parsePacket();

      if (size >= NTP_PACKET_SIZE) {
        Serial.println("Receive NTP Response");
        Udp.read(packetBuffer, NTP_PACKET_SIZE); // read packet into the buffer
        unsigned long secsSince1900;
        // convert four bytes starting at location 40 to a long integer
        secsSince1900 = (unsigned long)packetBuffer[40] << 24;
        secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
        secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
        secsSince1900 |= (unsigned long)packetBuffer[43];
        // Serial.println(secsSince1900 - 2208988800UL + timeZone * SECS_PER_HOUR);
        return secsSince1900 - 2208988800UL + timeZone * SECS_PER_HOUR;
      }
      delay(10);
    }
  }
  Serial.println("NTP request timed out");
  return 0;
}

// 向NTP服务器发送请求
void sendNTPpacket(IPAddress &address) {
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011; // LI, Version, Mode
  packetBuffer[1] = 0;          // Stratum, or type of clock
  packetBuffer[2] = 6;          // Polling Interval
  packetBuffer[3] = 0xEC;       // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12] = 49;
  packetBuffer[13] = 0x4E;
  packetBuffer[14] = 49;
  packetBuffer[15] = 52;
  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  Udp.beginPacket(address, 123); // NTP requests are to port 123
  Udp.write(packetBuffer, NTP_PACKET_SIZE);
  Udp.endPacket();
}

void esp_reset(Button2 &btn) { 
  ESP.restart(); 
}

// 更新时间
void reflashTime() {
  prevDisplay = now();
  // timeClockDisplay(1);
  digitalClockDisplay();
  prevTime = 0;
}

// 切换天气 or 空气质量
void reflashBanner() { scrollBanner(); }

// 所有需要联网后更新的方法都放在这里
void WIFI_reflash_All() {
  if (Wifi_en == 1) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("WIFI connected");
      wifi_disconnect_shown = false;

      // 语音录音/播放期间跳过天气和 NTP，避免阻塞 Voice::Loop()
      if (Voice::IsRecording() || Voice::IsPlaying()) {
        Wifi_en = 0;
        return;
      }

      // Serial.println("getCityWeather start");
      getCityWeather();
      // Serial.println("getCityWeather end");

      getNtpTime();
      // 其他需要联网的方法写在后面

      // WiFi.forceSleepBegin(); // Wifi Off
      Serial.println("WIFI sleep......");
      Wifi_en = 0;
    } else {
      // Serial.println("WIFI unconnected");
      if (!wifi_disconnect_shown) {
        wifi_disconnect_shown = true;
        goto_missu_page("wifi unconnected", TFT_RED);
      }
      // 避免回到主页后每轮循环再次强制跳页；下次 openWifi 再检查
      Wifi_en = 0;
    }
  }
}

// 打开WIFI
void openWifi() {
  Serial.println("WIFI reset......");
  // WiFi.forceSleepWake(); // wifi on
  Wifi_en = 1;
}

// 强制屏幕刷新
void LCD_reflash() {
  reflashTime();
  reflashBanner();
  openWifi();
}

// 清空屏幕
void clear_screen() { tft.fillScreen(bgColor); }

// 守护线程池
void Supervisor_controller() {
  if (controller.shouldRun()) {
    // Serial.println("controller 启动");
    controller.run();
  }
}

void Supervisor_love_controller() {
  if (controller_love.shouldRun()) {
    controller_love.run();
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("LoveTV direct boot...");

  // VOL+ 单击：发送"想你"消息 / 消息页确认退出；长按：音量+
  Button_vol_up.setLongClickTime(600);
  Button_vol_up.setClickHandler(on_vol_up_click);
  Button_vol_up.setLongClickDetectedHandler(on_vol_up_long);
  Button_vol_up.setDoubleClickTime(300);
  Button_vol_up.setDoubleClickHandler(on_vol_up_double);

  // VOL- 单击：开始/结束录音；双击：调整亮度；长按：音量-
  Button_vol_down.setLongClickTime(600);
  Button_vol_down.setClickHandler(on_vol_down_click);
  Button_vol_down.setLongClickDetectedHandler(on_vol_down_long);
  Button_vol_down.setDoubleClickTime(300);
  Button_vol_down.setDoubleClickHandler(on_vol_down_double);

  pinMode(LCD_BL_PIN, OUTPUT);
  digitalWrite(LCD_BL_PIN, LOW);
  const uint32_t backlightFrequency = ledcSetup(
      BACKLIGHT_LEDC_CHANNEL, BACKLIGHT_LEDC_FREQUENCY, BACKLIGHT_LEDC_RESOLUTION);
  if (backlightFrequency == 0) {
    Serial.println("[backlight] LEDC setup failed");
  }
  ledcAttachPin(LCD_BL_PIN, BACKLIGHT_LEDC_CHANNEL);
  ledcWrite(BACKLIGHT_LEDC_CHANNEL, 0);

  tft.init(); /* TFT init */
  tft.invertDisplay(1); //反转所有显示颜色：1反转，0正常
  // tft.setSwapBytes(true); //使图片颜色由RGB->BGR
  tft.setRotation(LCD_Rotation);
  Serial.println("TFT bus: SPI3_HOST, mode 3, 80MHz");

  // TFT_eSPI 初始化会把 GPIO13 配置成普通数字输出并置高，覆盖之前的 LEDC 绑定。
  // 这里重新挂回 LEDC，后续 ledcWrite 才能真正调整背光亮度。
  ledcAttachPin(LCD_BL_PIN, BACKLIGHT_LEDC_CHANNEL);
  apply_backlight_brightness();
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_BLACK, bgColor);
  Serial.println("TFT init ok");

  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  TJpgDec.setCallback(tft_output);

  /* 首次使用自动进入配网模式；正常重启只读 NVS，不清除已有配置。 */
  bool wifiConfigLoaded = preferences.begin("wifi", true);
  if (wifiConfigLoaded) {
    PrefSSID = preferences.getString("ssid", "");
    PrefPassword = preferences.getString("password", "");
    cityCode = preferences.getString("citycode", "");
    preferences.end();
  }
  const bool credentialsValid = wifiConfigLoaded && PrefSSID.length() > 0 &&
                                PrefSSID.length() <= 32 && PrefPassword.length() <= 63;
  Serial.printf("[config] wifi_loaded=%d ssid_length=%u password_length=%u\n",
                credentialsValid ? 1 : 0,
                static_cast<unsigned>(PrefSSID.length()),
                static_cast<unsigned>(PrefPassword.length()));

  // LoveTV 扩展配置（网页写入，重启生效）
  int savedVolume = 40;
  if (preferences.begin("lovetv", true)) {
    cfgYear = preferences.getInt("year", YEAR);
    cfgMonth = preferences.getInt("month", MONTH);
    cfgDay = preferences.getInt("day", DAY);
    bottomWord = preferences.getString("bottom", BUTTOM_WORD);
    selfId = preferences.getString("self_id", DEFAULT_SELF_ID);
    peerId = preferences.getString("peer_id", DEFAULT_PEER_ID);
    brightness = static_cast<uint8_t>(
        constrain(preferences.getInt("brightness", 60), 0, 100));
    savedVolume = constrain(preferences.getInt("volume", 40), 0, 100);
    preferences.end();
  }
  Voice::SetVolume(savedVolume);
  if (!isValidDate(cfgYear, cfgMonth, cfgDay)) {
    cfgYear = YEAR;
    cfgMonth = MONTH;
    cfgDay = DAY;
  }
  if (!isValidDeviceId(selfId) || !isValidDeviceId(peerId) || selfId == peerId) {
    selfId = DEFAULT_SELF_ID;
    peerId = DEFAULT_PEER_ID;
  }

  // 服务器固定在固件中，忽略旧版本 NVS 中可能残留的自定义地址。
  mqttHost = DEFAULT_MQTT_HOST;
  mqttPort = DEFAULT_MQTT_PORT;
  voiceHost = DEFAULT_VOICE_HOST;
  voicePort = DEFAULT_VOICE_PORT;

  // 必须先读取配置，再计算纪念日起始时间。
  setTime(0, 0, 0, cfgDay, cfgMonth, cfgYear);
  love_start_time = now();

  randomSeed(esp_random());
  Mode = random(2, 4); // 2=宇航员，3=胡桃
  mode_switch_ms = millis();

  if (cityCode.length() != 9 || !cityCode.startsWith("101") || !isDigits(cityCode)) {
    cityCode = DEFAULT_CITY;
  }

  targetTime = millis() + 1000;  

  if (!credentialsValid) {
    setWiFi();
  }
  Serial.println("正在连接"+ PrefSSID + " ...");
  WiFi.begin(PrefSSID.c_str(), PrefPassword.c_str());
  WiFi.setSleep(false); // 弱 WiFi 下保持稳定，语音优先
  
  while (WiFi.status() != WL_CONNECTED) {
    for(byte n=0;n<10;n++){ 
      loading(30);
      connectTimes++;
      if(connectTimes >= 190) { //进度条即将结束时还未连接成功，则提示wifi连接失败，自动进入配网模式
        connectTimes = 0;
        setWiFi();
      }
    }
  }

  while(loadNum < 194 & connectTimes <= 189){ //让动画走完
    loading(1);
    connectTimes = 0;
  }

  Serial.print("本地IP： ");
  Serial.println(WiFi.localIP());
  scrollText[5] = "IP " + WiFi.localIP().toString();
  Serial.println("启动UDP");
  Udp.begin(localPort);
  Serial.println("等待同步...");
  setSyncProvider(getNtpTime);
  setSyncInterval(300);

  // love线程
  reflash_Love_Animate.setInterval(TMS / 10);
  reflash_Love_Animate.onRun(refresh_loveImage);

  reflash_Love_openWifi.setInterval(WORD_UPDATE_TIME * 60 * TMS); // 5分钟刷新情话
  reflash_Love_openWifi.onRun(openWifi);
  controller_love.run();

  tft.fillScreen(TFT_BLACK);                                  // 清屏
  TJpgDec.drawJpg(15, 183, temperature, sizeof(temperature)); // 温度图标
  TJpgDec.drawJpg(15, 213, humidity, sizeof(humidity));       // 湿度图标

  Battery();
  getCityWeather();
  
  // MQTT 文字互通（esp-mqtt，QoS1，自动重连）
  MqttLovetv::Get().SetMessageHandler(on_mqtt_message);
  MqttLovetv::Get().Begin(mqttHost, mqttPort, selfId, peerId);

  // 语音对讲（I2S + IMA ADPCM + WebSocket 9002）
  Voice::SetStatusCallback(on_voice_status);
  Voice::Init(voiceHost, voicePort, selfId, peerId);

  // WiFi.forceSleepBegin(); // wifi off
  Serial.println("WIFI休眠......");
  Wifi_en = 0;

  // 时钟线程
  reflash_time.setInterval(300); // 设置所需间隔 300毫秒
  reflash_time.onRun(reflashTime);

  reflash_Battery.setInterval(60 * TMS);
  reflash_Battery.onRun(Battery);

  reflash_Banner.setInterval(2 * TMS); // 设置所需间隔 2秒
  reflash_Banner.onRun(reflashBanner);

  reflash_openWifi.setInterval(updateweater_time * 60 * TMS); // 设置所需间隔 1分钟
  reflash_openWifi.onRun(openWifi);

  reflash_Animate.setInterval(TMS / 10); // 设置帧率
  reflash_Animate.onRun(refresh_AnimatedImage);
  controller.run();
}

const uint8_t *Animate_value; // 指向关键帧的指针
uint32_t Animate_size;        // 指向关键帧大小的指针
void refresh_AnimatedImage() {
  if (millis() - Amimate_reflash_Time > 50) // x ms切换一次
  {
    Amimate_reflash_Time = millis();
    imgAnim(&Animate_value, &Animate_size, Mode);
    TJpgDec.drawJpg(160, 160, Animate_value, Animate_size);
    // 胡桃/宇航员每 15 秒切换一次，不再按单轮动画结束切换。
    if (millis() - mode_switch_ms >= 15000) {
      mode_switch_ms = millis();
      Mode = (Mode == 2) ? 3 : 2;
    }
  }
}

long t = 0;

void loop() {
  handle_serial_debug();
  Button_vol_up.loop();
  Button_vol_down.loop();
  Voice::Loop();          // WebSocket 事件、心跳、发送队列、播放状态

  if (page == CLOCK_PAGE) {
    Supervisor_controller(); // 守护线程池
    WIFI_reflash_All();      // WIFI应用
  } else if (page == LOVE_PAGE) {
    show_mg_love_page();
  } else if (page == MISSU_PAGE) {
    if (missu_auto_close_ms != 0 && (int32_t)(millis() - missu_auto_close_ms) >= 0) {
      missu_auto_close_ms = 0;
      goto_clock_page();
    } else {
      show_missu_page("", TFT_WHITE);
    }
  } else if (page == VOICE_PAGE) {
    if (millis() - lastVoiceDraw > 200) { // 每 200ms 刷新录音秒数/连接状态
      lastVoiceDraw = millis();
      draw_voice_page();
    }
  }
}
/******************************************************************************
 * 纪念日页面
 ********************************************************************************/

/**
 * 计算纪念日天数  start_time 参数是从在一起的时间-秒
 */
unsigned int get_love_day(time_t start_time) {
  // 获取最新时间
  const time_t cur_time = now();
  if (cur_time <= start_time) {
    return 1;
  }
  const uint64_t love_seconds = static_cast<uint64_t>(cur_time - start_time);
  const uint64_t day = love_seconds / (24ULL * 3600ULL) + 1ULL;
  return static_cast<unsigned int>(day > 9999ULL ? 9999ULL : day);
}

/**
 * 显示恋爱时间
 */
unsigned int DAY_SIGN = 0;
const uint8_t DAY_Y = 110; // 日期y坐标
const uint8_t DAY_FONT_SIZE = 2;
const byte OFFSET_X = 65; // x坐标偏移量
const uint16_t NUM_COLOR = fhColor;
TFT_eSprite sdm = TFT_eSprite(&tft);

// 用快速线方法绘制数字
void drawLineFontForDay(uint32_t _x, uint32_t _y, uint32_t _num, uint32_t _size,
                        uint32_t _color) {
  _num %= 10;
  uint32_t fontSize;
  const LineAtom *fontOne;
  fontOne = middleLineFont[_num];
  fontSize = middleLineFont_size[_num];

  // 绘制前清理字体绘制区域  周围填充4像素
  tft.fillRoundRect(_x - 4, _y - 4, 26, 38, 4, whiteColor);

  for (uint32_t i = 0; i < fontSize; i++) {
    tft.drawFastHLine(fontOne[i].xValue + _x, fontOne[i].yValue + _y,
                      fontOne[i].lValue, _color);
  }
}

void show_love_day(int reflash_en = 0) {
  unsigned int day = get_love_day(love_start_time);

  // 显示相关文字
  sdm.setColorDepth(16);
  sdm.loadFont(LoveTV_UI_18);

  sdm.createSprite(58, 30);
  // sdm.createSprite(46, 22);
  sdm.fillSprite(pinkColor);
  sdm.setTextWrap(false);
  // sdm.fillRoundRect(0, 0, 46, 22, 4, pinkColor);
  sdm.setTextDatum(CC_DATUM);
  sdm.setTextColor(zongseColor, pinkColor);
  sdm.drawString("相识", 29, 16);
  sdm.pushSprite(0, 110);
  sdm.deleteSprite();

  sdm.createSprite(30, 30);
  sdm.fillSprite(pinkColor);
  sdm.setTextWrap(false);
  sdm.setTextDatum(CC_DATUM);
  sdm.setTextColor(zongseColor, pinkColor);
  sdm.drawString("天", 15, 16);
  sdm.pushSprite(185, 110);
  sdm.deleteSprite();

  sdm.unloadFont();

  // 显示数字
  if (day != DAY_SIGN || reflash_en == 1) // relash_en == 1强制刷新
  {
    drawLineFontForDay(OFFSET_X, DAY_Y, day / 1000, DAY_FONT_SIZE, NUM_COLOR);
    drawLineFontForDay(30 + OFFSET_X, DAY_Y, day / 100 % 10, DAY_FONT_SIZE, NUM_COLOR);
    drawLineFontForDay(60 + OFFSET_X, DAY_Y, day / 10 % 10, DAY_FONT_SIZE,  NUM_COLOR);
    drawLineFontForDay(90 + OFFSET_X, DAY_Y, day % 10, DAY_FONT_SIZE, NUM_COLOR);
    DAY_SIGN = day;
  }
}

//////////////////////////////////////////////////////////////////
// 显示动画
///////////////////////////////////////////////////////////////
const uint8_t *Anilove_value; // 指向关键帧的指针
uint32_t Anilove_size;        // 指向关键帧大小的指针
void refresh_loveImage() {

  if (millis() - Amilove_reflash_Time > 10) // x ms切换一次
  {
    uint16_t w = 0, h = 0;
    bool swith_animate = false;

    Amilove_reflash_Time = millis();
    loveImg(&Anilove_value, &Anilove_size, &swith_animate);

    // Get the width and height in pixels of the jpeg if you wish

    TJpgDec.getJpgSize(&w, &h, Anilove_value, Anilove_size);
    // Serial.print("Width = ");
    // Serial.print(w);
    // Serial.print(", height = ");
    // Serial.println(h);
    uint8_t x = (240 - w) / 2;
    uint8_t y = (102 - h) / 2;

    // Draw the image, top left at 0,0
    TJpgDec.drawJpg(x, y, Anilove_value, Anilove_size);

    // 如果到了切换动画，把之前的残影消除，否则影响视觉效果
    if (swith_animate) {
      sdm.createSprite(w, h);
      sdm.fillSprite(pinkColor);
      sdm.pushSprite(x, y);
      sdm.deleteSprite();
    }
  }
}

/**
 * 显示底部文字
 *
 */
void show_buttom(String s) {
  clk.setColorDepth(16);
  // clk.loadFont(URW_Chancery_L_Medium_Italic_21Bitmaps);
  clk.setFreeFont(&URW_Chancery_L_Medium_Italic_20);
  clk.createSprite(240, 30);
  clk.fillSprite(pinkColor);
  clk.setTextDatum(CC_DATUM);
  clk.setTextColor(zongseColor, pinkColor);
  clk.drawString(s, 120, 15);
  clk.pushSprite(0, 210);
  clk.deleteSprite();

  clk.unloadFont();
}



/*电量显示*/
void Battery() {
  // 8 次采样取平均（沿用星智板 ADC2_CH6，12bit，atten 12dB）
  long sum = 0;
  for (int i = 0; i < 8; i++) {
    sum += analogRead(BATTERY_PIN);
    delay(2);
  }
  uint16_t batteryLevel = sum / 8;
  bool charging = digitalRead(CHARGING_PIN) == HIGH;
  Serial.printf("battery adc=%d charging=%d\n", batteryLevel, charging ? 1 : 0);

  // 阈值沿用星智驱动：>=2200 满、>=2100 中、>=1970 低、<1970 空
  // 原版图标：battery0=满 ... battery3=空
  if (charging || batteryLevel >= 2200) {
    TJpgDec.drawJpg(190, 85, battery0, sizeof(battery0));
  } else if (batteryLevel >= 2100) {
    TJpgDec.drawJpg(190, 85, battery1, sizeof(battery1));
  } else if (batteryLevel >= 1970) {
    TJpgDec.drawJpg(190, 85, battery2, sizeof(battery2));
  } else {
    TJpgDec.drawJpg(190, 85, battery3, sizeof(battery3));
  }
}

/**
 * 主页面
 */

void show_mg_love_page() {
  // 显示底部文字
  show_buttom(bottomWord);

  // 显示恋爱天数
  show_love_day(0);

  Supervisor_love_controller();
}

/**
 * MQTT 消息回调（imu / check）
 **/
void on_mqtt_message(const String &type) {
  Serial.println("[MQTT] recv type=" + type);
  if (type == "imu") {
    if (page == VOICE_PAGE) {
      Voice::CancelAndExit();
    }
    role = RECEIVER;
    messageState = MESSAGE_RECEIVED_IMU;
    goto_missu_page("对方喊你：芜湖！", TFT_WHITE);
  } else if (type == "check") {
    // 不管当前在哪个页面，收到对方回复都弹出“对方已收到”，1 秒后自动关闭
    if (page == VOICE_PAGE) {
      Voice::CancelAndExit();
    }
    role = SENDER;
    messageState = MESSAGE_ACKED;
    goto_missu_page("对方已收到", TFT_WHITE);
    missu_auto_close_ms = millis() + 1000;
  }
}

/**
 * 发送"想你"消息
 **/
void send_imu() {
  String word = "已发送：芜湖！";
  uint16_t word_color = TFT_WHITE;
  if (!MqttLovetv::Get().Publish("imu")) {
    word = "发送失败，检查网络";
    word_color = TFT_RED;
    messageState = MESSAGE_NONE;
  } else {
    messageState = MESSAGE_WAITING_ACK;
  }
  role = SENDER;
  goto_missu_page(word, word_color);
}

/**
 * BOOT 单击：只负责主界面⇄纪念日页。
 **/
void on_boot_click() {
  Serial.printf("[BOOT] short press (Button2) - current page=%d\n", page);
  if (page == CLOCK_PAGE) {
    Serial.println("[BOOT] switching to LOVE_PAGE");
    goto_love_page();
  } else if (page == LOVE_PAGE) {
    Serial.println("[BOOT] switching to CLOCK_PAGE");
    goto_clock_page();
  } else {
    Serial.printf("[BOOT] unhandled page=%d\n", page);
  }
}

/**
 * BOOT 长按：循环背光 60/100/30（仅串口调试用，不再接物理按键）
 **/
void on_boot_long() {
  cycle_brightness();
}

void apply_backlight_brightness() {
  const uint32_t duty =
      (static_cast<uint32_t>(brightness) * BACKLIGHT_LEDC_MAX_DUTY + 50) / 100;
  ledcWrite(BACKLIGHT_LEDC_CHANNEL, duty);
}

void save_brightness() {
  if (lovetv_prefs.begin("lovetv", false)) {
    lovetv_prefs.putInt("brightness", brightness);
    lovetv_prefs.end();
  }
}

void save_volume(int volumeValue) {
  if (lovetv_prefs.begin("lovetv", false)) {
    lovetv_prefs.putInt("volume", volumeValue);
    lovetv_prefs.end();
  }
}

void cycle_brightness() {
  if (brightness == 60) {
    brightness = 100;
  } else if (brightness == 100) {
    brightness = 30;
  } else {
    brightness = 60;
  }
  apply_backlight_brightness();
  save_brightness();
  Serial.printf("brightness=%d\n", brightness);
  show_notify("亮度 " + String(brightness));
}

/*
 * 以下函数已废弃 - 改用 Button2 库处理 BOOT 按键
 * 保留代码用于参考和调试
 */

#if 0  // 废弃的定时器实现

void poll_boot_button() {
  if (boot_event_queue == nullptr) {
    return;
  }

  BootButtonEvent event;
  while (xQueueReceive(boot_event_queue, &event, 0) == pdTRUE) {
    if (event == BOOT_EVENT_RAW_PRESSED) {
      Serial.println("[BOOT] raw LOW(pressed)");
    } else if (event == BOOT_EVENT_RAW_RELEASED) {
      Serial.println("[BOOT] raw HIGH(released)");
    } else if (event == BOOT_EVENT_SHORT_PRESS) {
      on_boot_click();
    } else if (event == BOOT_EVENT_LONG_PRESS) {
      on_boot_long();
    }
  }
}

void boot_button_timer_cb(void *arg) {
  (void)arg;
  if (boot_event_queue == nullptr) {
    return;
  }

  static bool initialized = false;
  static bool raw_pressed = false;
  static bool stable_pressed = false;
  static bool long_fired = false;
  static uint32_t raw_changed_at = 0;
  static uint32_t pressed_at = 0;

  const uint32_t now = millis();
  const bool current_raw_pressed = digitalRead(BTN_BOOT) == LOW;

  // 首次初始化，读取当前状态作为基准
  if (!initialized) {
    initialized = true;
    raw_pressed = current_raw_pressed;
    stable_pressed = current_raw_pressed;
    raw_changed_at = now;
    pressed_at = now;
    Serial.printf("[BOOT] timer_cb initialized, initial_state=%s\n",
                  current_raw_pressed ? "LOW(pressed)" : "HIGH(released)");
    return;  // 跳过第一次，避免误触发
  }

  if (current_raw_pressed != raw_pressed) {
    raw_pressed = current_raw_pressed;
    raw_changed_at = now;
    const BootButtonEvent raw_event = raw_pressed ? BOOT_EVENT_RAW_PRESSED
                                                  : BOOT_EVENT_RAW_RELEASED;
    xQueueSend(boot_event_queue, &raw_event, 0);
  }

  if (raw_pressed != stable_pressed && now - raw_changed_at >= BOOT_DEBOUNCE_MS) {
    stable_pressed = raw_pressed;
    if (stable_pressed) {
      pressed_at = now;
      long_fired = false;
    } else if (!long_fired) {
      const BootButtonEvent event = BOOT_EVENT_SHORT_PRESS;
      xQueueSend(boot_event_queue, &event, 0);
    }
  }

  if (stable_pressed && !long_fired && now - pressed_at >= BOOT_LONG_PRESS_MS) {
    long_fired = true;
    const BootButtonEvent event = BOOT_EVENT_LONG_PRESS;
    xQueueSend(boot_event_queue, &event, 0);
  }
}

#endif  // 废弃的定时器实现

/**
 * 串口调试命令：用于区分软件逻辑和硬件按键问题。
 *   help           列出命令
 *   boot_short     模拟 BOOT 单击
 *   boot_long      模拟 BOOT 长按
 *   boot_level     读取一次 GPIO0 电平
 *   boot_scan      连续扫描 GPIO0 3 秒（期间手动按/放 BOOT）
 *   btn_scan       连续扫描 BOOT/VOL+/VOL- 3 秒
 *   boot_test      5 秒内按/放 BOOT 三次，统计电平变化
 */
void handle_serial_debug() {
  static String serial_line = "";
  while (Serial.available()) {
    const char ch = static_cast<char>(Serial.read());
    if (ch == '\n' || ch == '\r') {
      if (serial_line.length() == 0) {
        continue;
      }
      serial_line.trim();
      Serial.println("[SERIAL] cmd: " + serial_line);
      if (serial_line == "help") {
        Serial.println("[SERIAL] cmds: boot_short, boot_long, boot_level, boot_test, help");
      } else if (serial_line == "boot_short") {
        on_boot_click();
      } else if (serial_line == "boot_long") {
        on_boot_long();
      } else if (serial_line == "boot_level") {
        Serial.printf("[SERIAL] GPIO%d=%s\n", BTN_BOOT,
                      digitalRead(BTN_BOOT) == LOW ? "LOW" : "HIGH");
      } else if (serial_line == "boot_test") {
        Serial.println("[TEST] press and release BOOT 3 times within 5s...");
        const uint32_t test_start = millis();
        bool last_level = digitalRead(BTN_BOOT) == LOW;
        int transitions = 0;
        while (millis() - test_start < 5000) {
          const bool current_level = digitalRead(BTN_BOOT) == LOW;
          if (current_level != last_level) {
            transitions++;
            Serial.printf(
                "[TEST] t=%lu level=%s\n",
                static_cast<unsigned long>(millis() - test_start),
                current_level ? "LOW" : "HIGH");
            last_level = current_level;
          }
          delay(50);
        }
        Serial.printf("[TEST] done transitions=%d final=%s\n", transitions,
                      last_level ? "LOW" : "HIGH");
      } else {
        Serial.println("[SERIAL] unknown command: " + serial_line);
      }
      serial_line = "";
    } else {
      serial_line += ch;
    }
  }
}

/**
 * VOL+ 单击：仅主页发送"想你"消息；纪念日页返回主页；消息页确认/退出
 **/
void on_vol_up_click(Button2 &btn) {
  long now_ms = millis();
  if (now_ms - vol_up_long_ms < 500) {
    vol_up_long_ms = -100000;
    return;
  }
  if (page == VOICE_PAGE) {
    Voice::CancelAndExit();
    goto_clock_page();
    return;
  }
  if (page == LOVE_PAGE) {
    goto_clock_page();
    return;
  }
  if (page == MISSU_PAGE) {
    if (messageState == MESSAGE_RECEIVED_IMU) {
      if (!MqttLovetv::Get().Publish("check")) {
        goto_missu_page("回复失败，检查网络", TFT_RED);
        return;
      }
      goto_missu_page("已回复：起飞！", TFT_WHITE);
      messageState = MESSAGE_NONE;
      missu_auto_close_ms = millis() + 1000; // 1 秒后自动回到主页
      return;
    }
    messageState = MESSAGE_NONE;
    goto_clock_page();
    return;
  }
  send_imu();
}

/**
 * VOL+ 双击：主界面 ⇄ 纪念日页（替代失灵的 BOOT 键）
 **/
void on_vol_up_double(Button2 &btn) {
  long now_ms = millis();
  if (now_ms - vol_up_long_ms < 500) {
    vol_up_long_ms = -100000;
    return;
  }
  Serial.println("[VOL+] double click");
  if (page == CLOCK_PAGE) {
    goto_love_page();
  } else if (page == LOVE_PAGE) {
    goto_clock_page();
  }
}

void on_vol_up_long(Button2 &btn) {
  vol_up_long_ms = millis();
  int v = Voice::GetVolume() + 10;
  if (v > 100) v = 100;
  Voice::SetVolume(v);
  save_volume(v);
  Serial.printf("volume=%d\n", v);
  show_notify("音量 " + String(v));
}

/**
 * VOL- 单击：开始/结束录音
 **/
void on_vol_down_click(Button2 &btn) {
  long now_ms = millis();
  if (now_ms - vol_down_long_ms < 500) {
    vol_down_long_ms = -100000;
    return;
  }
  if (Voice::IsRecording()) {
    Voice::StopAndSend();
  } else {
    if (page != VOICE_PAGE) {
      goto_voice_page("正在录音，请说话…", true);
    }
    Voice::StartRecording();
  }
}

/**
 * VOL- 双击：循环调整背光亮度 60 → 100 → 30
 **/
void on_vol_down_double(Button2 &btn) {
  long now_ms = millis();
  if (now_ms - vol_down_long_ms < 500) {
    vol_down_long_ms = -100000;
    return;
  }
  Serial.println("[VOL-] double click");
  cycle_brightness();
}

void on_vol_down_long(Button2 &btn) {
  vol_down_long_ms = millis();
  int v = Voice::GetVolume() - 10;
  if (v < 0) v = 0;
  Voice::SetVolume(v);
  save_volume(v);
  Serial.printf("volume=%d\n", v);
  show_notify("音量 " + String(v));
}

/**
 * 语音状态回调（voice 模块通知，在 loop 上下文执行）
 **/
void on_voice_status(const String &status, bool showPage) {
  voiceStatus = status;
  if (showPage || page == VOICE_PAGE) {
    goto_voice_page(status, true);
  }
}

/**
 * 切换到语音页
 **/
void goto_voice_page(String status, bool showPage) {
  if (!showPage && page == VOICE_PAGE) {
    voiceStatus = status;
    return;
  }
  voiceStatus = status;
  if (page == VOICE_PAGE) {
    draw_voice_page();
    return;
  }
  clear_screen();
  page = VOICE_PAGE;
  tft.fillScreen(pinkColor);
  draw_voice_page();
  Serial.println("已切换到语音页");
}

/**
 * 绘制语音页（状态 + 录音秒数 + 连接状态）
 **/
void draw_voice_page() {
  TFT_eSprite vs = TFT_eSprite(&tft);
  vs.setColorDepth(16);
  vs.loadFont(LoveTV_UI_18);

  // 标题
  vs.createSprite(240, 30);
  vs.fillSprite(pinkColor);
  vs.setTextWrap(false);
  vs.setTextDatum(CC_DATUM);
  vs.setTextColor(zongseColor, pinkColor);
  vs.drawString("语音对讲", 120, 16);
  vs.pushSprite(0, 85);
  vs.deleteSprite();

  // 状态
  vs.createSprite(240, 30);
  vs.fillSprite(pinkColor);
  vs.setTextWrap(false);
  vs.setTextDatum(CC_DATUM);
  vs.setTextColor(TFT_WHITE, pinkColor);
  vs.drawString(voiceStatus, 120, 16);
  vs.pushSprite(0, 145);
  vs.deleteSprite();

  // 辅助信息
  String extra;
  if (Voice::IsRecording()) {
    extra = "已录音 " + String(Voice::GetRecordingDurationMs() / 1000) + " 秒";
  } else if (Voice::IsPlaying()) {
    extra = "正在播放语音…";
  } else {
    extra = Voice::IsConnected() ? "语音通道已连接" : "语音通道未连接";
  }
  vs.createSprite(240, 20);
  vs.fillSprite(pinkColor);
  vs.setTextWrap(false);
  vs.setTextDatum(CC_DATUM);
  vs.setTextColor(zongseColor, pinkColor);
  vs.drawString(extra, 120, 11);
  vs.pushSprite(0, 185);
  vs.deleteSprite();

  // 底部提示
  vs.createSprite(240, 20);
  vs.fillSprite(pinkColor);
  vs.setTextWrap(false);
  vs.setTextDatum(CC_DATUM);
  vs.setTextColor(zongseColor, pinkColor);
  vs.drawString("VOL- 录音/发送  VOL+ 返回", 120, 11);
  vs.pushSprite(0, 210);
  vs.deleteSprite();

  vs.unloadFont();
}

/**
 * 时钟页顶部提示（2 秒后会被 banner 覆盖）
 **/
void show_notify(String text) {
  if (page != CLOCK_PAGE) {
    return;
  }
  clk.setColorDepth(8);
  clk.loadFont(LoveTV_UI_18);
  clk.createSprite(240, 30);
  clk.fillSprite(bgColor);
  clk.setTextDatum(CC_DATUM);
  clk.setTextColor(TFT_WHITE, bgColor);
  clk.drawString(text, 120, 16);
  clk.pushSprite(0, 45);
  clk.deleteSprite();
  clk.unloadFont();
}

/**
 * 切换到想你页面，前边做清屏等准备工作，
 **/
void goto_missu_page(String word, uint16_t word_color) {
  missu_auto_close_ms = 0; // 新页面先取消旧的自动关闭，需要时由调用方重新设置
  // 切换到 纪念日页面
  Serial.println("正在切换到想你页面....");
  clear_screen(); // 清屏
  page = MISSU_PAGE;
  tft.fillScreen(pinkColor);
  show_missu_page(word, word_color);
  Serial.println("已切换到想你页面");
}

/**
 * 切换到时钟页面
 **/
void goto_clock_page() {

  missu_auto_close_ms = 0;
  // WiFi.forceSleepWake(); // wifi on
  clear_screen();                                             // 清屏
  page = CLOCK_PAGE;                                          // 记录页面
  TJpgDec.drawJpg(15, 183, temperature, sizeof(temperature)); // 温度图标
  TJpgDec.drawJpg(15, 213, humidity, sizeof(humidity));       // 湿度图标
  Battery();
  digitalClockDisplay(1); // 强制刷新显示时间
  weaterData(&(weather_msg.dataSK), &(weather_msg.dataFC)); // 显示缓存温湿度缓存值
  Serial.println("已切换到时钟页面");
}

/**
 * 显示默认情话
 */
void show_default_words() {
  // 解析JSON获取默认文字
  DynamicJsonDocument doc(300);
  deserializeJson(doc, words);
  JsonObject data = doc.as<JsonObject>();
  String word = data["word"];
  
  // 字符串预处理
  word.replace("<br>", " | ");
  word.replace(",", "，");
  word.replace("?", "？");
  word.replace(".", "...");
  word.replace("=", " = ");
  word.replace("(", "（");
  word.replace(")", "）");

  // 去掉顿号
  int index_str = word.indexOf("、");
  if (index_str == -1) {
    index_str = 0;
  } else {
    index_str += 3;
  }

  uint16_t num = 11; // 每行显示多少中文字符
  // 分割字符串，一行 11 个中文字符
  String first_str = word.substring(index_str, index_str + num * 3);
  String second_str = word.substring(index_str + num * 3, index_str + num * 3 * 2);
  String third_str = word.substring(index_str + num * 3 * 2, index_str + num * 3 * 3);
  String forth_str = word.substring(index_str + num * 3 * 3, index_str + num * 3 * 4);

  // 如果超过四行,这段话不显示；显示自定义的
  if (forth_str != "") {
    first_str = "Happy Every Day";
    second_str = "";
    third_str = "";
  }

  // 如果只有一行，那么居中显示，（显示在第二行）
  if (second_str == "") {
    second_str = first_str;
    first_str = "";
  }

  // 显示文字
  sdm.setColorDepth(16);
  sdm.loadFont(KT_20);
  
  // 第一行
  sdm.createSprite(240, 20);
  sdm.fillSprite(pinkColor);
  sdm.setTextWrap(false);
  sdm.setTextDatum(CC_DATUM);
  sdm.setTextColor(TFT_WHITE, pinkColor);
  sdm.drawString(first_str, 120, 11);
  sdm.pushSprite(0, 153);
  sdm.deleteSprite();

  // 第二行
  sdm.createSprite(240, 20);
  sdm.fillSprite(pinkColor);
  sdm.setTextWrap(false);
  sdm.setTextDatum(CC_DATUM);
  sdm.setTextColor(TFT_WHITE, pinkColor);
  sdm.drawString(second_str, 120, 11);
  sdm.pushSprite(0, 173);
  sdm.deleteSprite();

  // 第三行
  sdm.createSprite(240, 20);
  sdm.fillSprite(pinkColor);
  sdm.setTextWrap(false);
  sdm.setTextDatum(CC_DATUM);
  sdm.setTextColor(TFT_WHITE, pinkColor);
  sdm.drawString(third_str, 120, 11);
  sdm.pushSprite(0, 193);
  sdm.deleteSprite();

  sdm.unloadFont();
}

/**
 * //切换到 纪念日页面
 */
void goto_love_page() {

  clear_screen(); // 清屏
  page = LOVE_PAGE;
  tft.fillScreen(pinkColor); // 填充背景粉色 tft.color565(255,182,193)
                             // tft.color565(255,174,201)
  show_love_day(1);          // 强制刷新显示天数
  show_default_words();      // 显示默认文字
  Serial.println("已切换到纪念日页面");
}

void goto_page(byte page) {
  switch (page) {
  case CLOCK_PAGE:
    goto_clock_page();
    break;
  case LOVE_PAGE:
    goto_love_page();
    break;
  default:
    Serial.println("错误");
    break;
  }
}
