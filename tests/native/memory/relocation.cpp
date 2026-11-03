#include <atomic>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "test_common.h"

using VkDeviceSize = uint64_t;
template<typename T> using Rc = std::shared_ptr<T>;
namespace dxvk { using mutex = TestMutex; }
struct RcHash {
  template<typename T> size_t operator()(const Rc<T>& resource) const {
    return std::hash<Rc<T>>{}(resource);
  }
};

enum class DxvkAllocationMode { NoDeviceMemory = 1 };
struct DxvkAllocationModes {
  unsigned bits = 0;
  bool operator==(const DxvkAllocationModes&) const = default;
  bool test(DxvkAllocationMode value) const { return bits & unsigned(value); }
};
struct DxvkResourceMemoryInfo { uint64_t memory = 0, offset = 0, size = 0; };
struct DxvkResourceAllocation {
  DxvkResourceMemoryInfo info;
  DxvkResourceMemoryInfo getMemoryInfo() const { return info; }
};
struct DxvkPagedResource {
  explicit DxvkPagedResource(uint64_t id) : id(id) { live++; }
  ~DxvkPagedResource() { live--; }
  uint64_t cookie() const { return id; }
  uint64_t id;
  static inline std::atomic<unsigned> live = 0;
};
struct DxvkRelocationEntry {
  Rc<DxvkPagedResource> resource;
  DxvkAllocationModes mode;
  bool changesResidency;
  static inline thread_local std::function<void()> onConstruct;

  DxvkRelocationEntry(Rc<DxvkPagedResource>&& resource, DxvkAllocationModes mode, bool changesResidency)
  : resource(std::move(resource)), mode(mode), changesResidency(changesResidency) {
    if (onConstruct)
      onConstruct();
  }
};

#include "production.h"

static void testEmptyWaitsForInsertion() {
  DxvkRelocationList list;
  Observation started, release, query;
  std::thread writer([&] {
    DxvkRelocationEntry::onConstruct = [&] {
      started.set(1);
      release.get();
    };
    list.addResource(std::make_shared<DxvkPagedResource>(1), nullptr, {}, false);
  });
  started.get();
  bool empty = true;
  std::thread reader([&] {
    TestMutex::onWait = [&] { query.set(1); };
    empty = list.empty();
    query.set(2);
  });
  unsigned observed = query.get();
  release.set(1);
  writer.join();
  reader.join();
  require(observed == 1, "Empty check bypassed the in-progress insertion");
  require(!empty, "Empty check missed the completed insertion");
}

static void testOwnershipAndRemoval() {
  DxvkRelocationList list;
  require(list.empty(), "New relocation list was not empty");
  auto resource = std::make_shared<DxvkPagedResource>(1);
  list.addResource(Rc<DxvkPagedResource>(resource), nullptr, {}, false);
  list.addResource(Rc<DxvkPagedResource>(resource), nullptr, {}, false);
  require(resource.use_count() == 3, "Duplicate relocation retained an extra reference");
  require(!list.empty(), "Inserted resource was not visible");
  require(list.poll(0, 0).empty(), "Zero-count poll removed entries");
  auto removed = list.poll(1, 0);
  require(removed.size() == 1, "Poll did not return the queued entry");
  require(list.empty(), "Poll left a duplicate entry");
  require(resource.use_count() == 2, "Poll dropped the returned resource reference");
  removed.clear();
  require(resource.use_count() == 1, "Returned entry retained a resource reference");
  list.addResource(Rc<DxvkPagedResource>(resource), nullptr, {}, false);
  list.clear();
  require(list.empty(), "Clear left a queued entry");
  require(resource.use_count() == 1, "Clear retained a resource reference");
}

static void testConcurrentAddAndPoll() {
  DxvkRelocationList list;
  std::atomic<bool> done = false;
  std::thread writer([&] {
    for (unsigned i = 0; i < 10000; i++)
      list.addResource(std::make_shared<DxvkPagedResource>(i), nullptr, {}, false);
    done = true;
  });
  unsigned removed = 0;
  do {
    if (!list.empty())
      removed += list.poll(32, 16u << 20).size();
  } while (!done.load() || !list.empty());
  writer.join();
  require(removed == 10000, "Concurrent polling lost a relocation request");
  require(list.empty(), "Concurrent polling left entries behind");
}

int main() {
  try {
    testEmptyWaitsForInsertion();
    testOwnershipAndRemoval();
    testConcurrentAddAndPoll();
    require(DxvkPagedResource::live == 0, "Tests leaked resource references");
    std::cout << "Relocation tests passed, including 10000 concurrent requests\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
