#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

#include <SPI.h>
#include <SD.h>

// =====================================
// LCD I2C
// =====================================
#define SDA_PIN 21
#define SCL_PIN 22

LiquidCrystal_I2C lcd(0x27, 16, 2);

// =====================================
// SD CARD PINS
// =====================================
#define SD_SCK   14
#define SD_MISO  2
#define SD_MOSI  15
#define SD_CS    13

SPIClass sdSPI(VSPI);

// =====================================
// TEMPORARY TEST DATA
// =====================================
float current = 0.0;
float power = 0.0;

float voltage = 230.0;

// Used to increase/decrease current
float increment = 0.25;

// =====================================
// SAVE DATA TO SD
// =====================================
void saveToSD(float currentValue, float powerValue) {

  File file = SD.open("/energy_data.csv", FILE_APPEND);

  if (!file) {
    Serial.println("ERROR: Cannot open energy_data.csv");
    return;
  }

  // milliseconds
  file.print(millis());
  file.print(",");

  file.print(currentValue, 2);
  file.print(",");

  file.println(powerValue, 2);

  file.close();

  Serial.println("Data saved to SD card.");
}

// =====================================
// SETUP
// =====================================
void setup() {

  Serial.begin(115200);

  delay(1000);

  Serial.println();
  Serial.println("===============================");
  Serial.println("LILYGO LCD + SD TEST");
  Serial.println("===============================");

  // =====================================
  // START LCD
  // =====================================
  Wire.begin(SDA_PIN, SCL_PIN);

  lcd.init();
  lcd.backlight();

  lcd.setCursor(0, 0);
  lcd.print("LILYGO A7670E");

  lcd.setCursor(0, 1);
  lcd.print("Starting...");

  delay(2000);

  // =====================================
  // START SD CARD
  // =====================================
  Serial.println("Starting SD Card...");

  sdSPI.begin(
    SD_SCK,
    SD_MISO,
    SD_MOSI,
    SD_CS
  );

  if (!SD.begin(SD_CS, sdSPI, 1000000)) {

    Serial.println("SD Card initialization FAILED!");

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("SD CARD ERROR");

    lcd.setCursor(0, 1);
    lcd.print("Check wiring");

    delay(3000);

  } else {

    Serial.println("SD Card initialized successfully!");

    lcd.clear();

    lcd.setCursor(0, 0);
    lcd.print("SD CARD OK");

    lcd.setCursor(0, 1);
    lcd.print("LCD OK");

    // =====================================
    // CREATE CSV HEADER
    // =====================================

    if (!SD.exists("/energy_data.csv")) {

      File file = SD.open("/energy_data.csv", FILE_WRITE);

      if (file) {

        file.println("Time_ms,Current_A,Power_W");

        file.close();

        Serial.println("CSV file created.");
      }
    }

    delay(2000);
  }

  lcd.clear();
}

// =====================================
// LOOP
// =====================================
void loop() {

  // =====================================
  // GENERATE TEMPORARY CURRENT
  // =====================================

  current += increment;

  // Maximum test current = 10A
  if (current >= 10.0) {

    current = 10.0;

    // Start decreasing
    increment = -0.25;
  }

  // Minimum = 0A
  if (current <= 0.0) {

    current = 0.0;

    // Start increasing
    increment = 0.25;
  }


  // =====================================
  // CALCULATE TEMPORARY POWER
  // =====================================

  power = voltage * current;


  // =====================================
  // SERIAL MONITOR
  // =====================================

  Serial.print("Current: ");
  Serial.print(current, 2);
  Serial.print(" A");

  Serial.print(" | Power: ");
  Serial.print(power, 2);
  Serial.println(" W");


  // =====================================
  // LCD DISPLAY
  // =====================================

  lcd.setCursor(0, 0);

  lcd.print("I:");
  lcd.print(current, 2);
  lcd.print(" A      ");


  lcd.setCursor(0, 1);

  lcd.print("P:");
  lcd.print(power, 1);
  lcd.print(" W      ");


  // =====================================
  // SAVE TO SD CARD
  // =====================================

  saveToSD(current, power);


  // New data every 2 seconds
  delay(2000);
}