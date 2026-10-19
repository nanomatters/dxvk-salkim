#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
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
  using time_point = uint64_t;
  static time_point now() { return 1000u; }
  static time_point get_time_from_counter(uint64_t counter) { return counter; }
};

struct Sleep {
  inline static std::vector<uint64_t> targets;
  static void sleepUntil(uint64_t, uint64_t target) { targets.push_back(target); }
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

struct Limiter {
  uint32_t calls = 0u;
  void delay() { ++calls; }
};

struct Presenter {
  Rc<Device> m_device = std::make_shared<Device>();
  Rc<Dispatch> m_vkd = std::make_shared<Dispatch>();
  Rc<Signal> m_signal = std::make_shared<Signal>();
  VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
  VkPresentModeKHR m_presentMode = VK_PRESENT_MODE_FIFO_KHR;
  bool m_hasPresentWait = true;
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
  Limiter m_fpsLimiter;

  bool updatePresentTiming(uint64_t) { ++timingPolls; return timingAvailable; }
  bool hasQpcDomain() { return qpcAvailable; }
  uint64_t translateTimestamp(VkTimeDomainKHR, uint64_t, uint64_t,
                             VkTimeDomainKHR, uint64_t) { return translatedTarget; }
  void signalFrameValue(uint64_t id) { m_signal->current = id; }
  bool waitUntilFrameTargetTime(const PresenterFrame& frame);
  void runFrameThread();
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
  check(presenter.m_fpsLimiter.calls == 1u, name, "limiter fallback retained");
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

}

int main(int argc, char** argv) {
  if (argc != 2 || (std::string(argv[1]) != "mode" && std::string(argv[1]) != "all")) return 2;
  dxvk::modeCases();
  std::cout << "presenter: " << dxvk::checks << " checks passed\n";
}
