#include "arm_sequence.h"

#include <Preferences.h>

#include "motor_ctrl.h"

namespace arm_sequence {
namespace {
constexpr Settings DEFAULTS = {
    {2400, 2030, 1440, 1530, 2000, 10, 8, 8, 10, 10, 10, 300, 2000}};
Settings config = DEFAULTS;
enum class Phase {
  BOOT,
  IDLE,
  CATCH_HOME_MIDDLE,
  CATCH_OUT_OPEN,
  CATCH_CLOSE,
  CATCH_RETURN,
  RELEASE_OUT,
  RELEASE_OPEN,
  RELEASE_MIDDLE,
  RELEASE_HOME_CLOSE,
  FAULT
};
Phase current = Phase::BOOT;
bool holding = false;
uint16_t pos[2] = {2400, 2000};
uint16_t startPos[2], target[2];
uint32_t duration[2], started, total;
constexpr uint32_t VERSION = 2;
struct Stored {
  uint32_t version;
  Settings settings;
};
// Ancien format conserve pour ne pas perdre les calibrations deja sauvegardees.
struct LegacyStored {
  uint32_t version;
  uint16_t value[12];
};

bool valid(const Settings& c) {
  for (uint8_t i = HOME; i <= CLOSED; ++i)
    if (c.value[i] < 500 || c.value[i] > 2500) return false;
  if (!(c.value[OUT] < c.value[MIDDLE] && c.value[MIDDLE] < c.value[HOME]) ||
      c.value[OPEN] >= c.value[CLOSED])
    return false;
  for (uint8_t i = HOME_MIDDLE; i <= GRIP_CLOSE; ++i)
    if (c.value[i] < 1 || c.value[i] > 100) return false;
  return c.value[SETTLE] >= 100 && c.value[SETTLE] <= 3000 &&
         c.value[LOOP_PAUSE] <= 60000;
}
void write(uint8_t i, uint16_t us) {
  pos[i] = us;
  // Arrondi au tick le plus proche, sans descendre sous la limite 500 us.
  uint16_t ticks = (uint32_t(us) * 65536 + 10000) / 20000;
  if (ticks < motor_ctrl::MIN_TICKS) ticks = motor_ctrl::MIN_TICKS;
  if (!motor_ctrl::setTicks(i, ticks)) current = Phase::FAULT;
}
uint32_t travel(uint16_t a, uint16_t b, uint16_t step) {
  return ((uint32_t(abs(int(a) - int(b))) + step - 1) / step) * 20;
}
void move(Phase p, uint16_t arm, uint16_t grip, Setting armStep,
          Setting gripStep, bool earlyGrip = false) {
  current = p;
  startPos[0] = pos[0];
  startPos[1] = pos[1];
  target[0] = arm;
  target[1] = grip;
  duration[0] = travel(pos[0], arm, config.value[armStep]);
  duration[1] = travel(pos[1], grip, config.value[gripStep]);
  // La pince finit au plus tard a 80% du trajet du bras.
  // Ralentir le bras si le pas de pince demande plus de temps.
  if (earlyGrip) duration[0] = max(duration[0], (duration[1] * 5 + 3) / 4);
  total = max(duration[0], duration[1]);
  started = millis();
}
}  // namespace
const Settings& settings() { return config; }
uint16_t position(uint8_t motor) { return motor < 2 ? pos[motor] : 0; }
bool fault() { return current == Phase::FAULT; }
bool busy() { return current != Phase::IDLE; }
bool canCatch() {
  return !busy() && !holding && pos[0] == config.value[HOME] &&
         pos[1] == config.value[CLOSED];
}
bool canRelease() {
  return !busy() && holding && pos[0] == config.value[MIDDLE] &&
         pos[1] == config.value[CLOSED];
}
void begin() {
  config = DEFAULTS;
  Preferences prefs;
  if (prefs.begin("thymio-arm", true)) {
    Stored stored{};
    if (prefs.getBytesLength("settings") == sizeof(stored) &&
        prefs.getBytes("settings", &stored, sizeof(stored)) == sizeof(stored) &&
        stored.version == VERSION && valid(stored.settings))
      config = stored.settings;
    else if (prefs.getBytesLength("settings") == sizeof(LegacyStored)) {
      LegacyStored legacy{};
      Settings migrated = DEFAULTS;
      if (prefs.getBytes("settings", &legacy, sizeof(legacy)) ==
              sizeof(legacy) &&
          legacy.version == 1) {
        for (uint8_t i = 0; i < 12; ++i) migrated.value[i] = legacy.value[i];
        if (valid(migrated)) config = migrated;
      }
    }
    prefs.end();
  }
  current = Phase::BOOT;
  holding = false;
  write(0, config.value[HOME]);
  write(1, config.value[CLOSED]);
  // Toujours actives ; aucun bouton OFF dans l'interface.
  const bool armOk = motor_ctrl::setEnabled(0, true);
  const bool gripOk = motor_ctrl::setEnabled(1, true);
  if (!armOk || !gripOk) current = Phase::FAULT;
  started = millis();
}
bool catchObject() {
  if (!canCatch()) return false;
  move(Phase::CATCH_HOME_MIDDLE, config.value[MIDDLE], config.value[CLOSED],
       HOME_MIDDLE, GRIP_CLOSE);
  return true;
}
bool releaseObject() {
  if (!canRelease()) return false;
  move(Phase::RELEASE_OUT, config.value[OUT], config.value[CLOSED], MIDDLE_OUT,
       GRIP_CLOSE);
  return true;
}
bool manual(uint8_t motor, uint16_t us) {
  if (busy() || motor >= 2 || us < 500 || us > 2500) return false;
  // Un mouvement manuel invalide le resultat du dernier Catch.
  holding = false;
  write(motor, us);
  return !fault();
}
bool save(const Settings& candidate) {
  if (busy() || !valid(candidate)) return false;
  Preferences prefs;
  if (!prefs.begin("thymio-arm", false)) return false;
  Stored stored{VERSION, candidate};
  bool ok =
      prefs.putBytes("settings", &stored, sizeof(stored)) == sizeof(stored);
  prefs.end();
  if (ok) {
    config = candidate;
  }
  return ok;
}
void update() {
  if (current == Phase::FAULT || current == Phase::IDLE) return;
  const uint32_t elapsed = millis() - started;
  if (current == Phase::BOOT) {
    if (elapsed >= 2000) current = Phase::IDLE;
    return;
  }
  static uint32_t last = 0;
  if (millis() - last < 20) return;
  last = millis();
  for (uint8_t i = 0; i < 2; ++i) {
    uint32_t t = min(elapsed, duration[i]);
    int32_t delta = int32_t(target[i]) - startPos[i];
    write(i, duration[i]
                 ? startPos[i] + delta * int32_t(t) / int32_t(duration[i])
                 : target[i]);
  }
  // MIDDLE est un point de passage, pas un arret. En fin de sequence,
  // rendre aussi la commande suivante disponible des la derniere consigne.
  const bool noPause =
      current == Phase::CATCH_HOME_MIDDLE || current == Phase::CATCH_RETURN ||
      current == Phase::RELEASE_MIDDLE || current == Phase::RELEASE_HOME_CLOSE;
  const uint32_t pause = noPause ? 0 : config.value[SETTLE];
  if (fault() || elapsed < total + pause) return;
  switch (current) {
    case Phase::CATCH_HOME_MIDDLE:
      move(Phase::CATCH_OUT_OPEN, config.value[OUT], config.value[OPEN],
           MIDDLE_OUT, GRIP_OPEN, true);
      break;
    case Phase::CATCH_OUT_OPEN:
      move(Phase::CATCH_CLOSE, config.value[OUT], config.value[CLOSED],
           MIDDLE_OUT, GRIP_CLOSE);
      break;
    case Phase::CATCH_CLOSE:
      move(Phase::CATCH_RETURN, config.value[MIDDLE], config.value[CLOSED],
           OUT_MIDDLE, GRIP_CLOSE);
      break;
    case Phase::CATCH_RETURN:
      holding = true;
      current = Phase::IDLE;
      break;
    case Phase::RELEASE_OUT:
      move(Phase::RELEASE_OPEN, config.value[OUT], config.value[OPEN],
           MIDDLE_OUT, GRIP_OPEN);
      break;
    case Phase::RELEASE_OPEN:
      move(Phase::RELEASE_MIDDLE, config.value[MIDDLE], config.value[OPEN],
           OUT_MIDDLE, GRIP_OPEN);
      break;
    case Phase::RELEASE_MIDDLE:
      move(Phase::RELEASE_HOME_CLOSE, config.value[HOME], config.value[CLOSED],
           MIDDLE_HOME, GRIP_CLOSE, true);
      break;
    case Phase::RELEASE_HOME_CLOSE:
      holding = false;
      current = Phase::IDLE;
      break;
    default:
      break;
  }
}
const char* phase() {
  switch (current) {
    case Phase::BOOT:
      return "Initialisation HOME + GRIP-CLOSED";
    case Phase::CATCH_HOME_MIDDLE:
      return "Catch : HOME vers MIDDLE";
    case Phase::CATCH_OUT_OPEN:
      return "Catch : vers OUT + ouverture";
    case Phase::CATCH_CLOSE:
      return "Catch : fermeture de la pince";
    case Phase::CATCH_RETURN:
      return "Catch : retour MIDDLE";
    case Phase::RELEASE_OUT:
      return "Release : vers OUT";
    case Phase::RELEASE_OPEN:
      return "Release : ouverture de la pince";
    case Phase::RELEASE_MIDDLE:
      return "Release : retour MIDDLE";
    case Phase::RELEASE_HOME_CLOSE:
      return "Release : retour HOME + fermeture";
    case Phase::FAULT:
      return "Erreur PWM : redemarrer le systeme";
    default:
      return canCatch() ? "HOME - pret pour Catch"
             : canRelease()
                 ? "MIDDLE - objet suspendu (suppose)"
                 : "Reglage manuel - rejoindre HOME + GRIP-CLOSED pour Catch";
  }
}
}  // namespace arm_sequence
