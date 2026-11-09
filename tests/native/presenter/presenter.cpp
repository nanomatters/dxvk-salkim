#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dxvk {

namespace env {
  void setThreadName(const char*) { }
}

namespace str {
  template<typename... T> std::string format(T&&...) { return {}; }
}

struct Logger {
  static void err(const std::string&) { }
};

struct high_resolution_clock {
  using time_point = std::chrono::time_point<std::chrono::steady_clock, std::chrono::nanoseconds>;
  inline static uint64_t counter = 1000u;
  static time_point now() { return get_time_from_counter(counter); }
  static uint64_t get_counter() { return counter; }
  static uint64_t get_ticks(std::chrono::nanoseconds duration) { return duration.count(); }
  static time_point get_time_from_counter(uint64_t counter) {
    return time_point(std::chrono::nanoseconds(counter));
  }
};

struct Sleep {
  inline static std::vector<uint64_t> targets;
  static void sleepUntil(high_resolution_clock::time_point, high_resolution_clock::time_point target) {
    targets.push_back(target.time_since_epoch().count());
    high_resolution_clock::counter = target.time_since_epoch().count();
  }
};

template<typename T> using Rc = std::shared_ptr<T>;

struct DxvkLatencyTracker {
  void notifyGpuPresentEnd(uint64_t) { }
};

#include "frame.h"

struct Device {
  struct Features {
    struct { bool presentWait2 = false; } khrPresentWait2;
  } value;

  const Features& features() const { return value; }
};

struct Dispatch {
  uint32_t wait1 = 0u;
  uint32_t wait2 = 0u;
  uint64_t lastId = 0u;
  VkResult result = VK_SUCCESS;

  VkDevice device() { return VK_NULL_HANDLE; }

  VkResult vkWaitForPresentKHR(VkDevice, VkSwapchainKHR, uint64_t id, uint64_t timeout) {
    if (timeout != std::numeric_limits<uint64_t>::max()) std::abort();
    ++wait1;
    lastId = id;
    return result;
  }

  VkResult vkWaitForPresent2KHR(VkDevice, VkSwapchainKHR, const VkPresentWait2InfoKHR* info) {
    if (info->timeout != std::numeric_limits<uint64_t>::max()) std::abort();
    ++wait2;
    lastId = info->presentId;
    return result;
  }
};

struct Signal {
  uint64_t current = 0u;
  uint64_t value() const { return current; }
};

struct Presenter {
  Rc<Device> m_device = std::make_shared<Device>();
  Rc<Dispatch> m_vkd = std::make_shared<Dispatch>();
  Rc<Signal> m_signal = std::make_shared<Signal>();
  VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
  VkPresentModeKHR m_presentMode = VK_PRESENT_MODE_FIFO_KHR;
  bool m_hasPresentWait = true;
  bool m_hasPresentId = true;
  struct { uint64_t frameIntervalNs = 1000u; } m_timingMode;
  bool timingAvailable = false;
  bool qpcAvailable = false;
  uint64_t translatedTarget = 2000u;
  uint32_t timingPolls = 0u;

  std::mutex m_frameMutex;
  std::mutex m_timingMutex;
  std::mutex m_pacingMutex;
  std::condition_variable m_frameCond;
  std::condition_variable m_frameDrain;
  std::array<PresenterFrame, 2> m_frameQueue;
  size_t m_frameQueuePushId = 2u;
  size_t m_frameQueuePopId = 0u;
  uint64_t m_lastCompleted = 0u;
  uint64_t m_lastSignaled = 1u;
  uint64_t m_lastDiscardedFrameTimeHost = 0u;

  bool updatePresentTiming(uint64_t) { ++timingPolls; return timingAvailable; }
  void updatePresentTimingFeedback(uint64_t, VkPresentStageFlagsEXT) { ++timingPolls; }
  bool hasQpcDomain() { return qpcAvailable; }
  uint64_t translateTimestamp(VkTimeDomainKHR, uint64_t, uint64_t,
                             VkTimeDomainKHR, uint64_t) { return translatedTarget; }
  void signalFrameValue(uint64_t id) { m_signal->current = id; }
  void pushFrame(const PresenterFrame& frame) { m_frameQueue[0] = frame; }
  void snapshotFrame(uint64_t frameId, const Rc<DxvkLatencyTracker>& tracker,
                     VkResult status, uint64_t frameTargetTime,
                     const VkPresentTimingInfoEXT& timingInfo);
  bool waitUntilFrameTargetTime(const PresenterFrame& frame);
  void runFrameThread();
  void completeFrame(uint64_t frameId);
};

#include "worker.h"

static uint32_t checks = 0u;

static void check(bool condition, const char* test, const char* property) {
  ++checks;
  if (!condition) {
    std::cerr << test << ": " << property << "\n";
    std::exit(1);
  }
}

static void modeCase(const char* name, VkPresentModeKHR frameMode, VkPresentModeKHR nextMode,
                     bool wait2, bool supported, VkResult presentResult, uint32_t expectedWaits) {
  Presenter presenter;
  high_resolution_clock::counter = 1000u;
  Sleep::targets.clear();
  presenter.m_frameQueue[0].frameId = 1u;
  presenter.m_frameQueue[0].mode = frameMode;
  presenter.m_frameQueue[0].result = presentResult;
  presenter.m_presentMode = nextMode;
  presenter.m_hasPresentWait = supported;
  presenter.m_device->value.khrPresentWait2.presentWait2 = wait2;
  presenter.runFrameThread();

  check(presenter.m_vkd->wait1 == (wait2 ? 0u : expectedWaits), name, "KHR wait count");
  check(presenter.m_vkd->wait2 == (wait2 ? expectedWaits : 0u), name, "KHR2 wait count");
  check(presenter.timingPolls == 1u, name, "timing feedback polled");
  check(Sleep::targets.empty(), name, "no target means no limiter sleep");
  check(presenter.m_signal->current == 1u, name, "frame completion signalled");
  if (expectedWaits)
    check(presenter.m_vkd->lastId == 1u, name, "wait uses queued present ID");
}

static void modeCases() {
  modeCase("FIFO then IMMEDIATE", VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR,
    false, true, VK_SUCCESS, 1u);
  modeCase("IMMEDIATE then FIFO", VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR,
    false, true, VK_SUCCESS, 0u);
  modeCase("MAILBOX then FIFO", VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR,
    false, true, VK_SUCCESS, 0u);
  modeCase("RELAXED then MAILBOX", VK_PRESENT_MODE_FIFO_RELAXED_KHR, VK_PRESENT_MODE_MAILBOX_KHR,
    true, true, VK_SUCCESS, 1u);
  modeCase("FIFO presentWait2", VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_KHR,
    true, true, VK_SUBOPTIMAL_KHR, 1u);
  modeCase("FIFO without present-wait", VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_KHR,
    false, false, VK_SUCCESS, 0u);
  modeCase("failed FIFO", VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_KHR,
    true, true, VK_ERROR_OUT_OF_DATE_KHR, 0u);
}

struct TimingCase {
  const char* name;
  uint64_t requestedTime = 2000u;
  uint64_t hostTarget = 2000u;
  VkPresentStageFlagsEXT queries = VK_PRESENT_STAGE_IMAGE_FIRST_PIXEL_OUT_BIT_EXT;
  VkResult presentResult = VK_SUCCESS;
  bool hasPresentId = true;
  uint32_t expectedSleeps = 1u;
  uint32_t expectedWaits = 1u;
  bool wait2 = false;
  bool waitSupported = true;
  uint64_t frameId = 1u;
  VkPresentTimingInfoFlagsEXT flags = 0u;
  bool expectedTimed = true;
};

static void timingCase(const TimingCase& test) {
  Presenter presenter;
  high_resolution_clock::counter = 1000u;
  presenter.m_hasPresentId = test.hasPresentId;
  presenter.m_hasPresentWait = test.waitSupported;
  presenter.m_device->value.khrPresentWait2.presentWait2 = test.wait2;
  VkPresentTimingInfoEXT timing = { VK_STRUCTURE_TYPE_PRESENT_TIMING_INFO_EXT };
  timing.targetTime = test.requestedTime;
  timing.flags = test.flags;
  timing.presentStageQueries = test.queries;
  timing.timeDomainId = 7u;
  presenter.snapshotFrame(test.frameId, nullptr, test.presentResult, test.hostTarget, timing);

  check(presenter.m_frameQueue[0].targetTimeHost == test.hostTarget, test.name, "snapshot host target retained");
  check(presenter.m_frameQueue[0].deadlineHost == (test.hostTarget ? test.hostTarget + 1000u : 0u),
    test.name, "deadline uses host ticks");
  check(presenter.m_frameQueue[0].timingDomainId == 7u, test.name, "snapshot time domain retained");
  check(presenter.m_frameQueue[0].isTimed == test.expectedTimed, test.name, "snapshot timing request accepted");
  Sleep::targets.clear();
  presenter.runFrameThread();

  check(Sleep::targets.size() == test.expectedSleeps, test.name, "target sleep count");
  if (test.expectedSleeps)
    check(Sleep::targets[0] == test.hostTarget, test.name, "sleep uses the captured host target");
  check(presenter.m_vkd->wait1 == (test.wait2 ? 0u : test.expectedWaits), test.name, "KHR wait count");
  check(presenter.m_vkd->wait2 == (test.wait2 ? test.expectedWaits : 0u), test.name, "KHR2 wait count");
  check(presenter.timingPolls == !!test.frameId, test.name, "feedback polled for every completed frame");
  check(presenter.m_signal->current == test.frameId, test.name, "frame completion signalled");
}

static void timingCases() {
  TimingCase test { "reference target without requested timing" };
  test.requestedTime = 0u;
  test.expectedTimed = false;
  timingCase(test);

  test = TimingCase { "absolute timing skips redundant FIFO wait" };
  test.expectedWaits = 0u;
  timingCase(test);
  test = TimingCase { "relative timing" };
  test.requestedTime = 1000u;
  test.flags = VK_PRESENT_TIMING_INFO_PRESENT_AT_RELATIVE_TIME_BIT_EXT;
  test.expectedWaits = 0u;
  test.wait2 = true;
  timingCase(test);

  test = TimingCase { "no host target yet" };
  test.hostTarget = 0u;
  test.expectedSleeps = 0u;
  timingCase(test);

  test = TimingCase { "expired host target" };
  test.hostTarget = 500u;
  test.expectedWaits = 0u;
  test.expectedSleeps = 0u;
  timingCase(test);

  test = TimingCase { "host target reached now" };
  test.hostTarget = 1000u;
  test.expectedWaits = 0u;
  test.expectedSleeps = 0u;
  timingCase(test);

  test = TimingCase { "host target at wait limit" };
  test.hostTarget = 500001000u;
  test.expectedWaits = 0u;
  timingCase(test);

  test = TimingCase { "host target exceeds wait limit" };
  test.hostTarget = 501001000u;
  test.expectedSleeps = 0u;
  timingCase(test);

  test = TimingCase { "failed timed present" };
  test.presentResult = VK_ERROR_OUT_OF_DATE_KHR;
  test.expectedTimed = false;
  test.expectedWaits = 0u;
  timingCase(test);

  test = TimingCase { "suboptimal timed present" };
  test.presentResult = VK_SUBOPTIMAL_KHR;
  test.expectedWaits = 0u;
  timingCase(test);

  test = TimingCase { "no present ID support" };
  test.hasPresentId = false;
  test.expectedTimed = false;
  test.waitSupported = false;
  test.expectedWaits = 0u;
  timingCase(test);

  test = TimingCase { "no timing stage query" };
  test.queries = 0u;
  test.expectedTimed = false;
  timingCase(test);

  test = TimingCase { "exit sentinel" };
  test.frameId = 0u;
  test.expectedTimed = false;
  test.expectedSleeps = 0u;
  test.expectedWaits = 0u;
  timingCase(test);

  Presenter presenter;
  high_resolution_clock::counter = 1000u;
  Sleep::targets.clear();
  presenter.completeFrame(1u);
  check(Sleep::targets.empty(), "discarded frame", "first completion has no prior target");
  presenter.completeFrame(2u);
  check(Sleep::targets.size() == 1u && Sleep::targets[0] == 2000u,
    "discarded frame", "second completion uses host-clock limiter");
  check(presenter.m_signal->current == 2u, "discarded frame", "completion signalled");
  check(!presenter.m_vkd->wait1 && !presenter.m_vkd->wait2 && !presenter.timingPolls,
    "discarded frame", "no WSI wait or feedback query");
  presenter.m_timingMode.frameIntervalNs = 0u;
  presenter.completeFrame(3u);
  check(Sleep::targets.size() == 1u, "discarded frame", "disabled limiter does not sleep");
}

}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  std::string selection = argv[1];
  if (selection != "mode" && selection != "timing" && selection != "all") return 2;
  if (selection != "timing") dxvk::modeCases();
  if (selection != "mode") dxvk::timingCases();
  std::cout << "presenter: " << dxvk::checks << " checks passed\n";
}
