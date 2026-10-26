#include <Preferences.h>
#include <freertos/task.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "motor_ctrl.h"
#include "motor_runtime.h"

uint32_t fakeMillis = 0;  // Horloge ecrite uniquement par le thread moteur.
std::vector<uint8_t> Preferences::bytes;
bool Preferences::failWrite = false;
std::atomic<unsigned> writes{0};
namespace motor_ctrl {
State states[2] = {{4915, false}, {4915, false}};
void begin() { assert(mockCore == 1); }
bool setTicks(uint8_t motor, uint16_t ticks) {
  assert(mockCore == 1 && motor < 2 && ticks >= MIN_TICKS &&
         ticks <= MAX_TICKS);
  states[motor].ticks = ticks;
  ++writes;
  return true;
}
bool setEnabled(uint8_t motor, bool enabled) {
  assert(mockCore == 1);
  states[motor].enabled = enabled;
  return true;
}
const State& state(uint8_t motor) {
  assert(mockCore == 1);
  return states[motor];
}
}  // namespace motor_ctrl
namespace runtime = motor_runtime;
void tick() {
  mockCore = 1;
  fakeMillis += 2;
  runtime::update();
}
runtime::Command cmd(runtime::CommandType type) {
  runtime::Command c;
  c.type = type;
  return c;
}
uint32_t submit(const runtime::Command& c) {
  mockCore = 0;
  uint32_t id = 0;
  assert(runtime::submit(c, id));
  assert(id != 0);
  return id;
}
void checkSnapshot(const runtime::Snapshot& s) {
  assert(s.ready);
  for (unsigned i = 0; i < 2; ++i) {
    const uint16_t expected =
        std::max(1639u, (uint32_t(s.positions[i]) * 65536 + 10000) / 20000);
    assert(s.ticks[i] == expected);
  }
  assert(std::strlen(s.phase) < sizeof(s.phase));
  if (s.busy) assert(!s.canCatch && !s.canRelease);
}
int main() {
  mockCore = 0;
  uint32_t id;
  assert(!runtime::get_state().ready);
  assert(!runtime::submit(cmd(runtime::CommandType::CATCH), id));
  mockCore = 1;
  runtime::begin();
  for (int i = 0; i < 1000; ++i) tick();
  auto snapshot = runtime::get_state();
  checkSnapshot(snapshot);
  assert(snapshot.canCatch);
  assert(runtime::get_ticks(0) == snapshot.ticks[0] &&
         runtime::get_ticks(2) == 0);
  snapshot.positions[0] = 500;
  snapshot.settings.value[0] = 500;
  assert(runtime::get_state().positions[0] == 2400);  // Getters par copie.

  id = submit(cmd(runtime::CommandType::RELEASE));
  assert(runtime::get_result(id) == runtime::Result::PENDING);
  assert(runtime::get_state().busy && !runtime::get_state().canCatch);
  uint32_t other;
  assert(!runtime::submit(cmd(runtime::CommandType::CATCH), other));
  tick();
  assert(runtime::get_result(id) == runtime::Result::REJECTED);
  assert(runtime::get_result(0) == runtime::Result::UNKNOWN);

  auto save = cmd(runtime::CommandType::SAVE);
  save.settings = runtime::get_state().settings;
  save.settings.value[arm_sequence::HOME_MIDDLE] = 9;
  id = submit(save);
  save.settings.value[arm_sequence::HOME_MIDDLE] = 100;  // Copie de commande.
  assert(runtime::get_state().settings.value[arm_sequence::HOME_MIDDLE] == 10);
  tick();
  assert(runtime::get_result(id) == runtime::Result::OK);
  assert(runtime::get_state().settings.value[arm_sequence::HOME_MIDDLE] == 9);
  const unsigned count = writes;
  tick();
  assert(writes == count);  // La commande acquittee n'est pas rejouee.

  id = submit(cmd(runtime::CommandType::CATCH));
  tick();
  assert(runtime::get_result(id) == runtime::Result::OK);
  id = submit(save);
  tick();
  assert(runtime::get_result(id) == runtime::Result::REJECTED);
  // Le moteur continue pendant une interface simulee bloquee : aucun appel web.
  const auto start = runtime::get_state();
  for (int i = 0; i < 40000 && !runtime::get_state().canRelease; ++i) tick();
  assert(runtime::get_state().canRelease &&
         runtime::get_state().positions[0] != start.positions[0]);
  id = submit(cmd(runtime::CommandType::RELEASE));
  tick();
  for (int i = 0; i < 40000 && !runtime::get_state().canCatch; ++i) tick();
  assert(runtime::get_state().canCatch);

  auto send = [&](runtime::CommandType type) {
    const auto ticket = submit(cmd(type));
    tick();
    return runtime::get_result(ticket);
  };
  auto until = [&](const char* phase) {
    for (int i = 0; i < 100000; ++i) {
      if (std::strstr(runtime::get_state().phase, phase)) return;
      tick();
    }
    assert(false && "Loop phase not reached");
  };
  auto finishLoop = [&] {
    for (int i = 0; i < 100000 && runtime::get_state().loopActive; ++i) tick();
    const auto s = runtime::get_state();
    assert(!s.loopActive && !s.loopStopping && s.canCatch && s.canLoop);
    assert(s.positions[0] == s.settings.value[arm_sequence::HOME]);
    assert(s.positions[1] == s.settings.value[arm_sequence::CLOSED]);
  };
  assert(send(runtime::CommandType::STOP) == runtime::Result::REJECTED);
  assert(send(runtime::CommandType::LOOP) == runtime::Result::OK);
  assert(runtime::get_state().canStop && !runtime::get_state().canLoop);
  until("LOOP : pause avant Release");
  auto rejected = cmd(runtime::CommandType::MANUAL);
  rejected.us = 2400;
  id = submit(rejected);
  tick();
  assert(runtime::get_result(id) == runtime::Result::REJECTED);
  id = submit(save);
  tick();
  assert(runtime::get_result(id) == runtime::Result::REJECTED);
  assert(send(runtime::CommandType::CATCH) == runtime::Result::REJECTED);
  assert(send(runtime::CommandType::RELEASE) == runtime::Result::REJECTED);
  assert(send(runtime::CommandType::LOOP) == runtime::Result::REJECTED);
  until("LOOP : pause avant Catch");
  const uint32_t homeAt = fakeMillis;
  for (int i = 0; i < 999; ++i) tick();
  assert(fakeMillis - homeAt == 1998);
  assert(std::strstr(runtime::get_state().phase, "pause avant Catch"));
  tick();
  assert(std::strstr(runtime::get_state().phase, "HOME vers MIDDLE"));
  until("LOOP : pause avant Release");
  for (int i = 0; i < 999; ++i) tick();
  assert(std::strstr(runtime::get_state().phase, "pause avant Release"));
  tick();
  assert(std::strstr(runtime::get_state().phase, "Release : vers OUT"));
  assert(send(runtime::CommandType::STOP) == runtime::Result::OK);
  finishLoop();

  // STOP a chacune des phases termine toujours la paire Catch-Release a HOME.
  const char* phases[] = {"HOME vers MIDDLE",        "vers OUT + ouverture",
                          "fermeture de la pince",   "Catch : retour MIDDLE",
                          "pause avant Release",     "Release : vers OUT",
                          "Release : ouverture",     "Release : retour MIDDLE",
                          "retour HOME + fermeture", "pause avant Catch"};
  for (const char* phase : phases) {
    assert(send(runtime::CommandType::LOOP) == runtime::Result::OK);
    until(phase);
    assert(send(runtime::CommandType::STOP) == runtime::Result::OK);
    if (runtime::get_state().loopActive)
      assert(runtime::get_state().loopStopping);
    finishLoop();
    for (int i = 0; i < 1100; ++i) tick();
    assert(runtime::get_state().canCatch && !runtime::get_state().loopActive);
  }
  // Pause personnalisable, sauvegardee, et comparaison temporelle avec
  // rollover.
  save.settings = runtime::get_state().settings;
  save.settings.value[arm_sequence::LOOP_PAUSE] = 60001;
  id = submit(save);
  tick();
  assert(runtime::get_result(id) == runtime::Result::REJECTED);
  save.settings.value[arm_sequence::LOOP_PAUSE] = 0;
  id = submit(save);
  tick();
  assert(runtime::get_result(id) == runtime::Result::OK);
  mockCore = 1;
  runtime::begin();
  for (int i = 0; i < 1000; ++i) tick();
  assert(runtime::get_state().settings.value[arm_sequence::LOOP_PAUSE] == 0);
  fakeMillis = 0xFFFFFF00u;
  assert(send(runtime::CommandType::LOOP) == runtime::Result::OK);
  until("Release : vers OUT");
  assert(send(runtime::CommandType::STOP) == runtime::Result::OK);
  finishLoop();

  // Lecteurs et producteur concurrents pendant que le vrai runtime publie.
  std::atomic<bool> stop{false};
  std::atomic<unsigned> reads{0};
  std::thread owner([&] {
    mockCore = 1;
    while (!stop) {
      tick();
      std::this_thread::yield();
    }
  });
  std::vector<std::thread> readers;
  for (int r = 0; r < 3; ++r)
    readers.emplace_back([&] {
      mockCore = 0;
      while (!stop) {
        checkSnapshot(runtime::get_state());
        const auto ticks = runtime::get_ticks(0);
        assert(ticks >= 1639 && ticks <= 8192);
        ++reads;
      }
    });
  mockCore = 0;
  for (unsigned n = 0; n < 300; ++n) {
    auto manual = cmd(runtime::CommandType::MANUAL);
    manual.motor = n % 2;
    manual.us = 500 + n * 5;
    uint32_t ticket;
    assert(runtime::submit(manual, ticket));
    for (int attempts = 0;
         runtime::get_result(ticket) == runtime::Result::PENDING; ++attempts) {
      assert(attempts < 5000);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(runtime::get_result(ticket) == runtime::Result::OK);
    assert(runtime::get_state().positions[manual.motor] == manual.us);
  }
  stop = true;
  owner.join();
  for (auto& reader : readers) reader.join();
  assert(reads > 0);
  std::cout << "PASS runtime: core ownership, snapshots, mailbox isolation, "
               "acknowledgements, interlocks, web stall, concurrent "
               "readers/commands ("
            << reads << " reads)\n";
}
