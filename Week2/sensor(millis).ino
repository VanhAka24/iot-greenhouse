#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "DHT.h"

#define DHTPIN 14
#define DHTTYPE DHT22

DHT dht(DHTPIN, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);

unsigned long previousMillis = 0;
const long interval = 2000;

void setup() {
  Serial.begin(115200);
  dht.begin();
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("He thong IoT");

  Serial.println("\nKiểm tra ADC & I2C");
  int rawADC = analogRead(34);
  float voltage = (rawADC / 4095.0) * 3.3;
  Serial.print("ADC Raw (0-4095): ");
  Serial.println(rawADC);
  Serial.print("Dien ap: ");
  Serial.print(voltage);
  Serial.println(" V");

  Wire.begin(21, 22);
  Serial.println("Quet bus I2C...");
  byte count = 0;
  for (byte address = 1; address < 127; address++) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      Serial.print("-> Tim thay thiet bi I2C tai dia chi: 0x");
      if (address < 16) Serial.print("0");
      Serial.println(address, HEX);
      count++;
    }
  }
  if (count == 0) Serial.println("Khong tim thay thiet bi I2C!");
  Serial.println("==========================\n");

  Serial.println("timestamp,raw_value,converted_value,unit,valid");
}

void loop() {
  if (Serial.available() > 0) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') {
      Serial.print("-> [");
      Serial.print(millis());
      Serial.println("] Có phản hồi nút bấm");
    }
  }

  unsigned long currentMillis = millis();
  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;

    float t = dht.readTemperature();
    int valid = !isnan(t);

    if (valid) {
      lcd.setCursor(0, 1);
      lcd.print("Nhiet do: ");
      lcd.print(t, 1);
      lcd.print("C  ");
    } else {
      lcd.setCursor(0, 1);
      lcd.print("Loi cam bien!   ");
    }

    Serial.print(millis());
    Serial.print(",");
    Serial.print(t);
    Serial.print(",");
    Serial.print(t);
    Serial.print(",");
    Serial.print("C");
    Serial.print(",");
    Serial.println(valid);
  }
}