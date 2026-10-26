#include <Arduino.h>

#include "motor_ctrl.h"
#include "interface_wifi.h"

void setup() {
  Serial.begin(115200);
  motor_ctrl::begin();
  if (!interface_wifi::begin()) Serial.println("Erreur Wi-Fi");
}

void loop() {
  interface_wifi::update();
  delay(2);
}

