/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/thread/mutex.h>

#include <algorithm>
#include <cstdlib>
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif
#include <rex/platform.h>

namespace rex::thread {

std::recursive_mutex& global_critical_region::mutex() {
  static std::recursive_mutex global_mutex;
  return global_mutex;
}

int global_critical_region::spin_count() {
  static const int count = [] {
#if REX_PLATFORM_PS5
    int value = 100;
#else
    int value = 0;
#endif
    if (const char* env = std::getenv("REX_GLOBAL_LOCK_SPIN"); env && *env) {
      value = std::max(0, std::atoi(env));
    }
    return value;
  }();
  return count;
}

void global_critical_region::LockSpin() {
  std::recursive_mutex& m = mutex();
  if (m.try_lock()) {
    return;
  }
  for (int round = spin_count(); round > 0; --round) {
    for (int k = 0; k < 4; ++k) {
#if defined(__x86_64__) || defined(_M_X64)
      _mm_pause();
#endif
    }
    if (m.try_lock()) {
      return;
    }
  }
  m.lock();
}

}  // namespace rex::thread
