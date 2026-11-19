#pragma once
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <functional>
#include "config.h"

class GatewaySide;
void bridge_set_gateway(GatewaySide *g);

// ===== 回调(main 注册)=====
// 命令:aid=设备, dp=dp键, value=字符串值, ctype=设备通路
extern std::function<void(const char *aid, const char *dp, const char *value, uint8_t ctype)> command_cb;
// 状态:cid=设备 nodeId, dp=dp键, value=字符串值
extern std::function<void(const char *cid, const char *dp, const char *value)> state_cb;

// ===== 涂鸦云 MQTT(灯光 ctype 路径 + 状态回执)=====
namespace tuya {
extern PubSubClient client;
extern bool connected;
void setup(const BridgeConfig &cfg);
void loop();
bool publish_dps(const char *cid, const char *dp, bool value, uint8_t ctype);
bool publish_dps(const char *cid, const char *dp, int value, uint8_t ctype);
}  // namespace tuya

// ===== bemfa MQTT(米家/小爱入口,双主题)=====
namespace bemfa {
extern PubSubClient client;
extern bool connected;
void setup(const BridgeConfig &cfg);
void loop();
void add_topic(const char *topic, const char *device_aid, const char *kind, const char *dp);
void del_topic(const char *topic);
String topics_json();
void sync_state(const char *device_aid, const char *dp, const char *value);
}  // namespace bemfa

// ===== HA MQTT(discovery + 命令 + 状态)=====
namespace ha {
extern PubSubClient client;
extern bool connected;
void setup(const BridgeConfig &cfg);
void loop();
void publish_discovery();
void publish_state(const char *aid, const char *dps_json);
void publish_temp_state(const char *aid, const char *dp, const char *value);
}  // namespace ha
