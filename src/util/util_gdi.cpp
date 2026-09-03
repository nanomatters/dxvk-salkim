#include "util_gdi.h"
#include "thread.h"
#include "log/log.h"

#include <unordered_set>

namespace dxvk {

  struct WinePresentationSourceProcs {
    using ActivateProc = BOOL (WINAPI*) (HWND, UINT);
    using HasProc = BOOL (WINAPI*) (HWND);
    using RegisterProc = UINT (WINAPI*) (HWND);
    using UnregisterProc = BOOL (WINAPI*) (HWND, UINT);

    ActivateProc activate = nullptr;
    HasProc has = nullptr;
    RegisterProc registerSource = nullptr;
    UnregisterProc unregisterSource = nullptr;

    explicit operator bool () const {
      return activate && has && registerSource && unregisterSource;
    }
  };

  static const WinePresentationSourceProcs& getWinePresentationSourceProcs() {
#ifdef _WIN32
    static const auto procs = [] {
      WinePresentationSourceProcs result;
      HMODULE module = ::GetModuleHandleW(L"win32u.dll");
      if (module) {
        result.activate = reinterpret_cast<WinePresentationSourceProcs::ActivateProc>(
          ::GetProcAddress(module, "__wine_activate_window_flip_presenter"));
        result.has = reinterpret_cast<WinePresentationSourceProcs::HasProc>(
          ::GetProcAddress(module, "__wine_has_window_flip_presenter"));
        result.registerSource = reinterpret_cast<WinePresentationSourceProcs::RegisterProc>(
          ::GetProcAddress(module, "__wine_register_window_flip_presenter"));
        result.unregisterSource = reinterpret_cast<WinePresentationSourceProcs::UnregisterProc>(
          ::GetProcAddress(module, "__wine_unregister_window_flip_presenter"));
      }
      return result;
    }();
    return procs;
#else
    static const WinePresentationSourceProcs procs;
    return procs;
#endif
  }


  static dxvk::mutex presentationSourceMutex;
  static std::unordered_set<HWND> presentationSources;

#ifndef _WIN32
  NTSTATUS WINAPI D3DKMTAcquireKeyedMutex(D3DKMT_ACQUIREKEYEDMUTEX *desc) {
    Logger::warn("D3DKMTAcquireKeyedMutex: Not available on this platform.");
    return -1;
  }

  NTSTATUS D3DKMTCloseAdapter(const D3DKMT_CLOSEADAPTER *desc) {
    Logger::warn("D3DKMTCloseAdapter: Not available on this platform.");
    return -1;
  }

  NTSTATUS D3DKMTCreateDCFromMemory(D3DKMT_CREATEDCFROMMEMORY *desc) {
    Logger::warn("D3DKMTCreateDCFromMemory: Not available on this platform.");
    return -1;
  }

  NTSTATUS D3DKMTCreateDevice(D3DKMT_CREATEDEVICE *desc) {
    Logger::warn("D3DKMTCreateDevice: Not available on this platform.");
    return -1;
  }

  NTSTATUS D3DKMTCreateKeyedMutex2(D3DKMT_CREATEKEYEDMUTEX2 *desc) {
    Logger::warn("D3DKMTCreateKeyedMutex2: Not available on this platform.");
    return -1;
  }

  NTSTATUS D3DKMTDestroyAllocation(const D3DKMT_DESTROYALLOCATION *desc) {
    Logger::warn("D3DKMTDestroyAllocation: Not available on this platform.");
    return -1;
  }

  NTSTATUS D3DKMTDestroyDCFromMemory(const D3DKMT_DESTROYDCFROMMEMORY *desc) {
    Logger::warn("D3DKMTDestroyDCFromMemory: Not available on this platform.");
    return -1;
  }

  NTSTATUS D3DKMTDestroyDevice(const D3DKMT_DESTROYDEVICE *desc) {
    Logger::warn("D3DKMTDestroyDevice: Not available on this platform.");
    return -1;
  }

  NTSTATUS D3DKMTDestroyKeyedMutex(const D3DKMT_DESTROYKEYEDMUTEX *desc) {
    Logger::warn("D3DKMTDestroyKeyedMutex: Not available on this platform.");
    return -1;
  }

  NTSTATUS WINAPI D3DKMTDestroySynchronizationObject(const D3DKMT_DESTROYSYNCHRONIZATIONOBJECT *desc) {
    Logger::warn("D3DKMTDestroySynchronizationObject: Not available on this platform.");
    return -1;
  }

  NTSTATUS D3DKMTEscape(const D3DKMT_ESCAPE *desc) {
    Logger::warn("D3DKMTEscape: Not available on this platform.");
    return -1;
  }

  NTSTATUS D3DKMTOpenAdapterFromLuid(D3DKMT_OPENADAPTERFROMLUID *desc) {
    Logger::warn("D3DKMTOpenAdapterFromLuid: Not available on this platform.");
    return -1;
  }

  NTSTATUS D3DKMTOpenKeyedMutex(D3DKMT_OPENKEYEDMUTEX *desc) {
    Logger::warn("D3DKMTOpenKeyedMutex: Not available on this platform.");
    return -1;
  }

  NTSTATUS D3DKMTOpenResource2(D3DKMT_OPENRESOURCE *desc) {
    Logger::warn("D3DKMTOpenResource2: Not available on this platform.");
    return -1;
  }

  NTSTATUS D3DKMTOpenResourceFromNtHandle(D3DKMT_OPENRESOURCEFROMNTHANDLE *desc) {
    Logger::warn("D3DKMTOpenResourceFromNtHandle: Not available on this platform.");
    return -1;
  }

  NTSTATUS WINAPI D3DKMTOpenSynchronizationObject(D3DKMT_OPENSYNCHRONIZATIONOBJECT *desc) {
    Logger::warn("D3DKMTOpenSynchronizationObject: Not available on this platform.");
    return -1;
  }

  NTSTATUS WINAPI D3DKMTOpenSyncObjectFromNtHandle(D3DKMT_OPENSYNCOBJECTFROMNTHANDLE *desc) {
    Logger::warn("D3DKMTOpenSyncObjectFromNtHandle: Not available on this platform.");
    return -1;
  }

  NTSTATUS D3DKMTQueryAdapterInfo(D3DKMT_QUERYADAPTERINFO *desc) {
    return D3DKMT_STATUS_NOT_IMPLEMENTED;
  }

  NTSTATUS D3DKMTQueryResourceInfo(D3DKMT_QUERYRESOURCEINFO *desc) {
    Logger::warn("D3DKMTQueryResourceInfo: Not available on this platform.");
    return -1;
  }

  NTSTATUS D3DKMTQueryResourceInfoFromNtHandle(D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE *desc) {
    Logger::warn("D3DKMTQueryResourceInfoFromNtHandle: Not available on this platform.");
    return -1;
  }

  NTSTATUS WINAPI D3DKMTReleaseKeyedMutex(D3DKMT_RELEASEKEYEDMUTEX *desc) {
    Logger::warn("D3DKMTReleaseKeyedMutex: Not available on this platform.");
    return -1;
  }

  NTSTATUS WINAPI D3DKMTShareObjects(UINT count, const D3DKMT_HANDLE *handles, OBJECT_ATTRIBUTES *attr, UINT access, HANDLE *handle) {
    Logger::warn("D3DKMTShareObjects: Not available on this platform.");
    return -1;
  }
#else
  static NTSTATUS WINAPI NoD3DKMTAcquireKeyedMutex(D3DKMT_ACQUIREKEYEDMUTEX *desc) {
    return -1;
  }

  NTSTATUS WINAPI D3DKMTAcquireKeyedMutex(D3DKMT_ACQUIREKEYEDMUTEX *desc) {
    static decltype(D3DKMTAcquireKeyedMutex) *func;
    if (!func) {
      InterlockedCompareExchangePointer((void **)&func, (void *)GetProcAddress(GetModuleHandle("gdi32"), "D3DKMTAcquireKeyedMutex"), NULL);
      InterlockedCompareExchangePointer((void **)&func, (void *)NoD3DKMTAcquireKeyedMutex, NULL);
    }
    return func(desc);
  }

  static NTSTATUS WINAPI NoD3DKMTReleaseKeyedMutex(D3DKMT_RELEASEKEYEDMUTEX *desc) {
    return -1;
  }

  NTSTATUS WINAPI D3DKMTReleaseKeyedMutex(D3DKMT_RELEASEKEYEDMUTEX *desc) {
    static decltype(D3DKMTReleaseKeyedMutex) *func;
    if (!func) {
      InterlockedCompareExchangePointer((void **)&func, (void *)GetProcAddress(GetModuleHandle("gdi32"), "D3DKMTReleaseKeyedMutex"), NULL);
      InterlockedCompareExchangePointer((void **)&func, (void *)NoD3DKMTReleaseKeyedMutex, NULL);
    }
    return func(desc);
  }
#endif

  WinePresentationSource::WinePresentationSource(
          WinePresentationSource&& other) noexcept
  : m_window(other.m_window), m_id(other.m_id),
    m_registered(other.m_registered), m_active(other.m_active),
    m_exclusive(other.m_exclusive) {
    other.m_window = nullptr;
    other.m_id = 0;
    other.m_registered = false;
    other.m_active = false;
    other.m_exclusive = false;
  }


  WinePresentationSource& WinePresentationSource::operator = (
          WinePresentationSource&& other) noexcept {
    if (this != &other) {
      reset();
      m_window = other.m_window;
      m_id = other.m_id;
      m_registered = other.m_registered;
      m_active = other.m_active;
      m_exclusive = other.m_exclusive;
      other.m_window = nullptr;
      other.m_id = 0;
      other.m_registered = false;
      other.m_active = false;
      other.m_exclusive = false;
    }

    return *this;
  }


  WinePresentationSource::~WinePresentationSource() {
    reset();
  }


  WinePresentationSourceStatus WinePresentationSource::registerFlip(
          HWND  window) {
    return registerFlip(window, false);
  }


  WinePresentationSourceStatus WinePresentationSource::registerExclusiveFlip(
          HWND  window) {
    return registerFlip(window, true);
  }


  WinePresentationSourceStatus WinePresentationSource::registerFlip(
          HWND  window,
          bool  exclusive) {
    if (m_registered && m_window == window && m_exclusive == exclusive)
      return WinePresentationSourceStatus::Registered;

    reset();

    if (!window)
      return WinePresentationSourceStatus::Conflict;

    std::lock_guard lock(presentationSourceMutex);

    if (exclusive && !presentationSources.insert(window).second)
      return WinePresentationSourceStatus::Conflict;

    const auto& procs = getWinePresentationSourceProcs();

    if (procs && !(m_id = procs.registerSource(window))) {
      if (exclusive)
        presentationSources.erase(window);
      return WinePresentationSourceStatus::Conflict;
    }

    m_window = window;
    m_registered = true;
    m_active = false;
    m_exclusive = exclusive;
    return WinePresentationSourceStatus::Registered;
  }


  void WinePresentationSource::reset() {
    if (!m_registered)
      return;

    std::lock_guard lock(presentationSourceMutex);

    const auto& procs = getWinePresentationSourceProcs();

    if (m_id && procs)
      procs.unregisterSource(m_window, m_id);

    if (m_exclusive)
      presentationSources.erase(m_window);

    m_window = nullptr;
    m_id = 0;
    m_registered = false;
    m_active = false;
    m_exclusive = false;
  }


  void WinePresentationSource::activate() {
    if (!m_registered || m_active)
      return;

    const auto& procs = getWinePresentationSourceProcs();

    if (!m_id || (procs && procs.activate(m_window, m_id)))
      m_active = true;
  }


  WinePresentationBlitStatus WinePresentationSource::checkBlit(HWND window) {
    const auto& procs = getWinePresentationSourceProcs();

    if (!procs)
      return WinePresentationBlitStatus::Unavailable;

    if (procs.has(window))
      return WinePresentationBlitStatus::Suppressed;

    return WinePresentationBlitStatus::Allowed;
  }
}
