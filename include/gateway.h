#pragma once
#include <Arduino.h>
#include <HTTPClient.h>

// 网关本地 KNX 控制(已实测:controlKnx addr/type/value + PHPSESSID 会话)
// 扩展:loadModules/loadElements/queryKnx —— 设备发现 + 状态轮询(替代涂鸦云状态)
class GatewaySide {
public:
  GatewaySide(const char *host, const char *user, const char *pass)
      : base_(String("http://") + host), user_(user), pass_(pass), logged_in_(false) {}

  void configure(const char *host, const char *user, const char *pass) {
    base_ = String("http://") + host;
    user_ = user;
    pass_ = pass;
    logged_in_ = false;
  }

  bool login() {
    HTTPClient http;
    http.begin(base_ + "/api.php?service=UserService&method=login");
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    http.setTimeout(8000);
    int code = http.POST(String("name=") + user_ + "&pwd=" + pass_);
    String resp = http.getString();
    http.end();
    logged_in_ = (code == 200) && resp.indexOf("\"code\":200") >= 0;
    Serial.printf("[gw] login %s\n", logged_in_ ? "OK" : "FAIL");
    return logged_in_;
  }

  void ensure_login() {
    if (!logged_in_) login();
  }

  // 通用 POST 表单(返回 body;会话失效自动重登一次)
  String post_api(const char *url, const String &body) {
    ensure_login();
    HTTPClient http;
    http.begin(base_ + url);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    http.setTimeout(8000);
    int code = http.POST(body);
    String resp = http.getString();
    http.end();
    if (!((code == 200) && resp.indexOf("\"code\":200") >= 0)) {
      // 会话可能过期,重登重试一次
      if (login()) {
        http.begin(base_ + url);
        http.addHeader("Content-Type", "application/x-www-form-urlencoded");
        http.setTimeout(8000);
        code = http.POST(body);
        resp = http.getString();
        http.end();
      }
    }
    return resp;
  }

  bool control_knx(const char *addr, int type, int value) {
    String body = String("addr=") + addr + "&type=" + type + "&value=" + value;
    String resp = post_api("/api.php?service=KnxService&method=controlKnx", body);
    bool ok = resp.indexOf("\"code\":200") >= 0;
    Serial.printf("[gw] controlKnx %s type=%d value=%d -> %s\n", addr, type, value, ok ? "OK" : "FAIL");
    return ok;
  }

  // 模块发现:loadModules roomId=-1(全屋);返回原始 JSON
  String load_modules(const char *room_id = "-1") {
    return post_api("/api.php?service=KnxService&method=loadModules", String("roomId=") + room_id);
  }

  // 单模块地址表(loadElements)
  String load_elements(const char *module_id) {
    return post_api("/api.php?service=KnxService&method=loadElements", String("moduleId=") + module_id);
  }

  // 单模块当前状态(queryKnx)
  String query_knx(const char *module_id) {
    return post_api("/api.php?service=KnxService&method=queryKnx", String("moduleId=") + module_id);
  }

  // zigbee 子设备刷新(面板/窗帘等先 sync 再查)
  String sync_zigbee() {
    ensure_login();
    HTTPClient http;
    http.begin(base_ + "/api.php?service=KnxService&method=syncZigbee");
    http.setTimeout(15000);
    int code = http.GET();
    String resp = http.getString();
    http.end();
    Serial.printf("[gw] syncZigbee code=%d len=%d\n", code, resp.length());
    return resp;
  }

private:
  String base_;
  String user_;
  String pass_;
  bool logged_in_;
};
