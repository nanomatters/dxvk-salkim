#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace dxvk {

static unsigned checks;
static void check(bool value, const char* message) {
  ++checks;
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

class CheckedMutex {
  std::mutex m_mutex;
  inline static thread_local std::vector<const CheckedMutex*> s_owned;

public:
  void lock() {
    m_mutex.lock();
    s_owned.push_back(this);
  }
  void unlock() {
    s_owned.erase(std::find(s_owned.begin(), s_owned.end(), this));
    m_mutex.unlock();
  }
  bool owned() const {
    return std::find(s_owned.begin(), s_owned.end(), this) != s_owned.end();
  }
};

struct high_resolution_clock {
  static uint64_t get_counter() { return 1000u; }
  static VkTimeDomainKHR get_vk_time_domain() { return VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR; }
};

template<typename T> using Rc = std::shared_ptr<T>;
struct DxvkLatencyTracker {};
#include "frame.h"

class CheckedFrames {
  std::array<PresenterFrame, 16u> m_frames;
  const CheckedMutex& m_mutex;

  void requireLock() const {
    if (!m_mutex.owned()) {
      std::cerr << "Frame queue accessed without its mutex\n";
      std::exit(1);
    }
  }

public:
  explicit CheckedFrames(const CheckedMutex& mutex) : m_mutex(mutex) {}
  const PresenterFrame* begin() const { requireLock(); return m_frames.data(); }
  const PresenterFrame* end() const { requireLock(); return m_frames.data() + size(); }
  PresenterFrame& operator[](size_t index) { requireLock(); return m_frames[index]; }
  size_t size() const { return m_frames.size(); }
};

namespace str {
  template<typename... T> std::string format(T&&...) { return {}; }
}
struct Logger {
  static void warn(const std::string&) {}
};

struct Dispatch {
  unsigned queries = 0u;
  VkDevice device() { return VK_NULL_HANDLE; }
  VkResult vkGetPastPresentationTimingEXT(VkDevice, const VkPastPresentationTimingInfoEXT*,
      VkPastPresentationTimingPropertiesEXT* properties) {
    ++queries;
    properties->presentationTimingCount = 0u;
    return VK_SUCCESS;
  }
};

struct Presenter {
  static constexpr unsigned FrameQueueSize = 16u;
  struct PresentQueueTime { uint64_t presentId = 0u, timeNs = 0u; };
  struct Properties { uint64_t updateCounter = 0u; };

  CheckedMutex m_timingMutex, m_frameMutex, m_presentTelemetryMutex;
  CheckedFrames m_frameQueue {m_frameMutex};
  std::condition_variable_any m_frameCond, m_frameDrain;
  uint64_t m_frameQueuePushId = 0u, m_frameQueuePopId = 0u;
  std::array<PresentQueueTime, FrameQueueSize> m_presentQueueTimes;
  struct {
    VkPresentStageFlagsEXT presentStage = 0u;
    uint64_t timeDomainId = 0u;
    PresenterTimingFeedback reference, lastReport;
  } m_timingMode;
  PresenterTimingFeedback m_timingFeedback;
  std::optional<Properties> m_timingDisplayInfo {Properties()};
  std::optional<Properties> m_timingDomains {Properties()};
  Rc<Dispatch> m_vkd = std::make_shared<Dispatch>();
  VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
  uint64_t m_reportQueueTakeId = 0u;

  const PresenterFrame* findFrameEntry(uint64_t frameId) const;
  PresenterTimingFeedback updatePresentTimingFeedback(uint64_t frameId, VkPresentStageFlagsEXT stages);
  PresenterTimingFeedback commitTimingFeedback(const PresenterTimingFeedback&, bool);
  void pushFrame(const PresenterFrame& frame);
  void updateDisplayTiming() {}
  void updateTimingDomains() {}
  bool recalibrateTimeDomains() { return true; }
  void updateTimingMode() {}
  void accumulatePresentTelemetry(const VkPastPresentationTimingEXT&) {}
  uint64_t translateTimestamp(VkTimeDomainKHR, uint64_t, uint64_t time, VkTimeDomainKHR, uint64_t) { return time; }
  bool hasTimeDomain(VkTimeDomainKHR) { return true; }
};

#include "feedback.h"

static void feedbackCases() {
  Presenter presenter;
  PresenterFrame frame;
  frame.frameId = 1u;
  frame.deadlineHost = 2000u;
  presenter.pushFrame(frame);
  auto feedback = presenter.updatePresentTimingFeedback(1u, 0u);
  check(feedback.frameId == 1u && feedback.presentTimeHost == 1000u,
    "completion feedback uses the host clock without timing support");
  check(presenter.m_timingMode.reference.frameId == 1u,
    "first completion establishes a pacing reference");
  check(!presenter.m_frameMutex.owned() && !presenter.m_timingMutex.owned(),
    "feedback releases both mutexes");

  frame.frameId = 2u;
  presenter.pushFrame(frame);
  presenter.updatePresentTimingFeedback(2u, 0u);
  check(presenter.m_timingMode.reference.frameId == 1u,
    "a completion before its deadline retains the reference");

  frame.frameId = 3u;
  frame.deadlineHost = 500u;
  presenter.pushFrame(frame);
  presenter.updatePresentTimingFeedback(3u, 0u);
  check(presenter.m_timingMode.reference.frameId == 3u,
    "a missed deadline updates the reference");

  feedback = presenter.updatePresentTimingFeedback(0u, 0u);
  check(feedback.frameId == 3u, "a statistics query keeps the last completion");
  feedback = presenter.updatePresentTimingFeedback(2u, 0u);
  check(feedback.frameId == 3u, "older feedback does not move completion backwards");
  presenter.updatePresentTimingFeedback(4u, 0u);
  check(presenter.m_timingMode.reference.frameId == 4u,
    "a missing frame entry restarts the reference");
  check(presenter.m_vkd->queries == 0u, "unsupported timing never queries Vulkan");
}

static void concurrentQueries() {
  Presenter presenter;
  std::atomic<bool> start {false};
  std::thread reader([&] {
    while (!start.load()) std::this_thread::yield();
    for (unsigned i = 0; i < 100000u; i++)
      presenter.updatePresentTimingFeedback(0u, 0u);
  });
  start.store(true);
  for (unsigned i = 0; i < 100000u; i++) {
    PresenterFrame frame;
    frame.frameId = i + 1u;
    frame.deadlineHost = i + 1000u;
    presenter.pushFrame(frame);
    std::lock_guard lock(presenter.m_frameMutex);
    presenter.m_frameQueuePopId = presenter.m_frameQueuePushId;
  }
  reader.join();
  check(presenter.m_frameQueuePushId == 100000u,
    "concurrent statistics queries preserve all frame submissions");
}

}

int main() {
  dxvk::feedbackCases();
  dxvk::concurrentQueries();
  std::cout << "Presenter feedback: " << dxvk::checks << " checks passed\n";
}
