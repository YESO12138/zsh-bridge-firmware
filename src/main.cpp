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

// ================= 命令路由 =================
// KNX 优先(气候),回退涂鸦 ctype;灯光走 ctype1
static void route_command(const char *aid, const char *dp, const char *value, uint8_t ctype) {
  const DevEntry *dev = find_dev(aid);
  if (!dev) return;
  bool bval = (strcmp(value, "true") == 0);
  int ival = atoi(value);

  // KNX 本地直控(气候设备有映射时,毫秒级)
  {
    const char *addr = nullptr;
    int type = 0;
    int val = 0;
    if (strcmp(dp, "1") == 0 && dev->knx_sw) {
      addr = dev->knx_sw; type = 0; val = bval ? 1 : 0;
    } else if (strcmp(dp, "32") == 0 && dev->knx_temp) {
      addr = dev->knx_temp; type = 2; val = ival;
    } else if (strcmp(dp, "2") == 0 && dev->knx_temp) {
      addr = dev->knx_temp; type = 2; val = ival;
    } else if (strcmp(dp, "4") == 0 && dev->knx_mode) {
      addr = dev->knx_mode; type = 1; val = ival;
    } else if (strcmp(dp, "5") == 0 && dev->knx_fan) {
      addr = dev->knx_fan; type = 1; val = ival;
    }
    if (addr && gw.control_knx(addr, type, val)) {
      Serial.printf("[exec] %s dp%s=%s via KNX %s\n", aid, dp, addr);
      return;  // 本地成功,无需转发
    }
    Serial.printf("[exec] %s dp%s KNX 不可用,转发 Mac 桥\n", aid, dp);
  }
  // 转发 Mac 桥执行(灯光/窗帘走涂鸦通路;涂鸦 TLS 指纹仅 Python 客户端可过)
  ha::forward_command(aid, dp, value);
}

// ================= 状态路由 =================
static void route_state(const char *cid, const char *dp, const char *value) {
  const char *aid = aid_of(cid);
  if (!aid) return;
  // HA 状态:{dp:value} retained
  String payload = String("{\"") + dp + "\":" + (strcmp(value, "true") == 0 || strcmp(value, "false") == 0 ? value : String(atoi(value)).c_str()) + "}";
  ha::publish_state(aid, payload.c_str());
  // bemfa 状态同步(主题表驱动:匹配设备+dp 自动回写 on/off)
  bemfa::sync_state(aid, dp, value);
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
                "空调群组主题(005): <input name=bf_ac_topic value='" + String(cfg.bemfa_ac_topic) + "'><br>"
                "射灯主题(002): <input name=bf_lt_topic value='" + String(cfg.bemfa_light_topic) + "'><br>"
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
                "<p><a href=/topics>主题管理(米家/小爱入口)</a></p></body></html>";
  web.send(200, "text/html", html);
}

static void handle_save() {
  if (web.hasArg("bemfa_uid")) strlcpy(cfg.bemfa_uid, web.arg("bemfa_uid").c_str(), sizeof(cfg.bemfa_uid));
  if (web.hasArg("bf_ac_topic")) strlcpy(cfg.bemfa_ac_topic, web.arg("bf_ac_topic").c_str(), sizeof(cfg.bemfa_ac_topic));
  if (web.hasArg("bf_lt_topic")) strlcpy(cfg.bemfa_light_topic, web.arg("bf_lt_topic").c_str(), sizeof(cfg.bemfa_light_topic));
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

  // 三路 MQTT + 网关
  bridge_set_gateway(&gw);
  tuya::setup(cfg);
  bemfa::setup(cfg);
  ha::setup(cfg);

  // Web 配置页
  web.on("/", HTTP_GET, handle_root);
  web.on("/save", HTTP_POST, handle_save);
  web.on("/topics", HTTP_GET, []() {
    String html = "<html><head><meta charset=utf-8><title>主题管理</title></head><body>"
                  "<h2>bemfa 主题管理</h2><h3>现有主题</h3><ul>";
    // 解析 bemfa topics_json
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
                  "<tr><th>aid</th><th>名称</th><th>类型</th><th>KNX 开关</th><th>KNX 温度</th></tr>";
    for (uint8_t i = 0; i < DEVICE_COUNT; i++) {
      const DevEntry &dev = DEVICES[i];
      html += String("<tr><td>") + dev.aid + "</td><td>" + dev.name + "</td><td>" + dev.type +
              "</td><td>" + (dev.knx_sw ? dev.knx_sw : "-") + "</td><td>" +
              (dev.knx_temp ? dev.knx_temp : "-") + "</td></tr>";
    }
    for (uint8_t i = 0; i < PANEL_COUNT; i++) {
      const PanelEntry &pn = PANELS[i];
      html += String("<tr><td>") + pn.aid + "</td><td>" + pn.name + "</td><td>面板状态</td><td>-</td><td>-</td></tr>";
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

  // WiFi 重连
  if (millis() - last_wifi_check > 30000) {
    last_wifi_check = millis();
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect();
      WiFi.reconnect();
    }
  }
  // HA 掉线重发 discovery(ha::loop 内部处理重连,重连后 on_connect 重发订阅;
  // discovery 由 retained 保持,无需周期重发)
  delay(2);
}
