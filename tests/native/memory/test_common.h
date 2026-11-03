#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stdexcept>

static void require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}

class TestMutex {
public:
  static inline thread_local std::function<void()> onWait;

  void lock() {
    require(m_owner.load() != token(), "Recursive test lock");
    if (!m_mutex.try_lock()) {
      if (onWait)
        onWait();
      m_mutex.lock();
    }
    m_owner.store(token());
  }

  void unlock() {
    m_owner.store(nullptr);
    m_mutex.unlock();
  }

private:
  static void* token() {
    static thread_local char identity;
    return &identity;
  }

  std::mutex m_mutex;
  std::atomic<void*> m_owner = nullptr;
};

class Observation {
public:
  void set(unsigned value) {
    std::lock_guard lock(m_mutex);
    if (!m_value)
      m_value = value;
    m_ready.notify_all();
  }

  unsigned get() {
    std::unique_lock lock(m_mutex);
    bool ready = m_ready.wait_for(lock, std::chrono::seconds(5), [&] { return m_value != 0; });
    require(ready, "Worker did not reach synchronization point");
    return m_value;
  }

private:
  std::mutex m_mutex;
  std::condition_variable m_ready;
  unsigned m_value = 0;
};
