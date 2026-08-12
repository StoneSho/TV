//#######################################################
//   时钟页面配置
//#######################################################
#define TMS 1000               //一千毫秒
#define SD_FONT_YELLOW 0xD404  // 黄色字体颜色
#define SD_FONT_WHITE 0xFFFF   // 白色字体颜色
#define WEATHER_UPDATE_TIME 5 //默认天气更新时间
#define timeY 82               // 定义高度

//#######################################################
//    纪念日页面配置
//#######################################################
#define DEFAULT_WORDS "{\"word\":\"Happy Every Day\"}" //只能修改汉字内容
#define WORD_UPDATE_TIME 3                                          //情话切换时间单位 - 分钟
#define ANIMATE_CHANGE_FREQUENTCE 20                                //动画切换频次，20次切换一张

#define BUTTOM_WORD "known since 2019.9.1"                         //纪念日页面底部文字

#define YEAR 2019  //纪念日 年
#define MONTH 9  //纪念日  月
#define DAY 1   // 纪念日  日

//#######################################################
//    想你页面配置（默认值，实际以网页配置/NVS 为准）
//#######################################################
#define DEFAULT_CITY "101120501"          // 默认城市：烟台
#define DEFAULT_SELF_ID "A"               // 本机 ID
#define DEFAULT_PEER_ID "B"               // 对方 ID
#define DEFAULT_MQTT_HOST "117.72.44.85"  // MQTT 服务器
#define DEFAULT_MQTT_PORT 1883            // MQTT 端口
#define DEFAULT_VOICE_HOST "117.72.44.85" // 语音服务器
#define DEFAULT_VOICE_PORT 9002           // 语音 WebSocket 端口

//#######################################################
//    星智 1.54TFT WiFi 版硬件引脚
//#######################################################
#define LCD_BL_PIN 13        // 背光
#define BTN_BOOT 0           // BOOT 按键（低电平有效）
#define BTN_VOL_UP 40        // VOL+ 按键（低电平有效）
#define BTN_VOL_DOWN 39      // VOL- 按键（低电平有效）
#define BATTERY_PIN 17       // 电池 ADC2_CH6（GPIO17）
#define CHARGING_PIN 38      // 充电检测引脚（高电平=充电中）

// I2S 麦克风 INMP441
#define MIC_SCK 5
#define MIC_WS 4
#define MIC_DIN 6
// I2S 功放 MAX98357
#define SPK_BCLK 15
#define SPK_LRCK 16
#define SPK_DOUT 7
