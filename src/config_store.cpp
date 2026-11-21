#include <Preferences.h>
#include "config.h"

static Preferences prefs;

void config_load(BridgeConfig &cfg) {
  cfg = DEFAULT_CONFIG;
  prefs.begin("zshbridge", true);
  if (prefs.isKey("bemfa_uid"))   strlcpy(cfg.bemfa_uid,      prefs.getString("bemfa_uid").c_str(),      sizeof(cfg.bemfa_uid));
  if (prefs.isKey("tuya_user"))   strlcpy(cfg.tuya_user,      prefs.getString("tuya_user").c_str(),      sizeof(cfg.tuya_user));
  if (prefs.isKey("tuya_pass"))   strlcpy(cfg.tuya_pass,      prefs.getString("tuya_pass").c_str(),      sizeof(cfg.tuya_pass));
  if (prefs.isKey("tuya_cid"))    strlcpy(cfg.tuya_client_id, prefs.getString("tuya_cid").c_str(),       sizeof(cfg.tuya_client_id));
  if (prefs.isKey("session_key")) strlcpy(cfg.session_key,    prefs.getString("session_key").c_str(),    sizeof(cfg.session_key));
  if (prefs.isKey("gw_dev"))      strlcpy(cfg.gateway_dev_id, prefs.getString("gw_dev").c_str(),         sizeof(cfg.gateway_dev_id));
  if (prefs.isKey("ha_host"))     strlcpy(cfg.ha_host,        prefs.getString("ha_host").c_str(),        sizeof(cfg.ha_host));
  cfg.ha_port = prefs.getUShort("ha_port", cfg.ha_port);
  if (prefs.isKey("gw_host"))     strlcpy(cfg.gw_host,        prefs.getString("gw_host").c_str(),        sizeof(cfg.gw_host));
  if (prefs.isKey("gw_user"))     strlcpy(cfg.gw_user,        prefs.getString("gw_user").c_str(),        sizeof(cfg.gw_user));
  if (prefs.isKey("gw_pass"))     strlcpy(cfg.gw_pass,        prefs.getString("gw_pass").c_str(),        sizeof(cfg.gw_pass));
  prefs.end();
}

void config_save(const BridgeConfig &cfg) {
  prefs.begin("zshbridge", false);
  prefs.putString("bemfa_uid", cfg.bemfa_uid);
  prefs.putString("tuya_user", cfg.tuya_user);
  prefs.putString("tuya_pass", cfg.tuya_pass);
  prefs.putString("tuya_cid", cfg.tuya_client_id);
  prefs.putString("session_key", cfg.session_key);
  prefs.putString("gw_dev", cfg.gateway_dev_id);
  prefs.putString("ha_host", cfg.ha_host);
  prefs.putUShort("ha_port", cfg.ha_port);
  prefs.putString("gw_host", cfg.gw_host);
  prefs.putString("gw_user", cfg.gw_user);
  prefs.putString("gw_pass", cfg.gw_pass);
  prefs.end();
}
