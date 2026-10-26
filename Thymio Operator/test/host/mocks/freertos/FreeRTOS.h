#pragma once
#include <cassert>
#include <mutex>
using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED \
  {                                  \
  }
#define configASSERT(condition) assert(condition)
inline void portENTER_CRITICAL(portMUX_TYPE* mux) { mux->lock(); }
inline void portEXIT_CRITICAL(portMUX_TYPE* mux) { mux->unlock(); }
