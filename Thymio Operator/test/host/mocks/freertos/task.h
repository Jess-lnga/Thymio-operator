#pragma once
inline thread_local int mockCore = 1;
inline int xPortGetCoreID() { return mockCore; }
