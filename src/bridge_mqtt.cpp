#include "bridge_mqtt.h"
#include "gateway.h"
#include <mbedtls/gcm.h>
#include <time.h>
#include <Preferences.h>

// ===== 回调 =====
std::function<void(const char *aid, const char *dp, const char *value, uint8_t ctype)> command_cb;
std::function<void(const char *cid, const char *dp, const char *value)> state_cb;

// ===== 网关指针 =====
static GatewaySide *gw_ptr = nullptr;
void bridge_set_gateway(GatewaySide *g) { gw_ptr = g; }

// ============================================================
// AES-128-GCM
// ============================================================
static bool gcm_crypt(bool encrypt, const uint8_t *key, const uint8_t *nonce,
                      const uint8_t *aad, size_t aad_len,
                      const uint8_t *in, size_t in_len,
                      uint8_t *out, uint8_t *tag) {
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  if (mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128) != 0) {
    mbedtls_gcm_free(&ctx);
    return false;
  }
  int r = mbedtls_gcm_crypt_and_tag(&ctx,
      encrypt ? MBEDTLS_GCM_ENCRYPT : MBEDTLS_GCM_DECRYPT,
      in_len, nonce, 12, aad, aad_len, in, out, 16, tag);
  mbedtls_gcm_free(&ctx);
  return r == 0;
}

// ============================================================
// 涂鸦云 MQTT(2.3 协议)
// ============================================================
namespace tuya {

WiFiClientSecure secure;
PubSubClient client(secure);
bool connected = false;

static char gw_dev[32] = "";
static char user[128] = "";
static char pass[64] = "";
static char cid_client[128] = "";
static uint8_t key[17] = {0};
static uint32_t seq = 100;
static unsigned long last_reconnect = 0;
static unsigned long deadline_resp = 0;

static bool build_control(const char *cid, const char *dp, const char *value,
                          uint8_t ctype, uint8_t *out, size_t *out_len) {
  JsonDocument doc;
  JsonObject data = doc["data"].to<JsonObject>();
  data["cid"] = cid;
  data["ctype"] = ctype;
  JsonObject dps = data["dps"].to<JsonObject>();
  if (strcmp(value, "true") == 0) dps[dp] = true;
  else if (strcmp(value, "false") == 0) dps[dp] = false;
  else dps[dp] = atoi(value);
  doc["protocol"] = 5;
  doc["t"] = (uint32_t)time(nullptr);
  char pt[256];
  size_t pt_len = serializeJson(doc, pt, sizeof(pt));
  if (pt_len == 0 || pt_len >= sizeof(pt)) return false;

  uint8_t header[12];
  header[0] = '2'; header[1] = '.'; header[2] = '3';
  uint32_t seq_be = htonl(seq);
  memcpy(header + 3, &seq_be, 4);
  header[7] = 0x00; header[8] = 0x09;
  header[9] = 0x14; header[10] = 0xfe; header[11] = 0x00;

  uint8_t nonce[12];
  for (int i = 0; i < 12; i++) nonce[i] = random(256);
  uint8_t ctb[336];
  if (!gcm_crypt(true, key, nonce, header, 12, (const uint8_t *)pt, pt_len, ctb, ctb + pt_len)) return false;

  memcpy(out, header, 12);
  memcpy(out + 12, nonce, 12);
  memcpy(out + 24, ctb, pt_len + 16);
  *out_len = 24 + pt_len + 16;
  return true;
}

static void on_message(char *topic, byte *payload, unsigned int length) {
  // 协议 4 状态:header(12)+nonce(12)+密文(16 tag 在末尾)
  if (length < 28 + 16 || length >= 1024) return;
  uint8_t buf[1024];
  memcpy(buf, payload, length);
  uint8_t pt[512];
  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  if (mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 128) != 0) { mbedtls_gcm_free(&ctx); return; }
  int rr = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_DECRYPT,
                                     length - 40, buf + 12, 12, buf, 12,
                                     buf + 24, pt, 16, buf + length - 16);
  mbedtls_gcm_free(&ctx);
  if (rr != 0) return;
  size_t pt_len = length - 40;
  pt[pt_len] = 0;

  JsonDocument doc;
  if (deserializeJson(doc, pt, pt_len) != DeserializationError::Ok) return;
  const char *cid = doc["data"]["cid"] | "";
  JsonObject dps = doc["data"]["dps"];
  for (JsonPair kv : dps) {
    const char *dp = kv.key().c_str();
    char val[16];
    if (kv.value().is<bool>()) strlcpy(val, kv.value().as<bool>() ? "true" : "false", sizeof(val));
    else strlcpy(val, String(kv.value().as<int>()).c_str(), sizeof(val));
    if (state_cb) state_cb(cid, dp, val);
  }
}

void reconnect() {
  unsigned long now = millis();
  if (now - last_reconnect < 5000) return;
  last_reconnect = now;
  String in_topic = String("smart/mb/in/") + gw_dev;
  client.setServer(TUYA_MQTT_HOST, TUYA_MQTT_PORT);
  Serial.printf("[tuya] TLS connect to %s:%d...\n", TUYA_MQTT_HOST, TUYA_MQTT_PORT);
  int tcp_ok = secure.connect(TUYA_MQTT_HOST, TUYA_MQTT_PORT);
  Serial.printf("[tuya] TCP/TLS result=%d\n", tcp_ok);
  if (tcp_ok == 1) {
    // 手工 MQTT CONNECT,看服务器原始响应
    size_t cid_l = strlen(cid_client), u_l = strlen(user), p_l = strlen(pass);
    size_t payload_len = 2 + cid_l + 2 + u_l + 2 + p_l;
    uint8_t pkt[512]; size_t oi = 0;
    size_t remain = 10 + payload_len;
    uint8_t rl[4]; size_t rl_n = 0;
    size_t x = remain;
    do { uint8_t d = x % 128; x /= 128; if (x) d |= 0x80; rl[rl_n++] = d; } while (x);
    pkt[oi++] = 0x10;
    memcpy(pkt + oi, rl, rl_n); oi += rl_n;
    pkt[oi++] = 0x00; pkt[oi++] = 0x04; memcpy(pkt + oi, "MQTT", 4); oi += 4;
    pkt[oi++] = 4; pkt[oi++] = 0xC2; pkt[oi++] = 0; pkt[oi++] = 60;
    pkt[oi++] = (cid_l >> 8) & 0xFF; pkt[oi++] = cid_l & 0xFF;
    memcpy(pkt + oi, cid_client, cid_l); oi += cid_l;
    pkt[oi++] = (u_l >> 8) & 0xFF; pkt[oi++] = u_l & 0xFF;
    memcpy(pkt + oi, user, u_l); oi += u_l;
    pkt[oi++] = (p_l >> 8) & 0xFF; pkt[oi++] = p_l & 0xFF;
    memcpy(pkt + oi, pass, p_l); oi += p_l;
    secure.write(pkt, oi);
    delay(800);
    uint8_t resp[64] = {0};
    int rn = 0;
    deadline_resp = millis() + 3000;
    while (millis() < deadline_resp && rn < 4) {
      int got = secure.read(resp + rn, 64 - rn);
      if (got > 0) rn += got;
      delay(20);
    }
    Serial.printf("[tuya] CONNACK(%d):", rn);
    for (int i = 0; i < rn; i++) Serial.printf(" %02X", resp[i]);
    Serial.println();
  }
  secure.stop();
  delay(100);
  if (client.connect(cid_client, user, pass)) {
    connected = true;
    client.setCallback(on_message);
    client.subscribe(in_topic.c_str(), 1);
    Serial.println("[tuya] connected");
  } else {
    Serial.printf("[tuya] connect fail state=%d\n", client.state());
  }
}

void loop() {
  if (!client.connected()) { reconnect(); }
  client.loop();
}

bool publish_dps(const char *cid, const char *dp, const char *value, uint8_t ctype) {
  if (!connected) return false;
  seq++;
  uint8_t out[512];
  size_t out_len = 0;
  if (!build_control(cid, dp, value, ctype, out, &out_len)) return false;
  String out_topic = String("smart/mb/out/") + gw_dev;
  return client.publish(out_topic.c_str(), out, out_len, false);
}

void setup(const BridgeConfig &cfg) {
  strlcpy(gw_dev, cfg.gateway_dev_id, sizeof(gw_dev));
  strlcpy(user, cfg.tuya_user, sizeof(user));
  strlcpy(pass, cfg.tuya_pass, sizeof(pass));
  strlcpy(cid_client, cfg.tuya_client_id, sizeof(cid_client));
  strlcpy((char *)key, cfg.session_key, 17);
  key[16] = 0;
  secure.setInsecure();
  client.setBufferSize(768);
}
}  // namespace tuya
