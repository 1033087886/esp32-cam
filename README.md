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

## 摄像头兼容说明（OV2640 / OV3660 / GC2145）

- `initCamera()` 会打印探测 PID，并区分 `OV2640 / OV3660 / GC2145 / UNKNOWN`。
- 默认输出模式是 `PIXFORMAT_JPEG`，并根据是否有 PSRAM 选择：
  - 有 PSRAM：`VGA`
  - 无 PSRAM：`QVGA`
- 检测到 OV3660 时，会应用 OV3660 保守参数。
- 检测到（或强制为）GC2145 时，会应用 GC2145 稳定参数（优先 JPEG 推流稳定性）。

## GC2145 依赖版本说明（含网络要求）

- 当前 `platformio.ini` 默认固定 `esp32-camera` 为 gitee 镜像（commit pin）：
  - `https://gitee.com/EspressifSystems/esp32-camera.git#efe711df9a348c56e56e5d7b8961836d94a0f9c1`
- 若你所在网络可直连（或代理可达）GitHub，也可切换到同一 commit pin：
  - `https://github.com/espressif/esp32-camera.git#efe711df9a348c56e56e5d7b8961836d94a0f9c1`
  - 该来源要求当前代理/网络可访问 `github.com`。
- 若 gitee / github 都不可用，可从 `components.espressif.com` 获取 `esp32-camera` 源码后本地引用（例如放入 `lib/` 后使用本地库路径）。
- 若该依赖版本不含 GC2145 驱动，`esp_camera_init()` 会失败或 PID 识别异常。
- 固件启动日志会打印：
  - `"[CAM] 依赖检查: gc2145 header hint=found/not-found"`
  - 可用于快速判断当前编译环境是否看到了 gc2145 相关头文件。
- 建议在编译时做一次明确验证（需已安装 PlatformIO）：

```bash
pio run -e esp32cam -v | rg -i "gc2145|sensor_gc2145"
```

若输出中出现 `sensor_gc2145` 相关编译条目，说明 GC2145 驱动已参与构建。

## 强制传感器类型（CAM_FORCE_SENSOR）

项目支持通过编译宏强制传感器类型，用于 probe 误判场景。默认 `auto`。

- 可选值：`auto` / `ov2640` / `ov3660` / `gc2145`
- 默认值：`platformio.ini` 中 `-D CAM_FORCE_SENSOR=auto`

示例：强制按 GC2145 初始化参数分支

```bash
pio run -e esp32cam --project-option="build_flags=-D CAM_FORCE_SENSOR=gc2145"
```

也可以直接修改 `platformio.ini` 中的 `build_flags`。

## GC2145 硬件与验证

- 硬件上需使用兼容 ESP32-CAM DVP 引脚定义的 GC2145 模组（本项目使用 AI Thinker 引脚映射）。
- 上电后串口日志应至少看到以下信息：
  - `"[CAM] 探测 PID=0x2145, detected=GC2145"`（或强制模式下出现强制覆盖日志）
  - `"[CAM] CAM_FORCE_SENSOR raw=... parsed=... enabled=..."`
  - `"[CAM] 最终配置 pixformat=JPEG(...), framesize=QVGA/VGA(...), jpeg_quality=..."`
- 若出现 `参数设置失败` 日志，说明某项 sensor setter 未生效；固件会继续运行并保留告警。

## Cloudflare Worker 无 NAS 中转方案

该方案使用 `Cloudflare Workers + Durable Objects` 代替 NAS 中转：

- ESP32 作为 publisher 连接 `WS /esp32`
- 浏览器 viewer 连接 `WS /viewer`
- Durable Object 按 `room` 分房，把 publisher 的 JPEG 二进制帧广播给同房间全部 viewer
- `GET /healthz` 可用于健康检查

### 架构说明

1. ESP32 发送帧到 Worker 的 `/esp32?room=...&token=...`
2. Worker 进行 token 鉴权后，将连接路由到对应 room 的 Durable Object
3. Viewer 连接 `/viewer?room=...`（可选 token）
4. Durable Object 接收 publisher 二进制帧并广播给所有 viewer

### 优势

- 不依赖家庭/NAS 公网入口
- 原生 HTTPS/WSS 与边缘接入，部署简单
- 多 viewer 分发由 Durable Object 承担，ESP 只维护一条上行

### 限制

- 仍受 ESP 上行带宽与帧率/分辨率约束
- Worker/DO 有平台配额与计费限制
- 全球跨区观看会引入网络抖动，建议结合前端重连与码率控制

### 快速部署入口

- 直接按文档执行：`cf-worker/README.md`
- 目录：`cf-worker/`

### ESP 与前端改动点

- ESP 端默认推流地址已改为：
  - `wss://stream.rose980.eu.cc/esp32?room=cam01`
  - 如需鉴权，在查询参数追加 `&token=...`（不要硬编码 token）
- 前端 `index.html` 已支持：
  - 优先读取 URL 参数 `ws`（例如 `?ws=wss%3A%2F%2Fstream.example.com%2Fviewer%3Froom%3Dcam01`）
  - 未传 `ws` 时默认使用 `wss://stream.rose980.eu.cc/viewer?room=cam01`
