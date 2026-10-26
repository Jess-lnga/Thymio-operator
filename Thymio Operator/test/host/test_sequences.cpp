#include <Preferences.h>

#include <cassert>
#include <iostream>
#include <string>

#include "arm_sequence.h"
#include "motor_ctrl.h"

uint32_t fakeMillis = 0;
std::vector<uint8_t> Preferences::bytes;
bool Preferences::failWrite = false;
namespace motor_ctrl {
State motors[2] = {{4915, false}, {4915, false}};
bool failEnable = false;
void begin() {}
bool setTicks(uint8_t id, uint16_t ticks) {
  assert(id < 2 && ticks >= MIN_TICKS && ticks <= MAX_TICKS);
  motors[id].ticks = ticks;
  return true;
}
bool setEnabled(uint8_t id, bool enabled) {
  motors[id].enabled = enabled;
  return !failEnable;
}
const State& state(uint8_t id) { return motors[id]; }
}  // namespace motor_ctrl
namespace seq = arm_sequence;
void advance(uint32_t ms) {
  for (uint32_t i = 0; i < ms; i += 20) {
    fakeMillis += 20;
    seq::update();
  }
}
void finish() {
  for (int i = 0; i < 10000 && seq::busy(); ++i) advance(20);
  assert(!seq::busy());
}
bool phase(const char* text) {
  return std::string(seq::phase()).find(text) != std::string::npos;
}
void runCycle() {
  assert(seq::canCatch() && !seq::canRelease());
  assert(seq::catchObject());
  assert(!seq::catchObject() && !seq::releaseObject());
  assert(!seq::manual(0, 2000));
  assert(!seq::save(seq::settings()));
  bool openedBeforeOut = false, closedBeforeHome = false;
  for (int i = 0; i < 10000 && seq::busy(); ++i) {
    if (phase("HOME vers MIDDLE"))
      assert(seq::position(0) != seq::settings().value[seq::MIDDLE]);
    if (phase("Catch : retour MIDDLE"))
      assert(seq::position(0) != seq::settings().value[seq::MIDDLE]);
    // Pince fermee pendant HOME -> MIDDLE.
    if (phase("HOME vers MIDDLE"))
      assert(seq::position(1) == seq::settings().value[seq::CLOSED]);
    if (phase("vers OUT + ouverture") &&
        seq::position(1) == seq::settings().value[seq::OPEN] &&
        seq::position(0) > seq::settings().value[seq::OUT])
      openedBeforeOut = true;
    if (phase("fermeture de la pince"))
      assert(seq::position(0) == seq::settings().value[seq::OUT]);
    if (phase("retour MIDDLE"))
      assert(seq::position(1) == seq::settings().value[seq::CLOSED]);
    advance(20);
  }
  assert(openedBeforeOut && seq::canRelease() && !seq::canCatch());
  assert(seq::releaseObject());
  assert(!seq::catchObject() && !seq::releaseObject());
  for (int i = 0; i < 10000 && seq::busy(); ++i) {
    if (phase("Release : retour MIDDLE"))
      assert(seq::position(0) != seq::settings().value[seq::MIDDLE]);
    if (phase("retour HOME + fermeture"))
      assert(seq::position(0) != seq::settings().value[seq::HOME]);
    if (phase("Release : vers OUT"))
      assert(seq::position(1) == seq::settings().value[seq::CLOSED]);
    if (phase("Release : retour MIDDLE"))
      assert(seq::position(1) == seq::settings().value[seq::OPEN]);
    if (phase("retour HOME + fermeture") &&
        seq::position(1) == seq::settings().value[seq::CLOSED] &&
        seq::position(0) < seq::settings().value[seq::HOME])
      closedBeforeHome = true;
    advance(20);
  }
  assert(closedBeforeHome && seq::canCatch() && !seq::canRelease());
}
int main() {
  seq::begin();
  assert(seq::busy());
  assert(!seq::canCatch() && !seq::releaseObject());
  assert(seq::position(0) == 2400 && seq::position(1) == 2000);
  assert(motor_ctrl::state(0).enabled && motor_ctrl::state(1).enabled);
  advance(1980);
  assert(seq::busy());
  advance(20);
  assert(seq::canCatch());
  runCycle();
  assert(!seq::manual(2, 1500) && !seq::manual(0, 499) &&
         !seq::manual(1, 2501));
  assert(seq::manual(0, 500));
  assert(!seq::canCatch() && !seq::canRelease());
  assert(seq::manual(0, 2400));
  assert(seq::canCatch());
  auto candidate = seq::settings();
  candidate.value[seq::HOME] = 2300;
  assert(seq::settings().value[seq::HOME] == 2400);  // Brouillon sans Save.
  auto invalid = candidate;
  invalid.value[seq::OUT] = 2400;
  assert(!seq::save(invalid));
  invalid = candidate;
  invalid.value[seq::GRIP_OPEN] = 0;
  assert(!seq::save(invalid));
  invalid = candidate;
  invalid.value[seq::OPEN] = 2100;
  assert(!seq::save(invalid));
  Preferences::failWrite = true;
  assert(!seq::save(candidate));
  assert(seq::settings().value[seq::HOME] == 2400);
  Preferences::failWrite = false;
  assert(seq::save(candidate));
  assert(!seq::canCatch());
  seq::begin();
  finish();
  assert(seq::position(0) == 2300 &&
         seq::canCatch());  // Chargement persistant.
  // Pince tres lente : bras ralenti pour garder l'ouverture en avance.
  candidate = seq::settings();
  candidate.value[seq::GRIP_OPEN] = 1;
  candidate.value[seq::MIDDLE_OUT] = 100;
  assert(seq::save(candidate));
  runCycle();
  // Debordement de millis pendant une sequence.
  fakeMillis = 0xFFFFFF00u;
  runCycle();
  // Un reglage manuel, meme sans changement, invalide la detention supposee.
  assert(seq::catchObject());
  finish();
  assert(seq::canRelease());
  candidate = seq::settings();
  candidate.value[seq::GRIP_CLOSE] = 5;
  assert(seq::save(candidate));
  assert(seq::canRelease());  // Ajuster la vitesse ne perd pas l'objet suppose.
  assert(seq::manual(1, seq::position(1)));
  assert(!seq::canRelease());
  assert(seq::manual(0, seq::settings().value[seq::HOME]));
  assert(seq::canCatch());
  motor_ctrl::failEnable = true;
  seq::begin();
  assert(seq::fault());
  assert(!seq::canCatch());
  std::cout << "PASS: startup, Catch/Release order, early gripper completion, "
               "interlocks, manual mode, validation, Save/reboot, storage/PWM "
               "failures, millis rollover\n";
}
