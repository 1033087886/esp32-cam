# Cloudflare Worker + Durable Objects（无 NAS）部署指南

本目录提供一个可直接部署的中转方案：

- `GET /healthz`：健康检查，返回 `ok`
- `WS /esp32`：ESP32 作为 publisher 推流入口
- `WS /viewer`：观众端拉流入口
- Durable Object 负责按 `room` 分房，并将 publisher 的二进制帧广播给所有 viewer

## 1. 前置准备

- Cloudflare 账号
- Node.js 18+（建议 20+）
- 在本机可执行 `npx wrangler`

## 2. 登录 Cloudflare

在本目录执行：

```bash
cd cf-worker
npx wrangler login
```

## 3. 配置密钥

必填（ESP 推流鉴权）：

```bash
npx wrangler secret put INGEST_TOKEN
```

可选（viewer 鉴权）：

```bash
npx wrangler secret put VIEWER_TOKEN
```

说明：

- 若配置 `VIEWER_TOKEN`，`/viewer` 必须携带 `token`。
- 若不配置 `VIEWER_TOKEN`，`/viewer` 允许匿名访问。

## 4. 部署

```bash
npx wrangler deploy
```

部署后会拿到一个 `workers.dev` 域名，例如：

`https://esp32-cam-relay.<subdomain>.workers.dev`

## 5. 验证

健康检查：

```bash
curl https://esp32-cam-relay.<subdomain>.workers.dev/healthz
```

应返回：

```text
ok
```

WebSocket 地址示例：

- ESP 推流（publisher）  
  `wss://esp32-cam-relay.<subdomain>.workers.dev/esp32?room=cam01&token=<INGEST_TOKEN>`
- 观众拉流（viewer，无 viewer token 时）  
  `wss://esp32-cam-relay.<subdomain>.workers.dev/viewer?room=cam01`
- 观众拉流（viewer，已配置 viewer token 时）  
  `wss://esp32-cam-relay.<subdomain>.workers.dev/viewer?room=cam01&token=<VIEWER_TOKEN>`

## 6. 绑定自定义域名

推荐在 Cloudflare Dashboard 操作：

1. 进入 `Workers & Pages` -> 选择该 Worker
2. 打开 `Settings` -> `Domains & Routes`
3. 点击 `Add Custom Domain`
4. 填入例如 `stream.example.com`
5. 等待证书签发并生效

生效后，你可以直接使用：

- `wss://stream.example.com/esp32?room=cam01&token=...`
- `wss://stream.example.com/viewer?room=cam01`

## 7. 与 ESP / 前端联动

- ESP：把 `src/main.cpp` 的推流地址改为 `/esp32?room=...`，`token` 通过查询参数追加，不要硬编码到固件仓库。
- 前端：`index.html` 支持 `?ws=<viewer_ws_url>` 动态覆盖，便于不同环境切换。
