#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

#define STDMETHODCALLTYPE
#define unlikely(x) (x)
using HRESULT = int32_t;
using UINT = uint32_t;
constexpr HRESULT D3D_OK = 0;
constexpr HRESULT D3DERR_INVALIDCALL = -1;
constexpr uint32_t D3DCREATE_ADAPTERGROUP_DEVICE = 0x200;

struct IDirect3DSurface9 { int capturedHead = -1; };
struct D3D9DeviceLock { ~D3D9DeviceLock() {} };
struct D3D9SwapChainEx {
  int head;
  HRESULT GetFrontBufferData(IDirect3DSurface9* dest) {
    dest->capturedHead = head;
    return D3D_OK;
  }
};
struct Ptr {
  D3D9SwapChainEx* value;
  D3D9SwapChainEx* ptr() const { return value; }
};
struct D3D9DeviceEx {
  uint32_t m_behaviorFlags;
  std::vector<Ptr> m_groupHeads;
  Ptr m_implicitSwapchain;
  D3D9SwapChainEx* m_mostRecentlyUsedSwapchain;
  D3D9DeviceLock LockDevice() { return {}; }
  HRESULT GetFrontBufferData(UINT, IDirect3DSurface9*);
  D3D9SwapChainEx* GetHeadSwapChain(UINT) const;
#include "setter.h"
};
#include "production.h"

int main() {
  D3D9SwapChainEx head0{0}, head1{1}, head2{2}, extra{3};
  D3D9DeviceEx device{D3DCREATE_ADAPTERGROUP_DEVICE,
    {{&head1}, {&head2}}, {&head0}, &head0};
  unsigned checks = 0, failures = 0;
  auto check = [&](UINT requested, int expected) {
    IDirect3DSurface9 output;
    HRESULT result = device.GetFrontBufferData(requested, &output);
    HRESULT expectedResult = expected < 0 ? D3DERR_INVALIDCALL : D3D_OK;
    ++checks;
    if (result != expectedResult || output.capturedHead != expected) {
      std::fprintf(stderr, "requested=%u expected=%d captured=%d status=%d\n",
        requested, expected, output.capturedHead, result);
      ++failures;
    }
  };
  for (auto* presented : {&head0, &head1, &head2}) {
    // Present records this swapchain before the application requests a capture.
    device.SetMostRecentlyUsedSwapchain(presented);
    check(0, 0);
    check(1, 1);
    check(2, 2);
    check(3, -1);
    check(std::numeric_limits<UINT>::max(), -1);
  }
  device.m_groupHeads.clear();
  device.SetMostRecentlyUsedSwapchain(&head0);
  check(0, 0);
  check(1, -1);

  // Ordinary windowed devices retain the additional-swapchain workaround.
  device.m_behaviorFlags = 0;
  device.SetMostRecentlyUsedSwapchain(&extra);
  check(0, 3);
  check(1, -1);
  device.SetMostRecentlyUsedSwapchain(&head0);
  check(0, 0);
  std::printf("Front-buffer selection: %u checks, %u failures\n", checks, failures);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
