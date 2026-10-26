#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
using std::max;
using std::min;
extern uint32_t fakeMillis;
inline uint32_t millis() { return fakeMillis; }
