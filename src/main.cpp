#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <time.h>
#include <Preferences.h>
#include "config.h"
#include "config_store.h"
#include "bridge_mqtt.h"
#include "gateway.h"

// 全局网关实例(main 持有,指针传给桥)
static GatewaySide gw("", "", "");

// ================= 运行时状态 =================
static BridgeConfig cfg;
static WebServer web(80);
static bool wifi_ok = false;
static bool ap_mode = false;          // 配网模式(WiFi 连不上时开热点)
static unsigned long wifi_started = 0;

// ================= 设备查找 =================
static const DevEntry *find_dev(const char *aid) {
  for (uint8_t i = 0; i < DEVICE_COUNT; i++)
    if (strcmp(DEVICES[i].aid, aid) == 0) return &DEVICES[i];
  return nullptr;
}
static const char *aid_of(const char *cid) {
  for (uint8_t i = 0; i < DEVICE_COUNT; i++)
    if (strcmp(DEVICES[i].cid, cid) == 0) return DEVICES[i].aid;
  for (uint8_t i = 0; i < PANEL_COUNT; i++)
    if (strcmp(PANELS[i].cid, cid) == 0) return PANELS[i].aid;
  return nullptr;
}

// ================= 地址反查表(状态轮询 → 设备/dp)=================
struct AddrMap {
  char addr[16];
  const DevEntry *dev;
  const char *dp;
};
static std::vector<AddrMap> addr_map;

static void addr_fb(String &a) {  // 反馈地址 = 发送地址末段 +128(气候类实测规律)
  int p = a.lastIndexOf('/');
  if (p > 0) { int s = a.substring(p + 1).toInt() + 128; if (s <= 255) a = a.substring(0, p + 1) + s; }
}
static void addrmap_add(const char *addr, const DevEntry *dev, const char *dp) {
  if (!addr) return;
  String a(addr);
  AddrMap m; strlcpy(m.addr, a.c_str(), sizeof(m.addr)); m.dev = dev; m.dp = dp;
  addr_map.push_back(m);
  String fb(a); addr_fb(fb);
  if (fb != a) { AddrMap f; strlcpy(f.addr, fb.c_str(), sizeof(f.addr)); f.dev = dev; f.dp = dp; addr_map.push_back(f); }
}
static void addrmap_build() {
  addr_map.clear();
  for (uint8_t i = 0; i < DEVICE_COUNT; i++) {
    const DevEntry *d = &DEVICES[i];
    addrmap_add(d->knx_sw, d, d->state_dp);
    addrmap_add(d->knx_temp, d, strcmp(d->type, "thermostat") == 0 ? "32" : "2");
    addrmap_add(d->knx_mode, d, "4");
    addrmap_add(d->knx_fan, d, "5");
  }
  Serial.printf("[map] 地址反查表 %d 条\n", addr_map.size());
}
static const AddrMap *addrmap_find(const char *addr) {
  for (auto &m : addr_map)
    if (strcmp(m.addr, addr) == 0) return &m;
  return nullptr;
}

// ================= 状态路由 =================
static void route_state(const char *aid, const char *dp, const char *value);

// ================= 命令路由(KNX 本地直控,毫秒级)=================
static void route_command(const char *aid, const char *dp, const char *value, uint8_t ctype) {
  (void)ctype;  // 全本地执行,ctype 仅作日志
  const DevEntry *dev = find_dev(aid);
  if (!dev) { Serial.printf("[exec] 未知设备 %s\n", aid); return; }
  bool bval = (strcmp(value, "true") == 0);
  int ival = atoi(value);

  const char *addr = nullptr;
  int type = 0;
  int val = 0;
  if (strcmp(dp, "1") == 0 && dev->knx_sw) {
    addr = dev->knx_sw; type = 0; val = bval ? 1 : 0;
  } else if ((strcmp(dp, "32") == 0 || strcmp(dp, "2") == 0) && dev->knx_temp) {
    addr = dev->knx_temp; type = 2; val = ival;
  } else if (strcmp(dp, "4") == 0 && dev->knx_mode) {
    addr = dev->knx_mode; type = 1; val = ival;
  } else if (strcmp(dp, "5") == 0 && dev->knx_fan) {
    addr = dev->knx_fan; type = 1; val = ival;
  } else if (dev->knx_sw) {
    // 任意回路 dp(面板灯 24-27):一律走 knx_sw 开关语义
    addr = dev->knx_sw; type = 0; val = bval ? 1 : 0;
  }
  if (addr && gw.control_knx(addr, type, val)) {
    Serial.printf("[exec] %s dp%s=%s via KNX %s\n", aid, dp, value, addr);
    // 立即回显状态(乐观更新,轮询会校正)
    route_state(dev->aid, dp, bval ? "true" : String(ival).c_str());
    return;
  }
  Serial.printf("[exec] %s dp%s 无 KNX 地址或执行失败(待网关导出)\n", aid, dp);
}

// ================= 状态路由 =================
static void route_state(const char *aid, const char *dp, const char *value) {
  // HA 状态:温度走 temp_state,其余走 state
  // HA 状态:温度走 temp_state,其余走 state
  bool is_temp = (strcmp(dp, "32") == 0 || strcmp(dp, "2") == 0 || strcmp(dp, "24") == 0);
  String payload = String("{\"") + dp + "\":" + (strcmp(value, "true") == 0 || strcmp(value, "false") == 0 ? value : String(atoi(value)).c_str()) + "}";
  if (is_temp) ha::publish_temp_state(aid, dp, strcmp(value, "true") == 0 ? "1" : value);
  else {
    ha::publish_state(aid, payload.c_str());
    bemfa::sync_state(aid, dp, value);
  }
}

// ================= 状态轮询(queryKnx 循环,替代涂鸦云状态)=================
static char poll_modules[16][16];   // 去重后的 moduleId 列表
static uint8_t poll_count = 0;
static uint8_t poll_idx = 0;
static unsigned long last_poll = 0;
static bool poll_raw_logged = false;   // 首轮打印原始响应(现场调整解析用)

static void poll_build() {
  poll_count = 0;
  for (uint8_t i = 0; i < DEVICE_COUNT && poll_count < 16; i++) {
    const char *m = DEVICES[i].module;
    if (!m || !m[0]) continue;
    bool dup = false;
    for (uint8_t j = 0; j < poll_count; j++)
      if (strcmp(poll_modules[j], m) == 0) { dup = true; break; }
    if (!dup) { strlcpy(poll_modules[poll_count], m, 16); poll_count++; }
  }
  Serial.printf("[poll] 轮询模块 %d 个\n", poll_count);
}

// 容错解析:递归找 {addr 地址键, value 值键} 对
static void poll_walk(JsonVariant v, std::vector<std::pair<String, String>> &out, int depth) {
  if (depth > 6 || out.size() >= 64) return;
  if (v.is<JsonObject>()) {
    JsonObject o = v.as<JsonObject>();
    String addr = "";
    for (const char *k : {"addr", "address", "ga", "group"})
      if (o[k].is<const char*>() || o[k].is<int>()) { addr = o[k].as<String>(); break; }
    if (addr.length()) {
      for (const char *vk : {"value", "val", "current", "state", "cur"}) {
        if (o[vk].is<JsonVariant>() && !o[vk].isNull()) {
          out.push_back({addr, o[vk].as<String>()});
          break;
        }
      }
    }
    for (JsonPair kv : o) poll_walk(kv.value(), out, depth + 1);
  } else if (v.is<JsonArray>()) {
    for (JsonVariant e : v.as<JsonArray>()) poll_walk(e, out, depth + 1);
  }
}

static void poll_once() {
  if (poll_count == 0 || !wifi_ok) return;
  const char *mod = poll_modules[poll_idx];
  poll_idx = (poll_idx + 1) % poll_count;
  String resp = gw.query_knx(mod);
  if (resp.length() == 0) return;
  if (!poll_raw_logged) { Serial.printf("[poll] %s 原始响应: %s\n", mod, resp.c_str()); poll_raw_logged = true; }
  JsonDocument doc;
  if (deserializeJson(doc, resp) != DeserializationError::Ok) return;
  std::vector<std::pair<String, String>> pairs;
  poll_walk(doc.as<JsonVariant>(), pairs, 0);
  for (auto &p : pairs) {
    const AddrMap *m = addrmap_find(p.first.c_str());
    if (!m) continue;
    String raw = p.second;
    raw.toLowerCase();
    const char *val = (raw == "1" || raw == "true" || raw == "on") ? "true"
                    : (raw == "0" || raw == "false" || raw == "off") ? "false" : raw.c_str();
    route_state(m->dev->aid, m->dp, val);
  }
}

// ================= Web 配置页 =================
static void handle_root() {
  String html = "<html><head><meta charset=utf-8><title>桥配置</title></head><body>"
                "<h2>全屋桥配置</h2>"
                "<h3>WiFi(首次必配)</h3>"
                "WiFi 名称: <input name=wifi_ssid value=''><br>"
                "WiFi 密码: <input name=wifi_pass type=password value=''><br>"
                "<h3>服务凭据</h3>"
                "<form method=POST action=/save>"
                "bemfa uid: <input name=bemfa_uid value='" + String(cfg.bemfa_uid) + "'><br>"
                "涂鸦 user: <input name=tuya_user value='" + String(cfg.tuya_user) + "'><br>"
                "涂鸦 pass: <input name=tuya_pass value='" + String(cfg.tuya_pass) + "'><br>"
                "涂鸦 clientId: <input name=tuya_cid value='" + String(cfg.tuya_client_id) + "'><br>"
                "会话 localKey: <input name=session_key value='" + String(cfg.session_key) + "'><br>"
                "网关 devId: <input name=gw_dev value='" + String(cfg.gateway_dev_id) + "'><br>"
                "HA broker: <input name=ha_host value='" + String(cfg.ha_host) + "'><br>"
                "网关地址: <input name=gw_host value='" + String(cfg.gw_host) + "'><br>"
                "网关账号: <input name=gw_user value='" + String(cfg.gw_user) + "'><br>"
                "网关密码: <input name=gw_pass value='" + String(cfg.gw_pass) + "'><br>"
                "<input type=submit value=保存并重启></form>"
                "<p><a href=/topics>主题管理(米家/小爱入口)</a> | <a href=/devices>设备对照表</a></p></body></html>";
  web.send(200, "text/html", html);
}

static void handle_save() {
  if (web.hasArg("bemfa_uid")) strlcpy(cfg.bemfa_uid, web.arg("bemfa_uid").c_str(), sizeof(cfg.bemfa_uid));
  if (web.hasArg("tuya_user")) strlcpy(cfg.tuya_user, web.arg("tuya_user").c_str(), sizeof(cfg.tuya_user));
  if (web.hasArg("tuya_pass")) strlcpy(cfg.tuya_pass, web.arg("tuya_pass").c_str(), sizeof(cfg.tuya_pass));
  if (web.hasArg("tuya_cid")) strlcpy(cfg.tuya_client_id, web.arg("tuya_cid").c_str(), sizeof(cfg.tuya_client_id));
  if (web.hasArg("session_key")) strlcpy(cfg.session_key, web.arg("session_key").c_str(), sizeof(cfg.session_key));
  if (web.hasArg("gw_dev")) strlcpy(cfg.gateway_dev_id, web.arg("gw_dev").c_str(), sizeof(cfg.gateway_dev_id));
  if (web.hasArg("ha_host")) strlcpy(cfg.ha_host, web.arg("ha_host").c_str(), sizeof(cfg.ha_host));
  if (web.hasArg("gw_host")) strlcpy(cfg.gw_host, web.arg("gw_host").c_str(), sizeof(cfg.gw_host));
  if (web.hasArg("gw_user")) strlcpy(cfg.gw_user, web.arg("gw_user").c_str(), sizeof(cfg.gw_user));
  if (web.hasArg("gw_pass")) strlcpy(cfg.gw_pass, web.arg("gw_pass").c_str(), sizeof(cfg.gw_pass));
  config_save(cfg);
  if (web.hasArg("wifi_ssid") && web.arg("wifi_ssid").length() > 0) {
    Preferences wp;
    wp.begin("wifi", false);
    wp.putString("ssid", web.arg("wifi_ssid"));
    wp.putString("pass", web.arg("wifi_pass"));
    wp.end();
  }
  bool has_wifi = false;
  {
    Preferences wp;
    wp.begin("wifi", true);
    has_wifi = wp.getString("ssid", "").length() > 0;
    wp.end();
  }
  web.send(200, "text/html",
           String("已保存。") + (has_wifi ? "WiFi 已配置,3 秒后重启连家里网络…" : "WiFi 未配置,重启后进入配网热点…"));
  delay(1500);
  ESP.restart();
}

// ================= setup / loop =================
static unsigned long last_wifi_check = 0;

void setup_wifi() {
  Preferences p;
  p.begin("wifi", true);
  String ssid = p.getString("ssid", "");
  String pass = p.getString("pass", "");
  p.end();
  if (ssid.length() == 0) { ssid = "YOUR_WIFI_SSID"; pass = "YOUR_WIFI_PASS"; }
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  int wait = 0;
  while (WiFi.status() != WL_CONNECTED && wait < 30) { delay(500); wait++; }
  wifi_ok = WiFi.status() == WL_CONNECTED;
  if (!wifi_ok) {
    // 配网模式:开热点 ZSH-Bridge,手机连 192.168.4.1 配置
    ap_mode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP("ZSH-Bridge", "zsh12345");
    Serial.println("[wifi] AP 模式:连接热点 ZSH-Bridge,浏览器开 192.168.4.1 配置");
  } else {
    ap_mode = false;
    Serial.printf("[wifi] %s OK\n", WiFi.localIP().toString().c_str());
  }
  configTime(8 * 3600, 0, "ntp.aliyun.com", "pool.ntp.org");
}

void setup() {
  Serial.begin(115200);
  delay(200);
  config_load(cfg);
  setup_wifi();

  // OTA
  ArduinoOTA.setHostname("zsh-bridge");
  ArduinoOTA.setPassword("zshota");
  ArduinoOTA.begin();

  // 回调接线(bemfa/HA 命令 → route_command;轮询状态 → route_state)
  command_cb = route_command;

  // 网关 + 两路 MQTT(涂鸦云状态通路已弃用:TLS 指纹白名单仅 Python 可过,全本地化)
  gw.configure(cfg.gw_host, cfg.gw_user, cfg.gw_pass);
  gw.ensure_login();
  bemfa::setup(cfg);
  ha::setup(cfg);
  addrmap_build();
  poll_build();

  // Web 配置页
  web.on("/", HTTP_GET, handle_root);
  web.on("/save", HTTP_POST, handle_save);
  web.on("/topics", HTTP_GET, []() {
    String html = "<html><head><meta charset=utf-8><title>主题管理</title></head><body>"
                  "<h2>bemfa 主题管理</h2><h3>现有主题</h3><ul>";
    String tj = bemfa::topics_json();
    JsonDocument doc;
    if (deserializeJson(doc, tj) == DeserializationError::Ok) {
      for (JsonObject o : doc.as<JsonArray>()) {
        html += "<li>" + String(o["topic"].as<const char*>()) + " → " +
                String(o["device"].as<const char*>()) + " (" + String(o["kind"].as<const char*>()) +
                " dp" + String(o["dp"].as<const char*>()) + ") <form style=display:inline method=POST action=/topics/del>"
                "<input type=hidden name=topic value='" + String(o["topic"].as<const char*>()) +
                "'><input type=submit value=删除></form></li>";
      }
    } else {
      html += "<li>(解析失败)</li>";
    }
    html += "</ul><h3>新增主题</h3><form method=POST action=/topics/add>"
            "主题名: <input name=topic><br>"
            "设备 aid: <input name=device><br>"
            "类型: <select name=kind>"
                "<option value=socket>插座(001)</option>"
                "<option value=light>灯(002)</option>"
                "<option value=fan>风扇(003)</option>"
                "<option value=sensor>传感器(004)</option>"
                "<option value=ac>空调(005)</option>"
                "<option value=switch>开关(006)</option>"
                "<option value=curtain>窗帘(009)</option>"
                "</select><br>"
            "主状态 dp: <input name=dp value=1><br>"
            "<input type=submit value=添加></form>"
            "<p><a href=/>返回配置页</a></p></body></html>";
    web.send(200, "text/html", html);
  });
  web.on("/topics/add", HTTP_POST, []() {
    if (web.hasArg("topic") && web.hasArg("device") && web.hasArg("kind"))
      bemfa::add_topic(web.arg("topic").c_str(), web.arg("device").c_str(),
                       web.arg("kind").c_str(), web.arg("dp").length() ? web.arg("dp").c_str() : "1");
    web.send(200, "text/html", "已添加,新增主题即时订阅生效。<a href=/topics>返回</a>");
  });
  web.on("/devices", HTTP_GET, []() {
    String html = "<html><head><meta charset=utf-8><title>设备对照表</title></head><body>"
                  "<h2>设备 aid 对照表(添加主题时用)</h2><table border=1 cellpadding=4>"
                  "<tr><th>aid</th><th>名称</th><th>类型</th><th>KNX 开关</th><th>KNX 温度</th><th>模块</th></tr>";
    for (uint8_t i = 0; i < DEVICE_COUNT; i++) {
      const DevEntry &dev = DEVICES[i];
      html += String("<tr><td>") + dev.aid + "</td><td>" + dev.name + "</td><td>" + dev.type +
              "</td><td>" + (dev.knx_sw ? dev.knx_sw : "-") + "</td><td>" +
              (dev.knx_temp ? dev.knx_temp : "-") + "</td><td>" + (dev.module[0] ? dev.module : "-") + "</td></tr>";
    }
    html += "</table><p><a href=/topics>返回主题管理</a> | <a href=/>返回配置页</a></p></body></html>";
    web.send(200, "text/html", html);
  });
  web.on("/topics/del", HTTP_POST, []() {
    if (web.hasArg("topic")) bemfa::del_topic(web.arg("topic").c_str());
    web.send(200, "text/html", "已删除。<a href=/topics>返回</a>");
  });
  web.begin();
}

void loop() {
  ArduinoOTA.handle();
  web.handleClient();
  bemfa::loop();
  ha::loop();

  // 状态轮询:每 8s 查一个模块(顺序低频,避免打崩网关协议栈)
  if (millis() - last_poll > 8000) {
    last_poll = millis();
    poll_once();
  }

  // WiFi 重连
  if (millis() - last_wifi_check > 30000) {
    last_wifi_check = millis();
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect();
      WiFi.reconnect();
    }
  }
  delay(2);
}
