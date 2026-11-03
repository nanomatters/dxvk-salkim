#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using VkDeviceSize = uint64_t;
template<typename T> using Rc = std::shared_ptr<T>;
constexpr uint32_t VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT = 1u;

#include "test_common.h"

enum class DxvkAllocationMode { NoAllocation = 1, NoFallback = 2, NoDeviceMemory = 4 };
struct DxvkAllocationModes {
  unsigned bits;
  DxvkAllocationModes(DxvkAllocationMode value) : bits(unsigned(value)) { }
  void set(DxvkAllocationMode value) { bits |= unsigned(value); }
};
enum class DxvkAllocationFlag { CanMove };
struct AllocationFlags {
  bool movable = true;
  bool test(DxvkAllocationFlag) const { return movable; }
};
struct Allocation {
  std::atomic<uint64_t> m_resourceCookie = 0;
  Allocation* m_nextInChunk = nullptr;
  AllocationFlags properties;
  AllocationFlags flags() const { return properties; }
  struct Info { uint64_t size = 4096; };
  Info getMemoryInfo() const { return {}; }
};
struct Chunk {
  struct { uint64_t size = 64; } memory;
  Allocation* allocationList = nullptr;
};
struct PageAllocator {
  uint32_t pagesUsed(uint32_t i) const { return used.at(i); }
  bool chunkIsAvailable(uint32_t i) const { return live.at(i); }
  void killChunk(uint32_t i) { live.at(i) = false; }
  void reviveChunk(uint32_t i) { live.at(i) = true; }
  std::array<bool, 2> live = { true, true };
  std::array<uint32_t, 2> used = { 1, 1 };
};
struct Pool {
  std::vector<Chunk> chunks;
  PageAllocator pageAllocator;
  uint32_t nextEvictChunk = ~0u;
  uint32_t nextDefragChunk = ~0u;
  uint64_t maxChunkSize = 64;
};
struct Heap { bool enforceBudget = true; unsigned index = 0; };
struct DxvkMemoryType {
  Heap* heap = nullptr;
  struct { unsigned propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT; } properties;
  Pool devicePool;
};
struct DxvkMemoryStats { uint64_t memoryAllocated = 128, memoryBudget = 128; };
struct DxvkMemoryAllocator;

struct DxvkPagedResource : public std::enable_shared_from_this<DxvkPagedResource> {
  DxvkPagedResource(DxvkMemoryAllocator& allocator, uint64_t id);
  ~DxvkPagedResource();
  uint64_t cookie() const { return id; }

  Rc<DxvkPagedResource> tryAcquire() {
    auto reference = weak_from_this().lock();
    if (onAcquire)
      onAcquire();
    return reference;
  }

  bool requestEviction() {
    bool result = demoted.exchange(true);
    if (onRequest)
      onRequest();
    return result;
  }

  DxvkMemoryAllocator& allocator;
  uint64_t id;
  std::atomic<bool> demoted = false;
  std::function<void()> onAcquire;
  std::function<void()> onRequest;
  std::function<void()> onDestroy;
};

struct Relocations {
  bool addResource(Rc<DxvkPagedResource>&& resource, Allocation*, DxvkAllocationModes, bool) {
    for (const auto& entry : entries) {
      if (entry == resource)
        return false;
    }
    entries.push_back(std::move(resource));
    return true;
  }
  std::vector<Rc<DxvkPagedResource>> entries;
};

struct DxvkMemoryAllocator {
  std::unordered_map<uint64_t, DxvkPagedResource*> m_resourceMap;
  TestMutex m_resourceMutex;
  TestMutex m_mutex;
  Relocations m_relocations;
  DxvkMemoryStats stats;
  DxvkMemoryStats getMemoryStats(unsigned) const { return stats; }
  void evictResources(DxvkMemoryType&);
  void moveDefragChunk(DxvkMemoryType&);
  void registerResource(DxvkPagedResource*);
  void unregisterResource(DxvkPagedResource*);
};

#include "production.h"

DxvkPagedResource::DxvkPagedResource(DxvkMemoryAllocator& allocator, uint64_t id)
: allocator(allocator), id(id) {
  allocator.registerResource(this);
}

DxvkPagedResource::~DxvkPagedResource() {
  allocator.unregisterResource(this);
  std::unique_lock lock(allocator.m_mutex);
  if (onDestroy)
    onDestroy();
}

struct Fixture {
  DxvkMemoryAllocator allocator;
  Heap heap;
  DxvkMemoryType type;
  Allocation allocation;
  Rc<DxvkPagedResource> resource;

  Fixture() {
    type.heap = &heap;
    allocation.m_resourceCookie = 1;
    type.devicePool.chunks.emplace_back().allocationList = &allocation;
    resource = std::make_shared<DxvkPagedResource>(allocator, 1);
  }

  void evict() {
    std::lock_guard lock(allocator.m_mutex);
    allocator.evictResources(type);
  }

  ~Fixture() {
    allocator.m_relocations.entries.clear();
    resource.reset();
  }
};

static void testLookupSerialization() {
  Fixture f;
  Observation observation;
  f.resource->onRequest = [&] { observation.set(2); };
  std::unique_lock lock(f.allocator.m_resourceMutex);
  std::thread worker([&] {
    TestMutex::onWait = [&] { observation.set(1); };
    f.evict();
  });
  unsigned observed = observation.get();
  lock.unlock();
  worker.join();
  require(observed == 1, "Eviction accessed a resource while its map was locked by another thread");
}

static void testMovabilityChange() {
  Fixture f;
  Observation observation;
  bool acquired = false;
  f.resource->onRequest = [&] {
    acquired = true;
    observation.set(2);
  };
  std::unique_lock lock(f.allocator.m_resourceMutex);
  std::thread worker([&] {
    TestMutex::onWait = [&] { observation.set(1); };
    f.evict();
  });
  unsigned observed = observation.get();
  f.allocation.properties.movable = false;
  lock.unlock();
  worker.join();
  require(observed == 1, "Eviction did not wait for the address lock");
  require(!acquired, "Eviction used a stale movability flag");
}

static void testLastReference() {
  Fixture f;
  Observation started, released;
  bool destroyed = false;
  f.resource->onDestroy = [&] { destroyed = true; };
  f.resource->onRequest = [&] {
    started.set(1);
    released.get();
  };
  std::thread releaser([&] {
    started.get();
    TestMutex::onWait = [&] { released.set(1); };
    f.resource.reset();
    released.set(2);
  });
  f.evict();
  releaser.join();
  require(destroyed, "Final resource reference was not released");
  require(f.allocator.m_resourceMap.empty(), "Destroyed resource stayed registered");
  require(f.allocator.m_relocations.entries.empty(), "Active resource was incorrectly evicted");
}

static void testEvictedLastReference() {
  Fixture f;
  bool destroyed = false;
  f.resource->demoted = true;
  f.resource->onDestroy = [&] { destroyed = true; };
  f.resource->onAcquire = [&] { f.resource.reset(); };
  f.evict();
  require(!destroyed, "Evicted resource was destroyed under the allocator lock");
  require(!f.allocator.m_relocations.entries.empty(), "Evicted resource reference was not retained");
  f.allocator.m_relocations.entries.clear();
  require(destroyed, "Evicted resource reference was leaked");
}

static void testEvictionAndDefrag() {
  Fixture f;
  f.evict();
  require(f.allocator.m_relocations.entries.empty(), "First pass should only demote");
  f.evict();
  require(f.allocator.m_relocations.entries.size() == 1, "Eviction and defrag did not retain one resource reference");
  require(!f.type.devicePool.pageAllocator.live[0], "Evicted chunk stayed available");
}

static void testSkippedResources() {
  {
    Fixture f;
    f.resource.reset();
    f.evict();
    require(f.allocator.m_relocations.entries.empty(), "Missing resource was evicted");
  }
  {
    Fixture f;
    f.allocation.properties.movable = false;
    f.evict();
    require(!f.resource->demoted, "Immovable resource was demoted");
  }
  {
    Fixture f;
    f.allocator.stats.memoryBudget = 1024;
    f.evict();
    require(!f.resource->demoted, "Eviction ran with enough headroom");
  }
  {
    Fixture f;
    f.heap.enforceBudget = false;
    f.evict();
    require(!f.resource->demoted, "Eviction ran with no enforced budget");
  }
}

static void testConcurrentRegistration() {
  Fixture f;
  std::atomic<bool> start = false, done = false;
  std::thread writer([&] {
    while (!start.load())
      std::this_thread::yield();
    while (!done.load()) {
      f.allocator.unregisterResource(f.resource.get());
      f.allocator.registerResource(f.resource.get());
      std::lock_guard lock(f.allocator.m_resourceMutex);
      f.allocation.properties.movable = !f.allocation.properties.movable;
    }
  });
  start = true;
  for (unsigned i = 0; i < 10000; i++)
    f.evict();
  done = true;
  writer.join();
  require(!f.allocator.m_relocations.entries.empty(), "Concurrent test never exercised relocation");
}

int main() {
  try {
    testLookupSerialization();
    testMovabilityChange();
    testLastReference();
    testEvictedLastReference();
    testEvictionAndDefrag();
    testSkippedResources();
    testConcurrentRegistration();
    std::cout << "Eviction tests passed, including 10000 concurrent scans\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
