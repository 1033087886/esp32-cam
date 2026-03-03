# ESP32-CAM 问题处理总结（2026-03-03）

## 1. 已解决的问题

1. PlatformIO 依赖拉取失败（无法编译）
- 现象：`https://gitee.com/EspressifSystems/esp32-camera.git` 返回 404。
- 根因：`platformio.ini` 中依赖指向失效的 Gitee 镜像。
- 处理：改为官方 GitHub 固定 commit。

2. 编译错误：`esp_crt_bundle_attach` 未声明
- 现象：`esp_crt_bundle_attach was not declared in this scope`。
- 根因：Arduino-ESP32 环境中常用符号是 `arduino_esp_crt_bundle_attach`，而不是 `esp_crt_bundle_attach`。
- 处理：增加兼容解析逻辑（优先 `arduino_esp_crt_bundle_attach`，回退 `esp_crt_bundle_attach`）。

3. WS TLS 握手失败（`-0x2700` / `-0x7680`）
- 现象：
  - `-0x2700`：证书校验失败。
  - `-0x7680`：缺少可用 CA 链。
  - 并出现 `arduino_esp_crt_bundle_attach(): Failed to attach bundle`。
- 根因：
  - 证书包在自定义 `esp_tls` 路径下未正确可用。
  - 站点实际证书链与原硬编码证书不一致时会失败。
- 处理：
  - 为 WS 改为明确 CA 链（WE1 + GlobalSign ECC Root R4）。
  - 保留并增强 TLS 失败日志，输出 `tlsCode/tlsFlags` 便于后续定位。

4. WS 连接仍失败（非设备问题）
- 现象：连接失败但 WiFi/NTP/MQTT 正常。
- 定位：实测握手返回 `401 Unauthorized`，响应体 `invalid ingest token`。
- 根因：Worker `env.INGEST_TOKEN` 与设备 URL 中 `token` 不一致。
- 处理：确认 Worker 侧必须使用密钥名 `INGEST_TOKEN`，其值与设备端 token 完全一致。

5. Token 硬编码风险
- 现象：`main.cpp` 里写死了 `token`，改一次要改代码。
- 处理：改为 `build_flags` 注入（`STREAM_WS_HOST/ROOM/TOKEN`），避免反复改源码。

## 2. 代码改动摘要

### `platformio.ini`
- 依赖源改为官方：
  - `https://github.com/espressif/esp32-camera.git#efe711df9a348c56e56e5d7b8961836d94a0f9c1`
- 新增流媒体配置构建参数：
  - `-D STREAM_WS_HOST=\"stream.rose980.eu.cc\"`
  - `-D STREAM_WS_ROOM=\"cam01\"`
  - `-D STREAM_WS_TOKEN=\"1033087886\"`

### `src/main.cpp`
- 新增 WS 宏配置（可由 `build_flags` 注入）：
  - `STREAM_WS_HOST / STREAM_WS_ROOM / STREAM_WS_TOKEN`
- URL/Path 从硬编码改为宏拼接：
  - `streamWsUrl = STREAM_WS_URL_LITERAL`
  - `streamWsPath = STREAM_WS_PATH_LITERAL`
- TLS 兼容性增强：
  - `esp_crt_bundle.h` 双 include 兼容。
  - `arduino_esp_crt_bundle_attach / esp_crt_bundle_attach` 兼容解析。
- TLS 诊断增强：
  - 连接失败时打印 `ret/tlsCode/tlsFlags/host/port`。
- WS 证书链更新：
  - 使用 `WE1 + GlobalSign ECC Root R4`。
- 启动告警：
  - `STREAM_WS_TOKEN` 为空时打印提示（避免误判 401）。

## 3. 已完成验证

多次本地编译验证通过：

```powershell
C:\Users\rose\.platformio\penv\Scripts\platformio.exe run -e esp32cam
```

结果：`[SUCCESS]`

并通过请求验证到 Worker 返回：
- `401 Unauthorized`
- `invalid ingest token`

这与 Worker 鉴权逻辑一致，说明当时失败主因是服务端 token 不匹配，而不是摄像头初始化或 MQTT 问题。

## 4. 你现在只需做的一步

在 Cloudflare Worker Secrets 中：
- 只保留密钥名：`INGEST_TOKEN`
- 将其值设置为：`1033087886`（或把本地 `STREAM_WS_TOKEN` 改成 Worker 的当前值）
- 重新部署 Worker

配置对齐后，WS 握手应返回 `101 Switching Protocols`，串口会出现“推流连接成功”。
