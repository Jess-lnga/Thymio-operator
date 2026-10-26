#include "motor_ctrl.h"
#include <esp_arduino_version.h>

namespace motor_ctrl {
namespace {
constexpr uint8_t PINS[] = {25, 26};
State states[] = {{4915, false}, {4915, false}};
void write(uint8_t motor, uint16_t ticks) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PINS[motor], ticks);
#else
  ledcWrite(motor, ticks);
#endif
}
}
void begin() {
  for (uint8_t i = 0; i < MOTOR_COUNT; ++i) {
    pinMode(PINS[i], OUTPUT);
    digitalWrite(PINS[i], LOW);
  }
}
bool setTicks(uint8_t motor, uint16_t ticks) {
  if (motor >= MOTOR_COUNT || ticks < MIN_TICKS || ticks > MAX_TICKS) return false;
  states[motor].ticks = ticks;
  if (states[motor].enabled) write(motor, ticks);
  return true;
}
bool setEnabled(uint8_t motor, bool enabled) {
  if (motor >= MOTOR_COUNT) return false;
  if (states[motor].enabled == enabled) return true;
  if (enabled) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    if (!ledcAttach(PINS[motor], 50, 16)) return false;
#else
    if (ledcSetup(motor, 50, 16) == 0) return false;
    ledcAttachPin(PINS[motor], motor);
#endif
    write(motor, states[motor].ticks);
  } else {
    write(motor, 0);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcDetach(PINS[motor]);
#else
    ledcDetachPin(PINS[motor]);
#endif
    pinMode(PINS[motor], OUTPUT);
    digitalWrite(PINS[motor], LOW);
  }
  states[motor].enabled = enabled;
  return true;
}
const State& state(uint8_t motor) { return states[motor]; }
}
