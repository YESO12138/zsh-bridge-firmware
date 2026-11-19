#include "bridge_mqtt.h"
#include <WiFiClient.h>
#include <Preferences.h>

// ============================================================
// bemfa MQTT(米家/小爱入口,主题表驱动)
// 主题表存 NVS(key: bf_topics,JSON 数组):
//   [{"t":"主题","d":"设备aid","k":"ac|light","dp":"主状态dp"}]
// kind: "ac"(bemfa 空调格式 on#温度) | "light"(on/off → dp)
// ============================================================
namespace bemfa {

WiFiClient plain;
PubSubClient client(plain);
bool connected = false;

struct TopicEntry {
  char topic[64];
  char device_aid[40];
  char kind[10];
  char dp[8];
};
static std::vector<TopicEntry> topics;
static char uid[64] = "";
static unsigned long last_reconnect = 0;

struct LastRaw {
  char topic[64];
  char raw[32];
};
static std::vector<LastRaw> last_raws;
static const char *last_raw_of(const char *topic) {
  for (auto &lr : last_raws)
    if (strcmp(lr.topic, topic) == 0) return lr.raw;
  return nullptr;
}
static void set_last_raw(const char *topic, const char *raw) {
  for (auto &lr : last_raws)
    if (strcmp(lr.topic, topic) == 0) { strlcpy(lr.raw, raw, sizeof(lr.raw)); return; }
  LastRaw lr;
  strlcpy(lr.topic, topic, sizeof(lr.topic));
  strlcpy(lr.raw, raw, sizeof(lr.raw));
  last_raws.push_back(lr);
}

static void route_command(const TopicEntry &t, const char *raw) {
  if (!command_cb) return;
  if (strcmp(t.kind, "ac") == 0) {
    if (strcmp(raw, "on") == 0) { command_cb(t.device_aid, "1", "true", 0); return; }
    if (strcmp(raw, "off") == 0) { command_cb(t.device_aid, "1", "false", 0); return; }
    if (strncmp(raw, "on#", 3) == 0) {
      int temp = atoi(raw + 3);
      if (temp >= 5 && temp <= 35) command_cb(t.device_aid, "32", raw + 3, 0);
    }
  } else if (strcmp(t.kind, "light") == 0) {
    const char *v = strcmp(raw, "on") == 0 ? "true" : strcmp(raw, "off") == 0 ? "false" : nullptr;
    if (v) command_cb(t.device_aid, t.dp, v, 1);
  }
}

static void on_message(char *topic, byte *payload, unsigned int length) {
  char raw[32] = {0};
  size_t n = length < sizeof(raw) - 1 ? length : sizeof(raw) - 1;
  memcpy(raw, payload, n);
  raw[n] = 0;
  const char *prev = last_raw_of(topic);
  if (prev && strcmp(prev, raw) == 0) return;  // 回显去重
  set_last_raw(topic, raw);
  // 遍历所有匹配条目(群组主题 = 同名多映射,每台设备各自执行)
  for (auto &te : topics)
    if (strcmp(te.topic, topic) == 0) route_command(te, raw);
}

static void on_connect(char *, byte *, unsigned int) {
  Serial.println("[bemfa] connected");
  for (auto &te : topics) client.subscribe(te.topic, 1);
  connected = true;
}

void reconnect() {
  unsigned long now = millis();
  if (now - last_reconnect < 5000) return;
  last_reconnect = now;
  client.setServer("mqtt.bemfa.com", 9501);
  if (client.connect(uid, uid, "")) {
    client.setCallback(on_message);
    on_connect(nullptr, nullptr, 0);
  } else {
    Serial.printf("[bemfa] connect fail state=%d\n", client.state());
  }
}

void loop() {
  if (!client.connected()) reconnect();
  client.loop();
}

void load_topics() {
  Preferences p;
  p.begin("zshbridge", true);
  String json = p.getString("bf_topics", "[]");
  p.end();
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) return;
  topics.clear();
  for (JsonObject item : doc.as<JsonArray>()) {
    TopicEntry t;
    strlcpy(t.topic, item["t"] | "", sizeof(t.topic));
    strlcpy(t.device_aid, item["d"] | "", sizeof(t.device_aid));
    strlcpy(t.kind, item["k"] | "light", sizeof(t.kind));
    strlcpy(t.dp, item["dp"] | "1", sizeof(t.dp));
    if (strlen(t.topic) && strlen(t.device_aid)) topics.push_back(t);
  }
}

void save_topics() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (auto &t : topics) {
    JsonObject o = arr.add<JsonObject>();
    o["t"] = t.topic;
    o["d"] = t.device_aid;
    o["k"] = t.kind;
    o["dp"] = t.dp;
  }
  String out;
  serializeJson(doc, out);
  Preferences p;
  p.begin("zshbridge", false);
  p.putString("bf_topics", out);
  p.end();
}

void add_topic(const char *topic, const char *device_aid, const char *kind, const char *dp) {
  for (auto &t : topics)
    if (strcmp(t.topic, topic) == 0) return;  // 已存在
  TopicEntry t;
  strlcpy(t.topic, topic, sizeof(t.topic));
  strlcpy(t.device_aid, device_aid, sizeof(t.device_aid));
  strlcpy(t.kind, kind, sizeof(t.kind));
  strlcpy(t.dp, dp, sizeof(t.dp));
  topics.push_back(t);
  save_topics();
  if (connected) client.subscribe(topic, 1);  // 立即订阅生效
}

// 主题列表(web 配置页用)
String topics_json() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (auto &t : topics) {
    JsonObject o = arr.add<JsonObject>();
    o["topic"] = t.topic;
    o["device"] = t.device_aid;
    o["kind"] = t.kind;
    o["dp"] = t.dp;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

void del_topic(const char *topic) {
  for (auto it = topics.begin(); it != topics.end(); ++it)
    if (strcmp(it->topic, topic) == 0) { topics.erase(it); break; }
  save_topics();
}

// 状态回写:设备 dp 状态 → 匹配主题(on/off),回显去重
void sync_state(const char *device_aid, const char *dp, const char *value) {
  for (auto &t : topics) {
    if (strcmp(t.device_aid, device_aid) != 0) continue;
    const char *key = (strcmp(t.kind, "ac") == 0) ? "1" : t.dp;
    if (strcmp(dp, key) != 0) continue;
    const char *raw = (strcmp(value, "true") == 0 || strcmp(value, "1") == 0) ? "on" : "off";
    const char *prev = last_raw_of(t.topic);
    if (prev && strcmp(prev, raw) == 0) continue;
    set_last_raw(t.topic, raw);
    client.publish(t.topic, raw, 1);
    Serial.printf("[bemfa] state sync %s = %s\n", t.topic, raw);
  }
}

void setup(const BridgeConfig &cfg) {
  strlcpy(uid, cfg.bemfa_uid, sizeof(uid));
  load_topics();
}

}  // namespace bemfa
