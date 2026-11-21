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

// ================= 命令路由(KNX 本地直控,毫秒级)=================
static void route_state(const char *aid, const char *dp, const char *value);
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
  // 回显值:开关类 true/false,数值类(温度/模式/风速)保持数字
  String echo = (strcmp(value, "true") == 0 || strcmp(value, "false") == 0) ? String(value) : String(ival);
  if (strcmp(dev->type, "curtain") == 0) echo = (ival > 0) ? "true" : "false";
  if (addr && gw.control_knx(addr, type, val)) {
    Serial.printf("[exec] %s dp%s=%s via KNX %s\n", aid, dp, value, addr);
    // 立即回显状态(乐观更新,轮询会校正)
    route_state(dev->aid, dp, echo.c_str());
    return;
  }
  // 无 KNX 地址(灯光/窗帘,zigbee 面板回路)→ 涂鸦云端 ctype 路径
  String val2 = value;
  const char *dp2 = dp;
  if (strcmp(dev->type, "curtain") == 0) {
    dp2 = "1";  // 枚举控制 dp
    val2 = (bval || strcmp(value, "false") == 0 || ival > 0) ? (bval || ival > 0 ? "open" : "close") : value;
    if (strcmp(value, "true") == 0) val2 = "open";
    if (strcmp(value, "false") == 0) val2 = "close";
  }
  if (tuya::publish_dps(dev->cid, dp2, val2.c_str(), ctype)) {
    Serial.printf("[exec] %s dp%s=%s via 涂鸦云 ctype%d\n", aid, dp2, val2.c_str(), ctype);
    route_state(dev->aid, dp, echo.c_str());
    return;
  }
  Serial.printf("[exec] %s dp%s 执行失败(KNX 无地址且涂鸦云未连)\n", aid, dp);
}

// ================= 状态路由 =================
static void route_state(const char *aid, const char *dp, const char *value) {
  const DevEntry *st_dev = find_dev(aid);
  if (st_dev && strcmp(st_dev->type, "curtain") == 0 && strcmp(dp, st_dev->state_dp) == 0) {
    value = (atoi(value) > 0 || strcmp(value, "true") == 0) ? "true" : "false";
  }
  // HA 状态:温度走 temp_state,其余走 state
  // HA 状态:温度走 temp_state,其余走 state
  const DevEntry *dev = find_dev(aid);
  bool is_temp = (strcmp(dp, "32") == 0 || strcmp(dp, "2") == 0 ||
                  (strcmp(dp, "24") == 0 && dev && strcmp(dev->type, "thermostat") == 0));
  String payload = String("{\"") + dp + "\":" + (strcmp(value, "true") == 0 || strcmp(value, "false") == 0 ? value : String(atoi(value)).c_str()) + "}";
  if (is_temp) ha::publish_temp_state(aid, dp, strcmp(value, "true") == 0 ? "1" : value);
  else {
    ha::publish_state(aid, payload.c_str());
    bemfa::sync_state(aid, dp, value);
  }
}

// ================= 状态轮询(queryKnx 循环;气候类 dp 键控)=================
struct PollMod {
  char id[16];
  const DevEntry *dev;    // 一模块对一台气候设备
};
static PollMod poll_mods[16];
static uint8_t poll_count = 0;
static uint8_t poll_idx = 0;
static unsigned long last_poll = 0;

// 数值型 dp(温度/模式/风速);其余按开关布尔处理
static bool numeric_dp(const DevEntry *dev, const char *dp) {
  if (strcmp(dp, "2") == 0 || strcmp(dp, "32") == 0 || strcmp(dp, "4") == 0 || strcmp(dp, "5") == 0) return true;
  if (strcmp(dp, "24") == 0 && strcmp(dev->type, "thermostat") == 0) return true;
  if (strcmp(dp, "102") == 0) return true;
  return false;
}

static void poll_build() {
  poll_count = 0;
  for (uint8_t i = 0; i < DEVICE_COUNT && poll_count < 16; i++) {
    const char *m = DEVICES[i].module;
    if (!m || !m[0]) continue;
    bool dup = false;
    for (uint8_t j = 0; j < poll_count; j++)
      if (strcmp(poll_mods[j].id, m) == 0) { dup = true; break; }
    if (!dup) {
      strlcpy(poll_mods[poll_count].id, m, 16);
      poll_mods[poll_count].dev = &DEVICES[i];
      poll_count++;
    }
  }
  Serial.printf("[poll] 轮询模块 %d 个\n", poll_count);
}

static void poll_once() {
  if (poll_count == 0 || !wifi_ok) return;
  PollMod &pm = poll_mods[poll_idx];
  poll_idx = (poll_idx + 1) % poll_count;
  String resp = gw.query_knx(pm.id);
  if (resp.length() == 0) return;
  JsonDocument doc;
  if (deserializeJson(doc, resp) != DeserializationError::Ok) return;
  JsonObject data = doc["data"].as<JsonObject>();
  if (!data) return;
  for (JsonPair kv : data) {
    const char *dp = kv.key().c_str();
    String raw = kv.value().as<String>();
    String val;
    if (numeric_dp(pm.dev, dp)) val = raw;
    else {
      int n = raw.toInt();
      val = (n == 1) ? "true" : (n == 0) ? "false" : raw;
    }
    route_state(pm.dev->aid, dp, val.c_str());
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

  // 回调接线(bemfa/HA 命令 → route_command;涂鸦状态 cid→aid → route_state)
  command_cb = route_command;
  state_cb = [](const char *cid, const char *dp, const char *value) {
    const char *aid = aid_of(cid);
    if (aid) route_state(aid, dp, value);
  };

  // 网关 + 三路 MQTT(气候 KNX 直控;灯光/窗帘涂鸦 ctype;状态 queryKnx 轮询 + 涂鸦回执)
  gw.configure(cfg.gw_host, cfg.gw_user, cfg.gw_pass);
  gw.ensure_login();
  tuya::setup(cfg);
  bemfa::setup(cfg);
  ha::setup(cfg);
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
  tuya::loop();
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
