#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoWebsockets.h>
#include "esp_camera.h"
#include <time.h>
#include <string.h>
#include <ctype.h>
#include "esp_netif.h"
#include "lwip/ip6_addr.h"
#if __has_include(<esp32/spiram.h>)
#include <esp32/spiram.h>
#define HAS_SPIRAM_CHIP_API 1
#else
#define HAS_SPIRAM_CHIP_API 0
#endif

#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#endif

#ifndef GC2145_PID
#define GC2145_PID 0x2145
#endif

#ifndef CAM_FORCE_SENSOR
#define CAM_FORCE_SENSOR auto
#endif

#ifndef STREAM_WS_HOST
#define STREAM_WS_HOST "8.166.129.84"
#endif

#ifndef STREAM_WS_PORT
#define STREAM_WS_PORT 8888
#endif

#ifndef STREAM_WS_ROOM
#define STREAM_WS_ROOM "cam01"
#endif

#ifndef STREAM_WS_TOKEN
#define STREAM_WS_TOKEN ""
#endif

#define CAM_STRINGIFY_IMPL(value) #value
#define CAM_STRINGIFY(value) CAM_STRINGIFY_IMPL(value)

#if __has_include("sensors/private_include/gc2145_settings.h")
#define HAS_GC2145_DRIVER_HEADER_HINT 1
#else
#define HAS_GC2145_DRIVER_HEADER_HINT 0
#endif

using namespace websockets;

enum class CameraSensorProfile : uint8_t {
  UNKNOWN = 0,
  OV2640,
  OV3660,
  GC2145
};

enum class WiFiControlState : uint8_t {
  BOOT = 0,
  CONNECTING,
  CONNECTED,
  PROVISIONING
};

void connectWiFi();
void maintainWiFiConnection();
void beginProvisioningCandidate(const String& ssid, const String& password);
bool loadStoredWiFiCredentials();
void saveStoredWiFiCredentials(const String& ssid, const String& password);
void clearStoredWiFiCredentials();
void startProvisioningAp(const char* reason);
void stopProvisioningAp();
void setupProvisioningServer();
void handleProvisioningRoot();
void handleProvisioningScan();
void handleProvisioningConnect();
void handleProvisioningStatus();
void handleProvisioningReset();
void handleProvisioningNotFound();
void handleLight();
String buildProvisioningStatusJson();
String escapeJson(const String& value);
const char* wifiControlStateToString(WiFiControlState state);
const char* wifiAuthModeToString(wifi_auth_mode_t authMode);
void doPortalAuth();
void testInternet();
void setupMqttTransport();
void syncTimeIfNeeded();
void connectMQTT();
void onWiFiEvent(WiFiEvent_t event);
void requestIPv6(const char* reason);
void refreshIPv6Status(bool verbose);
const char* classifyIPv6Type(const esp_ip6_addr_t& addr);
int ipv6TypeScore(const char* type);
void mqttCallback(char* topic, byte* payload, unsigned int length);
void publishHeartbeat();
void publishLightState();
void setFlashBrightness(uint8_t value, bool reportState);
String decodeCommandPayload(byte* payload, unsigned int length);
bool parseBrightnessCommand(String cmd, uint8_t& outValue);
bool initCamera();
void connectStreamWs();
void sendStreamFrameIfReady();
void setupTopics();
void setupFlashPwm();
void publishAck(const char* msg);
String extractJsonValue(const String& json, const String& key);
void printMemoryInfo(const char* stage);
const char* psramChipSizeText(int chipEnum);
const char* cameraSensorProfileToString(CameraSensorProfile profile);
CameraSensorProfile cameraSensorProfileFromPid(uint16_t pid);
CameraSensorProfile parseForcedSensorProfile(
  const char* raw,
  bool* forceEnabled,
  bool* valueValid,
  char* normalized,
  size_t normalizedSize
);
const char* pixformatToString(pixformat_t pixformat);
const char* frameSizeToString(framesize_t frameSize);
template <typename T>
bool applySensorSetting(sensor_t* sensor, const char* settingName, T value, int (*setter)(sensor_t*, T));

#ifndef DEFAULT_WIFI_SSID
#define DEFAULT_WIFI_SSID ""
#endif

#ifndef DEFAULT_WIFI_PASSWORD
#define DEFAULT_WIFI_PASSWORD ""
#endif

const char* wifiPrefsNamespace = "wifi_cfg";
const uint32_t wifiPrefsVersion = 1;
const unsigned long wifiConnectTimeoutInitial = 20000UL;
const unsigned long wifiConnectTimeoutRecovery = 60000UL;
const char* provisioningApPrefix = "ESP32CAM-";
const IPAddress provisioningApIp(192, 168, 4, 1);

const char provisioningPageHtml[] PROGMEM = R"HTML(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32-CAM 配网</title>
  <style>
    :root { color-scheme: light dark; }
    body { font-family: Arial, sans-serif; margin: 0; background: #0f172a; color: #e2e8f0; }
    .wrap { max-width: 760px; margin: 0 auto; padding: 20px; }
    .card { background: #111827; border-radius: 16px; padding: 18px; margin-bottom: 16px; box-shadow: 0 8px 30px rgba(0,0,0,.25); }
    h1, h2 { margin-top: 0; }
    label { display: block; margin: 12px 0 6px; font-weight: 600; }
    input, button { width: 100%; box-sizing: border-box; border-radius: 10px; border: 1px solid #334155; padding: 12px; font-size: 16px; }
    input { background: #0b1220; color: #e2e8f0; }
    button { background: #2563eb; color: white; border: 0; cursor: pointer; margin-top: 10px; }
    button.secondary { background: #334155; }
    button.warn { background: #b91c1c; }
    ul { list-style: none; padding: 0; margin: 0; }
    li { padding: 12px; border-radius: 10px; background: #0b1220; margin-bottom: 8px; cursor: pointer; }
    .muted { color: #94a3b8; }
    .status { white-space: pre-wrap; line-height: 1.6; }
    .row { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }
    @media (max-width: 640px) { .row { grid-template-columns: 1fr; } }
  </style>
</head>
<body>
  <div class="wrap">
    <div class="card">
      <h1>ESP32-CAM 配网</h1>
      <div id="status" class="status muted">正在读取状态...</div>
    </div>

    <div class="card">
      <div class="row">
        <button type="button" onclick="scanNetworks()">扫描附近 Wi‑Fi</button>
        <button type="button" class="secondary" onclick="refreshStatus()">刷新状态</button>
      </div>
      <div id="scanHint" class="muted" style="margin-top:10px">点击上方按钮获取网络列表，也可以手动输入隐藏 SSID。</div>
      <ul id="networks" style="margin-top:12px"></ul>
    </div>

    <div class="card">
      <h2>连接新的 Wi‑Fi</h2>
      <form id="wifiForm">
        <label for="ssid">Wi‑Fi 名称 (SSID)</label>
        <input id="ssid" name="ssid" autocomplete="off" placeholder="输入或点击上方列表自动填充">
        <label for="password">Wi‑Fi 密码</label>
        <input id="password" name="password" type="password" placeholder="至少 8 位，开放网络可留空">
        <button type="submit">保存并连接</button>
      </form>
      <button type="button" class="warn" onclick="resetConfig()">清除已保存配置并重新配网</button>
    </div>
  </div>

  <script>
    const statusEl = document.getElementById('status');
    const scanHintEl = document.getElementById('scanHint');
    const networksEl = document.getElementById('networks');
    const ssidEl = document.getElementById('ssid');
    const passwordEl = document.getElementById('password');

    function pickNetwork(name) {
      ssidEl.value = name;
      passwordEl.focus();
    }

    function renderStatus(data) {
      const lines = [
        `设备: ${data.device_id || '-'}`,
        `状态: ${data.state || '-'}`,
        `当前 Wi‑Fi: ${data.connected_ssid || '-'}`,
        `已保存 Wi‑Fi: ${data.saved_ssid || '-'}`,
        `STA IP: ${data.sta_ip || '-'}`,
        `AP 热点: ${data.ap_ssid || '-'} (${data.ap_ip || '-'})`,
        `提示: ${data.message || '-'}`
      ];
      statusEl.textContent = lines.join('\n');
    }

    async function refreshStatus() {
      try {
        const resp = await fetch('/status');
        const data = await resp.json();
        renderStatus(data);
      } catch (err) {
        statusEl.textContent = '读取状态失败: ' + err;
      }
    }

    async function scanNetworks() {
      networksEl.innerHTML = '';
      scanHintEl.textContent = '扫描中，请稍候...';
      try {
        const resp = await fetch('/scan');
        const data = await resp.json();
        const items = data.networks || [];
        if (!items.length) {
          scanHintEl.textContent = data.message || '没有扫描到 Wi‑Fi，请手动输入。';
          return;
        }
        scanHintEl.textContent = '点击列表项可自动填入 SSID。';
        items.forEach((item) => {
          const li = document.createElement('li');
          li.textContent = `${item.ssid}  ·  ${item.rssi} dBm  ·  ${item.auth}`;
          li.onclick = () => pickNetwork(item.ssid);
          networksEl.appendChild(li);
        });
      } catch (err) {
        scanHintEl.textContent = '扫描失败: ' + err;
      }
    }

    async function submitForm(event) {
      event.preventDefault();
      const ssid = ssidEl.value.trim();
      if (!ssid) {
        alert('SSID 不能为空');
        return;
      }
      const payload = new URLSearchParams({ ssid, password: passwordEl.value });
      const resp = await fetch('/connect', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: payload.toString()
      });
      const data = await resp.json();
      alert(data.message || '已提交，请等待设备连接');
      refreshStatus();
    }

    async function resetConfig() {
      if (!confirm('确定要清除已保存的 Wi‑Fi 配置吗？')) {
        return;
      }
      const resp = await fetch('/reset', { method: 'POST' });
      const data = await resp.json();
      alert(data.message || '已清除配置');
      refreshStatus();
    }

    document.getElementById('wifiForm').addEventListener('submit', submitForm);
    refreshStatus();
    scanNetworks();
    setInterval(refreshStatus, 2500);
  </script>
</body>
</html>
)HTML";

// Optional captive portal auth (from ESP32-mqtt project workflow)
const bool enablePortalAuth = false;
const char* portalAuthBaseUrl = "http://192.168.100.2:8080/wportal/Onekey";
const unsigned long authInterval = 14100000UL;  // 235 minutes
unsigned long previousAuthTime = 0;

// MQTT settings
const char* mqttHost = "hea1fbf1.ala.cn-hangzhou.emqxsl.cn";
const uint16_t mqttPort = 8883;
const char* mqttUsername = "rose";
const char* mqttPassword = "123456";
const bool mqttUseTls = true;

// Fill CA cert only when mqttUseTls=true and broker uses 8883
const char* mqttCaCert = R"EOF(
-----BEGIN CERTIFICATE-----
MIIDjjCCAnagAwIBAgIQAzrx5qcRqaC7KGSxHQn65TANBgkqhkiG9w0BAQsFADBh
MQswCQYDVQQGEwJVUzEVMBMGA1UEChMMRGlnaUNlcnQgSW5jMRkwFwYDVQQLExB3
d3cuZGlnaWNlcnQuY29tMSAwHgYDVQQDExdEaWdpQ2VydCBHbG9iYWwgUm9vdCBH
MjAeFw0xMzA4MDExMjAwMDBaFw0zODAxMTUxMjAwMDBaMGExCzAJBgNVBAYTAlVT
MRUwEwYDVQQKEwxEaWdpQ2VydCBJbmMxGTAXBgNVBAsTEHd3dy5kaWdpY2VydC5j
b20xIDAeBgNVBAMTF0RpZ2lDZXJ0IEdsb2JhbCBSb290IEcyMIIBIjANBgkqhkiG
9w0BAQEFAAOCAQ8AMIIBCgKCAQEAuzfNNNx7a8myaJCtSnX/RrohCgiN9RlUyfuI
2/Ou8jqJkTx65qsGGmvPrC3oXgkkRLpimn7Wo6h+4FR1IAWsULecYxpsMNzaHxmx
1x7e/dfgy5SDN67sH0NO3Xss0r0upS/kqbitOtSZpLYl6ZtrAGCSYP9PIUkY92eQ
q2EGnI/yuum06ZIya7XzV+hdG82MHauVBJVJ8zUtluNJbd134/tJS7SsVQepj5Wz
tCO7TG1F8PapspUwtP1MVYwnSlcUfIKdzXOS0xZKBgyMUNGPHgm+F6HmIcr9g+UQ
vIOlCsRnKPZzFBQ9RnbDhxSJITRNrw9FDKZJobq7nMWxM4MphQIDAQABo0IwQDAP
BgNVHRMBAf8EBTADAQH/MA4GA1UdDwEB/wQEAwIBhjAdBgNVHQ4EFgQUTiJUIBiV
5uNu5g/6+rkS7QYXjzkwDQYJKoZIhvcNAQELBQADggEBAGBnKJRvDkhj6zHd6mcY
1Yl9PMWLSn/pvtsrF9+wX3N3KjITOYFnQoQj8kVnNeyIv/iPsGEMNKSuIEyExtv4
NeF22d+mQrvHRAiGfzZ0JFrabA0UWTW98kndth/Jsw1HKj2ZL7tcu7XUIOGZX1NG
Fdtom/DzMNU+MeKNhJ7jitralj41E6Vf8PlwUHBHQRFXGU7Aj64GxJUTFy8bJZ91
8rGOmaFvE7FBcf6IKshPECBV1/MUReXgRPTqh5Uykw7+U0b6LJ3/iyK5S9kJRaTe
pLiaWN0bfVKfjllDiIGknibVb63dDcY3fe0Dkhvld1927jyNxF1WW6LZZm6zNTfl
MrY=
-----END CERTIFICATE-----
)EOF";

const char* deviceId = "esp32cam-01";

const unsigned long wifiReconnectInterval = 5000UL;
const unsigned long mqttReconnectInterval = 5000UL;
const unsigned long mqttHeartbeatInterval = 30000UL;
const unsigned long wsReconnectInterval = 3000UL;
const unsigned long streamInterval = 100UL;  // ~10 FPS target (quality-first)
const unsigned long streamSendWarmupMs = 1000UL;
const unsigned long streamStatsInterval = 5000UL;
const size_t streamPreferredMaxFrameBytes = 85000;
const uint8_t streamJpegQualityPsram = 8;      // smaller value => higher quality/larger frame
const uint8_t streamJpegQualityNoPsram = 12;
const uint8_t streamJpegQualityMax = 12;
const unsigned long wsPingInterval = 15000UL;
const unsigned long ipv6RetryInterval = 4000UL;
const uint8_t ipv6MaxRetry = 15;
const framesize_t streamFrameSizePsram = FRAMESIZE_VGA;
const framesize_t streamFrameSizeNoPsram = FRAMESIZE_QVGA;

const char* streamWsHost = STREAM_WS_HOST;
const uint16_t streamWsPort = STREAM_WS_PORT;
const char* streamWsRoom = STREAM_WS_ROOM;
const char* streamWsToken = STREAM_WS_TOKEN;

String buildStreamWsPath() {
  String path = "/esp32?room=";
  path += streamWsRoom;
  if (strlen(streamWsToken) > 0) {
    path += "&token=";
    path += streamWsToken;
  }
  return path;
}

String buildStreamWsUrl(const String& path) {
  String url = "ws://";
  url += streamWsHost;
  url += ":";
  url += String(streamWsPort);
  url += path;
  return url;
}

// ESP32-CAM (AI Thinker) pins
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

const uint8_t flashLedPin = 4;

#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
const bool useNewLedcApi = true;
#else
const bool useNewLedcApi = false;
const uint8_t flashLedChannel = 7;
#endif

WiFiClient mqttPlainClient;
WiFiClientSecure mqttTlsClient;
PubSubClient mqttClient(mqttPlainClient);
WebsocketsClient wsClient;
Preferences wifiPrefs;
WebServer provisioningServer(80);
bool wsConnected = false;
bool mqttUsingInsecure = false;

String topicCmdLight;
String topicStateLight;
String topicStateOnline;
String topicStateHeartbeat;
String topicAck;
String streamWsPath;
String streamWsUrl;

uint8_t currentLight = 0;
unsigned long lastWifiRetryTime = 0;
unsigned long lastMqttConnectAttempt = 0;
unsigned long lastHeartbeatTime = 0;
unsigned long lastWsConnectAttempt = 0;
unsigned long lastFrameTime = 0;
unsigned long lastStreamStatTime = 0;
unsigned long lastWsPingTime = 0;
unsigned long wsConnectedAt = 0;
unsigned long wsReconnectCount = 0;
unsigned long streamFramesSent = 0;
unsigned long streamFramesDropped = 0;
unsigned long streamBytesSent = 0;
unsigned long wsBusyDropCount = 0;
uint8_t currentJpegQuality = 0;
int streamViewerCount = 0;
bool streamPushEnabled = false;
bool wifiWasConnected = false;
bool wifiHasEverConnected = false;
bool provisioningApActive = false;
bool provisioningServerStarted = false;
bool pendingProvisionRequest = false;
bool pendingResetRequest = false;
bool pendingCredentialSave = false;
WiFiControlState wifiControlState = WiFiControlState::BOOT;
String savedWiFiSsid;
String savedWiFiPassword;
String connectTargetSsid;
String connectTargetPassword;
String pendingProvisionSsid;
String pendingProvisionPassword;
String provisioningApSsid;
String provisioningMessage = "初始化中";
unsigned long wifiConnectStartedAt = 0;
bool hasIPv6 = false;
String currentIPv6;
String currentIPv6Type = "NONE";
unsigned long lastIPv6TryTime = 0;
uint8_t ipv6TryCount = 0;
bool ipv6RetryExhaustedLogged = false;

void setup() {
  Serial.begin(115200);
  delay(150);
  Serial.println("\n--- ESP32-CAM MQTT + WS 推流启动 ---");
  printMemoryInfo("启动后");
  streamWsPath = buildStreamWsPath();
  streamWsUrl = buildStreamWsUrl(streamWsPath);
  if (strlen(streamWsToken) == 0) {
    Serial.println("[WS] STREAM_WS_TOKEN 为空，按无鉴权中转模式连接");
  }
  Serial.println("[WS] 推流默认地址: " + streamWsUrl);

  WiFi.persistent(false);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWiFiEvent);

  setupTopics();
  setupFlashPwm();
  setupProvisioningServer();

  if (!initCamera()) {
    Serial.println("[BOOT] 摄像头初始化失败，5秒后重启");
    delay(5000);
    ESP.restart();
  }
  printMemoryInfo("摄像头初始化后");

  setupMqttTransport();
  mqttClient.setServer(mqttHost, mqttPort);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512);
  mqttClient.setKeepAlive(60);
  mqttClient.setSocketTimeout(15);

  wsClient.onEvent([](WebsocketsEvent event, String data) {
    (void)data;
    if (event == WebsocketsEvent::ConnectionOpened) {
      wsConnected = true;
      streamPushEnabled = false;
      streamViewerCount = 0;
      wsConnectedAt = millis();
      lastWsPingTime = millis();
      lastStreamStatTime = millis();
      streamFramesSent = 0;
      streamFramesDropped = 0;
      streamBytesSent = 0;
      wsBusyDropCount = 0;
      Serial.println("[WS] 推流连接成功，等待 viewer 启用视频");
    } else if (event == WebsocketsEvent::ConnectionClosed) {
      wsConnected = false;
      streamPushEnabled = false;
      streamViewerCount = 0;
      unsigned long aliveMs = (wsConnectedAt > 0) ? (millis() - wsConnectedAt) : 0;
      Serial.printf("[WS] 推流连接断开，在线时长=%lu ms, RSSI=%d\n", aliveMs, WiFi.RSSI());
    }
  });

  wsClient.onMessage([](WebsocketsMessage msg) {
    if (!msg.isText()) return;
    String text = msg.data();
    String cmd = extractJsonValue(text, "cmd");
    cmd.toLowerCase();
    String valStr = extractJsonValue(text, "value");

    if (cmd == "stream") {
      String valLower = valStr;
      valLower.toLowerCase();
      const bool shouldPush = (valLower == "start" || valLower == "on" || valLower == "1");
      const bool shouldPause = (valLower == "stop" || valLower == "off" || valLower == "0");
      if (!shouldPush && !shouldPause) return;

      String viewersStr = extractJsonValue(text, "viewers");
      if (viewersStr.length() > 0) {
        long viewers = viewersStr.toInt();
        if (viewers < 0) viewers = 0;
        streamViewerCount = static_cast<int>(viewers);
      } else if (!shouldPush) {
        streamViewerCount = 0;
      }
      streamPushEnabled = shouldPush;
      lastFrameTime = 0;
      lastStreamStatTime = millis();
      streamFramesSent = 0;
      streamFramesDropped = 0;
      streamBytesSent = 0;
      wsBusyDropCount = 0;
      Serial.printf(
        "[WS] 推流状态已更新: %s, viewers=%d\n",
        streamPushEnabled ? "enabled" : "paused",
        streamViewerCount
      );
      return;
    }

    if (cmd == "light") {
      uint8_t nextValue;
      if (parseBrightnessCommand(valStr, nextValue)) {
        setFlashBrightness(nextValue, true);
        Serial.printf("[WS] 收到补光灯指令 value=%s -> %u\n", valStr.c_str(), nextValue);
      }
    }
  });
  wsClient.addHeader("User-Agent", "ESP32-CAM");

  if (loadStoredWiFiCredentials()) {
    connectTargetSsid = savedWiFiSsid;
    connectTargetPassword = savedWiFiPassword;
    connectWiFi();
  } else {
    startProvisioningAp("首次启动未发现已保存的 Wi‑Fi");
  }
}

void loop() {
  unsigned long now = millis();

  provisioningServer.handleClient();
  maintainWiFiConnection();

  if (WiFi.status() != WL_CONNECTED) {
    if (wifiWasConnected) {
      wifiWasConnected = false;
      Serial.println("[WiFi] 已断开，等待恢复或重新配网");
      wsConnected = false;
      streamPushEnabled = false;
      streamViewerCount = 0;
      if (wsClient.available()) wsClient.close();
    }

    delay(10);
    return;
  }

  if (!wifiWasConnected) {
    wifiWasConnected = true;
    wifiHasEverConnected = true;
    wifiControlState = WiFiControlState::CONNECTED;
    Serial.printf("[WiFi] 连接成功，SSID=%s, IP=%s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());

    if (pendingCredentialSave) {
      saveStoredWiFiCredentials(connectTargetSsid, connectTargetPassword);
      pendingCredentialSave = false;
      provisioningMessage = String("已连接并保存 Wi‑Fi: ") + connectTargetSsid;
    } else {
      provisioningMessage = String("已连接 Wi‑Fi: ") + WiFi.SSID();
    }

    stopProvisioningAp();

    if (enablePortalAuth) {
      doPortalAuth();
      previousAuthTime = now;
      testInternet();
    }
    syncTimeIfNeeded();
  }

  if (enablePortalAuth && now - previousAuthTime >= authInterval) {
    previousAuthTime = now;
    Serial.println("[Portal] 认证间隔到期，执行续期认证");
    doPortalAuth();
  }

  if (!hasIPv6 && ipv6TryCount < ipv6MaxRetry && now - lastIPv6TryTime >= ipv6RetryInterval) {
    requestIPv6("loop-retry");
  } else if (!hasIPv6 && ipv6TryCount >= ipv6MaxRetry && !ipv6RetryExhaustedLogged) {
    ipv6RetryExhaustedLogged = true;
    Serial.println("[WiFi] IPv6 多次申请仍未成功，当前热点可能未向 STA 分配 IPv6");
  }

  if (!mqttClient.connected() && now - lastMqttConnectAttempt >= mqttReconnectInterval) {
    lastMqttConnectAttempt = now;
    connectMQTT();
  }

  if (mqttClient.connected()) {
    mqttClient.loop();
    if (now - lastHeartbeatTime >= mqttHeartbeatInterval) {
      lastHeartbeatTime = now;
      publishHeartbeat();
    }
  }

  if (!wsClient.available() && now - lastWsConnectAttempt >= wsReconnectInterval) {
    lastWsConnectAttempt = now;
    connectStreamWs();
  }

  wsClient.poll();
  sendStreamFrameIfReady();

  if (wsClient.available() && now - lastWsPingTime >= wsPingInterval) {
    if (!wsClient.ping()) {
      Serial.println("[WS] ping 失败，主动重连");
      wsClient.close();
      wsConnected = false;
    }
    lastWsPingTime = now;
  }

  delay(2);
}

void syncTimeIfNeeded() {
  if (!mqttUseTls || mqttUsingInsecure) return;

  Serial.println("[NTP] 正在同步时间");
  configTime(0, 0, "ntp.aliyun.com", "pool.ntp.org", "time.google.com");

  time_t now = time(nullptr);
  int retry = 0;
  while (now < 1700000000 && retry < 20) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
    retry++;
  }

  if (now >= 1700000000) {
    Serial.printf("\n[NTP] 时间同步成功: %s", ctime(&now));
  } else {
    Serial.println("\n[NTP] 时间同步失败，TLS 校验可能失败");
  }
}

void onWiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED: {
      requestIPv6("STA_CONNECTED");
      break;
    }

    case ARDUINO_EVENT_WIFI_STA_GOT_IP: {
      wifiControlState = WiFiControlState::CONNECTED;
      wifiConnectStartedAt = 0;
      requestIPv6("STA_GOT_IP");
      break;
    }

    case ARDUINO_EVENT_WIFI_STA_GOT_IP6: {
      refreshIPv6Status(true);
      ipv6RetryExhaustedLogged = false;
      break;
    }

    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      hasIPv6 = false;
      currentIPv6 = "";
      currentIPv6Type = "NONE";
      lastIPv6TryTime = 0;
      ipv6TryCount = 0;
      ipv6RetryExhaustedLogged = false;
      if (wifiControlState != WiFiControlState::PROVISIONING) {
        wifiControlState = WiFiControlState::CONNECTING;
        if (wifiConnectStartedAt == 0) {
          wifiConnectStartedAt = millis();
        }
      }
      break;

    default:
      break;
  }
}

void requestIPv6(const char* reason) {
  if (WiFi.status() != WL_CONNECTED) return;
  bool ok = WiFi.enableIpV6();
  lastIPv6TryTime = millis();
  if (ipv6TryCount < 255) {
    ipv6TryCount++;
  }
  Serial.printf("[WiFi] IPv6 申请(%s): %s, 次数=%u\n", reason, ok ? "已发送" : "失败", ipv6TryCount);
}

const char* classifyIPv6Type(const esp_ip6_addr_t& addr) {
  const ip6_addr_t* lwipAddr = reinterpret_cast<const ip6_addr_t*>(&addr);
  if (ip6_addr_isglobal(lwipAddr)) return "GLOBAL";
  if (ip6_addr_isuniquelocal(lwipAddr)) return "ULA";
  if (ip6_addr_islinklocal(lwipAddr)) return "LINK_LOCAL";
  if (ip6_addr_isloopback(lwipAddr)) return "LOOPBACK";
  return "OTHER";
}

int ipv6TypeScore(const char* type) {
  if (strcmp(type, "GLOBAL") == 0) return 4;
  if (strcmp(type, "ULA") == 0) return 3;
  if (strcmp(type, "LINK_LOCAL") == 0) return 2;
  if (strcmp(type, "OTHER") == 0) return 1;
  return 0;
}

void refreshIPv6Status(bool verbose) {
  hasIPv6 = false;
  currentIPv6 = "";
  currentIPv6Type = "NONE";

  esp_netif_t* staNetif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (staNetif == nullptr) {
    if (verbose) {
      Serial.println("[WiFi] IPv6 查询失败: STA netif 不存在");
    }
    return;
  }

  esp_ip6_addr_t ip6List[5] = {};
  int count = esp_netif_get_all_ip6(staNetif, ip6List);
  if (count <= 0) {
    if (verbose) {
      Serial.println("[WiFi] 当前没有可用 IPv6 地址");
    }
    return;
  }

  int bestScore = -1;
  for (int i = 0; i < count; i++) {
    IPv6Address ip6(ip6List[i].addr);
    String addr = ip6.toString();
    if (addr.length() == 0 || addr == "::") {
      continue;
    }

    const char* type = classifyIPv6Type(ip6List[i]);
    if (verbose) {
      Serial.printf("[WiFi] IPv6[%d]=%s, type=%s\n", i, addr.c_str(), type);
    }

    int score = ipv6TypeScore(type);
    if (score > bestScore) {
      bestScore = score;
      currentIPv6 = addr;
      currentIPv6Type = String(type);
    }
  }

  hasIPv6 = bestScore >= 0;
  if (verbose && hasIPv6) {
    Serial.printf("[WiFi] IPv6 已选择: %s (%s)\n", currentIPv6.c_str(), currentIPv6Type.c_str());
    if (currentIPv6Type == "LINK_LOCAL") {
      Serial.println("[WiFi] 提示: LINK_LOCAL 不能直接公网访问");
    }
  }
}

void connectWiFi() {
  if (connectTargetSsid.length() == 0) {
    startProvisioningAp("未提供 Wi‑Fi 配置");
    return;
  }

  WiFi.mode((provisioningApActive || pendingCredentialSave) ? WIFI_AP_STA : WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(connectTargetSsid.c_str(), connectTargetPassword.c_str());

  wifiControlState = WiFiControlState::CONNECTING;
  wifiConnectStartedAt = millis();
  lastWifiRetryTime = wifiConnectStartedAt;
  provisioningMessage = pendingCredentialSave
    ? String("正在测试新 Wi‑Fi: ") + connectTargetSsid
    : String("正在连接已保存 Wi‑Fi: ") + connectTargetSsid;

  Serial.printf("[WiFi] 正在连接 %s%s\n", connectTargetSsid.c_str(), pendingCredentialSave ? " (新配置测试)" : "");
}

const char* wifiControlStateToString(WiFiControlState state) {
  switch (state) {
    case WiFiControlState::BOOT: return "BOOT";
    case WiFiControlState::CONNECTING: return "CONNECTING";
    case WiFiControlState::CONNECTED: return "CONNECTED";
    case WiFiControlState::PROVISIONING: return "PROVISIONING";
    default: return "UNKNOWN";
  }
}

const char* wifiAuthModeToString(wifi_auth_mode_t authMode) {
  switch (authMode) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    default: return "UNKNOWN";
  }
}

String escapeJson(const String& value) {
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); i++) {
    char ch = value[i];
    switch (ch) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out += ch; break;
    }
  }
  return out;
}

bool loadStoredWiFiCredentials() {
  savedWiFiSsid = "";
  savedWiFiPassword = "";

  if (wifiPrefs.begin(wifiPrefsNamespace, false)) {
    bool valid = wifiPrefs.getBool("valid", false);
    uint32_t version = wifiPrefs.getUInt("version", 0);
    if (valid && version == wifiPrefsVersion) {
      savedWiFiSsid = wifiPrefs.getString("ssid", "");
      savedWiFiPassword = wifiPrefs.getString("password", "");
    }
    wifiPrefs.end();
  }

  if (savedWiFiSsid.length() == 0) {
    String defaultSsid = DEFAULT_WIFI_SSID;
    String defaultPassword = DEFAULT_WIFI_PASSWORD;
    if (defaultSsid.length() > 0) {
      savedWiFiSsid = defaultSsid;
      savedWiFiPassword = defaultPassword;
      Serial.printf("[WiFi] 使用编译默认网络: %s\n", savedWiFiSsid.c_str());
    }
  }

  return savedWiFiSsid.length() > 0;
}

void saveStoredWiFiCredentials(const String& ssid, const String& password) {
  if (!wifiPrefs.begin(wifiPrefsNamespace, false)) {
    Serial.println("[WiFi] 保存配置失败: NVS 打开失败");
    return;
  }

  wifiPrefs.putUInt("version", wifiPrefsVersion);
  wifiPrefs.putBool("valid", true);
  wifiPrefs.putString("ssid", ssid);
  wifiPrefs.putString("password", password);
  wifiPrefs.end();

  savedWiFiSsid = ssid;
  savedWiFiPassword = password;
  Serial.printf("[WiFi] 已保存配置: %s\n", ssid.c_str());
}

void clearStoredWiFiCredentials() {
  if (!wifiPrefs.begin(wifiPrefsNamespace, false)) {
    Serial.println("[WiFi] 清除配置失败: NVS 打开失败");
    return;
  }
  wifiPrefs.clear();
  wifiPrefs.end();

  savedWiFiSsid = "";
  savedWiFiPassword = "";
  connectTargetSsid = "";
  connectTargetPassword = "";
  Serial.println("[WiFi] 已清除已保存的 Wi‑Fi 配置");
}

String buildProvisioningStatusJson() {
  const bool connected = WiFi.status() == WL_CONNECTED;
  String json = "{";
  json += "\"device_id\":\"" + escapeJson(deviceId) + "\",";
  json += "\"state\":\"" + String(wifiControlStateToString(wifiControlState)) + "\",";
  json += "\"connected\":";
  json += connected ? "true" : "false";
  json += ",";
  json += "\"connected_ssid\":\"" + escapeJson(connected ? WiFi.SSID() : String()) + "\",";
  json += "\"saved_ssid\":\"" + escapeJson(savedWiFiSsid) + "\",";
  json += "\"sta_ip\":\"" + escapeJson(connected ? WiFi.localIP().toString() : String()) + "\",";
  json += "\"ap_ssid\":\"" + escapeJson(provisioningApActive ? provisioningApSsid : String()) + "\",";
  json += "\"ap_ip\":\"" + escapeJson(provisioningApActive ? WiFi.softAPIP().toString() : String()) + "\",";
  json += "\"message\":\"" + escapeJson(provisioningMessage) + "\"";
  json += "}";
  return json;
}

void handleProvisioningRoot() {
  provisioningServer.send_P(200, "text/html; charset=utf-8", provisioningPageHtml);
}

void handleProvisioningStatus() {
  provisioningServer.send(200, "application/json; charset=utf-8", buildProvisioningStatusJson());
}

void handleProvisioningScan() {
  WiFi.scanDelete();
  int count = WiFi.scanNetworks();

  String json = "{\"ok\":true,\"networks\":[";
  bool first = true;

  if (count > 0) {
    for (int i = 0; i < count; i++) {
      String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) {
        continue;
      }
      if (!first) {
        json += ",";
      }
      first = false;
      json += "{\"ssid\":\"" + escapeJson(ssid) + "\",";
      json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
      json += "\"auth\":\"" + String(wifiAuthModeToString(static_cast<wifi_auth_mode_t>(WiFi.encryptionType(i)))) + "\"}";
    }
  }

  json += "]";
  if (count <= 0) {
    json += ",\"message\":\"没有扫描到 Wi‑Fi，请手动输入隐藏 SSID\"";
  }
  json += "}";

  WiFi.scanDelete();
  provisioningServer.send(200, "application/json; charset=utf-8", json);
}

void handleProvisioningConnect() {
  String ssid = provisioningServer.arg("ssid");
  String password = provisioningServer.arg("password");
  ssid.trim();

  if (ssid.length() == 0) {
    provisioningServer.send(400, "application/json; charset=utf-8", "{\"ok\":false,\"message\":\"SSID 不能为空\"}");
    return;
  }

  pendingProvisionSsid = ssid;
  pendingProvisionPassword = password;
  pendingProvisionRequest = true;
  provisioningMessage = String("已收到新配置，准备连接: ") + ssid;

  provisioningServer.send(200, "application/json; charset=utf-8", "{\"ok\":true,\"message\":\"已开始连接新 Wi‑Fi，请等待 10~20 秒\"}");
}

void handleProvisioningReset() {
  pendingResetRequest = true;
  provisioningMessage = "准备清除已保存配置";
  provisioningServer.send(200, "application/json; charset=utf-8", "{\"ok\":true,\"message\":\"已开始清除配置，设备将重新进入配网模式\"}");
}

void handleLight() {
  provisioningServer.sendHeader("Access-Control-Allow-Origin", "*");
  provisioningServer.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  provisioningServer.sendHeader("Access-Control-Allow-Headers", "Content-Type");

  if (provisioningServer.method() == HTTP_OPTIONS) {
    provisioningServer.send(204);
    return;
  }

  if (provisioningServer.method() == HTTP_GET) {
    String json = "{\"ok\":true,\"light\":" + String(currentLight) + "}";
    provisioningServer.send(200, "application/json; charset=utf-8", json);
    return;
  }

  String valStr = provisioningServer.hasArg("value")
    ? provisioningServer.arg("value")
    : provisioningServer.arg("plain");
  valStr.trim();

  String valLower = valStr;
  valLower.toLowerCase();

  uint8_t nextValue;
  if (valLower == "toggle" || valLower == "light/toggle") {
    nextValue = (currentLight > 0) ? 0 : 255;
  } else if (!parseBrightnessCommand(valStr, nextValue)) {
    provisioningServer.send(400, "application/json; charset=utf-8",
      "{\"ok\":false,\"message\":\"无效亮度值，接受 0-255 / on / off / toggle\"}");
    return;
  }

  setFlashBrightness(nextValue, true);
  String json = "{\"ok\":true,\"light\":" + String(currentLight) + "}";
  provisioningServer.send(200, "application/json; charset=utf-8", json);
}

void handleProvisioningNotFound() {
  provisioningServer.sendHeader("Location", "/");
  provisioningServer.send(302, "text/plain", "redirect");
}

void setupProvisioningServer() {
  provisioningServer.on("/", HTTP_GET, handleProvisioningRoot);
  provisioningServer.on("/scan", HTTP_GET, handleProvisioningScan);
  provisioningServer.on("/connect", HTTP_POST, handleProvisioningConnect);
  provisioningServer.on("/status", HTTP_GET, handleProvisioningStatus);
  provisioningServer.on("/reset", HTTP_POST, handleProvisioningReset);
  provisioningServer.on("/light", HTTP_GET, handleLight);
  provisioningServer.on("/light", HTTP_POST, handleLight);
  provisioningServer.on("/light", HTTP_OPTIONS, handleLight);
  provisioningServer.onNotFound(handleProvisioningNotFound);
}

void startProvisioningAp(const char* reason) {
  if (provisioningApSsid.length() == 0) {
    uint64_t chipId = ESP.getEfuseMac();
    char suffix[5] = {};
    snprintf(suffix, sizeof(suffix), "%04X", static_cast<unsigned int>(chipId & 0xFFFFULL));
    provisioningApSsid = String(provisioningApPrefix) + suffix;
  }

  if (mqttClient.connected()) {
    mqttClient.disconnect();
  }
  if (wsClient.available()) {
    wsClient.close();
  }
  wsConnected = false;
  streamPushEnabled = false;
  streamViewerCount = 0;
  wifiWasConnected = false;
  wifiControlState = WiFiControlState::PROVISIONING;
  wifiConnectStartedAt = 0;

  WiFi.disconnect();
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.softAPConfig(provisioningApIp, provisioningApIp, IPAddress(255, 255, 255, 0));

  if (!provisioningApActive) {
    provisioningApActive = WiFi.softAP(provisioningApSsid.c_str());
  }

  if (provisioningApActive && !provisioningServerStarted) {
    provisioningServer.begin();
    provisioningServerStarted = true;
    Serial.println("[WEB] 配网页服务已启动");
  }

  provisioningMessage = String(reason) + "。请连接热点 " + provisioningApSsid + "，然后打开 192.168.4.1";
  Serial.printf("[WiFi] 进入配网模式，AP=%s, IP=%s\n", provisioningApSsid.c_str(), WiFi.softAPIP().toString().c_str());
}

void stopProvisioningAp() {
  if (!provisioningApActive) {
    return;
  }

  WiFi.softAPdisconnect(true);
  provisioningApActive = false;
  WiFi.mode(WIFI_STA);
  Serial.println("[WiFi] 已关闭配网热点");
}

void beginProvisioningCandidate(const String& ssid, const String& password) {
  if (!provisioningApActive) {
    startProvisioningAp("准备切换到新的 Wi‑Fi");
  }

  connectTargetSsid = ssid;
  connectTargetPassword = password;
  pendingCredentialSave = true;
  connectWiFi();
}

void maintainWiFiConnection() {
  unsigned long now = millis();

  if (pendingResetRequest) {
    pendingResetRequest = false;
    pendingProvisionRequest = false;
    pendingCredentialSave = false;
    clearStoredWiFiCredentials();
    startProvisioningAp("已清除已保存的 Wi‑Fi 配置");
    return;
  }

  if (pendingProvisionRequest) {
    pendingProvisionRequest = false;
    beginProvisioningCandidate(pendingProvisionSsid, pendingProvisionPassword);
    return;
  }

  if (wifiControlState != WiFiControlState::CONNECTING) {
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  if (now - lastWifiRetryTime >= wifiReconnectInterval) {
    lastWifiRetryTime = now;
    WiFi.reconnect();
  }

  unsigned long timeout = (pendingCredentialSave || !wifiHasEverConnected)
    ? wifiConnectTimeoutInitial
    : wifiConnectTimeoutRecovery;

  if (wifiConnectStartedAt == 0 || now - wifiConnectStartedAt < timeout) {
    return;
  }

  WiFi.disconnect();

  if (pendingCredentialSave) {
    pendingCredentialSave = false;
    connectTargetSsid = "";
    connectTargetPassword = "";
    startProvisioningAp("新 Wi‑Fi 连接失败");
    provisioningMessage = "新 Wi‑Fi 连接失败，请检查密码、频段或信号强度";
    return;
  }

  if (savedWiFiSsid.length() > 0) {
    startProvisioningAp("已保存的 Wi‑Fi 连接超时");
    provisioningMessage = "旧 Wi‑Fi 长时间不可用，请重新配网";
  } else {
    startProvisioningAp("未找到已保存 Wi‑Fi");
  }
}

void setupTopics() {
  String base = String("esp32cam/") + deviceId;
  topicCmdLight = base + "/cmd/light";
  topicStateLight = base + "/state/light";
  topicStateOnline = base + "/state/online";
  topicStateHeartbeat = base + "/state/heartbeat";
  topicAck = base + "/ack";
}

void setupFlashPwm() {
  pinMode(flashLedPin, OUTPUT);

  if (useNewLedcApi) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
    if (!ledcAttach(flashLedPin, 5000, 8)) {
      Serial.println("[LED] ledcAttach 初始化失败");
    }
#endif
  } else {
#if !defined(ESP_ARDUINO_VERSION_MAJOR) || (ESP_ARDUINO_VERSION_MAJOR < 3)
    ledcSetup(flashLedChannel, 5000, 8);
    ledcAttachPin(flashLedPin, flashLedChannel);
#endif
  }

  setFlashBrightness(0, false);
}

void setFlashBrightness(uint8_t value, bool reportState) {
  currentLight = value;

  if (useNewLedcApi) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
    ledcWrite(flashLedPin, currentLight);
#endif
  } else {
#if !defined(ESP_ARDUINO_VERSION_MAJOR) || (ESP_ARDUINO_VERSION_MAJOR < 3)
    ledcWrite(flashLedChannel, currentLight);
#endif
  }

  Serial.printf("[LED] 当前亮度=%u\n", currentLight);

  if (reportState) {
    publishLightState();
  }
}

void publishLightState() {
  if (!mqttClient.connected()) return;

  String payload = String("{\"light\":") + String(currentLight) + ",\"pin\":" + String(flashLedPin) + "}";
  if (mqttClient.publish(topicStateLight.c_str(), payload.c_str(), true)) {
    Serial.println("[MQTT] 亮度状态: " + payload);
  }
}

void publishHeartbeat() {
  if (!mqttClient.connected()) return;

  String payload = "{";
  payload += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  if (hasIPv6 && currentIPv6.length() > 0) {
    payload += "\"ipv6\":\"" + currentIPv6 + "\",";
    payload += "\"ipv6_type\":\"" + currentIPv6Type + "\",";
  }
  payload += "\"mac\":\"" + WiFi.macAddress() + "\",";
  payload += "\"rssi\":" + String(WiFi.RSSI()) + ",";
  payload += "\"light\":" + String(currentLight);
  payload += "}";

  if (mqttClient.publish(topicStateHeartbeat.c_str(), payload.c_str(), false)) {
    Serial.println("[MQTT] 心跳: " + payload);
  }
}

void publishAck(const char* msg) {
  if (!mqttClient.connected()) return;
  mqttClient.publish(topicAck.c_str(), msg, false);
}
void setupMqttTransport() {
  if (!mqttUseTls) {
    mqttClient.setClient(mqttPlainClient);
    mqttUsingInsecure = true;
    Serial.println("[MQTT] 使用明文 TCP 模式（1883）");
    return;
  }

  mqttClient.setClient(mqttTlsClient);

  if (strstr(mqttCaCert, "PASTE_YOUR_CA_CERT_HERE") != nullptr) {
    mqttUsingInsecure = true;
    mqttTlsClient.setInsecure();
    Serial.println("[MQTT] TLS 未配置 CA，已启用 setInsecure");
  } else {
    mqttUsingInsecure = false;
    mqttTlsClient.setCACert(mqttCaCert);
    Serial.println("[MQTT] TLS 已加载 CA 证书");
  }
}

void doPortalAuth() {
  if (!enablePortalAuth || WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;

  String staIp = WiFi.localIP().toString();
  String staMac = WiFi.macAddress();
  String apMac = WiFi.BSSIDstr();
  staMac.toLowerCase();
  apMac.toLowerCase();

  String authUrl = String(portalAuthBaseUrl);
  authUrl += "?btn_onekey=true";
  authUrl += "&authtype=2";
  authUrl += "&pagetype=2";
  authUrl += "&vlan=4095";
  authUrl += "&staMac=" + staMac;
  authUrl += "&staIp=" + staIp;
  authUrl += "&apMac=" + apMac;
  authUrl += "&apIp=" + staIp;
  authUrl += "&supportTPAuth=true";

  Serial.println("[Portal] 请求地址: " + authUrl);

  http.begin(authUrl);
  int httpCode = http.GET();

  if (httpCode > 0) {
    Serial.printf("[Portal] HTTP 状态码: %d\n", httpCode);
    if (httpCode == 200) {
      Serial.println("[Portal] 认证完成");
    }
  } else {
    Serial.printf("[Portal] 认证失败: %s\n", HTTPClient::errorToString(httpCode).c_str());
  }

  http.end();
}

void testInternet() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  const char* testUrl = "http://captive.apple.com/hotspot-detect.html";
  const char* headerKeys[] = {"Location"};

  Serial.println("[NET] 正在检测外网连通性");
  http.collectHeaders(headerKeys, 1);
  http.begin(testUrl);
  int code = http.GET();

  if (code < 0) {
    Serial.printf("[NET] 请求失败: %s\n", HTTPClient::errorToString(code).c_str());
  } else if (code == HTTP_CODE_OK) {
    String payload = http.getString();
    payload.toLowerCase();
    if (payload.indexOf("success") >= 0) {
      Serial.println("[NET] 外网可用");
    } else {
      Serial.println("[NET] 收到 200，但内容异常");
    }
  } else if (code == HTTP_CODE_NO_CONTENT) {
    Serial.println("[NET] 收到 204，外网大概率可用");
  } else if (code == HTTP_CODE_MOVED_PERMANENTLY ||
             code == HTTP_CODE_FOUND ||
             code == HTTP_CODE_TEMPORARY_REDIRECT ||
             code == HTTP_CODE_PERMANENT_REDIRECT) {
    Serial.printf("[NET] 发生重定向（%d），location=%s\n", code, http.header("Location").c_str());
  } else {
    Serial.printf("[NET] 异常 HTTP 状态码: %d\n", code);
  }

  http.end();
}

void connectMQTT() {
  if (mqttClient.connected() || WiFi.status() != WL_CONNECTED) return;

  String clientId = String("esp32cam-") + WiFi.macAddress();
  clientId.replace(":", "");

  Serial.printf("[MQTT] 正在连接 %s:%u, clientId=%s\n", mqttHost, mqttPort, clientId.c_str());

  bool ok = false;
  if (strlen(mqttUsername) > 0) {
    ok = mqttClient.connect(
      clientId.c_str(),
      mqttUsername,
      mqttPassword,
      topicStateOnline.c_str(),
      1,
      true,
      "offline"
    );
  } else {
    ok = mqttClient.connect(
      clientId.c_str(),
      topicStateOnline.c_str(),
      1,
      true,
      "offline"
    );
  }

  if (!ok) {
    Serial.printf("[MQTT] 连接失败, state=%d\n", mqttClient.state());
    return;
  }

  mqttClient.subscribe(topicCmdLight.c_str(), 1);
  mqttClient.publish(topicStateOnline.c_str(), "online", true);
  publishLightState();
  publishHeartbeat();
  publishAck("connected");
  lastHeartbeatTime = millis();
  Serial.println("[MQTT] 连接成功并完成订阅");
}

String extractJsonValue(const String& json, const String& key) {
  String pattern = "\"" + key + "\"";
  int keyPos = json.indexOf(pattern);
  if (keyPos < 0) return "";

  int colonPos = json.indexOf(':', keyPos + pattern.length());
  if (colonPos < 0) return "";

  int start = colonPos + 1;
  while (start < (int)json.length() && isspace((unsigned char)json[start])) {
    start++;
  }
  if (start >= (int)json.length()) return "";

  if (json[start] == '"') {
    int endQuote = json.indexOf('"', start + 1);
    if (endQuote < 0) return "";
    return json.substring(start + 1, endQuote);
  }

  int end = start;
  while (end < (int)json.length() &&
         json[end] != ',' &&
         json[end] != '}' &&
         !isspace((unsigned char)json[end])) {
    end++;
  }
  return json.substring(start, end);
}

String decodeCommandPayload(byte* payload, unsigned int length) {
  String raw;
  raw.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) {
    raw += static_cast<char>(payload[i]);
  }
  raw.trim();

  if (!raw.startsWith("{")) {
    return raw;
  }

  String v = extractJsonValue(raw, "light");
  if (v.length() > 0) return v;

  v = extractJsonValue(raw, "msg");
  if (v.length() > 0) return v;

  return raw;
}

bool parseBrightnessCommand(String cmd, uint8_t& outValue) {
  cmd.trim();
  cmd.toLowerCase();

  if (cmd == "on" || cmd == "led/on" || cmd == "1") {
    outValue = 255;
    return true;
  }

  if (cmd == "off" || cmd == "led/off" || cmd == "0") {
    outValue = 0;
    return true;
  }

  char* endPtr = nullptr;
  long value = strtol(cmd.c_str(), &endPtr, 10);
  if (*endPtr != '\0') {
    return false;
  }

  if (value < 0) value = 0;
  if (value > 255) value = 255;
  outValue = static_cast<uint8_t>(value);
  return true;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String cmd = decodeCommandPayload(payload, length);
  String cmdLower = cmd;
  cmdLower.toLowerCase();

  Serial.printf("[MQTT] 收到消息 topic=%s payload=%s\n", topic, cmd.c_str());

  if (strcmp(topic, topicCmdLight.c_str()) != 0) {
    return;
  }

  if (cmdLower == "ping") {
    publishAck("pong");
    return;
  }

  if (cmdLower == "auth") {
    doPortalAuth();
    publishAck("portal-auth-done");
    return;
  }

  if (cmdLower == "status" || cmdLower == "light/status") {
    publishLightState();
    publishAck("light-status");
    return;
  }

  if (cmdLower == "toggle" || cmdLower == "light/toggle") {
    setFlashBrightness(currentLight > 0 ? 0 : 255, true);
    publishAck("light-toggle");
    return;
  }

  uint8_t nextValue = 0;
  if (parseBrightnessCommand(cmd, nextValue)) {
    setFlashBrightness(nextValue, true);
    publishAck("light-updated");
  } else {
    publishAck("unknown-cmd");
    Serial.println("[MQTT] 无效亮度命令，请用 0-255/on/off/toggle");
  }
}

const char* cameraSensorProfileToString(CameraSensorProfile profile) {
  switch (profile) {
    case CameraSensorProfile::OV2640: return "OV2640";
    case CameraSensorProfile::OV3660: return "OV3660";
    case CameraSensorProfile::GC2145: return "GC2145";
    default: return "UNKNOWN";
  }
}

CameraSensorProfile cameraSensorProfileFromPid(uint16_t pid) {
  if (pid == OV2640_PID) return CameraSensorProfile::OV2640;
  if (pid == OV3660_PID) return CameraSensorProfile::OV3660;
  if (pid == GC2145_PID) return CameraSensorProfile::GC2145;
  return CameraSensorProfile::UNKNOWN;
}

CameraSensorProfile parseForcedSensorProfile(
  const char* raw,
  bool* forceEnabled,
  bool* valueValid,
  char* normalized,
  size_t normalizedSize
) {
  if (forceEnabled != nullptr) *forceEnabled = false;
  if (valueValid != nullptr) *valueValid = false;
  if (normalized != nullptr && normalizedSize > 0) {
    normalized[0] = '\0';
  }

  const char* source = (raw != nullptr) ? raw : "";
  char token[24] = {0};
  size_t writePos = 0;

  for (size_t i = 0; source[i] != '\0' && writePos < sizeof(token) - 1; i++) {
    const unsigned char ch = static_cast<unsigned char>(source[i]);
    if (isspace(ch) || ch == '"' || ch == '\'' || ch == '-' || ch == '_') {
      continue;
    }
    token[writePos++] = static_cast<char>(tolower(ch));
  }
  token[writePos] = '\0';

  if (writePos == 0) {
    strcpy(token, "auto");
    writePos = strlen(token);
  }

  if (normalized != nullptr && normalizedSize > 0) {
    const size_t copyLen = (writePos < normalizedSize - 1) ? writePos : (normalizedSize - 1);
    memcpy(normalized, token, copyLen);
    normalized[copyLen] = '\0';
  }

  if (strcmp(token, "auto") == 0) {
    if (valueValid != nullptr) *valueValid = true;
    if (forceEnabled != nullptr) *forceEnabled = false;
    return CameraSensorProfile::UNKNOWN;
  }
  if (strcmp(token, "ov2640") == 0) {
    if (valueValid != nullptr) *valueValid = true;
    if (forceEnabled != nullptr) *forceEnabled = true;
    return CameraSensorProfile::OV2640;
  }
  if (strcmp(token, "ov3660") == 0) {
    if (valueValid != nullptr) *valueValid = true;
    if (forceEnabled != nullptr) *forceEnabled = true;
    return CameraSensorProfile::OV3660;
  }
  if (strcmp(token, "gc2145") == 0) {
    if (valueValid != nullptr) *valueValid = true;
    if (forceEnabled != nullptr) *forceEnabled = true;
    return CameraSensorProfile::GC2145;
  }

  return CameraSensorProfile::UNKNOWN;
}

const char* pixformatToString(pixformat_t pixformat) {
  if (pixformat == PIXFORMAT_JPEG) return "JPEG";
  return "NON_JPEG";
}

const char* frameSizeToString(framesize_t frameSize) {
  if (frameSize == FRAMESIZE_QVGA) return "QVGA";
  if (frameSize == FRAMESIZE_VGA) return "VGA";
  return "OTHER";
}

template <typename T>
bool applySensorSetting(sensor_t* sensor, const char* settingName, T value, int (*setter)(sensor_t*, T)) {
  if (sensor == nullptr || setter == nullptr) {
    Serial.printf("[CAM] 参数设置失败: %s=%d, setter 不可用\n", settingName, static_cast<int>(value));
    return false;
  }

  const int rc = setter(sensor, value);
  if (rc != 0) {
    Serial.printf("[CAM] 参数设置失败: %s=%d, rc=%d\n", settingName, static_cast<int>(value), rc);
    return false;
  }
  return true;
}

bool initCamera() {
  const char* forceRaw = CAM_STRINGIFY(CAM_FORCE_SENSOR);
  char forceNormalized[16] = {0};
  bool forceEnabled = false;
  bool forceValid = false;
  const CameraSensorProfile forcedProfile = parseForcedSensorProfile(
    forceRaw,
    &forceEnabled,
    &forceValid,
    forceNormalized,
    sizeof(forceNormalized)
  );
  if (!forceValid) {
    Serial.printf(
      "[CAM] CAM_FORCE_SENSOR 配置无效: raw=%s, 仅支持 auto/ov2640/ov3660/gc2145，已回退 auto\n",
      forceRaw
    );
  }
  Serial.printf(
    "[CAM] CAM_FORCE_SENSOR raw=%s parsed=%s enabled=%s\n",
    forceRaw,
    forceNormalized,
    forceEnabled ? "yes" : "no"
  );
  Serial.printf(
    "[CAM] 依赖检查: gc2145 header hint=%s\n",
    HAS_GC2145_DRIVER_HEADER_HINT ? "found" : "not-found"
  );

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  const bool hasPsram = psramFound();
  framesize_t targetFrameSize = hasPsram ? streamFrameSizePsram : streamFrameSizeNoPsram;
  if (forceEnabled &&
      forcedProfile == CameraSensorProfile::GC2145 &&
      targetFrameSize != FRAMESIZE_QVGA &&
      targetFrameSize != FRAMESIZE_VGA) {
    targetFrameSize = hasPsram ? FRAMESIZE_VGA : FRAMESIZE_QVGA;
    Serial.printf("[CAM] GC2145 模式已调整分辨率到 %s\n", frameSizeToString(targetFrameSize));
  }

  if (hasPsram) {
    config.frame_size = targetFrameSize;
    config.jpeg_quality = streamJpegQualityPsram;
    config.fb_count = 3;
  } else {
    config.frame_size = targetFrameSize;
    config.jpeg_quality = streamJpegQualityNoPsram;
    config.fb_count = 1;
  }

#if defined(CAMERA_GRAB_LATEST)
  config.grab_mode = CAMERA_GRAB_LATEST;
#endif
#if defined(CAMERA_FB_IN_PSRAM)
  config.fb_location = CAMERA_FB_IN_PSRAM;
#endif

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAM] 初始化失败: 0x%x\n", err);
    return false;
  }

  currentJpegQuality = static_cast<uint8_t>(config.jpeg_quality);
  sensor_t* sensor = esp_camera_sensor_get();
  uint16_t detectedPid = 0;
  CameraSensorProfile detectedProfile = CameraSensorProfile::UNKNOWN;
  if (sensor != nullptr) {
    detectedPid = sensor->id.PID;
    detectedProfile = cameraSensorProfileFromPid(detectedPid);
  }
  CameraSensorProfile activeProfile = forceEnabled ? forcedProfile : detectedProfile;

  Serial.printf(
    "[CAM] 探测 PID=0x%04X, detected=%s\n",
    detectedPid,
    cameraSensorProfileToString(detectedProfile)
  );
  if (forceEnabled) {
    Serial.printf(
      "[CAM] 强制传感器已启用: %s\n",
      cameraSensorProfileToString(forcedProfile)
    );
    if (forcedProfile != detectedProfile) {
      Serial.printf(
        "[CAM] 强制覆盖探测结果: %s -> %s\n",
        cameraSensorProfileToString(detectedProfile),
        cameraSensorProfileToString(forcedProfile)
      );
    }
  }
  Serial.printf("[CAM] 传感器决策 final=%s\n", cameraSensorProfileToString(activeProfile));

  bool settingOk = true;
  if (sensor != nullptr) {
    settingOk &= applySensorSetting(
      sensor,
      "set_pixformat",
      config.pixel_format,
      sensor->set_pixformat
    );
    settingOk &= applySensorSetting(
      sensor,
      "set_framesize",
      config.frame_size,
      sensor->set_framesize
    );
    settingOk &= applySensorSetting(
      sensor,
      "set_quality",
      static_cast<int>(currentJpegQuality),
      sensor->set_quality
    );

    settingOk &= applySensorSetting(sensor, "set_contrast", 2, sensor->set_contrast);
    settingOk &= applySensorSetting(sensor, "set_sharpness", 2, sensor->set_sharpness);
    settingOk &= applySensorSetting(sensor, "set_denoise", 0, sensor->set_denoise);
    settingOk &= applySensorSetting(sensor, "set_gainceiling", GAINCEILING_8X, sensor->set_gainceiling);
    settingOk &= applySensorSetting(sensor, "set_aec2", 1, sensor->set_aec2);
    settingOk &= applySensorSetting(sensor, "set_dcw", 0, sensor->set_dcw);
    settingOk &= applySensorSetting(sensor, "set_lenc", 1, sensor->set_lenc);
    settingOk &= applySensorSetting(sensor, "set_brightness", 0, sensor->set_brightness);
    settingOk &= applySensorSetting(sensor, "set_saturation", 0, sensor->set_saturation);

    if (activeProfile == CameraSensorProfile::OV3660) {
      settingOk &= applySensorSetting(sensor, "set_vflip", 1, sensor->set_vflip);
      settingOk &= applySensorSetting(sensor, "set_hmirror", 0, sensor->set_hmirror);
      settingOk &= applySensorSetting(sensor, "set_brightness", 1, sensor->set_brightness);
      settingOk &= applySensorSetting(sensor, "set_saturation", -1, sensor->set_saturation);
      Serial.println("[CAM] 已应用 OV3660 保守调优");
    } else if (activeProfile == CameraSensorProfile::GC2145) {
      settingOk &= applySensorSetting(sensor, "set_vflip", 0, sensor->set_vflip);
      settingOk &= applySensorSetting(sensor, "set_hmirror", 0, sensor->set_hmirror);
      settingOk &= applySensorSetting(sensor, "set_gainceiling", GAINCEILING_4X, sensor->set_gainceiling);
      settingOk &= applySensorSetting(sensor, "set_dcw", 1, sensor->set_dcw);
      settingOk &= applySensorSetting(sensor, "set_contrast", 1, sensor->set_contrast);
      settingOk &= applySensorSetting(sensor, "set_sharpness", 1, sensor->set_sharpness);
      settingOk &= applySensorSetting(sensor, "set_denoise", 1, sensor->set_denoise);
      settingOk &= applySensorSetting(sensor, "set_brightness", 0, sensor->set_brightness);
      settingOk &= applySensorSetting(sensor, "set_saturation", 0, sensor->set_saturation);
      Serial.println(
        "[CAM] 已应用 GC2145 稳定参数: JPEG + QVGA/VGA + gainceiling=4x + dcw=1"
      );
    }
  } else {
    settingOk = false;
    Serial.println("[CAM] 警告: 未获取到 sensor 句柄，无法应用参数");
  }

  Serial.printf(
    "[CAM] 最终配置 pixformat=%s(%d), framesize=%s(%d), jpeg_quality=%d, fb_count=%d\n",
    pixformatToString(config.pixel_format),
    static_cast<int>(config.pixel_format),
    frameSizeToString(config.frame_size),
    static_cast<int>(config.frame_size),
    config.jpeg_quality,
    config.fb_count
  );
  if (!settingOk) {
    Serial.println("[CAM] 警告: 部分参数设置失败，已继续运行");
  }
  return true;
}

void connectStreamWs() {
  if (wsClient.available() || WiFi.status() != WL_CONNECTED) return;

  wsReconnectCount++;
  Serial.println("[WS] 正在连接推流端点: " + streamWsUrl);
  Serial.printf("[WS] 重连次数: %lu\n", wsReconnectCount);
  Serial.printf("[WS] 连接前可用堆内存: %u\n", ESP.getFreeHeap());

  if (!wsClient.connect(streamWsHost, streamWsPort, streamWsPath.c_str())) {
    wsConnected = false;
    Serial.println("[WS] 连接失败");
    Serial.printf("[WS] 连接失败后可用堆内存: %u\n", ESP.getFreeHeap());
  }
}

void sendStreamFrameIfReady() {
  if (!wsClient.available() || !wsConnected || !streamPushEnabled) return;

  unsigned long now = millis();
  if (now - lastFrameTime < streamInterval) return;
  if (now - wsConnectedAt < streamSendWarmupMs) return;
  if (lastStreamStatTime == 0) {
    lastStreamStatTime = now;
  }

  unsigned long captureStart = millis();
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("[CAM] 抓帧失败");
    return;
  }
  unsigned long captureMs = millis() - captureStart;
  size_t frameBytes = fb->len;

  bool sent = wsClient.sendBinary(reinterpret_cast<const char*>(fb->buf), fb->len);
  unsigned long sendMs = millis() - captureStart - captureMs;
  esp_camera_fb_return(fb);

  if (!sent) {
    streamFramesDropped++;
    wsBusyDropCount++;
    if (!wsClient.available()) {
      wsConnected = false;
    }
    if (wsBusyDropCount == 1 || (wsBusyDropCount % 50UL) == 0) {
      Serial.printf(
        "[WS] 发送忙，丢帧 busy=%lu (capture=%lums send=%lums, frame=%uB)\n",
        wsBusyDropCount,
        captureMs,
        sendMs,
        (unsigned int)frameBytes
      );
    }
    return;
  }

  wsBusyDropCount = 0;
  streamFramesSent++;
  streamBytesSent += frameBytes;

  if (frameBytes > streamPreferredMaxFrameBytes && currentJpegQuality < streamJpegQualityMax) {
    sensor_t* sensor = esp_camera_sensor_get();
    if (sensor != nullptr) {
      currentJpegQuality = static_cast<uint8_t>(currentJpegQuality + 1);
      if (currentJpegQuality > streamJpegQualityMax) {
        currentJpegQuality = streamJpegQualityMax;
      }
      sensor->set_quality(sensor, currentJpegQuality);
      Serial.printf(
        "[CAM] 帧过大=%uB，自动降低码率 quality=%u\n",
        (unsigned int)frameBytes,
        currentJpegQuality
      );
    }
  }

  if (now - lastStreamStatTime >= streamStatsInterval) {
    unsigned long window = now - lastStreamStatTime;
    float fps = (window > 0) ? (1000.0f * static_cast<float>(streamFramesSent) / static_cast<float>(window)) : 0.0f;
    float kbps = (window > 0) ? (8.0f * static_cast<float>(streamBytesSent) / static_cast<float>(window)) : 0.0f;
    Serial.printf(
      "[WS] 推流统计 fps=%.1f kbps=%.1f sent=%lu drop=%lu busy=%lu q=%u frame=%uB cap=%lums send=%lums\n",
      fps,
      kbps,
      streamFramesSent,
      streamFramesDropped,
      wsBusyDropCount,
      currentJpegQuality,
      (unsigned int)frameBytes,
      captureMs,
      sendMs
    );
    lastStreamStatTime = now;
    streamFramesSent = 0;
    streamFramesDropped = 0;
    streamBytesSent = 0;
  }

  lastFrameTime = now;
}

void printMemoryInfo(const char* stage) {
  const bool hasPsram = psramFound();
#if HAS_SPIRAM_CHIP_API
  const int chipEnum = static_cast<int>(esp_spiram_get_chip_size());
  Serial.printf(
    "[MEM] %s | PSRAM芯片探测(enum=%d, %s)\n",
    stage,
    chipEnum,
    psramChipSizeText(chipEnum)
  );
#endif
  Serial.printf(
    "[MEM] %s | PSRAM=%s, PSRAM总=%u, PSRAM可用=%u, Heap总=%u, Heap可用=%u\n",
    stage,
    hasPsram ? "可用" : "不可用",
    ESP.getPsramSize(),
    ESP.getFreePsram(),
    ESP.getHeapSize(),
    ESP.getFreeHeap()
  );
}

const char* psramChipSizeText(int chipEnum) {
  switch (chipEnum) {
    case 0: return "16Mbit (2MB)";
    case 1: return "32Mbit (4MB)";
    case 2: return "64Mbit (8MB)";
    default: return "INVALID/UNKNOWN";
  }
}
