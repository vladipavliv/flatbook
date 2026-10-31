#pragma once

#include <cstdint>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace flatbook {

/**
 * @brief Pin the calling thread to @p core.
 *
 * Linux-only and best-effort: on other platforms, or when the affinity call
 * fails (e.g. the core does not exist), it is a no-op.
 */
inline void pinCurrentThread(std::uint32_t core) {
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
  (void)core;
#endif
}

/**
 * @brief Put the calling thread on SCHED_FIFO real-time scheduling.
 *
 * Linux-only and best-effort: requires privileges (root or `CAP_SYS_NICE` /
 * an adequate `RLIMIT_RTPRIO`); on failure it is a no-op so the caller can still
 * run without RT. @p priority is the SCHED_FIFO static priority (1..99).
 */
inline void setCurrentThreadRealTime(int priority = 99) {
#if defined(__linux__)
  sched_param param{};
  param.sched_priority = priority;
  (void)pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
#else
  (void)priority;
#endif
}

} // namespace flatbook
