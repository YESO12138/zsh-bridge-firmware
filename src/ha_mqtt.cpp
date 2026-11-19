#include "bridge_mqtt.h"
#include <WiFiClient.h>

// ============================================================
// HA MQTT(discovery 自动生成 + 命令订阅 + 状态发布)
// ============================================================
namespace ha {

WiFiClient plain;
PubSubClient client(plain);
bool connected = false;

static char host[96] = "";
static uint16_t port = 1883;
static unsigned long last_reconnect = 0;

static const DevEntry *find_dev(const char *aid) {
  for (uint8_t i = 0; i < DEVICE_COUNT; i++)
    if (strcmp(DEVICES[i].aid, aid) == 0) return &DEVICES[i];
  return nullptr;
}

static void on_connect(char *, byte *, unsigned int) {
  Serial.println("[ha] broker connected");
  connected = true;
  for (uint8_t i = 0; i < DEVICE_COUNT; i++) {
    String t1 = String("zhsh/") + DEVICES[i].aid + "/set";
    String t2 = String("zhsh/") + DEVICES[i].aid + "/temp_set";
    client.subscribe(t1.c_str(), 1);
    client.subscribe(t2.c_str(), 1);
  }
}

static void on_message(char *topic, byte *payload, unsigned int length) {
  // topic: zhsh/<aid>/set  payload: {"dp":"27","value":true}(dp 任意回路)
  String t(topic);
  if (!t.startsWith("zhsh/")) return;
  int p1 = t.indexOf('/', 5);
  if (p1 < 0) return;
  String aid = t.substring(5, p1);
  char value[32] = {0};
  size_t n = length < sizeof(value) - 1 ? length : sizeof(value) - 1;
  memcpy(value, payload, n);
  value[n] = 0;
  JsonDocument doc;
  if (deserializeJson(doc, payload, length) == DeserializationError::Ok && doc["dp"].is<const char*>()) {
    // payload 带 dp:{"dp":"27","value":true}
    String dp(doc["dp"].as<const char*>());
    String val;
    if (doc["value"].is<bool>()) val = doc["value"].as<bool>() ? "true" : "false";
    else val = String(doc["value"].as<int>());
    if (command_cb) command_cb(aid.c_str(), dp.c_str(), val.c_str(), 2);
  } else {
    // 裸 payload:按 /set → dp1, /temp_set → dp32
    if (command_cb) command_cb(aid.c_str(), "1", value, 2);
  }
}

void reconnect() {
  unsigned long now = millis();
  if (now - last_reconnect < 5000) return;
  last_reconnect = now;
  client.setServer(host, port);
  if (client.connect("ha_bridge_esp32")) {
    connected = true;
    client.setCallback(on_message);
    on_connect(nullptr, nullptr, 0);
    publish_discovery();
  } else {
    Serial.printf("[ha] connect fail state=%d\n", client.state());
  }
}

void loop() {
  if (!client.connected()) { reconnect(); }
  client.loop();
}

static String dev_info_json(const char *cid, const char *name, const char *type) {
  return String("{\"identifiers\":[\"zhsh_") + cid + "\"],\"name\":\"" + name +
         "\",\"manufacturer\":\"deepsmart(KNX via 智好生活)\",\"model\":\"" + type +
         "\",\"via_device\":\"ha_bridge\"}";
}

static String avail_json(const char *aid) {
  return String("\"availability_topic\":\"zhsh/") + aid +
         "/availability\",\"payload_available\":\"online\",\"payload_not_available\":\"offline\"";
}

void publish_discovery() {
  if (!connected) return;
  // 气候/灯设备:开关 + 温度 number + 温控器传感器
  for (uint8_t i = 0; i < DEVICE_COUNT; i++) {
    const DevEntry &dev = DEVICES[i];
    String aid(dev.aid);
    String eid = String("zhsh_") + aid;
    String dev_json = dev_info_json(dev.cid, dev.name, dev.type);
    String avail = avail_json(aid.c_str());

    String sw = String("{\"name\":\"") + dev.name + "\",\"unique_id\":\"" + eid +
                "\",\"command_topic\":\"zhsh/" + aid + "/set\",\"state_topic\":\"zhsh/" + aid +
                "/state\",\"payload_on\":\"true\",\"payload_off\":\"false\",\"state_on\":\"true\",\"state_off\":\"false\",\"value_template\":\"{{ value_json['" +
                dev.state_dp + "'] | lower }}\"," + avail + ",\"device\":" + dev_json + "}";
    client.publish((String("homeassistant/switch/") + eid + "/config").c_str(), sw.c_str(), true);

    if (dev.temp_hi > 0) {
      String num = String("{\"name\":\"") + dev.name + " 温度设定\",\"unique_id\":\"" + eid +
                   "_temp\",\"command_topic\":\"zhsh/" + aid + "/temp_set\",\"state_topic\":\"zhsh/" + aid +
                   "/temp_state\",\"value_template\":\"{{ value_json['" + (strcmp(dev.type, "thermostat") == 0 ? "32" : "2") +
                   "'] }}\",\"min\":" + String(dev.temp_lo) + ",\"max\":" + String(dev.temp_hi) +
                   ",\"step\":1,\"unit_of_measurement\":\"℃\"," + avail + ",\"device\":" + dev_json + "}";
      client.publish((String("homeassistant/number/") + eid + "_temp/config").c_str(), num.c_str(), true);
    }
    if (strcmp(dev.type, "thermostat") == 0) {
      String cur = String("{\"name\":\"") + dev.name + " 当前温度\",\"unique_id\":\"" + eid +
                   "_cur\",\"state_topic\":\"zhsh/" + aid +
                   "/temp_current\",\"value_template\":\"{{ value_json['24'] }}\",\"unit_of_measurement\":\"℃\",\"device\":" +
                   dev_json + "}";
      client.publish((String("homeassistant/sensor/") + eid + "_cur/config").c_str(), cur.c_str(), true);
      String valve = String("{\"name\":\"") + dev.name + " 阀门\",\"unique_id\":\"" + eid +
                     "_valve\",\"state_topic\":\"zhsh/" + aid +
                     "/valve\",\"value_template\":\"{{ value_json['36'] }}\",\"device\":" + dev_json + "}";
      client.publish((String("homeassistant/sensor/") + eid + "_valve/config").c_str(), valve.c_str(), true);
    }
  }
  // 面板状态传感器
  for (uint8_t i = 0; i < PANEL_COUNT; i++) {
    const PanelEntry &pn = PANELS[i];
    String aid(pn.aid);
    String eid = String("zhsh_") + aid;
    String dev_json = dev_info_json(pn.cid, pn.name, "panel");
    String cfg = String("{\"name\":\"") + pn.name + " 状态\",\"unique_id\":\"" + eid +
                 "\",\"state_topic\":\"zhsh/" + pn.aid +
                 "/state\",\"value_template\":\"{{ value_json | to_json }}\",\"device\":" + dev_json + "}";
    client.publish((String("homeassistant/sensor/") + eid + "/config").c_str(), cfg.c_str(), true);
  }
  Serial.println("[ha] discovery published");
}

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
