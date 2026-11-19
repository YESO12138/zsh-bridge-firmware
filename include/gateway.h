#pragma once
#include <Arduino.h>
#include <HTTPClient.h>

// 网关本地 KNX 控制(已实测:controlKnx addr/type/value + PHPSESSID 会话)
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
    bool ok = (code == 200);
    String resp = http.getString();
    http.end();
    logged_in_ = ok && resp.indexOf("\"code\":200") >= 0;
    Serial.printf("[gw] login %s\n", logged_in_ ? "OK" : "FAIL");
    return logged_in_;
  }

  void ensure_login() {
    if (!logged_in_) login();
  }

  bool control_knx(const char *addr, int type, int value) {
    ensure_login();
    HTTPClient http;
    http.begin(base_ + "/api.php?service=KnxService&method=controlKnx");
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    http.setTimeout(8000);
    String body = String("addr=") + addr + "&type=" + type + "&value=" + value;
    int code = http.POST(body);
    String resp = http.getString();
    http.end();
    bool ok = (code == 200) && resp.indexOf("\"code\":200") >= 0;
    if (!ok) logged_in_ = false;  // 会话可能过期
    Serial.printf("[gw] controlKnx %s type=%d value=%d -> %s\n", addr, type, value, ok ? "OK" : "FAIL");
    return ok;
  }

private:
  String base_;
  String user_;
  String pass_;
  bool logged_in_;
};
