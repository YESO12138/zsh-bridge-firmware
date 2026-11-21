#pragma once
#include <Arduino.h>
#include <HTTPClient.h>

// 网关本地 KNX 控制(controlKnx + queryKnx 轮询;PHPSESSID 会话必须带回)
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

  // 响应成功判定(兼容 "code":200 与 "code": 200 两种排版)
  static bool ok_code(const String &resp) {
    int i = resp.indexOf("\"code\"");
    if (i < 0) return false;
    int j = resp.indexOf('}', i);
    if (j < 0) j = i + 20;
    return resp.substring(i, j).indexOf("200") >= 0;
  }

  bool login() {
    HTTPClient http;
    http.begin(base_ + "/api.php?service=UserService&method=login");
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    const char *hk[] = {"Set-Cookie"};
    http.collectHeaders(hk, 1);
    http.setTimeout(8000);
    int code = http.POST(String("name=") + user_ + "&pwd=" + pass_);
    String resp = http.getString();
    String sc = http.header("Set-Cookie");
    http.end();
    if (sc.length()) {   // 抓 PHPSESSID,后续请求全部带回
      int semi = sc.indexOf(';');
      session_ = semi > 0 ? sc.substring(0, semi) : sc;
    }
    logged_in_ = (code == 200) && ok_code(resp);
    Serial.printf("[gw] login %s %s\n", logged_in_ ? "OK" : "FAIL", session_.c_str());
    return logged_in_;
  }

  void ensure_login() {
    if (!logged_in_) login();
  }

  // 通用 POST 表单(自动带 Cookie;失败重登一次)
  String post_api(const char *url, const String &body) {
    ensure_login();
    for (int attempt = 0; attempt < 2; attempt++) {
      HTTPClient http;
      http.begin(base_ + url);
      http.addHeader("Content-Type", "application/x-www-form-urlencoded");
      if (session_.length()) http.addHeader("Cookie", session_);
      http.setTimeout(8000);
      int code = http.POST(body);
      String resp = http.getString();
      http.end();
      if ((code == 200) && ok_code(resp)) { logged_in_ = true; return resp; }
      logged_in_ = false;
      if (attempt == 0) login();  // 会话过期重登后重试
    }
    return "";
  }

  bool control_knx(const char *addr, int type, int value) {
    String body = String("addr=") + addr + "&type=" + type + "&value=" + value;
    String resp = post_api("/api.php?service=KnxService&method=controlKnx", body);
    bool ok = ok_code(resp);
    Serial.printf("[gw] controlKnx %s type=%d value=%d -> %s\n", addr, type, value, ok ? "OK" : "FAIL");
    return ok;
  }

  String load_modules(const char *room_id = "-1") {
    return post_api("/api.php?service=KnxService&method=loadModules", String("roomId=") + room_id);
  }
  String load_elements(const char *module_id) {
    return post_api("/api.php?service=KnxService&method=loadElements", String("moduleId=") + module_id);
  }
  String query_knx(const char *module_id) {
    return post_api("/api.php?service=KnxService&method=queryKnx", String("moduleId=") + module_id);
  }

  String sync_zigbee() {
    return post_api("/api.php?service=KnxService&method=syncZigbee", "");
  }

private:
  String base_;
  String user_;
  String pass_;
  String session_;      // PHPSESSID=...
  bool logged_in_;
};
