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
