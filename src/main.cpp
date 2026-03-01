#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoWebsockets.h>
#include <tiny_websockets/network/tcp_client.hpp>
#include "esp_camera.h"
#include <time.h>
#include <string.h>
#include <ctype.h>
#include <memory>
#include "esp_netif.h"
#include "lwip/ip6_addr.h"
#include "lwip/sockets.h"
#include "esp_tls.h"
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

#define CAM_STRINGIFY_IMPL(value) #value
#define CAM_STRINGIFY(value) CAM_STRINGIFY_IMPL(value)

#if __has_include("sensors/private_include/gc2145_settings.h")
#define HAS_GC2145_DRIVER_HEADER_HINT 1
#else
#define HAS_GC2145_DRIVER_HEADER_HINT 0
#endif

using namespace websockets;

class EspTlsSecuredTcpClient : public websockets::network::TcpClient {
public:
  explicit EspTlsSecuredTcpClient(const char* caCert)
    : _tls(nullptr), _connected(false), _caCert(caCert) {
    _keepAlive.keep_alive_enable = true;
    _keepAlive.keep_alive_idle = 5;
    _keepAlive.keep_alive_interval = 5;
    _keepAlive.keep_alive_count = 3;
  }

  bool connect(const WSString& host, int port) override {
    close();
    _host = host;

    esp_tls_cfg_t cfg = {};
    cfg.timeout_ms = 10000;
    cfg.keep_alive_cfg = &_keepAlive;
    cfg.common_name = _host.c_str();

    if (_caCert != nullptr && strlen(_caCert) > 0) {
      cfg.cacert_buf = reinterpret_cast<const unsigned char*>(_caCert);
      cfg.cacert_bytes = strlen(_caCert) + 1;
    }

    _tls = esp_tls_init();
    if (_tls == nullptr) {
      return false;
    }

    int ret = esp_tls_conn_new_sync(_host.c_str(), _host.length(), port, &cfg, _tls);
    if (ret != 1) {
      esp_tls_conn_destroy(_tls);
      _tls = nullptr;
      _connected = false;
      return false;
    }

    _connected = true;
    return true;
  }

  bool poll() override {
    if (!_connected || _tls == nullptr) return false;

    int avail = esp_tls_get_bytes_avail(_tls);
    if (avail > 0) return true;

    int sockfd = -1;
    if (esp_tls_get_conn_sockfd(_tls, &sockfd) != ESP_OK || sockfd < 0) return false;

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(sockfd, &readSet);
    timeval tv = {};
    tv.tv_sec = 0;
    tv.tv_usec = 0;

    int rc = lwip_select(sockfd + 1, &readSet, nullptr, nullptr, &tv);
    return rc > 0;
  }

  bool available() override {
    return _connected && _tls != nullptr;
  }

  void send(const WSString& data) override {
    send(reinterpret_cast<const uint8_t*>(data.c_str()), data.size());
  }

  void send(const WSString&& data) override {
    send(reinterpret_cast<const uint8_t*>(data.c_str()), data.size());
  }

  void send(const uint8_t* data, const uint32_t len) override {
    if (!_connected || _tls == nullptr || data == nullptr || len == 0) return;

    uint32_t total = 0;
    while (total < len) {
      int written = esp_tls_conn_write(
        _tls,
        reinterpret_cast<const char*>(data + total),
        static_cast<size_t>(len - total)
      );
      if (written <= 0) {
        _connected = false;
        return;
      }
      total += static_cast<uint32_t>(written);
    }
  }

  WSString readLine() override {
    WSString line;
    if (!_connected || _tls == nullptr) return line;

    const uint32_t timeoutMs = 8000;
    unsigned long start = millis();
    while (_connected && millis() - start < timeoutMs) {
      char ch = 0;
      int ret = esp_tls_conn_read(_tls, &ch, 1);
      if (ret == 1) {
        line += ch;
        if (ch == '\n') {
          break;
        }
      } else if (ret == 0) {
        _connected = false;
        break;
      } else {
        delay(1);
      }
    }
    return line;
  }

  uint32_t read(uint8_t* buffer, const uint32_t len) override {
    if (!_connected || _tls == nullptr || buffer == nullptr || len == 0) return 0;
    int ret = esp_tls_conn_read(_tls, reinterpret_cast<char*>(buffer), len);
    if (ret <= 0) {
      if (ret == 0) {
        _connected = false;
      }
      return 0;
    }
    return static_cast<uint32_t>(ret);
  }

  void close() override {
    if (_tls != nullptr) {
      esp_tls_conn_destroy(_tls);
      _tls = nullptr;
    }
    _connected = false;
  }

  ~EspTlsSecuredTcpClient() override {
    close();
  }

protected:
  int getSocket() const override {
    return -1;
  }

private:
  esp_tls_t* _tls;
  bool _connected;
  const char* _caCert;
  WSString _host;
  tls_keep_alive_cfg_t _keepAlive;
};

enum class CameraSensorProfile : uint8_t {
  UNKNOWN = 0,
  OV2640,
  OV3660,
  GC2145
};

void connectWiFi();
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

// WiFi settings
// const char* ssid = "Keropok";
// const char* password = "ssz1151220817";
const char* ssid = "Xiaomi 13 Ultra";
const char* password = "1033087886";
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

// Stream URL for Cloudflare Worker relay (no NAS).
// Token should be appended via query parameter (e.g. &token=xxx), do not hardcode secrets in firmware.
const char* streamWsUrl = "wss://stream.rose980.eu.cc/esp32?room=cam01";
const char* streamWsHost = "stream.rose980.eu.cc";
const uint16_t streamWsPort = 443;
const char* streamWsPath = "/esp32?room=cam01";
const char* streamWsCaCert = R"EOF(
-----BEGIN CERTIFICATE-----
MIIG1TCCBL2gAwIBAgIQbFWr29AHksedBwzYEZ7WvzANBgkqhkiG9w0BAQwFADCB
iDELMAkGA1UEBhMCVVMxEzARBgNVBAgTCk5ldyBKZXJzZXkxFDASBgNVBAcTC0pl
cnNleSBDaXR5MR4wHAYDVQQKExVUaGUgVVNFUlRSVVNUIE5ldHdvcmsxLjAsBgNV
BAMTJVVTRVJUcnVzdCBSU0EgQ2VydGlmaWNhdGlvbiBBdXRob3JpdHkwHhcNMjAw
MTMwMDAwMDAwWhcNMzAwMTI5MjM1OTU5WjBLMQswCQYDVQQGEwJBVDEQMA4GA1UE
ChMHWmVyb1NTTDEqMCgGA1UEAxMhWmVyb1NTTCBSU0EgRG9tYWluIFNlY3VyZSBT
aXRlIENBMIICIjANBgkqhkiG9w0BAQEFAAOCAg8AMIICCgKCAgEAhmlzfqO1Mdgj
4W3dpBPTVBX1AuvcAyG1fl0dUnw/MeueCWzRWTheZ35LVo91kLI3DDVaZKW+TBAs
JBjEbYmMwcWSTWYCg5334SF0+ctDAsFxsX+rTDh9kSrG/4mp6OShubLaEIUJiZo4
t873TuSd0Wj5DWt3DtpAG8T35l/v+xrN8ub8PSSoX5Vkgw+jWf4KQtNvUFLDq8mF
WhUnPL6jHAADXpvs4lTNYwOtx9yQtbpxwSt7QJY1+ICrmRJB6BuKRt/jfDJF9Jsc
RQVlHIxQdKAJl7oaVnXgDkqtk2qddd3kCDXd74gv813G91z7CjsGyJ93oJIlNS3U
gFbD6V54JMgZ3rSmotYbz98oZxX7MKbtCm1aJ/q+hTv2YK1yMxrnfcieKmOYBbFD
hnW5O6RMA703dBK92j6XRN2EttLkQuujZgy+jXRKtaWMIlkNkWJmOiHmErQngHvt
iNkIcjJumq1ddFX4iaTI40a6zgvIBtxFeDs2RfcaH73er7ctNUUqgQT5rFgJhMmF
x76rQgB5OZUkodb5k2ex7P+Gu4J86bS15094UuYcV09hVeknmTh5Ex9CBKipLS2W
2wKBakf+aVYnNCU6S0nASqt2xrZpGC1v7v6DhuepyyJtn3qSV2PoBiU5Sql+aARp
wUibQMGm44gjyNDqDlVp+ShLQlUH9x8CAwEAAaOCAXUwggFxMB8GA1UdIwQYMBaA
FFN5v1qqK0rPVIDh2JvAnfKyA2bLMB0GA1UdDgQWBBTI2XhootkZaNU9ct5fCj7c
tYaGpjAOBgNVHQ8BAf8EBAMCAYYwEgYDVR0TAQH/BAgwBgEB/wIBADAdBgNVHSUE
FjAUBggrBgEFBQcDAQYIKwYBBQUHAwIwIgYDVR0gBBswGTANBgsrBgEEAbIxAQIC
TjAIBgZngQwBAgEwUAYDVR0fBEkwRzBFoEOgQYY/aHR0cDovL2NybC51c2VydHJ1
c3QuY29tL1VTRVJUcnVzdFJTQUNlcnRpZmljYXRpb25BdXRob3JpdHkuY3JsMHYG
CCsGAQUFBwEBBGowaDA/BggrBgEFBQcwAoYzaHR0cDovL2NydC51c2VydHJ1c3Qu
Y29tL1VTRVJUcnVzdFJTQUFkZFRydXN0Q0EuY3J0MCUGCCsGAQUFBzABhhlodHRw
Oi8vb2NzcC51c2VydHJ1c3QuY29tMA0GCSqGSIb3DQEBDAUAA4ICAQAVDwoIzQDV
ercT0eYqZjBNJ8VNWwVFlQOtZERqn5iWnEVaLZZdzxlbvz2Fx0ExUNuUEgYkIVM4
YocKkCQ7hO5noicoq/DrEYH5IuNcuW1I8JJZ9DLuB1fYvIHlZ2JG46iNbVKA3ygA
Ez86RvDQlt2C494qqPVItRjrz9YlJEGT0DrttyApq0YLFDzf+Z1pkMhh7c+7fXeJ
qmIhfJpduKc8HEQkYQQShen426S3H0JrIAbKcBCiyYFuOhfyvuwVCFDfFvrjADjd
4jX1uQXd161IyFRbm89s2Oj5oU1wDYz5sx+hoCuh6lSs+/uPuWomIq3y1GDFNafW
+LsHBU16lQo5Q2yh25laQsKRgyPmMpHJ98edm6y2sHUabASmRHxvGiuwwE25aDU0
2SAeepyImJ2CzB80YG7WxlynHqNhpE7xfC7PzQlLgmfEHdU+tHFeQazRQnrFkW2W
kqRGIq7cKRnyypvjPMkjeiV9lRdAM9fSJvsB3svUuu1coIG1xxI1yegoGM4r5QP4
RGIVvYaiI76C0djoSbQ/dkIUUXQuB8AL5jyH34g3BZaaXyvpmnV4ilppMXVAnAYG
ON51WhJ6W0xNdNJwzYASZYH+tmCWI+N60Gv2NNMGHwMZ7e9bXgzUCZH5FaBFDGR5
S9VWqHB73Q+OyIVvIbKYcSc2w/aSuFKGSA==
-----END CERTIFICATE-----
)EOF";

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
WebsocketsClient wsClient(std::make_shared<EspTlsSecuredTcpClient>(streamWsCaCert));
bool wsConnected = false;
bool mqttUsingInsecure = false;

String topicCmdLight;
String topicStateLight;
String topicStateOnline;
String topicStateHeartbeat;
String topicAck;

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
bool wifiWasConnected = false;
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
  WiFi.onEvent(onWiFiEvent);

  setupTopics();
  setupFlashPwm();

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
      wsConnectedAt = millis();
      lastWsPingTime = millis();
      lastStreamStatTime = millis();
      streamFramesSent = 0;
      streamFramesDropped = 0;
      streamBytesSent = 0;
      wsBusyDropCount = 0;
      Serial.println("[WS] 推流连接成功");
    } else if (event == WebsocketsEvent::ConnectionClosed) {
      wsConnected = false;
      unsigned long aliveMs = (wsConnectedAt > 0) ? (millis() - wsConnectedAt) : 0;
      Serial.printf("[WS] 推流连接断开，在线时长=%lu ms, RSSI=%d\n", aliveMs, WiFi.RSSI());
    }
  });
  wsClient.addHeader("Origin", "https://stream.rose980.eu.cc");
  wsClient.addHeader("User-Agent", "ESP32-CAM");

  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    wifiWasConnected = true;
    if (enablePortalAuth) {
      doPortalAuth();
      previousAuthTime = millis();
      delay(1000);
      testInternet();
    }
    syncTimeIfNeeded();
  }

  connectMQTT();
  connectStreamWs();
}

void loop() {
  unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED) {
    if (wifiWasConnected) {
      wifiWasConnected = false;
      Serial.println("[WiFi] 已断开，等待重连");
      wsConnected = false;
      if (wsClient.available()) wsClient.close();
    }

    if (now - lastWifiRetryTime >= wifiReconnectInterval) {
      lastWifiRetryTime = now;
      connectWiFi();
    }

    delay(10);
    return;
  }

  if (!wifiWasConnected) {
    wifiWasConnected = true;
    Serial.printf("[WiFi] 重连成功，IP=%s\n", WiFi.localIP().toString().c_str());
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

  if (!mqttClient.connected() && now - lastMqttConnectAttempt >= mqttReconnectInterval) {
    lastMqttConnectAttempt = now;
    connectMQTT();
  }

  if (!hasIPv6 && ipv6TryCount < ipv6MaxRetry && now - lastIPv6TryTime >= ipv6RetryInterval) {
    requestIPv6("loop-retry");
  } else if (!hasIPv6 && ipv6TryCount >= ipv6MaxRetry && !ipv6RetryExhaustedLogged) {
    ipv6RetryExhaustedLogged = true;
    Serial.println("[WiFi] IPv6 多次申请仍未成功，当前热点可能未向 STA 分配 IPv6");
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
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.printf("[WiFi] 正在连接 %s\n", ssid);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] 连接成功，IP=%s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] 连接失败，请检查 SSID/密码");
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
  Serial.println("[WS] 正在连接推流端点: " + String(streamWsUrl));
  Serial.printf("[WS] 重连次数: %lu\n", wsReconnectCount);
  Serial.printf("[WS] 连接前可用堆内存: %u\n", ESP.getFreeHeap());

  if (!wsClient.connect(streamWsHost, streamWsPort, streamWsPath)) {
    wsConnected = false;
    Serial.println("[WS] 连接失败");
    Serial.printf("[WS] 连接失败后可用堆内存: %u\n", ESP.getFreeHeap());
  }
}

void sendStreamFrameIfReady() {
  if (!wsClient.available() || !wsConnected) return;

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
