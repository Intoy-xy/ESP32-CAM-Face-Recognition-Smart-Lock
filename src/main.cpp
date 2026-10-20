#include <Arduino.h>
#include "esp_system.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Keypad_I2C.h>
#include "esp_camera.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "secrets.h"

#ifndef RTC_CNTL_BROWNOUT_REG
#define RTC_CNTL_BROWNOUT_REG RTC_CNTL_BROWN_OUT_REG
#endif

// ==========================================
// 1. ACCESS POINT (HOTSPOT) CONFIGURATION
//    No router or internet needed.
//    Connect your phone/laptop to this Wi-Fi,
//    then open http://192.168.4.1 in a browser.
// ==========================================

// ==========================================
// 2. HARDWARE PIN DEFINITIONS
// ==========================================
// L298N driver: IN1 controls the solenoid.
// EN tied to 5V (always enabled), IN2 tied to GND.
// HIGH = energise solenoid, LOW = de-energise.
#define L298N_IN_PIN    15
#define SOLENOID_UNLOCK HIGH
#define SOLENOID_LOCK   LOW

#define BUZZER_PIN   12
#define OLED_SDA     13
#define OLED_SCL     14

// ==========================================
// 3. OLED DISPLAY (128x64, I2C)
// ==========================================
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ==========================================
// 4. I2C KEYPAD (PCF8574 at 0x20)
// ==========================================
#define I2C_KEYPAD_ADDR 0x20

const byte ROWS = 4;
const byte COLS = 4;

char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};

// Row 0->P7, Row 1->P6, Row 2->P5, Row 3->P4
// Col 0->P3, Col 1->P2, Col 2->P1, Col 3->P0
byte rowPins[ROWS] = {7, 6, 5, 4};
byte colPins[COLS] = {3, 2, 1, 0};

Keypad_I2C keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS, I2C_KEYPAD_ADDR, PCF8574, &Wire);

// ==========================================
// 5. AI-THINKER CAMERA PINS
// ==========================================
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

int camera_vflip   = 0;
int camera_hmirror = 0;

// ==========================================
// 6. SYSTEM STATES
// ==========================================
enum SystemState {
  STATE_OPTIONS_MENU,   // Main Menu: 1=PIN, 2=Face
  STATE_PIN_ENTRY,      // PIN input
  STATE_FACE_MENU,      // Face submenu: 1=Unlock, 2=Enroll, D=Back
  STATE_FACE_UNLOCK,    // Laptop app checks the camera stream
  STATE_FACE_ENROLL,    // Laptop app enrolls from the camera stream
  STATE_UNLOCKED        // Solenoid open + countdown
};

volatile SystemState currentState = STATE_OPTIONS_MENU;

// ==========================================
// 7. LOGIC VARIABLES
// ==========================================
String inputBuffer = "";

unsigned long unlockTimerEnd    = 0;
const unsigned long UNLOCK_DURATION       = 5000;  // ms solenoid stays open
volatile bool laptopUnlockRequested = false;

int lastDisplayedSeconds = -1;

// ==========================================
// 8. WEB STREAM GLOBALS
// ==========================================
httpd_handle_t control_httpd = NULL;
httpd_handle_t stream_httpd  = NULL;
bool           controlRunning = false;
bool           streamRunning  = false;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY     = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART         = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// ==========================================
// 9. FORWARD DECLARATIONS
// ==========================================
void drawOptionsMenu();
void drawPinInterface();
void drawFaceMenu();
void drawUnlockFaceInterface();
void drawEnrollFaceInterface();
void unlockSolenoid(const String &source);
void lockSolenoid();
void startCameraServer();
char getCleanKey();

// ==========================================
// 11. BUZZER HELPERS
// ==========================================
void triggerBuzzer(int durationMs, int beepCount = 1) {
  for (int i = 0; i < beepCount; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(durationMs);
    digitalWrite(BUZZER_PIN, LOW);
    if (i < beepCount - 1) delay(80);
  }
}

void triggerErrorBuzzer() { triggerBuzzer(120, 3); }

// ==========================================
// 12. OLED DRAW FUNCTIONS
// ==========================================
void drawOptionsMenu() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("--- UNLOCK OPTIONS ---");
  display.setTextSize(2);
  display.setCursor(10, 20);
  display.println("1. PIN");
  display.setCursor(10, 42);
  display.println("2. Face");
  display.display();
}

void drawPinInterface() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("ENTER 4-DIGIT PIN:");
  display.setTextSize(2);
  display.setCursor(20, 24);
  for (int i = 0; i < 4; i++) {
    display.print(i < (int)inputBuffer.length() ? "* " : "_ ");
  }
  display.setTextSize(1);
  display.setCursor(0, 52);
  display.println("[D] Back / Delete");
  display.display();
}

void drawFaceMenu() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("--- FACE OPTIONS ---");
  display.setCursor(5, 18);
  display.println("1. Laptop Unlock");
  display.setCursor(5, 34);
  display.println("2. Enroll Face");
  display.setCursor(5, 52);
  display.println("[D] Back to Menu");
  display.display();
}

void drawUnlockFaceInterface() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("--- FACE UNLOCK ---");
  display.setCursor(5, 20);
  display.println("Run laptop app");
  display.setCursor(5, 32);
  display.println("Look at camera...");
  display.setCursor(0, 52);
  display.println("[D] Cancel");
  display.display();
}

void drawEnrollFaceInterface() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("--- ENROLL FACE ---");
  display.setCursor(5, 20);
  display.println("Run laptop app");
  display.setCursor(5, 32);
  display.println("to enroll your face");
  display.setCursor(0, 56);
  display.println("[D] Cancel");
  display.display();
}

// ==========================================
// 13. SOLENOID CONTROL
// ==========================================
void unlockSolenoid(const String &source) {
  currentState     = STATE_UNLOCKED;
  unlockTimerEnd   = millis() + UNLOCK_DURATION;
  lastDisplayedSeconds = -1;

  digitalWrite(L298N_IN_PIN, SOLENOID_UNLOCK);
  triggerBuzzer(80, 2);

  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("UNLOCKED!");
  display.setTextSize(1);
  display.setCursor(0, 24);
  display.println("Access Granted:");
  display.println(source);
  display.setCursor(0, 48);
  display.println("Auto-lock in: 5s");
  display.display();

  Serial.println("[SUCCESS] Unlocked via " + source);
}

void lockSolenoid() {
  digitalWrite(L298N_IN_PIN, SOLENOID_LOCK);
  triggerBuzzer(300, 1);

  currentState          = STATE_OPTIONS_MENU;
  inputBuffer           = "";
  lastDisplayedSeconds  = -1;

  drawOptionsMenu();
  Serial.println("[LOCKED] Solenoid re-locked.");
}

// ==========================================
// 14. CAMERA RESOLUTION HELPER
// ==========================================
// ==========================================
// 15. WEB STREAM HANDLER
// ==========================================
esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb  = NULL;
  esp_err_t    res = ESP_OK;
  char part_buf[128];

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) { vTaskDelay(15 / portTICK_PERIOD_MS); continue; }

    size_t   jpg_len = fb->len;
    uint8_t *jpg_buf = fb->buf;

    res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    if (res == ESP_OK) {
      size_t hlen = snprintf(part_buf, sizeof(part_buf), _STREAM_PART, jpg_len);
      res = httpd_resp_send_chunk(req, part_buf, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)jpg_buf, jpg_len);
    }

    esp_camera_fb_return(fb);
    fb = NULL;
    if (res != ESP_OK) break;
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
  return res;
}

// ==========================================
// 16. WEB DASHBOARD HANDLER
// ==========================================
esp_err_t index_handler(httpd_req_t *req) {
  const char index_html[] =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32-CAM Smart Lock</title>"
    "<style>"
    "body{font-family:Arial,sans-serif;background:#141414;color:#eee;text-align:center;margin:0;padding:15px;}"
    "h2{color:#4CAF50;margin-bottom:10px;}"
    ".status-card{background:#222;max-width:380px;margin:15px auto;padding:15px;border-radius:10px;"
    "text-align:left;box-shadow:0 4px 10px rgba(0,0,0,0.4);}"
    ".badge{display:inline-block;padding:3px 8px;border-radius:12px;background:#333;color:#f39c12;font-weight:bold;}"
    ".btn{padding:10px 18px;margin:5px;border:none;border-radius:6px;font-size:14px;cursor:pointer;font-weight:bold;}"
    ".btn-blue{background:#007BFF;color:#fff;}"
    ".btn-green{background:#28A745;color:#fff;}"
    "</style></head><body>"
    "<h2>Smart Lock Status</h2>"
    "<p>Run the laptop app for face enrollment and recognition.</p>"
    "<div class='status-card'>"
    "<p><b>System State:</b> <span id='st' class='badge'>Loading...</span></p>"
    "<p><b>Face unlock:</b> handled by the connected laptop app</p>"
    "<p style='font-size:12px;color:#aaa;'>"
    "Connect to <b>SmartLock-Cam</b> Wi-Fi, then open <b>http://192.168.4.1</b>"
    "</p>"
    "</div>"
    "<script>"
    "setInterval(()=>{"
    "fetch('/status').then(r=>r.json()).then(d=>{"
    "document.getElementById('st').innerText=d.state;"
    "}).catch(()=>{});"
    "},1000);"
    "</script></body></html>";

  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, index_html, HTTPD_RESP_USE_STRLEN);
}

// ==========================================
// 17. WEB API ENDPOINTS
// ==========================================
esp_err_t status_handler(httpd_req_t *req) {
  String stateStr = "MAIN MENU";
  if      (currentState == STATE_PIN_ENTRY)   stateStr = "PIN ENTRY";
  else if (currentState == STATE_FACE_MENU)   stateStr = "FACE MENU";
  else if (currentState == STATE_FACE_UNLOCK) stateStr = "SCANNING FACE";
  else if (currentState == STATE_FACE_ENROLL) stateStr = "ENROLLING FACE";
  else if (currentState == STATE_UNLOCKED)    stateStr = "UNLOCKED (5s)";

  String json = "{\"state\":\"" + stateStr + "\"}";
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, json.c_str(), HTTPD_RESP_USE_STRLEN);
}

esp_err_t flip_handler(httpd_req_t *req) {
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    camera_vflip   = !camera_vflip;
    camera_hmirror = camera_vflip;
    s->set_vflip(s, camera_vflip);
    s->set_hmirror(s, camera_hmirror);
    Serial.printf("[CAMERA] vflip=%d hmirror=%d\n", camera_vflip, camera_hmirror);
  }
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
}

bool laptopTokenMatches(httpd_req_t *req) {
  const size_t expectedLength = strlen(LAPTOP_UNLOCK_TOKEN);
  const size_t suppliedLength = httpd_req_get_hdr_value_len(req, "X-Lock-Token");
  if (suppliedLength != expectedLength || suppliedLength >= 96) return false;

  char suppliedToken[96] = {0};
  if (httpd_req_get_hdr_value_str(req, "X-Lock-Token", suppliedToken,
                                  sizeof(suppliedToken)) != ESP_OK) {
    return false;
  }

  // Constant-time comparison avoids revealing the matching token prefix.
  uint8_t difference = 0;
  for (size_t i = 0; i < expectedLength; ++i) {
    difference |= (uint8_t)(suppliedToken[i] ^ LAPTOP_UNLOCK_TOKEN[i]);
  }
  return difference == 0;
}

esp_err_t laptop_unlock_handler(httpd_req_t *req) {
  if (!laptopTokenMatches(req)) {
    httpd_resp_set_status(req, "401 Unauthorized");
    return httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
  }

  // Only accept face unlocks while the keypad has explicitly entered scan mode.
  if (currentState != STATE_FACE_UNLOCK) {
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_send(req, "Face scan is not active", HTTPD_RESP_USE_STRLEN);
  }

  // Acknowledge the request before the main loop energizes the solenoid.
  // This keeps the actuator and buzzer from interrupting the HTTP response.
  httpd_resp_set_status(req, "202 Accepted");
  esp_err_t responseResult = httpd_resp_send(req, "Unlock accepted", HTTPD_RESP_USE_STRLEN);
  if (responseResult == ESP_OK) {
    // Let loop() operate the display, buzzer and solenoid on its own task.
    laptopUnlockRequested = true;
    Serial.println("[FACE] Laptop unlock request acknowledged.");
  }
  return responseResult;
}

// ==========================================
// 18. CAMERA SERVER START / STOP
// ==========================================
void startCameraServer() {
  if (controlRunning && streamRunning) return;

  Serial.printf("[WEB] Starting servers; free heap=%u, free PSRAM=%u\n",
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram());

  httpd_config_t controlConfig = HTTPD_DEFAULT_CONFIG();
  controlConfig.server_port = 80;

  httpd_uri_t controlUris[] = {
    { .uri="/",             .method=HTTP_GET, .handler=index_handler,        .user_ctx=NULL },
    { .uri="/status",       .method=HTTP_GET, .handler=status_handler,       .user_ctx=NULL },
    { .uri="/flip",         .method=HTTP_GET, .handler=flip_handler,         .user_ctx=NULL },
    { .uri="/unlock",       .method=HTTP_POST, .handler=laptop_unlock_handler,.user_ctx=NULL },
  };

  if (!controlRunning && httpd_start(&control_httpd, &controlConfig) == ESP_OK) {
    for (auto &u : controlUris) httpd_register_uri_handler(control_httpd, &u);
    controlRunning = true;
    Serial.println("[WEB] Control server started on port 80.");
  } else if (!controlRunning) {
    control_httpd = NULL;
    Serial.println("[WEB] Control server failed to start.");
  }

  if (!streamRunning) {
    httpd_config_t streamConfig = HTTPD_DEFAULT_CONFIG();
    streamConfig.server_port = 81;
    streamConfig.ctrl_port = controlConfig.ctrl_port + 1;
    httpd_uri_t streamUri = {
      .uri="/stream", .method=HTTP_GET, .handler=stream_handler, .user_ctx=NULL
    };

    if (httpd_start(&stream_httpd, &streamConfig) == ESP_OK) {
      httpd_register_uri_handler(stream_httpd, &streamUri);
      streamRunning = true;
      Serial.println("[WEB] MJPEG stream started on port 81.");
    } else {
      stream_httpd = NULL;
      Serial.println("[WEB] MJPEG stream server failed to start.");
    }
  }

  if (controlRunning && streamRunning) {
    Serial.println("[WEB] Dashboard http://192.168.4.1, stream :81/stream");
  }
}

// ==========================================
// 19. CAMERA INITIALISATION
// ==========================================
void setupCamera() {
  camera_config_t config;
  config.ledc_channel  = LEDC_CHANNEL_0;
  config.ledc_timer    = LEDC_TIMER_0;
  config.pin_d0        = Y2_GPIO_NUM;
  config.pin_d1        = Y3_GPIO_NUM;
  config.pin_d2        = Y4_GPIO_NUM;
  config.pin_d3        = Y5_GPIO_NUM;
  config.pin_d4        = Y6_GPIO_NUM;
  config.pin_d5        = Y7_GPIO_NUM;
  config.pin_d6        = Y8_GPIO_NUM;
  config.pin_d7        = Y9_GPIO_NUM;
  config.pin_xclk      = XCLK_GPIO_NUM;
  config.pin_pclk      = PCLK_GPIO_NUM;
  config.pin_vsync     = VSYNC_GPIO_NUM;
  config.pin_href      = HREF_GPIO_NUM;
  config.pin_sscb_sda  = SIOD_GPIO_NUM;
  config.pin_sscb_scl  = SIOC_GPIO_NUM;
  config.pin_pwdn      = PWDN_GPIO_NUM;
  config.pin_reset     = RESET_GPIO_NUM;
  config.xclk_freq_hz  = 20000000;
  config.pixel_format  = PIXFORMAT_JPEG;

  // Keep VGA-free QVGA for usable face detail with less Wi-Fi transfer time.
  if (psramFound()) {
    config.frame_size   = FRAMESIZE_QVGA;
    config.jpeg_quality = 14;
    config.fb_count     = 2;
  } else {
    config.frame_size   = FRAMESIZE_QQVGA;
    config.jpeg_quality = 16;
    config.fb_count     = 1;
  }

#if defined(CAMERA_GRAB_LATEST)
  config.grab_mode = CAMERA_GRAB_LATEST;
#endif

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAM] Init failed: 0x%x\n", err);
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_vflip(s, camera_vflip);
    s->set_hmirror(s, camera_hmirror);
  }

}

// ==========================================
// 20. SETUP
// ==========================================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWNOUT_REG, 0);
  Serial.begin(115200);
  Serial.printf("[BOOT] Previous reset reason: %d\n", (int)esp_reset_reason());

  // Solenoid – start locked
  pinMode(L298N_IN_PIN, OUTPUT);
  digitalWrite(L298N_IN_PIN, SOLENOID_LOCK);

  // Buzzer
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  // OLED
  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("[OLED] Init failed!");
  }

  // Keypad
  keypad.begin();

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 5);
  display.println("BOOTING...");
  display.println("Init Camera...");
  display.display();

  setupCamera();

  // Start Wi-Fi ACCESS POINT (hotspot) — no router or internet needed
  display.println("Starting Hotspot...");
  display.display();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);

  IPAddress apIP = WiFi.softAPIP();
  Serial.println("[WiFi] AP started. SSID: " + String(AP_SSID));
  Serial.println("[WiFi] Connect to: http://" + apIP.toString());

  startCameraServer();

  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("HOTSPOT ACTIVE!");
  display.setCursor(0, 14);
  display.println("WiFi: SmartLock-Cam");
  display.setCursor(0, 26);
  display.print("Pass: ");
  String apPassword = AP_PASSWORD;
  display.println(apPassword.substring(0, 10));
  display.println(apPassword.substring(10));
  display.setCursor(0, 44);
  display.println("Open in browser:");
  display.setCursor(0, 54);
  display.println("http://192.168.4.1");
  display.display();
  delay(3000);

  lockSolenoid();
}

// ==========================================
// 21. MAIN LOOP (STATE MACHINE)
// ==========================================
void loop() {
  unsigned long currentMillis = millis();
  char key = getCleanKey();

  if (laptopUnlockRequested) {
    laptopUnlockRequested = false;
    if (currentState == STATE_FACE_UNLOCK) {
      unlockSolenoid("Laptop Face Match");
    }
  }

  switch (currentState) {

    // --------------------------------------------------
    // STATE 1: MAIN OPTIONS MENU
    // --------------------------------------------------
    case STATE_OPTIONS_MENU: {
      if (key == '1') {
        triggerBuzzer(50);
        inputBuffer  = "";
        currentState = STATE_PIN_ENTRY;
        drawPinInterface();
      } else if (key == '2') {
        triggerBuzzer(50);
        currentState = STATE_FACE_MENU;
        drawFaceMenu();
      }
      break;
    }

    // --------------------------------------------------
    // STATE 2: PIN ENTRY
    // --------------------------------------------------
    case STATE_PIN_ENTRY: {
      if (key != 0) {
        if (key >= '0' && key <= '9') {
          triggerBuzzer(50);
          inputBuffer += key;
          drawPinInterface();

          if (inputBuffer.length() == 4) {
            delay(200);
            if (inputBuffer == SECRET_PIN) {
              unlockSolenoid("PIN Correct");
            } else {
              triggerErrorBuzzer();
              display.clearDisplay();
              display.setTextSize(2);
              display.setTextColor(SSD1306_WHITE);
              display.setCursor(10, 10);
              display.println("WRONG PIN!");
              display.setTextSize(1);
              display.setCursor(10, 40);
              display.println("Please try again...");
              display.display();
              delay(1200);
              inputBuffer = "";
              drawPinInterface();
            }
          }
        } else if (key == 'D') {
          triggerBuzzer(50);
          if (inputBuffer.length() > 0) {
            inputBuffer.remove(inputBuffer.length() - 1);
            drawPinInterface();
          } else {
            currentState = STATE_OPTIONS_MENU;
            drawOptionsMenu();
          }
        }
      }
      break;
    }

    // --------------------------------------------------
    // STATE 3: FACE MENU
    // --------------------------------------------------
    case STATE_FACE_MENU: {
      if (key == '1') {
        triggerBuzzer(50);
        currentState = STATE_FACE_UNLOCK;
        drawUnlockFaceInterface();
      } else if (key == '2') {
        triggerBuzzer(50);
        currentState = STATE_FACE_ENROLL;
        drawEnrollFaceInterface();
      } else if (key == 'D') {
        triggerBuzzer(50);
        currentState = STATE_OPTIONS_MENU;
        drawOptionsMenu();
      }
      break;
    }

    // --------------------------------------------------
    // STATE 4: LAPTOP FACE UNLOCK
    // The laptop reads /stream and sends an authenticated POST /unlock.
    // The ESP32 does no face inference and remains locked without the laptop.
    // --------------------------------------------------
    case STATE_FACE_UNLOCK: {
      if (key == 'D') {
        triggerBuzzer(50);
        currentState = STATE_FACE_MENU;
        drawFaceMenu();
      }
      break;
    }

    // --------------------------------------------------
    // STATE 5: LAPTOP FACE ENROLLMENT
    // The laptop stores the face profile locally; the ESP32 only streams video.
    // --------------------------------------------------
    case STATE_FACE_ENROLL: {
      if (key == 'D') {
        triggerBuzzer(50);
        currentState = STATE_FACE_MENU;
        drawFaceMenu();
      }
      break;
    }

    // --------------------------------------------------
    // STATE 6: UNLOCKED – countdown then auto-lock
    // --------------------------------------------------
    case STATE_UNLOCKED: {
      if (currentMillis >= unlockTimerEnd) {
        lockSolenoid();
      } else {
        int secondsLeft = (unlockTimerEnd - currentMillis + 999) / 1000;
        if (secondsLeft != lastDisplayedSeconds) {
          lastDisplayedSeconds = secondsLeft;

          display.clearDisplay();
          display.setTextSize(2);
          display.setTextColor(SSD1306_WHITE);
          display.setCursor(0, 0);
          display.println("UNLOCKED!");
          display.setTextSize(1);
          display.setCursor(0, 24);
          display.println("Solenoid Open");
          display.setCursor(0, 42);
          display.setTextSize(2);
          display.print(secondsLeft);
          display.setTextSize(1);
          display.print("s remaining");
          display.display();
        }
      }
      break;
    }
  }
}

// ==========================================
// 22. KEYPAD & SERIAL INPUT HELPER
// ==========================================
char getCleanKey() {
  char key = keypad.getKey();
  if (key != NO_KEY) return key;
  if (Serial.available()) {
    char c = Serial.read();
    if (c >= ' ' && c <= '~') return c;
  }
  return 0;
}
