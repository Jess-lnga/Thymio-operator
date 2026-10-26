#pragma once
#include <Arduino.h>

namespace arm_sequence {
// Etat prive de la tache moteur (core 1). Pour les autres taches, utiliser
// motor_runtime : aucune fonction de ce module ne doit etre appelee du web.
// Positions et pas en microsecondes ; pause en millisecondes.
enum Setting : uint8_t {
  HOME,
  MIDDLE,
  OUT,
  OPEN,
  CLOSED,
  HOME_MIDDLE,
  MIDDLE_OUT,
  OUT_MIDDLE,
  MIDDLE_HOME,
  GRIP_OPEN,
  GRIP_CLOSE,
  SETTLE,
  COUNT
};
struct Settings {
  uint16_t value[COUNT];
};
void begin();
void update();
bool busy();
bool fault();
bool canCatch();
bool canRelease();
bool catchObject();
bool releaseObject();
bool manual(uint8_t motor, uint16_t us);
const char* phase();
const Settings& settings();
bool save(const Settings& candidate);
uint16_t position(uint8_t motor);
}  // namespace arm_sequence
