#include "bridge_mqtt.h"
#include <WiFiClient.h>

// ============================================================
// HA MQTT —— ESP32 角色限定为:
//   1. 转发器:本机 KNX 做不了的命令 → zhsh/<aid>/set(Mac 桥独占执行,涂鸦通路)
//   2. 状态同步器:订阅 zhsh/+/state(Mac 桥发布)→ 回写 bemfa 主题(米家显示)
// discovery 与命令执行由 Mac 桥独占(避免双执行/回环)。
// ============================================================
namespace ha {

WiFiClient plain;
PubSubClient client(plain);
bool connected = false;

static char host[96] = "";
static uint16_t port = 1883;
static unsigned long last_reconnect = 0;

// 转发命令:{"dp":"24","value":true} —— Mac 桥 _on_message 解析 dp/value
void forward_command(const char *aid, const char *dp, const char *value) {
  if (!connected) { Serial.println("[ha] forward 失败:broker 未连"); return; }
  bool is_bool = (strcmp(value, "true") == 0 || strcmp(value, "false") == 0);
  String payload = String("{\"dp\":\"") + dp + "\",\"value\":" +
                   (is_bool ? value : String(atoi(value))) + "}";
  String t = String("zhsh/") + aid + "/set";
  client.publish(t.c_str(), payload.c_str());
  Serial.printf("[ha] forward %s dp%s=%s\n", aid, dp, value);
}

static void on_connect(char *, byte *, unsigned int) {
  Serial.println("[ha] broker connected(转发器模式)");
  connected = true;
  client.subscribe("zhsh/+/state", 1);  // Mac 桥状态 → bemfa 回写
}

static void on_message(char *topic, byte *payload, unsigned int length) {
  // topic: zhsh/<aid>/state  payload: {"24":true} / {"2":26}(Mac 桥发布)
  String t(topic);
  int p1 = t.indexOf('/', 5);
  if (p1 < 0 || !t.endsWith("/state")) return;
  String aid = t.substring(5, p1);
  JsonDocument doc;
  if (deserializeJson(doc, payload, length) != DeserializationError::Ok) return;
  JsonObject obj = doc.as<JsonObject>();
  JsonObject::iterator it = obj.begin();
  if (it == obj.end()) return;
  const char *dp = it->key().c_str();
  char val[16] = {0};
  if (it->value().is<bool>()) strlcpy(val, it->value().as<bool>() ? "true" : "false", sizeof(val));
  else snprintf(val, sizeof(val), "%d", it->value().as<int>());
  bemfa::sync_state(aid.c_str(), dp, val);
}

void reconnect() {
  unsigned long now = millis();
  if (now - last_reconnect < 5000) return;
  last_reconnect = now;
  client.setServer(host, port);
  if (client.connect("ha_bridge_esp32")) {
    client.setCallback(on_message);
    on_connect(nullptr, nullptr, 0);
  } else {
    Serial.printf("[ha] connect fail state=%d\n", client.state());
  }
}

void loop() {
  if (!client.connected()) { reconnect(); }
  client.loop();
}

// 保留(ESP32 本机涂鸦状态通路恢复时可用)
void publish_state(const char *aid, const char *dps_json) {
  String t = String("zhsh/") + aid + "/state";
  client.publish(t.c_str(), dps_json, true);
}

void publish_temp_state(const char *aid, const char *dp, const char *value) {
  String t = String("zhsh/") + aid + "/" + (strcmp(dp, "32") == 0 ? "temp_state" : "temp_current");
  String payload = String("{\"") + dp + "\":" + value + "}";
  client.publish(t.c_str(), payload.c_str(), true);
}

void setup(const BridgeConfig &cfg) {
  strlcpy(host, cfg.ha_host, sizeof(host));
  port = cfg.ha_port;
}

}  // namespace ha
