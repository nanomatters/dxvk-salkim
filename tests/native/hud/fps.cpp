#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

namespace dxvk {
using high_resolution_clock = std::chrono::steady_clock;
struct RcObject {};
namespace str {
template<typename... T> std::string format(T... values) {
  std::ostringstream stream;
  (stream << ... << values);
  return stream.str();
}
}
namespace hud {
#include "fps.h"
}
}

static unsigned checks;
static void check(bool value, const char* message) {
  ++checks;
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
static auto at(int64_t ns) {
  return dxvk::high_resolution_clock::time_point(std::chrono::nanoseconds(ns));
}

int main() {
  using dxvk::hud::HudFpsData;
  HudFpsData regular, sparse, external;
  for (uint64_t i = 0; i <= 120; i++) {
    regular.update(at(i * 10'000'000), i + 1);
    external.update(at(i * 10'000'000));
    if (!(i % 3)) sparse.update(at(i * 10'000'000), i + 1);
  }
  for (auto* data : { &regular, &sparse, &external }) {
    check(data->text() == "100.0", "text counts frame IDs across skipped reports");
    check(std::abs(data->value() - 100.0f) < 0.01f, "graph counts frame IDs across skipped reports");
  }
  auto last = regular.sampleTimeNs();
  regular.update(at(1'300'000'000), 121);
  check(regular.sampleTimeNs() == last, "polling the same frame does not add a sample");
  regular.update(at(1'200'000'000), 122);
  check(regular.sampleTimeNs() == last, "identical timestamps do not divide by zero");
  regular.update(at(1'220'000'000), 123);
  check(std::abs(regular.value() - 100.0f) < 0.01f, "next timestamp counts skipped frames");

  regular.update(at(1'230'000'000), 1);
  check(regular.text() == "--" && regular.sampleTimeNs() == -1, "frame ID reset clears old statistics");
  for (uint64_t i = 1; i <= 60; i++) regular.update(at(1'230'000'000 + i * 10'000'000), i + 1);
  check(regular.text() == "100.0", "sampling recovers after frame ID reset");
  regular.update(at(10'000'000), 62);
  check(regular.text() == "--", "backward timestamp clears old statistics");

  HudFpsData bounded;
  for (uint64_t i = 0; i <= 110000; i++) bounded.update(at(i * 5000), i + 1);
  check(std::abs(bounded.value() - 200000.0f) < 0.01f, "bounded ring retains the correct rate after wrapping");
  check(bounded.text() == "200000.0", "text rate survives ring wrapping");
  std::cout << "HUD FPS: " << checks << " checks passed\n";
}
