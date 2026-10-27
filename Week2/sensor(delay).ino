#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "DHT.h"

#define DHTPIN 14
#define DHTTYPE DHT22

DHT dht(DHTPIN, DHTTYPE);
LiquidCrystal_I2C lcd(0x27, 16, 2);

void setup() {
  Serial.begin(115200);
  dht.begin();
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("He thong IoT");
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

  delay(2000);
}