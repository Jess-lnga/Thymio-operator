#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "interface_wifi.h"
#include "motor_runtime.h"

#if CONFIG_FREERTOS_UNICORE
#error "Ce firmware requiert un ESP32 a deux coeurs."
#endif

namespace {
void motorTask(void*) {
  Serial.printf("Tache moteurs : core %d\n", xPortGetCoreID());
  motor_runtime::begin();
  TickType_t lastWake = xTaskGetTickCount();
  for (;;) {
    motor_runtime::update();
    // FSM mise a jour frequemment ; les consignes progressent toutes les 20 ms.
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(2));
  }
}
void webTask(void*) {
  Serial.printf("Tache interface : core %d\n", xPortGetCoreID());
  while (!motor_runtime::get_state().ready) vTaskDelay(pdMS_TO_TICKS(10));
  if (!interface_wifi::begin()) {
    Serial.println("Erreur de demarrage Wi-Fi");
    vTaskDelete(nullptr);
    return;
  }
  for (;;) {
    interface_wifi::update();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}
}  // namespace
void setup() {
  Serial.begin(115200);
  // Les initialisations PWM et Wi-Fi ont lieu dans leurs taches respectives.
  if (xTaskCreatePinnedToCore(motorTask, "arm-motors", 6144, nullptr, 2,
                              nullptr, 1) != pdPASS) {
    Serial.println("Impossible de creer la tache moteurs");
    return;
  }
  if (xTaskCreatePinnedToCore(webTask, "arm-web", 8192, nullptr, 1, nullptr,
                              0) != pdPASS) {
    Serial.println("Impossible de creer la tache interface");
  }
}
void loop() {
  // La tache Arduino n'execute aucune logique applicative.
  vTaskDelay(pdMS_TO_TICKS(1000));
}
