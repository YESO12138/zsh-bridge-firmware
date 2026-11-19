# 智好生活桥固件(ESP32-S3)

将智好生活(KNX 全屋)接入米家/小爱/HA 的常驻桥固件。

> ⚠️ 表中的 nodeId / KNX 地址为**示例格式**,部署时按自家网关导出的配置修改(见下方"获取自家配置")。

## 功能

| 通路 | 说明 |
|---|---|
| 巴法云 MQTT | 米家/小爱语音入口,双主题(空调 005 格式 + 灯 002 格式),主题表驱动可持续添加 |
| HA MQTT | discovery 自动生成(开关/温度 number/状态传感器),命令订阅,状态 retained |
| 网关 controlKnx | 气候设备本地直控(毫秒级),会话自动重登 |
| Web 配置页 | :80 凭据维护 + 主题管理(增删即时生效)|
| OTA | 空中固件升级 |

## 编译烧录

```bash
pip3 install platformio
cd bridge_firmware
pio run -e esp32s3                # 编译
pio run -e esp32s3 -t upload      # 烧录(USB 连板)
pio device monitor                # 串口日志
```

板型:ESP32-S3 DevKitC(N16R8 参考);经典 ESP32 用 `esp32dev` 环境(需改回)。

## 部署配置

首次上电 → WiFi 连不上时自动开热点 `ZSH-Bridge`(密码 zsh12345)→ 手机连热点 → 浏览器 `192.168.4.1` → 配置页填写:

| 配置项 | 获取方法 |
|---|---|
| bemfa uid + 主题 | 巴法云控制台(主题后三位定类型:001 插座/002 灯/005 空调/006 开关/009 窗帘)|
| 涂鸦 user/pass/clientId/session key/gateway devId | 见"获取涂鸦凭据"(需手机 App 会话)|

## 获取涂鸦凭据

涂鸦 MQTT 凭据绑定手机 App 的登录会话,获取方法:

1. 手机与抓包环境同网段(工具与教程社区共有,略)
2. 智好生活 App 操作任意设备,抓取 MQTT CONNECT 参数(user/pass/clientId)
3. 抓取状态回执,解密获得 session key(localKey)与网关 devId

> 涂鸦 broker 对 TLS 客户端指纹有白名单(Python/OpenSSL ✓,mbedTLS ✗)——固件的涂鸦模块仅用于状态监听;灯光命令的实际执行由 Python 端完成,ESP32 负责转发。

## 获取自家设备表

1. 登录网关控制台(`http://192.168.1.200`)→ KnxService → exportConfig → 气候设备表(uuid + protocolObjId → KNX 地址)
2. 面板回路 dp 对照:智好生活 App 场景详情 + 设备 dpName(抓包或 API)
3. 将对照表填入 `include/config.h` 的 DEVICES 数组(nodeId/type/KNX 地址),重新编译

## 目录

```
include/config.h       设备表 + 协议常量 + 默认配置
include/gateway.h      网关控制类
src/bridge_mqtt.cpp    涂鸦云 MQTT(2.3 协议 + AES-GCM)
src/ha_bemfa.cpp       巴法云 MQTT(主题表驱动)
src/ha_mqtt.cpp        HA MQTT(discovery + 命令)
src/config_store.cpp   NVS 凭据存取
src/main.cpp           编排(WiFi/OTA/Web/三路循环)
```
