#include <SPI.h>
#include <WiFi.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <TJpg_Decoder.h>
#include <esp_heap_caps.h>

#define TFT_MOSI 11
#define TFT_SCLK 12
#define TFT_CS   10
#define TFT_DC    9
#define TFT_RST   8
#define TFT_MISO 13
#define TFT_BL   21

static const char *WIFI_SSID = "ESP32-CAM-VIDEO";
static const char *WIFI_PASSWORD = "12345678";
static const IPAddress CAMERA_IP(192, 168, 4, 1);
static constexpr uint16_t CAMERA_PORT = 80;
static constexpr uint32_t TFT_SPI_HZ = 40000000;
static constexpr size_t MAX_JPEG_BYTES = 90 * 1024;

Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);
WiFiClient client;

uint8_t *jpegBuffer = nullptr;

bool tftOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap)
{
  if (y >= tft.height() || x >= tft.width()) {
    return false;
  }

  if (x + w > tft.width()) {
    w = tft.width() - x;
  }
  if (y + h > tft.height()) {
    h = tft.height() - y;
  }

  tft.drawRGBBitmap(x, y, bitmap, w, h);
  return true;
}

String readHttpLine(uint32_t timeoutMs = 3000)
{
  String line;
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    while (client.available()) {
      char c = (char)client.read();
      if (c == '\n') {
        line.trim();
        return line;
      }
      if (c != '\r') {
        line += c;
      }
    }
    if (!client.connected()) {
      break;
    }
    delay(1);
  }
  return line;
}

bool connectToCamera()
{
  client.stop();
  Serial.println("Connecting to camera stream...");

  if (!client.connect(CAMERA_IP, CAMERA_PORT)) {
    Serial.println("Camera TCP connection failed");
    return false;
  }

  client.setNoDelay(true);
  client.print(String("GET /stream HTTP/1.1\r\n") +
               "Host: 192.168.4.1\r\n" +
               "Connection: keep-alive\r\n\r\n");

  String status = readHttpLine();
  Serial.println(status);
  if (!status.startsWith("HTTP/1.1 200")) {
    client.stop();
    return false;
  }

  while (client.connected()) {
    String line = readHttpLine();
    if (line.length() == 0) {
      return true;
    }
  }

  client.stop();
  return false;
}

bool readExact(uint8_t *dst, size_t len, uint32_t timeoutMs = 5000)
{
  size_t offset = 0;
  uint32_t start = millis();

  while (offset < len && millis() - start < timeoutMs) {
    int availableBytes = client.available();
    if (availableBytes > 0) {
      size_t chunk = min((size_t)availableBytes, len - offset);
      int readBytes = client.read(dst + offset, chunk);
      if (readBytes > 0) {
        offset += readBytes;
        start = millis();
      }
    } else {
      if (!client.connected()) {
        return false;
      }
      delay(1);
    }
  }

  return offset == len;
}

int readFrameLength()
{
  int contentLength = -1;

  while (client.connected()) {
    String line = readHttpLine();
    if (line.startsWith("--frame")) {
      break;
    }
    if (line.length() == 0) {
      continue;
    }
  }

  while (client.connected()) {
    String line = readHttpLine();
    if (line.length() == 0) {
      break;
    }

    String lower = line;
    lower.toLowerCase();
    if (lower.startsWith("content-length:")) {
      contentLength = line.substring(line.indexOf(':') + 1).toInt();
    }
  }

  return contentLength;
}

void showStatus(const char *message, uint16_t color)
{
  tft.fillScreen(ILI9341_BLACK);
  tft.setCursor(12, 104);
  tft.setTextColor(color);
  tft.setTextSize(2);
  tft.print(message);
}

void setupDisplay()
{
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  pinMode(TFT_RST, OUTPUT);
  digitalWrite(TFT_RST, LOW);
  delay(100);
  digitalWrite(TFT_RST, HIGH);
  delay(100);

  pinMode(TFT_CS, OUTPUT);
  digitalWrite(TFT_CS, HIGH);
  pinMode(TFT_DC, OUTPUT);

  SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.begin(TFT_SPI_HZ);
  tft.setRotation(3);
  tft.fillScreen(ILI9341_BLACK);
}

void setup()
{
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== ESP32-S3 TFT MJPEG Receiver ===");

  setupDisplay();
  showStatus("Allocating buffer", ILI9341_YELLOW);

  jpegBuffer = (uint8_t *)heap_caps_malloc(MAX_JPEG_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!jpegBuffer) {
    jpegBuffer = (uint8_t *)heap_caps_malloc(MAX_JPEG_BYTES, MALLOC_CAP_8BIT);
  }
  if (!jpegBuffer) {
    showStatus("No JPEG buffer", ILI9341_RED);
    Serial.println("JPEG buffer allocation failed");
    while (true) {
      delay(1000);
    }
  }

  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(false);
  TJpgDec.setCallback(tftOutput);

  showStatus("Connecting WiFi", ILI9341_YELLOW);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("S3 IP: ");
  Serial.println(WiFi.localIP());

  showStatus("Opening stream", ILI9341_YELLOW);
}

void loop()
{
  if (!client.connected()) {
    if (!connectToCamera()) {
      showStatus("Camera offline", ILI9341_RED);
      delay(1000);
      return;
    }
    tft.fillScreen(ILI9341_BLACK);
  }

  int frameLength = readFrameLength();
  if (frameLength <= 0 || frameLength > (int)MAX_JPEG_BYTES) {
    Serial.printf("Bad frame length: %d\n", frameLength);
    client.stop();
    return;
  }

  if (!readExact(jpegBuffer, frameLength)) {
    Serial.println("Frame read failed");
    client.stop();
    return;
  }

  TJpgDec.drawJpg(0, 0, jpegBuffer, frameLength);
}

