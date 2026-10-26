#pragma once
#include <Arduino.h>

#include "arm_sequence.h"

// Seule passerelle entre le serveur (core 0) et les moteurs (core 1).
// Les getters retournent des copies, jamais des references vers l'etat partage.
namespace motor_runtime {
enum class CommandType : uint8_t { CATCH, RELEASE, MANUAL, SAVE };
enum class Result : uint8_t { UNKNOWN, PENDING, OK, REJECTED };
struct Command {
  CommandType type = CommandType::CATCH;
  uint8_t motor = 0;
  uint16_t us = 0;
  arm_sequence::Settings settings{};
};
struct Snapshot {
  bool ready = false;
  bool busy = true;
  bool canCatch = false;
  bool canRelease = false;
  bool commandPending = false;
  uint16_t ticks[2] = {};
  uint16_t positions[2] = {};
  arm_sequence::Settings settings{};
  char phase[128] = "Initialisation des moteurs";
  uint32_t commandId = 0;
  Result commandResult = Result::UNKNOWN;
};
// A appeler uniquement par la tache moteur sur core 1.
void begin();
void update();
// Disponibles depuis les deux cores ; acces proteges par spinlock.
Snapshot get_state();
uint16_t get_ticks(uint8_t motor);
// Une seule commande en attente ; false si non pret ou boite occupee.
// Le ticket identifie la reponse : pas d'ecrasement ni de reexecution.
bool submit(const Command& command, uint32_t& ticket);
Result get_result(uint32_t ticket);
}  // namespace motor_runtime
