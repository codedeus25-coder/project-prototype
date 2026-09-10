#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

#include <time.h>

#include <Adafruit_ADS1X15.h>
#include <LiquidCrystal_I2C.h>

#include <math.h>

// =====================================================
// WIFI
// =====================================================
// Put your Wi-Fi credentials here.
const char* WIFI_SSID = "NARVASA_Wifi";
const char* WIFI_PASSWORD = "@Narvasa96";

// =====================================================
// FIREBASE REALTIME DATABASE
// =====================================================
const char* FIREBASE_DATABASE_URL =
  "https://currentrax-2fc0e-default-rtdb.asia-southeast1.firebasedatabase.app";

// LilyGO sensor data is written here
const char* FIREBASE_PATH = "/current.json";

// Mobile app writes the status here
const char* FIREBASE_STATUS_PATH = "/current/status.json";

// =====================================================
// FIREBASE STATUS
// =====================================================
//
// Expected values:
//
// Normal
// Warning / High Current
// Over Current
// Severe Overcurrent
// Possible Short Circuit
//
String firebaseStatus = "Unknown";

const unsigned long STATUS_CHECK_INTERVAL = 1000;
unsigned long lastStatusCheck = 0;

// =====================================================
// TIME / NTP - Philippines UTC+8
// =====================================================
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.google.com";

const long GMT_OFFSET_SEC = 8 * 3600;
const int DAYLIGHT_OFFSET_SEC = 0;

bool timeConfigured = false;

// =====================================================
// I2C
//
// ADS1115 = 0x48
// LCD     = 0x27
// =====================================================
#define SDA_PIN 21
#define SCL_PIN 22

Adafruit_ADS1115 ads;

#define ADS1115_ADDRESS 0x48

LiquidCrystal_I2C lcd(0x27, 16, 2);

// ADS1115 GAIN_ONE:
// +/- 4.096 V
// 1 bit = 0.125 mV
const float ADS_LSB = 0.000125;

// =====================================================
// SD CARD - LILYGO A7670G
// =====================================================
#define SD_SCK   14
#define SD_MISO  2
#define SD_MOSI  15
#define SD_CS    13

// Peripheral power pin used on this LILYGO board setup
#define BOARD_POWER_PIN 12

SPIClass sdSPI(HSPI);

bool sdAvailable = false;
String sdFileName = "";

// =====================================================
// BUZZER
// =====================================================
//
// This assumes an ACTIVE buzzer:
//
// HIGH = buzzer ON
// LOW  = buzzer OFF
//
#define BUZZER_PIN 18

const uint8_t BUZZER_ON_LEVEL = HIGH;
const uint8_t BUZZER_OFF_LEVEL = LOW;

bool buzzerActive = false;

// =====================================================
// CURRENT TRANSFORMER
//
// OPCT16AL
// 100A : 50mA
// =====================================================
const float CT_PRIMARY_CURRENT = 100.0;
const float CT_SECONDARY_CURRENT = 0.050;

// 100 / 0.050 = 2000
const float CT_RATIO =
  CT_PRIMARY_CURRENT / CT_SECONDARY_CURRENT;

// Burden resistor
const float BURDEN_RESISTOR = 20.0;

// =====================================================
// SAMPLING / CALIBRATION
// =====================================================
const int SAMPLES = 1000;

float CALIBRATION_FACTOR = 1.0;

// Any measured current below this is considered noise.
const float NOISE_THRESHOLD = 0.03;

// =====================================================
// ELECTRICAL DATA
// =====================================================
//
// Static for now.
// Replace these later when you add actual voltage
// and power-factor measurements.
//
float voltage = 238.88;
float powerFactor = 0.95;

// =====================================================
// INTERVALS
// =====================================================
const unsigned long FIREBASE_INTERVAL = 2000;
const unsigned long SD_SAVE_INTERVAL = 2000;
const unsigned long LCD_PAGE_INTERVAL = 2000;
const unsigned long WIFI_RETRY_INTERVAL = 10000;

unsigned long lastFirebaseUpdate = 0;
unsigned long lastSDSave = 0;
unsigned long lastLCDPageChange = 0;
unsigned long lastWiFiRetry = 0;

int lcdPage = 0;

// =====================================================
// WIFI
// =====================================================
void connectWiFiInitial()
{
  Serial.println();
  Serial.println("Connecting to WiFi...");

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Connecting WiFi");
  lcd.setCursor(0, 1);
  lcd.print("Please wait...");

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD
  );

  unsigned long started = millis();

  // Do not block forever.
  // SD logging can still operate when Wi-Fi is unavailable.
  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - started < 10000
  )
  {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("WiFi CONNECTED!");

    Serial.print("IP: ");
    Serial.println(WiFi.localIP());

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Connected");

    delay(1000);
  }
  else
  {
    Serial.println("WiFi not connected.");
    Serial.println("System will continue and retry later.");

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi Offline");

    lcd.setCursor(0, 1);
    lcd.print("SD still works");

    delay(1000);
  }
}

// =====================================================
// MAINTAIN WIFI
// =====================================================
void maintainWiFi()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    return;
  }

  if (
    millis() - lastWiFiRetry <
    WIFI_RETRY_INTERVAL
  )
  {
    return;
  }

  lastWiFiRetry = millis();

  Serial.println("Retrying WiFi...");

  WiFi.disconnect();

  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD
  );
}

// =====================================================
// NTP TIME
// =====================================================
void setupTime()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    return;
  }

  Serial.println("Synchronizing time...");

  configTime(
    GMT_OFFSET_SEC,
    DAYLIGHT_OFFSET_SEC,
    NTP_SERVER_1,
    NTP_SERVER_2
  );

  struct tm timeInfo;

  for (int attempt = 0; attempt < 20; attempt++)
  {
    if (getLocalTime(&timeInfo, 500))
    {
      Serial.println("Time synchronized!");

      timeConfigured = true;

      return;
    }

    Serial.print(".");
  }

  Serial.println();
  Serial.println("Time synchronization failed.");
}

// =====================================================
// ENSURE TIME IS CONFIGURED
// =====================================================
void ensureTimeConfigured()
{
  if (
    !timeConfigured &&
    WiFi.status() == WL_CONNECTED
  )
  {
    setupTime();
  }
}

// =====================================================
// GET TIMESTAMP
// =====================================================
String getTimestamp()
{
  struct tm timeInfo;

  if (!getLocalTime(&timeInfo, 50))
  {
    return "TIME_NOT_AVAILABLE";
  }

  char timestamp[25];

  strftime(
    timestamp,
    sizeof(timestamp),
    "%Y-%m-%d %H:%M:%S",
    &timeInfo
  );

  return String(timestamp);
}

// =====================================================
// GET TIME ONLY
// =====================================================
String getTimeOnly()
{
  struct tm timeInfo;

  if (!getLocalTime(&timeInfo, 50))
  {
    return "--:--:--";
  }

  char buffer[10];

  strftime(
    buffer,
    sizeof(buffer),
    "%H:%M:%S",
    &timeInfo
  );

  return String(buffer);
}

// =====================================================
// SD CARD - UNIQUE FILE NAME
// =====================================================
//
// First boot:
// /CRX001.CSV
//
// Next boot:
// /CRX002.CSV
//
// Next boot:
// /CRX003.CSV
//
// Existing files are not overwritten.
//
String createNewFileName()
{
  for (int number = 1; number <= 999; number++)
  {
    char fileName[16];

    snprintf(
      fileName,
      sizeof(fileName),
      "/CRX%03d.CSV",
      number
    );

    if (!SD.exists(fileName))
    {
      return String(fileName);
    }
  }

  return "/CRXNEW.CSV";
}

// =====================================================
// TEST SD WRITE
// =====================================================
bool testSDWrite()
{
  const char* testPath = "/TEST.TXT";

  if (SD.exists(testPath))
  {
    SD.remove(testPath);
  }

  File testFile =
    SD.open(testPath, FILE_WRITE);

  if (!testFile)
  {
    Serial.println(
      "ERROR: Cannot create /TEST.TXT"
    );

    return false;
  }

  testFile.println(
    "CurrentRax SD write test"
  );

  testFile.flush();
  testFile.close();

  if (!SD.exists(testPath))
  {
    Serial.println(
      "ERROR: TEST.TXT verification failed."
    );

    return false;
  }

  Serial.println(
    "SD write test successful."
  );

  SD.remove(testPath);

  return true;
}

// =====================================================
// SETUP SD
// =====================================================
void setupSD()
{
  Serial.println();
  Serial.println(
    "========================================"
  );
  Serial.println("Starting SD Card...");
  Serial.println(
    "========================================"
  );

  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("Starting SD...");

  // Enable board peripheral power.
  pinMode(
    BOARD_POWER_PIN,
    OUTPUT
  );

  digitalWrite(
    BOARD_POWER_PIN,
    HIGH
  );

  delay(1000);

  // Start custom SPI bus.
  sdSPI.begin(
    SD_SCK,
    SD_MISO,
    SD_MOSI,
    SD_CS
  );

  delay(500);

  // Start SD at low SPI frequency for reliability.
  if (!SD.begin(
      SD_CS,
      sdSPI,
      1000000
  ))
  {
    Serial.println(
      "SD CARD MOUNT FAILED!"
    );

    lcd.clear();

    lcd.setCursor(0, 0);
    lcd.print("SD Mount Error");

    sdAvailable = false;

    delay(1500);

    return;
  }

  uint8_t cardType =
    SD.cardType();

  if (cardType == CARD_NONE)
  {
    Serial.println(
      "No SD card detected."
    );

    lcd.clear();

    lcd.setCursor(0, 0);
    lcd.print("No SD Card");

    sdAvailable = false;

    delay(1500);

    return;
  }

  Serial.println(
    "SD CARD FOUND!"
  );

  Serial.print(
    "SD Card Type: "
  );

  if (cardType == CARD_MMC)
  {
    Serial.println("MMC");
  }
  else if (cardType == CARD_SD)
  {
    Serial.println("SDSC");
  }
  else if (cardType == CARD_SDHC)
  {
    Serial.println("SDHC");
  }
  else
  {
    Serial.println("UNKNOWN");
  }

  uint64_t cardSizeMB =
    SD.cardSize() /
    (1024ULL * 1024ULL);

  Serial.print(
    "SD Card Size: "
  );

  Serial.print(cardSizeMB);

  Serial.println(" MB");

  // Verify card is writable.
  if (!testSDWrite())
  {
    Serial.println(
      "SD card mounted but WRITE TEST FAILED."
    );

    Serial.println(
      "Check FAT32 formatting/card condition."
    );

    lcd.clear();

    lcd.setCursor(0, 0);
    lcd.print("SD Write Error");

    lcd.setCursor(0, 1);
    lcd.print("Check FAT32");

    sdAvailable = false;

    delay(2000);

    return;
  }

  // Create new file for each boot.
  sdFileName =
    createNewFileName();

  Serial.print(
    "New data file: "
  );

  Serial.println(
    sdFileName
  );

  File dataFile =
    SD.open(
      sdFileName.c_str(),
      FILE_WRITE
    );

  if (!dataFile)
  {
    Serial.println(
      "ERROR: Failed to create CSV file!"
    );

    lcd.clear();

    lcd.setCursor(0, 0);
    lcd.print("CSV Create Error");

    sdAvailable = false;

    delay(1500);

    return;
  }

  // Excel-compatible CSV header.
  dataFile.println(
    "Timestamp,Voltage,Current,PowerFactor,ApparentPower,RealPower"
  );

  dataFile.flush();
  dataFile.close();

  if (!SD.exists(
      sdFileName.c_str()
  ))
  {
    Serial.println(
      "ERROR: CSV verification failed!"
    );

    lcd.clear();

    lcd.setCursor(0, 0);
    lcd.print("CSV Verify Err");

    sdAvailable = false;

    delay(1500);

    return;
  }

  sdAvailable = true;

  Serial.println(
    "CSV file created successfully!"
  );

  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("SD Card Ready");

  lcd.setCursor(0, 1);
  lcd.print(
    sdFileName.substring(1)
  );

  delay(1500);
}

// =====================================================
// SAVE DATA TO SD
// =====================================================
void saveToSD(
  const String& timestamp,
  float voltageValue,
  float currentValue,
  float powerFactorValue,
  float apparentPowerValue,
  float realPowerValue
)
{
  if (!sdAvailable)
  {
    return;
  }

  File dataFile =
    SD.open(
      sdFileName.c_str(),
      FILE_APPEND
    );

  if (!dataFile)
  {
    Serial.println(
      "ERROR: Cannot append to SD file!"
    );

    return;
  }

  dataFile.print(timestamp);
  dataFile.print(",");

  dataFile.print(
    voltageValue,
    2
  );

  dataFile.print(",");

  dataFile.print(
    currentValue,
    2
  );

  dataFile.print(",");

  dataFile.print(
    powerFactorValue,
    2
  );

  dataFile.print(",");

  dataFile.print(
    apparentPowerValue,
    2
  );

  dataFile.print(",");

  dataFile.println(
    realPowerValue,
    2
  );

  // Flush data immediately.
  dataFile.flush();

  // Close after every row so previously written
  // data remains safer after sudden power loss.
  dataFile.close();

  Serial.print(
    "SD SAVED -> "
  );

  Serial.println(
    sdFileName
  );
}

// =====================================================
// BUZZER BASIC CONTROL
// =====================================================
void setBuzzer(bool turnOn)
{
  buzzerActive = turnOn;

  digitalWrite(
    BUZZER_PIN,
    turnOn
      ? BUZZER_ON_LEVEL
      : BUZZER_OFF_LEVEL
  );
}

// =====================================================
// CHECK IF FIREBASE STATUS IS AN ALARM
// =====================================================
bool isAlarmStatus(const String& status)
{
  if (
    status == "Warning / High Current" ||
    status == "Over Current" ||
    status == "Severe Overcurrent" ||
    status == "Possible Short Circuit"
  )
  {
    return true;
  }

  return false;
}

// =====================================================
// UPDATE BUZZER USING FIREBASE STATUS
// =====================================================
void updateBuzzerFromStatus(
  const String& status
)
{
  // Normal always turns the buzzer OFF.
  if (status == "Normal")
  {
    if (buzzerActive)
    {
      setBuzzer(false);

      Serial.println();
      Serial.println(
        "=============================="
      );
      Serial.println(
        "STATUS NORMAL"
      );
      Serial.println(
        "BUZZER OFF"
      );
      Serial.println(
        "=============================="
      );
    }

    return;
  }

  // All warning states turn the buzzer ON.
  if (isAlarmStatus(status))
  {
    if (!buzzerActive)
    {
      setBuzzer(true);

      Serial.println();
      Serial.println(
        "=============================="
      );
      Serial.println(
        "!!! CURRENT WARNING !!!"
      );

      Serial.print(
        "Firebase Status: "
      );

      Serial.println(status);

      Serial.println(
        "BUZZER ON"
      );
      Serial.println(
        "=============================="
      );
    }

    return;
  }

  // If Firebase returned Unknown/null/error,
  // keep the previous buzzer state.
  //
  // This prevents a temporary network problem
  // from incorrectly clearing an active alarm.
}

// =====================================================
// READ STATUS FROM FIREBASE
// =====================================================
String readStatusFromFirebase()
{
  if (
    WiFi.status() !=
    WL_CONNECTED
  )
  {
    Serial.println(
      "Status read skipped: WiFi offline."
    );

    return firebaseStatus;
  }

  String firebaseURL =
    String(FIREBASE_DATABASE_URL) +
    String(FIREBASE_STATUS_PATH);

  WiFiClientSecure client;

  // Prototype only:
  // skips HTTPS certificate verification.
  client.setInsecure();

  HTTPClient https;

  if (!https.begin(
      client,
      firebaseURL
  ))
  {
    Serial.println(
      "Firebase status HTTPS begin failed."
    );

    return firebaseStatus;
  }

  int httpResponseCode =
    https.GET();

  if (httpResponseCode == 200)
  {
    String payload =
      https.getString();

    payload.trim();

    https.end();

    // Firebase returns null when status
    // does not exist.
    if (
      payload.length() == 0 ||
      payload == "null"
    )
    {
      Serial.println(
        "Firebase status is empty."
      );

      return firebaseStatus;
    }

    // Firebase REST API returns:
    //
    // "Warning / High Current"
    //
    // Remove the surrounding quotes.
    if (
      payload.startsWith("\"") &&
      payload.endsWith("\"") &&
      payload.length() >= 2
    )
    {
      payload =
        payload.substring(
          1,
          payload.length() - 1
        );
    }

    return payload;
  }

  Serial.print(
    "Firebase status HTTP error: "
  );

  Serial.println(
    httpResponseCode
  );

  if (httpResponseCode > 0)
  {
    Serial.print(
      "Firebase response: "
    );

    Serial.println(
      https.getString()
    );
  }

  https.end();

  // Keep last known status if
  // Firebase cannot be reached.
  return firebaseStatus;
}

// =====================================================
// CHECK FIREBASE STATUS
// =====================================================
void checkFirebaseStatus()
{
  if (
    millis() - lastStatusCheck <
    STATUS_CHECK_INTERVAL
  )
  {
    return;
  }

  lastStatusCheck = millis();

  String newStatus =
    readStatusFromFirebase();

  // Only print when status changes.
  if (newStatus != firebaseStatus)
  {
    Serial.println();
    Serial.println(
      "------------------------------"
    );

    Serial.print(
      "Firebase Status Changed: "
    );

    Serial.print(firebaseStatus);

    Serial.print(" -> ");

    Serial.println(newStatus);

    Serial.println(
      "------------------------------"
    );

    firebaseStatus =
      newStatus;
  }

  updateBuzzerFromStatus(
    firebaseStatus
  );
}

// =====================================================
// LCD
// =====================================================
void updateLCD(
  float currentValue,
  float apparentPowerValue,
  float realPowerValue
)
{
  // ===================================================
  // ALARM DISPLAY
  // ===================================================
  if (buzzerActive)
  {
    lcd.setCursor(0, 0);

    if (
      firebaseStatus ==
      "Warning / High Current"
    )
    {
      lcd.print(
        "WARNING CURRENT "
      );
    }

    else if (
      firebaseStatus ==
      "Over Current"
    )
    {
      lcd.print(
        "OVER CURRENT!   "
      );
    }

    else if (
      firebaseStatus ==
      "Severe Overcurrent"
    )
    {
      lcd.print(
        "SEVERE CURRENT! "
      );
    }

    else if (
      firebaseStatus ==
      "Possible Short Circuit"
    )
    {
      lcd.print(
        "SHORT CIRCUIT!  "
      );
    }

    else
    {
      lcd.print(
        "CURRENT ALERT!  "
      );
    }

    lcd.setCursor(0, 1);

    lcd.print("I:");
    lcd.print(
      currentValue,
      2
    );

    lcd.print("A");

    // Clear remaining characters.
    lcd.print("         ");

    return;
  }

  // ===================================================
  // NORMAL LCD PAGE ROTATION
  // ===================================================
  if (
    millis() - lastLCDPageChange >=
    LCD_PAGE_INTERVAL
  )
  {
    lastLCDPageChange =
      millis();

    lcdPage++;

    if (lcdPage > 2)
    {
      lcdPage = 0;
    }

    lcd.clear();
  }

  // ===================================================
  // PAGE 1
  // Voltage + Current
  // ===================================================
  if (lcdPage == 0)
  {
    lcd.setCursor(0, 0);

    lcd.print("V:");
    lcd.print(
      voltage,
      2
    );
    lcd.print("V     ");

    lcd.setCursor(0, 1);

    lcd.print("I:");
    lcd.print(
      currentValue,
      2
    );
    lcd.print("A     ");
  }

  // ===================================================
  // PAGE 2
  // Real + Apparent Power
  // ===================================================
  else if (lcdPage == 1)
  {
    lcd.setCursor(0, 0);

    lcd.print("Real:");
    lcd.print(
      realPowerValue,
      1
    );
    lcd.print("W   ");

    lcd.setCursor(0, 1);

    lcd.print("App:");
    lcd.print(
      apparentPowerValue,
      1
    );
    lcd.print("VA  ");
  }

  // ===================================================
  // PAGE 3
  // Power Factor + Time
  // ===================================================
  else
  {
    lcd.setCursor(0, 0);

    lcd.print("PF:");
    lcd.print(
      powerFactor,
      2
    );
    lcd.print("          ");

    lcd.setCursor(0, 1);

    lcd.print("Time:");
    lcd.print(
      getTimeOnly()
    );
    lcd.print("   ");
  }
}

// =====================================================
// SEND SENSOR DATA TO FIREBASE
// =====================================================
bool sendToFirebase(
  float currentValue,
  float apparentPowerValue,
  float realPowerValue,
  float powerFactorValue,
  float voltageValue,
  const String& timestamp
)
{
  if (
    WiFi.status() !=
    WL_CONNECTED
  )
  {
    Serial.println(
      "Firebase skipped: WiFi offline."
    );

    return false;
  }

  String firebaseURL =
    String(FIREBASE_DATABASE_URL) +
    String(FIREBASE_PATH);

  String json;

  json.reserve(220);

  json = "{";

  json += "\"apparentPower\":";
  json += String(
    apparentPowerValue,
    2
  );

  json += ",\"current\":";
  json += String(
    currentValue,
    2
  );

  json += ",\"powerFactor\":";
  json += String(
    powerFactorValue,
    2
  );

  json += ",\"realPower\":";
  json += String(
    realPowerValue,
    2
  );

  json += ",\"timestamp\":\"";
  json += timestamp;
  json += "\"";

  json += ",\"voltage\":";
  json += String(
    voltageValue,
    2
  );

  json += "}";

  WiFiClientSecure client;

  // Prototype only:
  // skips server certificate verification.
  client.setInsecure();

  HTTPClient https;

  if (!https.begin(
      client,
      firebaseURL
  ))
  {
    Serial.println(
      "Firebase HTTPS begin failed."
    );

    return false;
  }

  https.addHeader(
    "Content-Type",
    "application/json"
  );

  // ===================================================
  // IMPORTANT:
  //
  // PATCH instead of PUT.
  //
  // PUT would replace the entire /current object and
  // could delete the "status" field written by the app.
  //
  // PATCH only updates these sensor fields and leaves
  // /current/status untouched.
  // ===================================================
  int httpResponseCode =
    https.sendRequest(
      "PATCH",
      json
    );

  bool success = false;

  if (
    httpResponseCode == 200 ||
    httpResponseCode == 204
  )
  {
    Serial.println(
      "Firebase SENSOR UPDATE SUCCESS!"
    );

    success = true;
  }
  else
  {
    Serial.print(
      "Firebase HTTP: "
    );

    Serial.println(
      httpResponseCode
    );

    if (httpResponseCode > 0)
    {
      Serial.print(
        "Firebase response: "
      );

      Serial.println(
        https.getString()
      );
    }
  }

  https.end();

  return success;
}

// =====================================================
// SETUP
// =====================================================
void setup()
{
  Serial.begin(115200);

  delay(1500);

  Serial.println();
  Serial.println(
    "========================================"
  );
  Serial.println(
    "CURRENTRAX ENERGY MONITOR"
  );
  Serial.println(
    "========================================"
  );

  // ===================================================
  // BOARD / PERIPHERAL POWER
  // ===================================================
  pinMode(
    BOARD_POWER_PIN,
    OUTPUT
  );

  digitalWrite(
    BOARD_POWER_PIN,
    HIGH
  );

  // ===================================================
  // BUZZER
  // ===================================================
  pinMode(
    BUZZER_PIN,
    OUTPUT
  );

  // Make sure buzzer starts OFF.
  setBuzzer(false);

  delay(500);

  // ===================================================
  // I2C
  // ===================================================
  Wire.begin(
    SDA_PIN,
    SCL_PIN
  );

  Wire.setClock(100000);

  // ===================================================
  // LCD
  // ===================================================
  lcd.init();

  lcd.backlight();

  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("CurrentRax");

  lcd.setCursor(0, 1);
  lcd.print("Starting...");

  delay(1000);

  // ===================================================
  // WIFI
  // ===================================================
  connectWiFiInitial();

  // ===================================================
  // TIME
  // ===================================================
  if (
    WiFi.status() ==
    WL_CONNECTED
  )
  {
    setupTime();
  }

  // ===================================================
  // ADS1115
  // ===================================================
  Serial.println(
    "Starting ADS1115..."
  );

  if (!ads.begin(
      ADS1115_ADDRESS,
      &Wire
  ))
  {
    Serial.println(
      "ADS1115 NOT FOUND!"
    );

    lcd.clear();

    lcd.setCursor(0, 0);
    lcd.print(
      "ADS1115 ERROR"
    );

    lcd.setCursor(0, 1);
    lcd.print(
      "Check Wiring"
    );

    while (true)
    {
      delay(1000);
    }
  }

  ads.setGain(
    GAIN_ONE
  );

  ads.setDataRate(
    RATE_ADS1115_860SPS
  );

  Serial.println(
    "ADS1115 FOUND!"
  );

  // ===================================================
  // SD CARD
  // ===================================================
  setupSD();

  // ===================================================
  // READ INITIAL FIREBASE STATUS
  // ===================================================
  if (
    WiFi.status() ==
    WL_CONNECTED
  )
  {
    Serial.println();
    Serial.println(
      "Reading initial Firebase status..."
    );

    firebaseStatus =
      readStatusFromFirebase();

    Serial.print(
      "Initial Status: "
    );

    Serial.println(
      firebaseStatus
    );

    updateBuzzerFromStatus(
      firebaseStatus
    );
  }

  // ===================================================
  // READY
  // ===================================================
  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print(
    "System Ready!"
  );

  lcd.setCursor(0, 1);

  if (sdAvailable)
  {
    lcd.print(
      "SD Logging ON"
    );
  }
  else
  {
    lcd.print(
      "SD Logging OFF"
    );
  }

  Serial.println();
  Serial.println(
    "SYSTEM READY"
  );

  delay(1500);

  lcd.clear();
}

// =====================================================
// LOOP
// =====================================================
void loop()
{
  // ===================================================
  // WIFI
  // ===================================================
  maintainWiFi();

  ensureTimeConfigured();

  // ===================================================
  // CURRENT SAMPLING
  // ===================================================
  double sum = 0.0;
  double sumSquares = 0.0;

  for (int i = 0; i < SAMPLES; i++)
  {
    int16_t rawValue =
      ads.readADC_Differential_0_1();

    double ctVoltage =
      (double)rawValue *
      ADS_LSB;

    sum +=
      ctVoltage;

    sumSquares +=
      ctVoltage *
      ctVoltage;
  }

  // ===================================================
  // RMS
  // ===================================================
  double meanVoltage =
    sum /
    SAMPLES;

  double meanSquare =
    sumSquares /
    SAMPLES;

  double rmsSquared =
    meanSquare -
    (
      meanVoltage *
      meanVoltage
    );

  if (rmsSquared < 0)
  {
    rmsSquared = 0;
  }

  float ctRmsVoltage =
    sqrt(
      rmsSquared
    );

  // ===================================================
  // CURRENT
  // ===================================================
  float secondaryCurrent =
    ctRmsVoltage /
    BURDEN_RESISTOR;

  float current =
    secondaryCurrent *
    CT_RATIO;

  // Apply calibration.
  current *=
    CALIBRATION_FACTOR;

  // Noise filter.
  if (
    current <
    NOISE_THRESHOLD
  )
  {
    current = 0.0;
  }

  // ===================================================
  // ROUND CURRENT DOWN TO 2 DECIMAL PLACES
  //
  // 8.239 -> 8.23
  // 8.231 -> 8.23
  // ===================================================
  current =
    floor(
      current * 100.0
    ) /
    100.0;

  // ===================================================
  // POWER
  // ===================================================
  float apparentPower =
    voltage *
    current;

  float realPower =
    apparentPower *
    powerFactor;

  // ===================================================
  // TIMESTAMP
  // ===================================================
  String timestamp =
    getTimestamp();

  // ===================================================
  // READ FIREBASE STATUS
  //
  // This controls the buzzer.
  // ===================================================
  checkFirebaseStatus();

  // ===================================================
  // LCD
  // ===================================================
  updateLCD(
    current,
    apparentPower,
    realPower
  );

  // ===================================================
  // SERIAL MONITOR
  // ===================================================
  Serial.println();
  Serial.println(
    "========================================"
  );

  Serial.print(
    "Timestamp       : "
  );
  Serial.println(
    timestamp
  );

  Serial.print(
    "Voltage         : "
  );
  Serial.print(
    voltage,
    2
  );
  Serial.println(
    " V"
  );

  Serial.print(
    "Current         : "
  );
  Serial.print(
    current,
    2
  );
  Serial.println(
    " A"
  );

  Serial.print(
    "Firebase Status : "
  );
  Serial.println(
    firebaseStatus
  );

  Serial.print(
    "Buzzer          : "
  );
  Serial.println(
    buzzerActive
      ? "ON"
      : "OFF"
  );

  Serial.print(
    "Power Factor    : "
  );
  Serial.println(
    powerFactor,
    2
  );

  Serial.print(
    "Apparent Power  : "
  );
  Serial.print(
    apparentPower,
    2
  );
  Serial.println(
    " VA"
  );

  Serial.print(
    "Real Power      : "
  );
  Serial.print(
    realPower,
    2
  );
  Serial.println(
    " W"
  );

  Serial.print(
    "SD File         : "
  );

  if (sdAvailable)
  {
    Serial.println(
      sdFileName
    );
  }
  else
  {
    Serial.println(
      "NOT AVAILABLE"
    );
  }

  Serial.println(
    "========================================"
  );

  // ===================================================
  // SAVE TO SD
  // ===================================================
  if (
    millis() - lastSDSave >=
    SD_SAVE_INTERVAL
  )
  {
    lastSDSave =
      millis();

    saveToSD(
      timestamp,
      voltage,
      current,
      powerFactor,
      apparentPower,
      realPower
    );
  }

  // ===================================================
  // SEND SENSOR DATA TO FIREBASE
  // ===================================================
  if (
    millis() - lastFirebaseUpdate >=
    FIREBASE_INTERVAL
  )
  {
    lastFirebaseUpdate =
      millis();

    sendToFirebase(
      current,
      apparentPower,
      realPower,
      powerFactor,
      voltage,
      timestamp
    );
  }
}
