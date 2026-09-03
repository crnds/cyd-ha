#pragma once
// Host-side stand-in for the ESP32 core's Arduino.h, used only so
// src/ui/*.cpp and include/state.h compile on the desktop for the
// equivalence harness. Not part of the firmware build — platformio.ini never
// sees this directory.
//
// Scope is deliberately narrow: only the handful of symbols screen.cpp,
// state.h and friends actually reference. See tools/host_check/README.md.

#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <initializer_list>

using std::int16_t;
using std::int32_t;
using std::int64_t;
using std::int8_t;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;

#ifndef NAN
#define NAN (__builtin_nanf(""))
#endif

// The harness controls elapsed time explicitly (see hostSetMillis() in
// host_env.h) rather than reading a real clock, so a sweep is deterministic
// and reproducible byte-for-byte between the pre- and post-refactor runs.
unsigned long millis();

#define OUTPUT 1
#define HIGH   1
#define LOW    0
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}

// Always "unsynced" (false) — deterministic, and exercises the same "--:--"
// placeholder path every run. The clock's own logic isn't in this harness's
// target list; this only needs to compile and behave the same way twice.
inline bool getLocalTime(struct tm*, uint32_t = 5000) { return false; }
