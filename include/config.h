#pragma once
#include <Arduino.h>

// ================= 设备表(全屋灯位/回路映射,来自实测)=================
struct DevEntry {
  const char* aid;        // ascii id(HA 实体/主题用)
  const char* name;       // 中文名(显示)
  const char* cid;        // 涂鸦 nodeId(面板=a4 开头,气候=0- 开头)
  uint8_t ctype;          // 0=气候类子设备, 1=Zigbee 面板类
  const char* type;       // thermostat/ac/light/panel(HA 实体区分)
  const char* state_dp;   // 主状态 dp(HA switch 用)
  const char* knx_sw;     // KNX 开关地址(nullptr=无)
  const char* knx_temp;   // KNX 温度地址(5-35 或 16-32)
  const char* knx_mode;   // KNX 模式地址
  const char* knx_fan;    // KNX 风速地址
  bool status_only;       // true=纯状态传感器(面板)
  uint8_t temp_lo;        // 温度 number 实体下限(0=无温度实体)
  uint8_t temp_hi;        // 温度 number 实体上限
};

// 灯光/气候设备表 —— ⚠️ KNX 地址和 nodeId 为示例格式,按自家网关导出的配置修改
static const DevEntry DEVICES[] = {
    // aid             name        cid(涂鸦 nodeId)              ct  sdp   KNX 开关        温度         模式        风速
    {"wenkongqi",    "温控器",     "0-YOUR_DEVICE_ID_01",          0, "thermostat", "1",  "11/0/16",     "11/0/18",  nullptr,    nullptr, false, 5, 35},
    {"canting_kt",   "餐厅空调",   "0-YOUR_DEVICE_ID_02",          0, "ac", "1",  "12/1/1",      "12/4/1",   "12/2/1",   "12/3/1", false, 16, 32},
    {"keting_kt",    "客厅空调",   "0-YOUR_DEVICE_ID_03",          0, "ac", "1",  "12/1/2",      "12/4/2",   "12/2/2",   "12/3/2", false, 16, 32},
    {"zhuwo_kt",     "主卧空调",   "0-YOUR_DEVICE_ID_04",          0, "ac", "1",  "12/1/4",      "12/4/4",   "12/2/4",   "12/3/4", false, 16, 32},
    {"ciwo_kt",      "次卧空调",   "0-YOUR_DEVICE_ID_05",          0, "ac", "1",  "12/1/3",      "12/4/3",   "12/2/3",   "12/3/3", false, 16, 32},
    {"beiciwo_kt",   "北次卧空调", "0-YOUR_DEVICE_ID_06",          0, "ac", "1",  "12/1/3",      "12/4/3",   "12/2/3",   "12/3/3", false, 16, 32},
    {"xinfeng",      "新风",      "0-YOUR_DEVICE_ID_07",          0, "freshair", "39", "14/0/2",      nullptr,    nullptr,    "14/1/2", false, 0, 0},
    {"keting_sd",    "客厅射灯",   "a4c-YOUR_PANEL_ID_01",            1, "light", "24", nullptr,       nullptr,    nullptr,    nullptr, false, 0, 0},
    {"keting_zd",    "客厅主灯",   "a4c-YOUR_PANEL_ID_01",            1, "light", "25", nullptr,       nullptr,    nullptr,    nullptr, false, 0, 0},
    {"canting_zd",   "餐厅主灯",   "a4c-YOUR_PANEL_ID_01",            1, "light", "26", nullptr,       nullptr,    nullptr,    nullptr, false, 0, 0},
    {"fenweideng",   "氛围灯",     "a4c-YOUR_PANEL_ID_01",            1, "light", "27", nullptr,       nullptr,    nullptr,    nullptr, false, 0, 0},
    {"dianshi_jg",   "电视酒柜灯带","a4c-YOUR_PANEL_ID_02",            1, "light", "24", nullptr,       nullptr,    nullptr,    nullptr, false, 0, 0},
    {"zoulang_sd",   "走廊射灯",   "a4c-YOUR_PANEL_ID_02",            1, "light", "25", nullptr,       nullptr,    nullptr,    nullptr, false, 0, 0},
    {"yijian_dd",    "一键断电",   "a4c-YOUR_PANEL_ID_02",            1, "light", "27", nullptr,       nullptr,    nullptr,    nullptr, false, 0, 0},
    {"yimao",        "衣帽",      "a4c-YOUR_PANEL_ID_03",            1, "light", "24", nullptr,       nullptr,    nullptr,    nullptr, false, 0, 0},
    {"zhuwo_zd",     "主卧主灯",   "a4c-YOUR_PANEL_ID_03",            1, "light", "25", nullptr,       nullptr,    nullptr,    nullptr, false, 0, 0},
    {"zhuwo_dd",     "主卧灯带",   "a4c-YOUR_PANEL_ID_03",            1, "light", "27", nullptr,       nullptr,    nullptr,    nullptr, false, 0, 0},
};
static const uint8_t DEVICE_COUNT = sizeof(DEVICES) / sizeof(DEVICES[0]);

// 纯状态传感器(面板,墙开动作上报)
struct PanelEntry {
  const char* aid;
  const char* name;
  const char* cid;
};
static const PanelEntry PANELS[] = {
    {"xuanguan_lijia",   "玄关离家", "a4c-YOUR_PANEL_ID_02"},
    {"zhuwo_menkou",     "主卧门口", "a4c-YOUR_PANEL_ID_03"},
    {"shufang",          "书房",    "a4c-YOUR_PANEL_ID_04"},
    {"zhuwo_chuanglian", "主卧窗帘", "a4c-YOUR_PANEL_ID_05"},
    {"ciwo_menkou",      "次卧门口", "a4c-YOUR_PANEL_ID_06"},
    {"zhuwo_chuangtou",  "主卧床头", "a4c-YOUR_PANEL_ID_09"},
    {"chufang",          "厨房",    "a4c-YOUR_PANEL_ID_07"},
    {"keting_chuanglian","客厅窗帘", "a4c-YOUR_PANEL_ID_08"},
    {"ciwo_chuangtou",   "次卧床头", "a4c-YOUR_PANEL_ID_09"},
};
static const uint8_t PANEL_COUNT = sizeof(PANELS) / sizeof(PANELS[0]);

// ================= 2.3 控制协议常量 =================
static const char* TUYA_MQTT_HOST = "m1-cn.aideepsmart.com";
static const uint16_t TUYA_MQTT_PORT = 8883;
static const char* KNX_SWITCH_TYPE = "0";
static const char* KNX_VALUE_TYPE = "2";
static const char* KNX_ENUM_TYPE = "1";

// ================= 运行时配置(NVS 存储)=================
struct BridgeConfig {
  // bemfa(米家/小爱入口)
  char bemfa_uid[64];        // 用户私钥
  char bemfa_ac_topic[64];   // 空调群组主题(005 后缀)
  char bemfa_light_topic[64];// 灯主题(002 后缀)
  // 涂鸦云 MQTT(灯光 ctype 路径)
  char tuya_user[128];       // pXXXX_v1_...
  char tuya_pass[64];        // 16hex
  char tuya_client_id[128];
  char session_key[48];      // localKey(16B ascii)
  char gateway_dev_id[32];   // HC5 devId
  // HA broker
  char ha_host[96];
  uint16_t ha_port;
  // 网关(本地 KNX)
  char gw_host[64];
  char gw_user[48];
  char gw_pass[48];
};

// 默认值(首次启动写入 NVS)
static const BridgeConfig DEFAULT_CONFIG = {
    "",                                      // bemfa_uid
    "",                                      // bemfa_ac_topic
    "",                                      // bemfa_light_topic
    "",                                      // tuya_user
    "",                                      // tuya_pass
    "",                                      // tuya_client_id
    "",                                      // session_key
    "",                                      // gateway_dev_id(配置页填写)
    "",                                      // ha_host(配置页填写)
    1883,                                    // ha_port
    "192.168.1.200",                         // gw_host(示例)
    "admin",                                 // gw_user(示例)
    "",                                      // gw_pass(配置页填写)
};
