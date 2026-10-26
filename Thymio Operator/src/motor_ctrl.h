#pragma once
#include <Arduino.h>

namespace motor_ctrl {
constexpr uint8_t MOTOR_COUNT = 2;
// PWM 50 Hz / 16 bits : 1 tick = 20000 / 65536 us.
// Plage commune aux deux sorties : ~500-2500 us.
// MIN_TICKS arrondi vers le haut : 1639 ticks = ~500.18 us.
constexpr uint16_t MIN_TICKS = 1639;
constexpr uint16_t MAX_TICKS = 8192;
struct State { uint16_t ticks; bool enabled; };
void begin();
bool setTicks(uint8_t motor, uint16_t ticks);
bool setEnabled(uint8_t motor, bool enabled);
const State& state(uint8_t motor); // Index valide : 0 ou 1.
}
