#pragma once
#include <Arduino.h>

// ================= 设备表(全屋灯位/回路映射,来自实测)=================
// ascii_id 与 Mac 桥 config.json 完全一致(转发 zhsh/<aid>/set 靠它对上)
// 空调 KNX 地址按网关 protocolObjs 物理位置:客厅=12/x/0 餐厅=1 主卧=2 次卧=3 书房=4
struct DevEntry {
  const char* aid;        // ascii id(HA 实体/主题用)
  const char* name;       // 中文名(显示)
  const char* cid;        // 涂鸦 nodeId(仅备用,本地控制不依赖)
  uint8_t ctype;          // 0=气候类子设备, 1=Zigbee 面板/窗帘类
  const char* type;       // thermostat/ac/freshair/light/curtain
  const char* state_dp;   // 主状态 dp
  const char* knx_sw;     // KNX 开关地址(nullptr=待网关导出后填入)
  const char* knx_temp;   // KNX 温度地址
  const char* knx_mode;   // KNX 模式地址
  const char* knx_fan;    // KNX 风速地址
  const char* module;     // 网关 moduleId(queryKnx 状态轮询用,""=不轮询)
  bool status_only;       // 保留字段
  uint8_t temp_lo;        // 温度范围(0=无)
  uint8_t temp_hi;
};

// 灯光/气候设备表 —— ⚠️ cid 为示例格式;KNX 地址按自家网关导出修改
static const DevEntry DEVICES[] = {
    // --- 气候(KNX 直控,毫秒级) ---
    // aid              name        cid(占位)                 ct  sdp   KNX 开关      温度        模式        风速
    {"wenkongqi", "温控器", "0-YOUR_DEVICE_ID_01", 0, "thermostat", "1", "11/0/16", "11/0/18", nullptr, nullptr, "261", false, 5, 35},
    {"keting_kt", "客厅空调", "0-YOUR_DEVICE_ID_02", 0, "ac", "1", "12/1/0", "12/4/0", "12/2/0", "12/3/0", "255", false, 16, 32},
    {"canting_kt", "餐厅空调", "0-YOUR_DEVICE_ID_03", 0, "ac", "1", "12/1/1", "12/4/1", "12/2/1", "12/3/1", "256", false, 16, 32},
    {"zhuwo_kt", "主卧空调", "0-YOUR_DEVICE_ID_04", 0, "ac", "1", "12/1/2", "12/4/2", "12/2/2", "12/3/2", "257", false, 16, 32},
    {"ciwo_kt", "次卧空调", "0-YOUR_DEVICE_ID_05", 0, "ac", "1", "12/1/3", "12/4/3", "12/2/3", "12/3/3", "258", false, 16, 32},
    {"shufang_kt", "书房空调", "0-YOUR_DEVICE_ID_06", 0, "ac", "1", "12/1/4", "12/4/4", "12/2/4", "12/3/4", "259", false, 16, 32},
    {"xinfeng", "新风", "0-YOUR_DEVICE_ID_07", 0, "freshair", "39", "14/0/2", nullptr, nullptr, "14/1/2", "262", false, 0, 0},
    // --- 灯光(面板回路,ctype1,执行走 Mac 桥涂鸦通路) ---
    // 玄关四建面板
    {"keting_shaideng", "客厅射灯",  "a4c-YOUR_PANEL_ID_11",    1, "light", "24", nullptr, nullptr, nullptr, nullptr, "", false, 0, 0},
    {"keting_zhudeng",  "客厅主灯",  "a4c-YOUR_PANEL_ID_11",    1, "light", "25", nullptr, nullptr, nullptr, nullptr, "", false, 0, 0},
    {"canting_zhudeng", "餐厅主灯",  "a4c-YOUR_PANEL_ID_11",    1, "light", "26", nullptr, nullptr, nullptr, nullptr, "", false, 0, 0},
    {"fenweideng",      "氛围灯",    "a4c-YOUR_PANEL_ID_11",    1, "light", "27", nullptr, nullptr, nullptr, nullptr, "", false, 0, 0},
    // 玄关离家面板
    {"dianshi_jgd",     "电视酒柜灯带","a4c-YOUR_PANEL_ID_12",   1, "light", "24", nullptr, nullptr, nullptr, nullptr, "", false, 0, 0},
    {"zoulang_shaideng","走廊射灯",  "a4c-YOUR_PANEL_ID_12",    1, "light", "25", nullptr, nullptr, nullptr, nullptr, "", false, 0, 0},
    {"yijian_duandian", "一键断电",  "a4c-YOUR_PANEL_ID_12",    1, "light", "27", nullptr, nullptr, nullptr, nullptr, "", false, 0, 0},
    // 主卧门口面板
    {"yimao",           "衣帽灯",    "a4c-YOUR_PANEL_ID_13",    1, "light", "24", nullptr, nullptr, nullptr, nullptr, "", false, 0, 0},
    {"zhuwo_zhudeng",   "主卧主灯",  "a4c-YOUR_PANEL_ID_13",    1, "light", "25", nullptr, nullptr, nullptr, nullptr, "", false, 0, 0},
    {"zhuwo_daidai",    "主卧灯带",  "a4c-YOUR_PANEL_ID_13",    1, "light", "27", nullptr, nullptr, nullptr, nullptr, "", false, 0, 0},
    // 书房面板
    {"shufang_zhudeng", "书房主灯",  "a4c-YOUR_PANEL_ID_14",    1, "light", "24", nullptr, nullptr, nullptr, nullptr, "", false, 0, 0},
    {"shufang_daidai",  "书房灯带",  "a4c-YOUR_PANEL_ID_14",    1, "light", "25", nullptr, nullptr, nullptr, nullptr, "", false, 0, 0},
    // 厨房面板
    {"chufang_shaideng","厨房射灯",  "a4c-YOUR_PANEL_ID_15",    1, "light", "24", nullptr, nullptr, nullptr, nullptr, "", false, 0, 0},
    {"chufang_daidai",  "厨房灯带",  "a4c-YOUR_PANEL_ID_15",    1, "light", "25", nullptr, nullptr, nullptr, nullptr, "", false, 0, 0},
    // 次卧门口面板
    {"ciwo_shaideng",   "次卧射灯",  "a4c-YOUR_PANEL_ID_16",    1, "light", "24", nullptr, nullptr, nullptr, nullptr, "", false, 0, 0},
    {"ciwo_zhudeng",    "次卧主灯",  "a4c-YOUR_PANEL_ID_16",    1, "light", "25", nullptr, nullptr, nullptr, nullptr, "", false, 0, 0},
    {"ciwo_daidai",     "次卧灯带",  "a4c-YOUR_PANEL_ID_16",    1, "light", "26", nullptr, nullptr, nullptr, nullptr, "", false, 0, 0},
    // 主卧床头面板
    {"zhuwo_ct_sd",     "主卧床头射灯","a4c-YOUR_PANEL_ID_17",   1, "light", "24", nullptr, nullptr, nullptr, nullptr, "", false, 0, 0},
    {"zhuwo_ct_dd",     "主卧床头灯带","a4c-YOUR_PANEL_ID_17",   1, "light", "25", nullptr, nullptr, nullptr, nullptr, "", false, 0, 0},
    // 次卧床头面板
    {"ciwo_ct_sd",      "次卧床头射灯","a4c-YOUR_PANEL_ID_18",   1, "light", "24", nullptr, nullptr, nullptr, nullptr, "", false, 0, 0},
    {"ciwo_ct_dd",      "次卧床头灯带","a4c-YOUR_PANEL_ID_18",   1, "light", "25", nullptr, nullptr, nullptr, nullptr, "", false, 0, 0},
    // --- 窗帘(每屋双轨:布帘/纱帘各一地址位,待网关导出回填) ---
    {"zhuwo_bulian",    "主卧布帘",  "a4c-YOUR_PANEL_ID_19",    1, "curtain", "3", nullptr, nullptr, nullptr, nullptr, "", false, 0, 0},
    {"zhuwo_shalian",   "主卧纱帘",  "a4c-YOUR_PANEL_ID_20",    1, "curtain", "3", nullptr, nullptr, nullptr, nullptr, "", false, 0, 0},
    {"keting_bulian",   "客厅布帘",  "a4c-YOUR_PANEL_ID_21",    1, "curtain", "3", nullptr, nullptr, nullptr, nullptr, "", false, 0, 0},
    {"keting_shalian",  "客厅纱帘",  "a4c-YOUR_PANEL_ID_22",    1, "curtain", "3", nullptr, nullptr, nullptr, nullptr, "", false, 0, 0},
};
static const uint8_t DEVICE_COUNT = sizeof(DEVICES) / sizeof(DEVICES[0]);

// 纯状态传感器(面板,墙开动作上报;窗帘已升级为可控设备,移出此表)
struct PanelEntry {
  const char* aid;
  const char* name;
  const char* cid;
};
static const PanelEntry PANELS[] = {
    {"xuanguan_sijian",  "玄关四建", "a4c-YOUR_PANEL_ID_11"},
    {"xuanguan_lijia",   "玄关离家", "a4c-YOUR_PANEL_ID_12"},
    {"zhuwo_menkou",     "主卧门口", "a4c-YOUR_PANEL_ID_13"},
    {"shufang",          "书房",    "a4c-YOUR_PANEL_ID_14"},
    {"chufang",          "厨房",    "a4c-YOUR_PANEL_ID_15"},
    {"ciwo_menkou",      "次卧门口", "a4c-YOUR_PANEL_ID_16"},
    {"zhuwo_chuangtou",  "主卧床头", "a4c-YOUR_PANEL_ID_17"},
    {"ciwo_chuangtou",   "次卧床头", "a4c-YOUR_PANEL_ID_18"},
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
  char bemfa_ac_topic[64];   // (遗留字段,主题现在由 NVS bf_topics 表驱动)
  char bemfa_light_topic[64];// (遗留字段)
  // 涂鸦云 MQTT(状态监听;当前 TLS 指纹受限,控制走 Mac 桥)
  char tuya_user[128];
  char tuya_pass[64];
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
    "",                                      // bemfa_ac_topic(遗留)
    "",                                      // bemfa_light_topic(遗留)
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
