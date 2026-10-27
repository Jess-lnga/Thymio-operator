#include "motor_runtime.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>

#include "motor_ctrl.h"

namespace motor_runtime {
namespace {
// Toutes les lectures/ecritures de ces donnees utilisent le meme verrou.
static portMUX_TYPE sharedMux = portMUX_INITIALIZER_UNLOCKED;
static Snapshot sharedState;
static Command mailbox;
static uint32_t commandId = 0;
static bool pending = false;
static bool claimed = false;
static Result result = Result::UNKNOWN;
// Coordinateur LOOP reserve a la tache moteur, jamais partage directement.
enum class LoopPhase { OFF, CATCH, WAIT_RELEASE, RELEASE, WAIT_CATCH };
LoopPhase loopPhase = LoopPhase::OFF;
bool stopRequested = false;
uint32_t pauseStarted = 0;

bool loopActive() { return loopPhase != LoopPhase::OFF; }
void updateLoop() {
  if (!loopActive()) return;
  if (arm_sequence::fault()) {
    loopPhase = LoopPhase::OFF;
    stopRequested = false;
    return;
  }
  const uint32_t pause =
      arm_sequence::settings().value[arm_sequence::LOOP_PAUSE];
  switch (loopPhase) {
    case LoopPhase::CATCH:
      if (arm_sequence::canRelease()) {
        loopPhase = LoopPhase::WAIT_RELEASE;
        pauseStarted = millis();
      }
      break;
    case LoopPhase::WAIT_RELEASE:
      if (millis() - pauseStarted >= pause && arm_sequence::releaseObject())
        loopPhase = LoopPhase::RELEASE;
      break;
    case LoopPhase::RELEASE:
      if (arm_sequence::canCatch()) {
        loopPhase = stopRequested ? LoopPhase::OFF : LoopPhase::WAIT_CATCH;
        pauseStarted = millis();
        if (!loopActive()) stopRequested = false;
      }
      break;
    case LoopPhase::WAIT_CATCH:
      // Deja a HOME : STOP empeche le demarrage du cycle suivant.
      if (stopRequested) {
        loopPhase = LoopPhase::OFF;
        stopRequested = false;
      } else if (millis() - pauseStarted >= pause &&
                 arm_sequence::catchObject()) {
        loopPhase = LoopPhase::CATCH;
      }
      break;
    default:
      break;
  }
}

Snapshot capture() {
  // Aucun acces depuis la tache web aux fonctions de la FSM ou du pilote PWM.
  Snapshot next;
  next.ready = true;
  next.fault = arm_sequence::fault();
  next.loopActive = loopActive();
  next.loopStopping = stopRequested;
  next.canLoop = !loopActive() && arm_sequence::canCatch();
  next.canStop = loopActive() && !stopRequested;
  next.busy = arm_sequence::busy() || loopActive();
  next.canCatch = !loopActive() && arm_sequence::canCatch();
  next.canRelease = !loopActive() && arm_sequence::canRelease();
  next.settings = arm_sequence::settings();
  for (uint8_t i = 0; i < 2; ++i) {
    next.positions[i] = arm_sequence::position(i);
    next.ticks[i] = motor_ctrl::state(i).ticks;
  }
  const char* phase = arm_sequence::phase();
  if (loopPhase == LoopPhase::WAIT_RELEASE)
    phase = "LOOP : pause avant Release";
  if (loopPhase == LoopPhase::WAIT_CATCH)
    phase = "LOOP : pause avant Catch (HOME)";
  std::strncpy(next.phase, phase, sizeof(next.phase) - 1);
  return next;
}
bool execute(const Command& command) {
  if (command.type == CommandType::STOP) {
    if (!loopActive()) return false;
    stopRequested = true;
    return true;
  }
  // Inclut les pauses LOOP : aucun mouvement manuel ni Save entre deux phases.
  if (loopActive()) return false;
  switch (command.type) {
    case CommandType::LOOP:
      if (!arm_sequence::catchObject()) return false;
      stopRequested = false;
      loopPhase = LoopPhase::CATCH;
      return true;
    case CommandType::CATCH:
      return arm_sequence::catchObject();
    case CommandType::RELEASE:
      return arm_sequence::releaseObject();
    case CommandType::MANUAL:
      return arm_sequence::manual(command.motor, command.us);
    case CommandType::SAVE:
      return arm_sequence::save(command.settings);
    default:
      return false;
  }
}
}  // namespace
void begin() {
  configASSERT(xPortGetCoreID() == 1);
  loopPhase = LoopPhase::OFF;
  stopRequested = false;
  motor_ctrl::begin();
  arm_sequence::begin();
  const Snapshot next = capture();
  portENTER_CRITICAL(&sharedMux);
  sharedState = next;
  portEXIT_CRITICAL(&sharedMux);
}
void update() {
  configASSERT(xPortGetCoreID() == 1);
  Command command;
  bool hasCommand = false;
  portENTER_CRITICAL(&sharedMux);
  if (pending && !claimed) {
    command = mailbox;
    claimed = true;
    hasCommand = true;
  }
  portEXIT_CRITICAL(&sharedMux);

  // PWM, FSM et Preferences sont utilises uniquement ici, hors du verrou.
  const bool accepted = hasCommand && execute(command);
  arm_sequence::update();
  updateLoop();
  const Snapshot next = capture();

  portENTER_CRITICAL(&sharedMux);
  sharedState = next;
  if (hasCommand) {
    result = accepted ? Result::OK : Result::REJECTED;
    pending = false;
    claimed = false;
  }
  portEXIT_CRITICAL(&sharedMux);
}
Snapshot get_state() {
  portENTER_CRITICAL(&sharedMux);
  Snapshot copy = sharedState;
  copy.commandPending = pending;
  copy.commandId = commandId;
  copy.commandResult = result;
  if (pending) {
    copy.busy = true;
    copy.canCatch = false;
    copy.canRelease = false;
    copy.canLoop = false;
    copy.canStop = false;
  }
  portEXIT_CRITICAL(&sharedMux);
  return copy;
}
uint16_t get_ticks(uint8_t motor) {
  if (motor >= 2) return 0;
  portENTER_CRITICAL(&sharedMux);
  const uint16_t ticks = sharedState.ticks[motor];
  portEXIT_CRITICAL(&sharedMux);
  return ticks;
}
bool submit(const Command& command, uint32_t& ticket) {
  portENTER_CRITICAL(&sharedMux);
  const bool available = sharedState.ready && !pending;
  if (available) {
    mailbox = command;
    if (++commandId == 0) ++commandId;
    ticket = commandId;
    result = Result::PENDING;
    claimed = false;
    pending = true;
  }
  portEXIT_CRITICAL(&sharedMux);
  return available;
}
Result get_result(uint32_t ticket) {
  portENTER_CRITICAL(&sharedMux);
  const Result copy = ticket && ticket == commandId ? result : Result::UNKNOWN;
  portEXIT_CRITICAL(&sharedMux);
  return copy;
}
}  // namespace motor_runtime
