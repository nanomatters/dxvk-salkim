#include "util_sleep.h"
#include "util_string.h"

#include <algorithm>

#include "./log/log.h"

using namespace std::chrono_literals;

namespace dxvk {

  Sleep Sleep::s_instance;


  Sleep::Sleep() {

  }


  Sleep::~Sleep() {

  }


  void Sleep::initialize() {
    std::lock_guard lock(m_mutex);

    if (m_initialized.load())
      return;

    initializePlatformSpecifics();
    m_sleepThreshold = 4 * m_sleepGranularity;

    m_initialized.store(true);
  }


  void Sleep::initializePlatformSpecifics() {
#ifdef _WIN32
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");

    if (ntdll) {
      m_adaptiveSleep = ::GetProcAddress(ntdll, "wine_get_version") != nullptr;

      NtDelayExecution = reinterpret_cast<NtDelayExecutionProc>(
        ::GetProcAddress(ntdll, "NtDelayExecution"));
      auto NtQueryTimerResolution = reinterpret_cast<NtQueryTimerResolutionProc>(
        ::GetProcAddress(ntdll, "NtQueryTimerResolution"));
      auto NtSetTimerResolution = reinterpret_cast<NtSetTimerResolutionProc>(
        ::GetProcAddress(ntdll, "NtSetTimerResolution"));

      ULONG min, max, cur;

      // Wine's implementation of these functions is a stub as of 6.10, which is fine
      // since it uses select() in NtDelayExecution. This is only relevant for Windows.
      if (NtQueryTimerResolution && !NtQueryTimerResolution(&min, &max, &cur)) {
        m_sleepGranularity = TimerDuration(cur);

        if (NtSetTimerResolution && !NtSetTimerResolution(max, TRUE, &cur)) {
          Logger::info(str::format("Setting timer interval to ", (double(max) / 10.0), " us"));
          m_sleepGranularity = TimerDuration(max);
        }
      }
    } else {
      // Assume 1ms sleep granularity by default
      m_sleepGranularity = TimerDuration(1ms);
    }
#else
    // Assume 0.5ms sleep granularity by default
    m_sleepGranularity = TimerDuration(500us);
    m_adaptiveSleep = true;
#endif
  }


  Sleep::TimePoint Sleep::sleep(TimePoint t0, TimerDuration duration) {
    if (duration <= TimerDuration::zero())
      return t0;

    // If necessary, initialize function pointers and some values
    if (!m_initialized.load())
      initialize();

    // Wine's reported timer granularity does not describe the host sleep
    // precision. Learn the oversleep on each calling thread instead of
    // spinning for several milliseconds. Keep native Windows unchanged.
    thread_local TimerDuration sleepMargin = TimerDuration(200us);
    TimerDuration sleepThreshold = m_adaptiveSleep ? sleepMargin : m_sleepThreshold;

    if (!m_adaptiveSleep && m_sleepGranularity != TimerDuration::zero())
      sleepThreshold += duration / 6;

    TimerDuration remaining = duration;
    TimePoint t1 = t0;

    while (remaining > sleepThreshold) {
      TimerDuration sleepDuration = remaining - sleepThreshold;

      systemSleep(sleepDuration);

      t1 = dxvk::high_resolution_clock::now();
      auto elapsed = std::chrono::duration_cast<TimerDuration>(t1 - t0);

      if (m_adaptiveSleep) {
        // React immediately to late wakeups, then decay slowly. Bound the
        // reserve so a scheduling stall cannot turn into a long busy-wait.
        auto observed = elapsed - sleepDuration + TimerDuration(50us);
        sleepMargin = std::clamp(std::max(observed, sleepMargin - sleepMargin / 64),
          TimerDuration(100us), TimerDuration(1ms));
        sleepThreshold = sleepMargin;
      }

      remaining -= elapsed;
      t0 = t1;
    }

    // Busy-wait until we have slept long enough
    while (remaining > TimerDuration::zero()) {
      t1 = dxvk::high_resolution_clock::now();
      remaining -= std::chrono::duration_cast<TimerDuration>(t1 - t0);
      t0 = t1;
    }

    return t1;
  }


  void Sleep::systemSleep(TimerDuration duration) {
#ifdef _WIN32
    if (NtDelayExecution) {
      LARGE_INTEGER ticks;
      ticks.QuadPart = -duration.count();

      NtDelayExecution(FALSE, &ticks);
    } else {
      std::this_thread::sleep_for(duration);
    }
#else
    std::this_thread::sleep_for(duration);
#endif
  }

}
