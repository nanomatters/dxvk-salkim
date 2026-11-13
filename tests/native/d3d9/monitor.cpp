#include <iterator>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <functional>
#include <string>
#include <thread>
#include <utility>

using HMONITOR = uintptr_t;
using WCHAR = wchar_t;

static void check(bool condition) {
  if (!condition)
    std::abort();
}

namespace wsi {
  static constexpr const WCHAR* names[] = {L"display1", L"display2"};

  HMONITOR getDefaultMonitor() {
    return 1;
  }

  HMONITOR enumMonitors(uint32_t index) {
    return index < std::size(names) ? index + 1 : 0;
  }

  bool getDisplayName(HMONITOR monitor, WCHAR (&name)[32]) {
    if (!monitor || monitor > std::size(names))
      return false;
    std::wcscpy(name, names[monitor - 1]);
    return true;
  }
}

namespace str {
  std::string fromws(const WCHAR* name) {
    return std::string(name, name + std::wcslen(name));
  }

  template<typename... Args>
  std::string format(Args&&...) {
    return "No monitor found";
  }
}

struct Logger {
  static inline std::atomic<unsigned> warnings{0};

  static void warn(const std::string&) {
    warnings.fetch_add(1, std::memory_order_relaxed);
  }
};

struct D3D9Adapter {
  std::string m_displayName;
  HMONITOR GetMonitor();
};

#include "monitor.h"

int main() {
  D3D9Adapter legacy{ "" }, primary{ "display1" }, secondary{ "display2" };
  check(legacy.GetMonitor() == wsi::getDefaultMonitor());
  check(primary.GetMonitor() == 1);
  check(secondary.GetMonitor() == 2);
  check(Logger::warnings.load() == 0);

  D3D9Adapter first{ "disconnected1" }, second{ "disconnected2" };
  std::atomic<unsigned> ready{0};
  auto query = [&ready](D3D9Adapter& adapter) {
    ready.fetch_add(1);
    while (ready.load() != 2)
      std::this_thread::yield();
    for (unsigned i = 0; i < 10000; i++)
      check(adapter.GetMonitor() == wsi::getDefaultMonitor());
  };
  std::thread a(query, std::ref(first)), b(query, std::ref(second));
  a.join();
  b.join();

  check(first.GetMonitor() == wsi::getDefaultMonitor());
  check(Logger::warnings.load() == 1);
  check(secondary.GetMonitor() == 2);
  std::puts("PASS: monitor selection, 20,000 concurrent fallbacks and one warning");
}
