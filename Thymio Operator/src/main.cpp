#include <Arduino.h>

#include "arm_sequence.h"
#include "interface_wifi.h"
#include "motor_ctrl.h"

void setup() {
  Serial.begin(115200);
  motor_ctrl::begin();
  arm_sequence::begin();
  if (!interface_wifi::begin()) Serial.println("Erreur Wi-Fi");
}

void loop() {
  arm_sequence::update();
  interface_wifi::update();
  delay(2);
}
