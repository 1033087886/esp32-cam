# ESP32-CAM MQTT + WebSocket 推流

这个项目基于 PlatformIO，使用 ESP32-CAM 实现：

- MQTT 远程控制补光灯亮度（0-255）
- MQTT 在线状态/心跳上报
- WebSocket 二进制 JPEG 推流
- IPv6 状态检测（含 `GLOBAL / LINK_LOCAL` 类型判定）

## 目录结构

- `src/main.cpp`：ESP32 固件主程序
- `server.py`：NAS 中转脚本（多观众模式）
- `index.html`：上位机网页端示例
- `platformio.ini`：PlatformIO 工程配置

## 编译与烧录

```bash
platformio run
platformio run -t upload
platformio device monitor
```

## 主要 MQTT 主题

设备基础前缀：

```text
esp32cam/<deviceId>/
```

示例主题：

- `cmd/light`：控制灯（`0-255` / `on` / `off` / `toggle`）
- `state/light`：灯状态
- `state/online`：在线状态（LWT）
- `state/heartbeat`：心跳（含 IPv4/IPv6/RSSI）
- `ack`：命令回执

## 注意事项

- 请在 `src/main.cpp` 中按实际环境修改 WiFi、MQTT、推流地址参数。
- 当前代码已支持 IPv6 申请与类型判定；若仅出现 `LINK_LOCAL`，表示不可直接公网路由。
- 手机热点场景通常不需要 Portal 认证，本项目默认已关闭 `enablePortalAuth`。
