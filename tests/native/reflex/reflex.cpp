#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define STDMETHODCALLTYPE
using HRESULT = int32_t;
using UINT8 = uint8_t;
using UINT32 = uint32_t;
using UINT64 = uint64_t;
constexpr HRESULT S_OK = 0, E_INVALIDARG = -2, DXGI_ERROR_INVALID_CALL = -3;

namespace dxvk {

static unsigned checks;
static void check(bool value, const char* message) {
  ++checks;
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

namespace str {
  template<typename... T> std::string format(T&&...) { return {}; }
}
struct Logger {
  static void err(const std::string&) {}
};
struct high_resolution_clock {
  inline static std::atomic<uint64_t> ticks = 0;
  static auto now() {
    return std::chrono::steady_clock::time_point(
      std::chrono::microseconds(ticks.fetch_add(10)));
  }
};

struct Dispatch {
  VkResult sleepResult = VK_SUCCESS;
  VkResult waitResult = VK_SUCCESS;
  std::vector<uint64_t> requests;
  std::vector<uint64_t> waits;
  std::mutex mutex;
  std::condition_variable cond;
  bool concurrent = false;
  bool barrierReached = true;

  VkDevice device() { return VK_NULL_HANDLE; }
  VkResult vkLatencySleepNV(VkDevice, VkSwapchainKHR, const VkLatencySleepInfoNV* info) {
    requests.push_back(info->value);
    return sleepResult;
  }
  VkResult vkWaitSemaphores(VkDevice, const VkSemaphoreWaitInfo* info, uint64_t) {
    std::unique_lock lock(mutex);
    waits.push_back(*info->pValues);
    cond.notify_all();
    if (concurrent)
      barrierReached &= cond.wait_for(lock, std::chrono::seconds(5), [&] { return waits.size() == 2; });
    return waitResult;
  }
};

struct Presenter {
  std::mutex m_surfaceMutex;
  std::shared_ptr<Dispatch> m_vkd = std::make_shared<Dispatch>();
  bool m_latencySleepSupported = true;
  VkSemaphore m_latencySemaphore = VK_NULL_HANDLE;
  VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
  uint64_t m_latencySleepValue = 0;
  uint64_t m_latencySleepCounter = 0;
  uint64_t m_latencySleepDurationUs = 0;
  VkResult createResult = VK_SUCCESS;

  VkResult createLatencySemaphore() {
    if (createResult == VK_SUCCESS)
      m_latencySemaphore = reinterpret_cast<VkSemaphore>(uintptr_t(1));
    return createResult;
  }
  void latencySleep();
};

#include "reports.h"

struct SwapChain {
  std::array<VkLatencyTimingsFrameReportNV, 64> reports = {};
  size_t available = 64;
  size_t GetLatencyInfo(size_t count, VkLatencyTimingsFrameReportNV* data) {
    count = std::min(count, available);
    for (size_t i = 0; i < count; i++)
      data[i] = reports[i];
    return count;
  }
};

struct D3D11ReflexDevice {
  std::mutex m_mutex;
  bool m_reflexEnabled = true;
  std::shared_ptr<SwapChain> m_swapChain;
  HRESULT GetLatencyInfo(D3D_LOW_LATENCY_RESULTS* results);
};

#include "production.h"

static void sleepTests() {
  Presenter p;
  p.m_latencySleepSupported = false;
  p.latencySleep();
  check(p.m_vkd->requests.empty(), "unsupported sleep submitted a request");
  p.m_latencySleepSupported = true;
  p.createResult = VK_ERROR_OUT_OF_HOST_MEMORY;
  p.latencySleep();
  check(p.m_vkd->requests.empty(), "failed semaphore creation submitted a request");

  p.createResult = VK_SUCCESS;
  p.m_vkd->sleepResult = VK_ERROR_DEVICE_LOST;
  p.latencySleep();
  check(p.m_vkd->requests == std::vector<uint64_t>{1}, "first request value");
  check(p.m_vkd->waits.empty(), "failed request waited on an unsignalled value");
  check(p.m_latencySleepCounter == 0, "failed request counted as a completed sleep");

  p.m_vkd->sleepResult = VK_SUCCESS;
  p.m_vkd->waitResult = VK_TIMEOUT;
  p.latencySleep();
  check(p.m_vkd->waits == std::vector<uint64_t>{2}, "wait did not use the captured request value");
  check(p.m_latencySleepCounter == 0, "failed wait counted as a completed sleep");
  check(p.m_latencySleepDurationUs == 0, "failed wait changed sleep duration");

  p.m_vkd->waitResult = VK_SUCCESS;
  p.latencySleep();
  check(p.m_vkd->requests == std::vector<uint64_t>({1, 2, 3}), "request values were reused after failure");
  check(p.m_vkd->waits == std::vector<uint64_t>({2, 3}), "recovery waited on the wrong value");
  check(p.m_latencySleepCounter == 1, "successful recovery was not counted");
  check(p.m_latencySleepDurationUs == 10, "successful recovery duration");

  Presenter concurrent;
  concurrent.m_vkd->concurrent = true;
  std::thread first([&] { concurrent.latencySleep(); });
  std::thread second([&] { concurrent.latencySleep(); });
  first.join();
  second.join();
  check(concurrent.m_vkd->barrierReached, "sleep wait held the presenter mutex");
  check(concurrent.m_vkd->requests == std::vector<uint64_t>({1, 2}), "concurrent requests reused a timeline value");
  std::sort(concurrent.m_vkd->waits.begin(), concurrent.m_vkd->waits.end());
  check(concurrent.m_vkd->waits == std::vector<uint64_t>({1, 2}), "concurrent waits used a shared counter");
  check(concurrent.m_latencySleepCounter == 2, "concurrent completions were lost");
}

static void reportTests() {
  D3D11ReflexDevice device;
  D3D_LOW_LATENCY_RESULTS results = {};
  check(device.GetLatencyInfo(nullptr) == E_INVALIDARG, "null report output accepted");
  device.m_reflexEnabled = false;
  results.frameReports[0].frameID = 42;
  check(device.GetLatencyInfo(&results) == DXGI_ERROR_INVALID_CALL, "unsupported report query accepted");
  check(results.frameReports[0].frameID == 0, "unsupported report query left stale data");

  device.m_reflexEnabled = true;
  check(device.GetLatencyInfo(&results) == S_OK, "missing swapchain query failed");
  device.m_swapChain = std::make_shared<SwapChain>();
  auto& reports = device.m_swapChain->reports;
  reports[0].presentID = uint64_t(1) << 40;
  reports[0].inputSampleTimeUs = 17;
  reports[0].gpuRenderStartTimeUs = 100;
  reports[0].gpuRenderEndTimeUs = 200;
  reports[1].gpuRenderEndTimeUs = 400;
  reports[2].gpuRenderStartTimeUs = 600;
  reports[2].gpuRenderEndTimeUs = 500;
  reports[3].gpuRenderStartTimeUs = 10;
  reports[3].gpuRenderEndTimeUs = 20;
  reports[4].gpuRenderStartTimeUs = 1;
  reports[4].gpuRenderEndTimeUs = uint64_t(UINT32_MAX) + 200;
  reports[5].gpuRenderStartTimeUs = 1;
  reports[6].gpuRenderStartTimeUs = 20;
  reports[6].gpuRenderEndTimeUs = 30;

  device.m_swapChain->available = 63;
  check(device.GetLatencyInfo(&results) == S_OK, "partial report query failed");
  check(results.frameReports[0].frameID == 0, "partial reports escaped the 64-frame contract");
  device.m_swapChain->available = 64;
  check(device.GetLatencyInfo(&results) == S_OK, "complete report query failed");
  check(results.frameReports[0].frameID == reports[0].presentID, "application frame ID changed");
  check(results.frameReports[0].inputSampleTime == 17, "driver marker timestamp changed");
  check(results.frameReports[0].gpuActiveRenderTimeUs == 100, "valid active duration");
  check(results.frameReports[1].gpuActiveRenderTimeUs == 0, "missing begin timestamp became an active duration");
  check(results.frameReports[0].gpuFrameTimeUs == 0, "first frame invented a prior timestamp");
  check(results.frameReports[1].gpuFrameTimeUs == 200, "valid frame interval");
  check(results.frameReports[2].gpuActiveRenderTimeUs == 0, "backwards active duration wrapped");
  check(results.frameReports[3].gpuFrameTimeUs == 0, "backwards frame interval wrapped");
  check(results.frameReports[4].gpuActiveRenderTimeUs == UINT32_MAX, "active duration did not saturate");
  check(results.frameReports[4].gpuFrameTimeUs == UINT32_MAX, "frame interval did not saturate");
  check(results.frameReports[5].gpuActiveRenderTimeUs == 0, "missing end timestamp became an active duration");
  check(results.frameReports[6].gpuFrameTimeUs == 0, "missing prior end timestamp became a frame interval");
}

}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  std::string selection = argv[1];
  if (selection != "sleep" && selection != "reports" && selection != "all") return 2;
  if (selection != "reports") dxvk::sleepTests();
  if (selection != "sleep") dxvk::reportTests();
  std::cout << "Reflex integration: " << dxvk::checks << " checks passed\n";
}
